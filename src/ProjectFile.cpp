#include "ProjectFile.h"

#include <QByteArray>
#include <QFile>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <span>
#include <type_traits>
#include <vector>

#include "Unicode.h"
#include "document/XpressCodec.h"

namespace cnote {

bool HasExtension(const std::filesystem::path& path, std::u16string_view extension) {
    const std::u16string actual = path.extension().u16string();
    if (actual.size() != extension.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const char16_t a = actual[index];
        const char16_t b = extension[index];
        const char16_t lowerA = (a >= u'A' && a <= u'Z') ? static_cast<char16_t>(a + 32) : a;
        const char16_t lowerB = (b >= u'A' && b <= u'Z') ? static_cast<char16_t>(b + 32) : b;
        if (lowerA != lowerB) {
            return false;
        }
    }
    return true;
}

namespace {

constexpr char kCnoteMagic[] = { 'C', 'N', 'O', 'T', 'E', '1', '\0', '\0' };
constexpr char kLegacyProjectMagic[] = { 'C', 'T', 'R', 'P', 'R', 'J', '1', '\0' };
constexpr std::uint32_t kProjectVersion = 16;
constexpr std::uint32_t kProjectFlagHasPdf = 1U << 0U;
constexpr std::uint32_t kProjectFlagInheritPageSize = 1U << 1U;
constexpr std::uint32_t kPageFlagHasRaster = 1U << 0U;
constexpr std::uint8_t kStrokeStyleFlagDotted = 1U << 0U;
constexpr std::uint8_t kTextStyleFlagBold = 1U << 0U;
constexpr std::uint8_t kTextStyleFlagItalic = 1U << 1U;
constexpr std::uint8_t kTextStyleFlagUnderline = 1U << 2U;
constexpr std::uint32_t kPayloadEncodingRaw = 0U;
constexpr std::uint32_t kPayloadEncodingXpressHuff = 1U;

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::filesystem::path& filePath) {
    QFile file(QString::fromUtf8(PathToUtf8(filePath).c_str()));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    const QByteArray bytes = file.readAll();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

bool WriteAllBytes(const std::filesystem::path& filePath, const std::vector<std::uint8_t>& bytes) {
    QSaveFile file(QString::fromUtf8(PathToUtf8(filePath).c_str()));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    if (!bytes.empty()
        && file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()))
            != static_cast<qint64>(bytes.size())) {
        file.cancelWriting();
        return false;
    }

    return file.commit();
}

struct ByteReader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;

    [[nodiscard]] std::size_t Remaining() const noexcept {
        return bytes.size() - offset;
    }

    [[nodiscard]] bool ReadExact(void* destination, std::size_t size) {
        if (size > Remaining()) {
            return false;
        }
        if (size != 0) std::memcpy(destination, bytes.data() + offset, size);
        offset += size;
        return true;
    }

    template <typename T>
    [[nodiscard]] bool Read(T& value) {
        static_assert(std::is_same_v<T, float> || std::is_same_v<T, std::uint8_t>
            || std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::uint64_t>
            || std::is_same_v<T, std::int32_t>);
        if constexpr (std::is_same_v<T, float>) {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
            std::uint32_t bits;
            if (!Read(bits)) return false;
            value = std::bit_cast<float>(bits);
        } else {
            if (sizeof(T) > Remaining()) return false;
            using Unsigned = std::make_unsigned_t<T>;
            Unsigned bits = 0;
            for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
                bits |= static_cast<Unsigned>(bytes[offset++]) << (byte * 8U);
            }
            value = std::bit_cast<T>(bits);
        }
        return true;
    }

    [[nodiscard]] bool ReadString(std::u16string& value) {
        std::uint32_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (size > Remaining()) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::string_view utf8(begin, size);
        value = Utf8ToUtf16(utf8);
        offset += size;
        return true;
    }

    [[nodiscard]] bool ReadPath(std::filesystem::path& value) {
        std::uint32_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (size > Remaining()) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
        value = Utf8ToPath(std::string_view(begin, size));
        offset += size;
        return true;
    }

    [[nodiscard]] bool ReadBlob(std::vector<std::uint8_t>& value) {
        std::uint64_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (size > Remaining()) {
            return false;
        }
        value.resize(static_cast<std::size_t>(size));
        if (!value.empty()) {
            std::memcpy(value.data(), bytes.data() + offset, value.size());
        }
        offset += value.size();
        return true;
    }
};

void AppendBytes(std::vector<std::uint8_t>& output, const void* data, std::size_t size) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), begin, begin + size);
}

template <typename T>
void AppendValue(std::vector<std::uint8_t>& output, const T& value) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, std::uint8_t>
        || std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::uint64_t>
        || std::is_same_v<T, std::int32_t>);
    if constexpr (std::is_same_v<T, float>) {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        AppendValue(output, std::bit_cast<std::uint32_t>(value));
    } else {
        using Unsigned = std::make_unsigned_t<T>;
        const auto bits = static_cast<Unsigned>(value);
        for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
            output.push_back(static_cast<std::uint8_t>(bits >> (byte * 8U)));
        }
    }
}

void AppendString(std::vector<std::uint8_t>& output, const std::u16string& value) {
    const std::string utf8 = Utf16ToUtf8(value);
    const auto size = static_cast<std::uint32_t>(utf8.size());
    AppendValue(output, size);
    if (!utf8.empty()) {
        AppendBytes(output, utf8.data(), utf8.size());
    }
}

void AppendPath(std::vector<std::uint8_t>& output, const std::filesystem::path& value) {
    const std::string utf8 = PathToUtf8(value);
    const auto size = static_cast<std::uint32_t>(utf8.size());
    AppendValue(output, size);
    if (!utf8.empty()) {
        AppendBytes(output, utf8.data(), utf8.size());
    }
}

void AppendBlob(std::vector<std::uint8_t>& output, const std::vector<std::uint8_t>& value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    AppendValue(output, size);
    if (!value.empty()) {
        AppendBytes(output, value.data(), value.size());
    }
}

void AppendPaletteProfile(std::vector<std::uint8_t>& output, const std::vector<std::uint32_t>& paletteProfile) {
    const auto colorCount = static_cast<std::uint32_t>(paletteProfile.size());
    AppendValue(output, colorCount);
    for (std::uint32_t color : paletteProfile) {
        AppendValue(output, color);
    }
}

bool ReadPaletteProfile(ByteReader& reader, std::vector<std::uint32_t>& paletteProfile) {
    std::uint32_t colorCount = 0;
    if (!reader.Read(colorCount) || colorCount > reader.Remaining() / 4U) {
        return false;
    }

    paletteProfile.clear();
    paletteProfile.reserve(colorCount);
    for (std::uint32_t colorIndex = 0; colorIndex < colorCount; ++colorIndex) {
        std::uint32_t color = 0;
        if (!reader.Read(color)) {
            return false;
        }
        paletteProfile.push_back(color);
    }
    return true;
}

void AppendStroke(std::vector<std::uint8_t>& output, const Stroke& stroke) {
    std::uint8_t styleFlags = 0;
    if (stroke.style.dotted) {
        styleFlags |= kStrokeStyleFlagDotted;
    }
    AppendValue(output, stroke.style.color);
    AppendValue(output, stroke.style.width);
    AppendValue(output, styleFlags);
    AppendValue(output, stroke.smoothing);
    const auto pointCount = static_cast<std::uint32_t>(stroke.points.size());
    AppendValue(output, pointCount);
    for (const Vec2& point : stroke.points) {
        AppendValue(output, point.x);
        AppendValue(output, point.y);
    }
}

bool ReadStroke(ByteReader& reader, std::uint32_t version, Stroke& stroke) {
    std::uint32_t pointCount = 0;
    std::uint8_t styleFlags = 0;
    if (!reader.Read(stroke.style.color) || !reader.Read(stroke.style.width)) {
        return false;
    }
    if (version >= 9U && !reader.Read(styleFlags)) {
        return false;
    }
    stroke.style.dotted = (styleFlags & kStrokeStyleFlagDotted) != 0U;
    if (version >= 10U && !reader.Read(stroke.smoothing)) {
        return false;
    }
    if (!reader.Read(pointCount) || pointCount > reader.Remaining() / 8U) {
        return false;
    }

    stroke.points.clear();
    stroke.points.reserve(pointCount);
    for (std::uint32_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
        Vec2 point {};
        if (!reader.Read(point.x) || !reader.Read(point.y)) {
            return false;
        }
        stroke.points.push_back(point);
    }
    return true;
}

void AppendImage(std::vector<std::uint8_t>& output, const PageImage& image) {
    AppendValue(output, image.x);
    AppendValue(output, image.y);
    AppendValue(output, image.width);
    AppendValue(output, image.height);
    AppendValue(output, image.rotationDegrees);
    AppendValue(output, static_cast<std::int32_t>(image.pixelWidth));
    AppendValue(output, static_cast<std::int32_t>(image.pixelHeight));
    AppendValue(output, static_cast<std::int32_t>(image.stride));
    AppendBlob(output, image.pixels);
}

bool ReadImage(ByteReader& reader, PageImage& image) {
    if (!reader.Read(image.x) || !reader.Read(image.y) || !reader.Read(image.width) || !reader.Read(image.height)
        || !reader.Read(image.rotationDegrees) || !reader.Read(image.pixelWidth)
        || !reader.Read(image.pixelHeight) || !reader.Read(image.stride)) {
        return false;
    }

    return reader.ReadBlob(image.pixels);
}

void AppendText(std::vector<std::uint8_t>& output, const PageText& text) {
    const auto alignment = static_cast<std::uint8_t>(text.alignment);
    const auto fontFamily = static_cast<std::uint8_t>(text.fontFamily);
    std::uint8_t styleFlags = 0;
    if (text.bold) {
        styleFlags |= kTextStyleFlagBold;
    }
    if (text.italic) {
        styleFlags |= kTextStyleFlagItalic;
    }
    if (text.underline) {
        styleFlags |= kTextStyleFlagUnderline;
    }
    constexpr std::uint8_t reserved = 0;
    AppendValue(output, text.id);
    AppendValue(output, text.x);
    AppendValue(output, text.y);
    AppendValue(output, text.width);
    AppendValue(output, text.height);
    AppendValue(output, text.fontSize);
    AppendValue(output, text.rotationDegrees);
    AppendValue(output, text.color);
    AppendValue(output, alignment);
    AppendValue(output, fontFamily);
    AppendValue(output, styleFlags);
    AppendValue(output, reserved);
    AppendString(output, text.content);
    AppendValue(output, static_cast<std::uint32_t>(text.styleSpans.size()));
    for (const PageTextStyleSpan& span : text.styleSpans) {
        const auto spanFontFamily = static_cast<std::uint8_t>(span.style.fontFamily);
        std::uint8_t spanFlags = 0;
        spanFlags |= span.style.bold ? kTextStyleFlagBold : 0;
        spanFlags |= span.style.italic ? kTextStyleFlagItalic : 0;
        spanFlags |= span.style.underline ? kTextStyleFlagUnderline : 0;
        AppendValue(output, span.start);
        AppendValue(output, span.length);
        AppendValue(output, span.style.fontSize);
        AppendValue(output, span.style.color);
        AppendValue(output, spanFontFamily);
        AppendValue(output, spanFlags);
    }
}

void AppendLatex(std::vector<std::uint8_t>& output, const PageLatex& latex) {
    AppendValue(output, latex.id);
    AppendValue(output, latex.x);
    AppendValue(output, latex.y);
    AppendValue(output, latex.fontSize);
    AppendValue(output, latex.rotationDegrees);
    AppendValue(output, latex.color);
    AppendString(output, latex.content);
}

bool ReadLatex(ByteReader& reader, std::uint32_t version, PageLatex& latex) {
    return reader.Read(latex.id)
        && reader.Read(latex.x)
        && reader.Read(latex.y)
        && reader.Read(latex.fontSize)
        && (version < 15U || reader.Read(latex.rotationDegrees))
        && reader.Read(latex.color)
        && reader.ReadString(latex.content);
}

bool ReadText(ByteReader& reader, std::uint32_t version, PageText& text) {
    std::uint8_t alignment = 0;
    std::uint8_t fontFamily = 0;
    std::uint8_t styleFlags = 0;
    std::uint8_t reserved = 0;
    const bool hasId = version < 13U || reader.Read(text.id);
    const bool ok = hasId
        && reader.Read(text.x)
        && reader.Read(text.y)
        && reader.Read(text.width)
        && reader.Read(text.height)
        && reader.Read(text.fontSize)
        && (version < 15U || reader.Read(text.rotationDegrees))
        && reader.Read(text.color)
        && reader.Read(alignment)
        && reader.Read(fontFamily)
        && reader.Read(styleFlags)
        && reader.Read(reserved)
        && reader.ReadString(text.content);
    if (!ok) {
        return false;
    }

    switch (alignment) {
    case 1:
        text.alignment = TextAlignment::Center;
        break;
    case 2:
        text.alignment = TextAlignment::Right;
        break;
    default:
        text.alignment = TextAlignment::Left;
        break;
    }

    switch (fontFamily) {
    case 1:
        text.fontFamily = TextFontFamily::Serif;
        break;
    case 2:
        text.fontFamily = TextFontFamily::Monospace;
        break;
    case 3:
        text.fontFamily = TextFontFamily::ShantellSans;
        break;
    default:
        text.fontFamily = TextFontFamily::Sans;
        break;
    }

    text.bold = (styleFlags & kTextStyleFlagBold) != 0;
    text.italic = (styleFlags & kTextStyleFlagItalic) != 0;
    text.underline = (styleFlags & kTextStyleFlagUnderline) != 0;
    if (version >= 13U) {
        std::uint32_t spanCount = 0;
        if (!reader.Read(spanCount) || spanCount > text.content.size() + 1U
            || spanCount > reader.Remaining() / 18U) {
            return false;
        }
        text.styleSpans.reserve(spanCount);
        for (std::uint32_t spanIndex = 0; spanIndex < spanCount; ++spanIndex) {
            PageTextStyleSpan span {};
            std::uint8_t spanFontFamily = 0;
            std::uint8_t spanFlags = 0;
            if (!reader.Read(span.start)
                || !reader.Read(span.length)
                || !reader.Read(span.style.fontSize)
                || !reader.Read(span.style.color)
                || !reader.Read(spanFontFamily)
                || !reader.Read(spanFlags)
                || span.start > text.content.size()
                || span.length > text.content.size() - span.start) {
                return false;
            }
            span.style.fontFamily = static_cast<TextFontFamily>(std::min<std::uint8_t>(spanFontFamily, 3U));
            span.style.bold = (spanFlags & kTextStyleFlagBold) != 0U;
            span.style.italic = (spanFlags & kTextStyleFlagItalic) != 0U;
            span.style.underline = (spanFlags & kTextStyleFlagUnderline) != 0U;
            text.styleSpans.push_back(std::move(span));
        }
    }
    return true;
}

void AppendRaster(std::vector<std::uint8_t>& output, const RasterImage& raster) {
    AppendValue(output, static_cast<std::int32_t>(raster.width));
    AppendValue(output, static_cast<std::int32_t>(raster.height));
    AppendValue(output, static_cast<std::int32_t>(raster.stride));
    AppendValue(output, raster.scale);
    AppendBlob(output, raster.pixels);
}

bool ReadRaster(ByteReader& reader, RasterImage& raster) {
    if (!reader.Read(raster.width) || !reader.Read(raster.height)
        || !reader.Read(raster.stride) || !reader.Read(raster.scale)) return false;
    return reader.ReadBlob(raster.pixels);
}

std::optional<Document> LoadProjectPayload(
    ByteReader& reader,
    std::uint32_t version,
    const std::filesystem::path& filePath) {
    std::uint32_t flags = 0;
    std::uint32_t currentPageIndex = 0;
    std::uint32_t pageCount = 0;
    Document::PageDefaults pageDefaults {};
    std::vector<Document::PdfSource> pdfSources;

    if (!reader.Read(flags) || !reader.Read(currentPageIndex) || !reader.Read(pageCount)) {
        return std::nullopt;
    }

    if (version >= 4U) {
        std::uint32_t sourceCount = 0;
        if (!reader.Read(sourceCount) || sourceCount > reader.Remaining() / 12U) {
            return std::nullopt;
        }
        pdfSources.reserve(sourceCount);
        for (std::uint32_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex) {
            std::filesystem::path sourcePath;
            std::vector<std::uint8_t> pdfBytes;
            if (!reader.ReadPath(sourcePath) || !reader.ReadBlob(pdfBytes)) {
                return std::nullopt;
            }
            pdfSources.push_back({
                std::move(sourcePath),
                std::make_shared<std::vector<std::uint8_t>>(std::move(pdfBytes)),
            });
        }
        if (!reader.Read(pageDefaults.width)
            || !reader.Read(pageDefaults.height)
            || !reader.Read(pageDefaults.backgroundColor)) {
            return std::nullopt;
        }
        if (version >= 11U && !ReadPaletteProfile(reader, pageDefaults.paletteProfile)) {
            return std::nullopt;
        }
    } else {
        std::filesystem::path sourcePath;
        std::vector<std::uint8_t> pdfBytes;
        if (!reader.ReadPath(sourcePath) || !reader.ReadBlob(pdfBytes)) {
            return std::nullopt;
        }
        if (version >= 3U
            && (!reader.Read(pageDefaults.width)
                || !reader.Read(pageDefaults.height)
                || !reader.Read(pageDefaults.backgroundColor))) {
            return std::nullopt;
        }
        if ((flags & kProjectFlagHasPdf) != 0U && !pdfBytes.empty()) {
            pdfSources.push_back({
                std::move(sourcePath),
                std::make_shared<std::vector<std::uint8_t>>(std::move(pdfBytes)),
            });
        }
    }
    // Older projects stored concrete future-page defaults without inheritance
    // metadata. Preserve those choices rather than applying a new app preset.
    pageDefaults.sizeOverridden = version >= 3U
        && (version < 16U || (flags & kProjectFlagInheritPageSize) == 0U);

    // Minimum wire bytes per page, excluding optional rasters and object data.
    const std::size_t minimumPageBytes = 16U + (version >= 2U ? 4U : 0U)
        + (version >= 3U ? 4U : 0U) + (version >= 4U ? 4U : 0U)
        + (version >= 5U ? 4U : 0U) + (version >= 11U ? 4U : 0U)
        + (version >= 14U ? 4U : 0U) + (version >= 15U ? 4U : 0U);
    if (pageCount > reader.Remaining() / minimumPageBytes) return std::nullopt;

    std::vector<Page> savedPages;
    savedPages.reserve(pageCount);
    for (std::uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        Page page {};
        std::uint32_t pageFlags = 0;
        std::uint32_t strokeCount = 0;
        std::uint32_t textCount = 0;
        std::uint32_t latexCount = 0;
        std::uint32_t imageCount = 0;
        if (!reader.Read(page.width) || !reader.Read(page.height)) {
            return std::nullopt;
        }
        if (version >= 15U && !reader.Read(page.rotationDegrees)) {
            return std::nullopt;
        }
        if (version >= 4U) {
            if (!reader.Read(page.sourcePdfSourceIndex) || !reader.Read(page.sourcePdfPageIndex)) {
                return std::nullopt;
            }
        } else if (version >= 2U) {
            if (!reader.Read(page.sourcePdfPageIndex)) {
                return std::nullopt;
            }
            page.sourcePdfSourceIndex = page.sourcePdfPageIndex >= 0 ? 0 : -1;
        } else {
            page.sourcePdfPageIndex = (flags & kProjectFlagHasPdf) != 0U ? static_cast<std::int32_t>(pageIndex) : -1;
            page.sourcePdfSourceIndex = page.sourcePdfPageIndex >= 0 ? 0 : -1;
        }
        if (version >= 3U) {
            if (!reader.Read(pageFlags)) {
                return std::nullopt;
            }
            if ((pageFlags & kPageFlagHasRaster) != 0U && !ReadRaster(reader, page.raster)) {
                return std::nullopt;
            }
        }
        if (version >= 11U && !ReadPaletteProfile(reader, page.paletteProfile)) {
            return std::nullopt;
        }
        if (!reader.Read(strokeCount)) return std::nullopt;
        if (version >= 5U && !reader.Read(textCount)) return std::nullopt;
        if (version >= 14U && !reader.Read(latexCount)) return std::nullopt;
        if (!reader.Read(imageCount)) return std::nullopt;

        const std::size_t minimumStrokeBytes = 12U + (version >= 9U ? 1U : 0U)
            + (version >= 10U ? 4U : 0U);
        if (strokeCount > reader.Remaining() / minimumStrokeBytes) return std::nullopt;
        page.strokes.reserve(strokeCount);
        for (std::uint32_t strokeIndex = 0; strokeIndex < strokeCount; ++strokeIndex) {
            Stroke stroke {};
            if (!ReadStroke(reader, version, stroke)) return std::nullopt;
            page.strokes.push_back(std::move(stroke));
        }

        const std::size_t minimumTextBytes = version >= 15U ? 48U : version >= 13U ? 44U
            : version >= 8U ? 32U : version >= 7U ? 28U : version >= 6U ? 24U : 20U;
        if (textCount > reader.Remaining() / minimumTextBytes) return std::nullopt;
        page.texts.reserve(textCount);
        for (std::uint32_t textIndex = 0; textIndex < textCount; ++textIndex) {
            PageText text {};
            if (version >= 8U) {
                if (!ReadText(reader, version, text)) {
                    return std::nullopt;
                }
            } else if (version >= 7U) {
                if (!reader.Read(text.x)
                    || !reader.Read(text.y)
                    || !reader.Read(text.width)
                    || !reader.Read(text.height)
                    || !reader.Read(text.fontSize)
                    || !reader.Read(text.color)
                    || !reader.ReadString(text.content)) {
                    return std::nullopt;
                }
            } else if (version >= 6U) {
                if (!reader.Read(text.x)
                    || !reader.Read(text.y)
                    || !reader.Read(text.width)
                    || !reader.Read(text.fontSize)
                    || !reader.Read(text.color)
                    || !reader.ReadString(text.content)) {
                    return std::nullopt;
                }
                text.height = 0.0f;
            } else {
                if (!reader.Read(text.x)
                    || !reader.Read(text.y)
                    || !reader.Read(text.fontSize)
                    || !reader.Read(text.color)
                    || !reader.ReadString(text.content)) {
                    return std::nullopt;
                }
                text.width = 0.0f;
                text.height = 0.0f;
            }
            if (version >= 13U) {
                page.texts.push_back(std::move(text));
            }
        }

        if (latexCount > reader.Remaining() / (version >= 15U ? 32U : 28U)) return std::nullopt;
        page.latexes.reserve(latexCount);
        for (std::uint32_t latexIndex = 0; latexIndex < latexCount; ++latexIndex) {
            PageLatex latex {};
            if (!ReadLatex(reader, version, latex)) {
                return std::nullopt;
            }
            page.latexes.push_back(std::move(latex));
        }

        if (imageCount > reader.Remaining() / 40U) return std::nullopt;
        page.images.reserve(imageCount);
        for (std::uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex) {
            PageImage image {};
            if (!ReadImage(reader, image)) {
                return std::nullopt;
            }
            page.images.push_back(std::move(image));
        }

        savedPages.push_back(std::move(page));
    }

    return Document::CreateProjectDocument(filePath, std::move(pdfSources), std::move(savedPages), currentPageIndex, pageDefaults);
}

std::vector<std::uint8_t> SerializeProjectPayload(const Document& document) {
    std::vector<std::uint8_t> bytes;
    const std::uint32_t flags = (document.HasPdfData() ? kProjectFlagHasPdf : 0U)
        | (document.NewPageDefaults().sizeOverridden ? 0U : kProjectFlagInheritPageSize);
    const std::uint32_t currentPageIndex = static_cast<std::uint32_t>(document.CurrentPageIndex());
    const std::uint32_t pageCount = static_cast<std::uint32_t>(document.PageCount());
    const std::uint32_t sourceCount = static_cast<std::uint32_t>(document.PdfSourceCount());

    AppendValue(bytes, flags);
    AppendValue(bytes, currentPageIndex);
    AppendValue(bytes, pageCount);
    AppendValue(bytes, sourceCount);
    for (std::size_t sourceIndex = 0; sourceIndex < document.PdfSourceCount(); ++sourceIndex) {
        const Document::PdfSource& pdfSource = document.PdfSourceAt(sourceIndex);
        AppendPath(bytes, pdfSource.sourcePath);
        AppendBlob(bytes, pdfSource.pdfData ? *pdfSource.pdfData : std::vector<std::uint8_t> {});
    }
    AppendValue(bytes, document.NewPageDefaults().width);
    AppendValue(bytes, document.NewPageDefaults().height);
    AppendValue(bytes, document.NewPageDefaults().backgroundColor);
    AppendPaletteProfile(bytes, document.NewPageDefaults().paletteProfile);

    for (std::size_t pageIndex = 0; pageIndex < document.PageCount(); ++pageIndex) {
        const Page& page = document.PageAt(pageIndex);
        const std::uint32_t pageFlags = (page.sourcePdfSourceIndex < 0 && !page.raster.pixels.empty()) ? kPageFlagHasRaster : 0U;
        const auto strokeCount = static_cast<std::uint32_t>(page.strokes.size());
        const auto textCount = static_cast<std::uint32_t>(page.texts.size());
        const auto latexCount = static_cast<std::uint32_t>(page.latexes.size());
        const auto imageCount = static_cast<std::uint32_t>(page.images.size());
        AppendValue(bytes, page.width);
        AppendValue(bytes, page.height);
        AppendValue(bytes, page.rotationDegrees);
        AppendValue(bytes, page.sourcePdfSourceIndex);
        AppendValue(bytes, page.sourcePdfPageIndex);
        AppendValue(bytes, pageFlags);
        if ((pageFlags & kPageFlagHasRaster) != 0U) {
            AppendRaster(bytes, page.raster);
        }
        AppendPaletteProfile(bytes, page.paletteProfile);
        AppendValue(bytes, strokeCount);
        AppendValue(bytes, textCount);
        AppendValue(bytes, latexCount);
        AppendValue(bytes, imageCount);
        for (const Stroke& stroke : page.strokes) {
            AppendStroke(bytes, stroke);
        }
        for (const PageText& text : page.texts) {
            AppendText(bytes, text);
        }
        for (const PageLatex& latex : page.latexes) {
            AppendLatex(bytes, latex);
        }
        for (const PageImage& image : page.images) {
            AppendImage(bytes, image);
        }
    }
    return bytes;
}

}  // namespace

std::optional<Document> ProjectFile::Load(const std::filesystem::path& filePath) {
    const auto bytes = ReadAllBytes(filePath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }

    return LoadFromBytes(*bytes, filePath);
}
std::optional<Document> ProjectFile::LoadFromBytes(
    const std::vector<std::uint8_t>& bytes,
    const std::filesystem::path& filePath) {
    ByteReader reader { bytes };
    char magic[sizeof(kCnoteMagic)] {};
    std::uint32_t version = 0;

    const bool validMagic = reader.ReadExact(magic, sizeof(magic))
        && (std::memcmp(magic, kCnoteMagic, sizeof(kCnoteMagic)) == 0
            || std::memcmp(magic, kLegacyProjectMagic, sizeof(kLegacyProjectMagic)) == 0);
    if (!validMagic || !reader.Read(version) || (version < 1U || version > kProjectVersion)) {
        return std::nullopt;
    }

    if (version >= 12U) {
        std::uint32_t payloadEncoding = kPayloadEncodingRaw;
        std::uint64_t uncompressedPayloadSize = 0;
        if (!reader.Read(payloadEncoding) || !reader.Read(uncompressedPayloadSize)) {
            return std::nullopt;
        }
        const auto storedPayload = reader.bytes.subspan(reader.offset);
        if (payloadEncoding == kPayloadEncodingRaw) {
            if (storedPayload.size() != uncompressedPayloadSize) return std::nullopt;
            ByteReader payloadReader { storedPayload };
            return LoadProjectPayload(payloadReader, version, filePath);
        } else if (payloadEncoding == kPayloadEncodingXpressHuff) {
            const auto decompressedPayload = DecompressXpressBuffer(storedPayload, uncompressedPayloadSize);
            if (!decompressedPayload.has_value()) {
                return std::nullopt;
            }
            ByteReader payloadReader { *decompressedPayload };
            return LoadProjectPayload(payloadReader, version, filePath);
        } else {
            return std::nullopt;
        }
    }

    return LoadProjectPayload(reader, version, filePath);
}

std::vector<std::uint8_t> ProjectFile::Serialize(const Document& document) {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> payload = SerializeProjectPayload(document);
    std::vector<std::uint8_t> storedPayload = payload;
    std::uint32_t payloadEncoding = kPayloadEncodingRaw;

    if (const auto compressedPayload = CompressXpressBuffer(payload);
        compressedPayload.has_value() && compressedPayload->size() < payload.size()) {
        storedPayload = std::move(*compressedPayload);
        payloadEncoding = kPayloadEncodingXpressHuff;
    }

    AppendBytes(bytes, kCnoteMagic, sizeof(kCnoteMagic));
    AppendValue(bytes, kProjectVersion);
    AppendValue(bytes, payloadEncoding);
    AppendValue(bytes, static_cast<std::uint64_t>(payload.size()));
    if (!storedPayload.empty()) {
        AppendBytes(bytes, storedPayload.data(), storedPayload.size());
    }

    return bytes;
}

bool ProjectFile::Save(const Document& document, const std::filesystem::path& filePath) {
    const std::vector<std::uint8_t> bytes = Serialize(document);
    return WriteAllBytes(filePath, bytes);
}

}  // namespace cnote
