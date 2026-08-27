#include "ProjectFile.h"

#include <algorithm>
#include <cmath>
#include <compressapi.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace cnote {

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

bool HasExtension(const std::wstring& path, const wchar_t* extension) {
    const std::filesystem::path filePath(path);
    return filePath.has_extension() && _wcsicmp(filePath.extension().c_str(), extension) == 0;
}

namespace {

constexpr char kCnoteMagic[] = { 'C', 'N', 'O', 'T', 'E', '1', '\0', '\0' };
constexpr char kLegacyProjectMagic[] = { 'C', 'T', 'R', 'P', 'R', 'J', '1', '\0' };
constexpr std::uint32_t kProjectVersion = 13;
constexpr std::uint32_t kProjectFlagHasPdf = 1U << 0U;
constexpr std::uint32_t kPageFlagHasRaster = 1U << 0U;
constexpr std::uint8_t kStrokeStyleFlagDotted = 1U << 0U;
constexpr std::uint8_t kTextStyleFlagBold = 1U << 0U;
constexpr std::uint8_t kTextStyleFlagItalic = 1U << 1U;
constexpr std::uint8_t kTextStyleFlagUnderline = 1U << 2U;
constexpr std::uint32_t kPayloadEncodingRaw = 0U;
constexpr std::uint32_t kPayloadEncodingXpressHuff = 1U;

std::optional<std::vector<std::uint8_t>> ReadAllBytes(const std::wstring& filePath) {
    std::ifstream input(std::filesystem::path(filePath), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            return std::nullopt;
        }
    }

    return bytes;
}

bool WriteAllBytes(const std::wstring& filePath, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(std::filesystem::path(filePath), std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    return static_cast<bool>(output);
}

struct ByteReader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;

    [[nodiscard]] bool ReadExact(void* destination, std::size_t size) {
        if (offset + size > bytes.size()) {
            return false;
        }
        std::memcpy(destination, bytes.data() + offset, size);
        offset += size;
        return true;
    }

    template <typename T>
    [[nodiscard]] bool Read(T& value) {
        return ReadExact(&value, sizeof(value));
    }

    [[nodiscard]] bool ReadString(std::wstring& value) {
        std::uint32_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (offset + size > bytes.size()) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
        std::string utf8(begin, begin + size);
        value = Utf8ToWide(utf8);
        offset += size;
        return true;
    }

    [[nodiscard]] bool ReadBlob(std::vector<std::uint8_t>& value) {
        std::uint64_t size = 0;
        if (!Read(size)) {
            return false;
        }
        if (size > bytes.size() || offset + static_cast<std::size_t>(size) > bytes.size()) {
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

struct CompressorCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            CloseCompressor(static_cast<COMPRESSOR_HANDLE>(handle));
        }
    }
};

struct DecompressorCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            CloseDecompressor(static_cast<DECOMPRESSOR_HANDLE>(handle));
        }
    }
};

void AppendBytes(std::vector<std::uint8_t>& output, const void* data, std::size_t size) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), begin, begin + size);
}

template <typename T>
void AppendValue(std::vector<std::uint8_t>& output, const T& value) {
    AppendBytes(output, &value, sizeof(value));
}

void AppendString(std::vector<std::uint8_t>& output, const std::wstring& value) {
    const std::string utf8 = WideToUtf8(value);
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
    if (!reader.Read(colorCount)) {
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
    if (!reader.Read(pointCount)) {
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
    AppendValue(output, image.pixelWidth);
    AppendValue(output, image.pixelHeight);
    AppendValue(output, image.stride);
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
    default:
        text.fontFamily = TextFontFamily::Sans;
        break;
    }

    text.bold = (styleFlags & kTextStyleFlagBold) != 0;
    text.italic = (styleFlags & kTextStyleFlagItalic) != 0;
    text.underline = (styleFlags & kTextStyleFlagUnderline) != 0;
    if (version >= 13U) {
        std::uint32_t spanCount = 0;
        if (!reader.Read(spanCount) || spanCount > text.content.size() + 1U) {
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
            span.style.fontFamily = static_cast<TextFontFamily>(std::min<std::uint8_t>(spanFontFamily, 2U));
            span.style.bold = (spanFlags & kTextStyleFlagBold) != 0U;
            span.style.italic = (spanFlags & kTextStyleFlagItalic) != 0U;
            span.style.underline = (spanFlags & kTextStyleFlagUnderline) != 0U;
            text.styleSpans.push_back(std::move(span));
        }
    }
    return true;
}

void AppendRaster(std::vector<std::uint8_t>& output, const RasterImage& raster) {
    AppendValue(output, raster.width);
    AppendValue(output, raster.height);
    AppendValue(output, raster.stride);
    AppendValue(output, raster.scale);
    AppendBlob(output, raster.pixels);
}

bool ReadRaster(ByteReader& reader, RasterImage& raster) {
    if (!reader.Read(raster.width) || !reader.Read(raster.height) || !reader.Read(raster.stride) || !reader.Read(raster.scale)) {
        return false;
    }
    return reader.ReadBlob(raster.pixels);
}

std::optional<std::vector<std::uint8_t>> CompressPayload(const std::vector<std::uint8_t>& payload) {
    if (payload.empty()) {
        return std::vector<std::uint8_t> {};
    }

    COMPRESSOR_HANDLE compressorHandle = nullptr;
    if (CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressorHandle) == FALSE) {
        return std::nullopt;
    }

    std::unique_ptr<void, CompressorCloser> compressor(compressorHandle);
    SIZE_T compressedSize = 0;
    if (Compress(
            compressorHandle,
            const_cast<std::uint8_t*>(payload.data()),
            payload.size(),
            nullptr,
            0,
            &compressedSize) == FALSE
        && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> compressed(compressedSize);
    if (Compress(
            compressorHandle,
            const_cast<std::uint8_t*>(payload.data()),
            payload.size(),
            compressed.data(),
            compressed.size(),
            &compressedSize) == FALSE) {
        return std::nullopt;
    }

    compressed.resize(compressedSize);
    return compressed;
}

std::optional<std::vector<std::uint8_t>> DecompressPayload(
    const std::vector<std::uint8_t>& payload,
    std::uint64_t expectedSize) {
    if (expectedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }

    if (expectedSize == 0U) {
        return std::vector<std::uint8_t> {};
    }

    DECOMPRESSOR_HANDLE decompressorHandle = nullptr;
    if (CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressorHandle) == FALSE) {
        return std::nullopt;
    }

    std::unique_ptr<void, DecompressorCloser> decompressor(decompressorHandle);
    std::vector<std::uint8_t> decompressed(static_cast<std::size_t>(expectedSize));
    SIZE_T decompressedSize = 0;
    if (Decompress(
            decompressorHandle,
            const_cast<std::uint8_t*>(payload.data()),
            payload.size(),
            decompressed.data(),
            decompressed.size(),
            &decompressedSize) == FALSE
        || decompressedSize != decompressed.size()) {
        return std::nullopt;
    }

    return decompressed;
}

std::optional<Document> LoadProjectPayload(
    ByteReader& reader,
    std::uint32_t version,
    const std::wstring& filePath,
    PdfiumBackend& pdfBackend) {
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
        if (!reader.Read(sourceCount)) {
            return std::nullopt;
        }
        pdfSources.reserve(sourceCount);
        for (std::uint32_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex) {
            std::wstring sourcePath;
            std::vector<std::uint8_t> pdfBytes;
            if (!reader.ReadString(sourcePath) || !reader.ReadBlob(pdfBytes)) {
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
        std::wstring sourcePath;
        std::vector<std::uint8_t> pdfBytes;
        if (!reader.ReadString(sourcePath) || !reader.ReadBlob(pdfBytes)) {
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

    std::vector<Page> savedPages;
    savedPages.reserve(pageCount);
    for (std::uint32_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) {
        Page page {};
        std::uint32_t pageFlags = 0;
        std::uint32_t strokeCount = 0;
        std::uint32_t textCount = 0;
        std::uint32_t imageCount = 0;
        if (!reader.Read(page.width) || !reader.Read(page.height)) {
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
        if (!reader.Read(strokeCount)) {
            return std::nullopt;
        }
        if (version >= 5U) {
            if (!reader.Read(textCount)) {
                return std::nullopt;
            }
        }
        if (!reader.Read(imageCount)) {
            return std::nullopt;
        }

        page.strokes.reserve(strokeCount);
        for (std::uint32_t strokeIndex = 0; strokeIndex < strokeCount; ++strokeIndex) {
            Stroke stroke {};
            if (!ReadStroke(reader, version, stroke)) {
                return std::nullopt;
            }
            page.strokes.push_back(std::move(stroke));
        }

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

    std::vector<std::optional<Document>> openedSources;
    openedSources.reserve(pdfSources.size());
    for (const Document::PdfSource& pdfSource : pdfSources) {
        if (!pdfSource.pdfData || pdfSource.pdfData->empty()) {
            openedSources.push_back(std::nullopt);
            continue;
        }
        const auto opened = pdfBackend.OpenFromMemory(pdfSource.sourcePath, pdfSource.pdfData);
        if (!opened.has_value()) {
            openedSources.push_back(std::nullopt);
        } else {
            openedSources.push_back(std::move(*opened));
        }
    }

    for (Page& savedPage : savedPages) {
        const bool hasValidPdfSource = savedPage.sourcePdfSourceIndex >= 0
            && static_cast<std::size_t>(savedPage.sourcePdfSourceIndex) < openedSources.size()
            && openedSources[static_cast<std::size_t>(savedPage.sourcePdfSourceIndex)].has_value()
            && savedPage.sourcePdfPageIndex >= 0
            && static_cast<std::size_t>(savedPage.sourcePdfPageIndex)
                < openedSources[static_cast<std::size_t>(savedPage.sourcePdfSourceIndex)]->PageCount();
        if (hasValidPdfSource) {
            const Document& sourceDocument = *openedSources[static_cast<std::size_t>(savedPage.sourcePdfSourceIndex)];
            const Page& sourcePage = sourceDocument.PageAt(static_cast<std::size_t>(savedPage.sourcePdfPageIndex));
            savedPage.width = sourcePage.width;
            savedPage.height = sourcePage.height;
            savedPage.raster = sourcePage.raster;
        } else {
            savedPage.sourcePdfSourceIndex = -1;
            savedPage.sourcePdfPageIndex = -1;
            if (savedPage.raster.pixels.empty()) {
                savedPage.raster = CreateSolidRaster(savedPage.width, savedPage.height, 1.5f, 0xFFFFFFFFU);
            }
        }
    }

    return Document::CreateProjectDocument(filePath, std::move(pdfSources), std::move(savedPages), currentPageIndex, pageDefaults);
}

std::vector<std::uint8_t> SerializeProjectPayload(const Document& document) {
    std::vector<std::uint8_t> bytes;
    const std::uint32_t flags = document.HasPdfData() ? kProjectFlagHasPdf : 0U;
    const std::uint32_t currentPageIndex = static_cast<std::uint32_t>(document.CurrentPageIndex());
    const std::uint32_t pageCount = static_cast<std::uint32_t>(document.PageCount());
    const std::uint32_t sourceCount = static_cast<std::uint32_t>(document.PdfSourceCount());

    AppendValue(bytes, flags);
    AppendValue(bytes, currentPageIndex);
    AppendValue(bytes, pageCount);
    AppendValue(bytes, sourceCount);
    for (std::size_t sourceIndex = 0; sourceIndex < document.PdfSourceCount(); ++sourceIndex) {
        const Document::PdfSource& pdfSource = document.PdfSourceAt(sourceIndex);
        AppendString(bytes, pdfSource.sourcePath);
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
        const auto imageCount = static_cast<std::uint32_t>(page.images.size());
        AppendValue(bytes, page.width);
        AppendValue(bytes, page.height);
        AppendValue(bytes, page.sourcePdfSourceIndex);
        AppendValue(bytes, page.sourcePdfPageIndex);
        AppendValue(bytes, pageFlags);
        if ((pageFlags & kPageFlagHasRaster) != 0U) {
            AppendRaster(bytes, page.raster);
        }
        AppendPaletteProfile(bytes, page.paletteProfile);
        AppendValue(bytes, strokeCount);
        AppendValue(bytes, textCount);
        AppendValue(bytes, imageCount);
        for (const Stroke& stroke : page.strokes) {
            AppendStroke(bytes, stroke);
        }
        for (const PageText& text : page.texts) {
            AppendText(bytes, text);
        }
        for (const PageImage& image : page.images) {
            AppendImage(bytes, image);
        }
    }

    return bytes;
}

}  // namespace

std::optional<Document> ProjectFile::Load(const std::wstring& filePath, PdfiumBackend& pdfBackend) {
    const auto bytes = ReadAllBytes(filePath);
    if (!bytes.has_value()) {
        return std::nullopt;
    }

    return LoadFromBytes(*bytes, filePath, pdfBackend);
}

std::optional<Document> ProjectFile::LoadFromBytes(
    const std::vector<std::uint8_t>& bytes,
    const std::wstring& filePath,
    PdfiumBackend& pdfBackend) {
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
        const std::vector<std::uint8_t> storedPayload(bytes.begin() + static_cast<std::ptrdiff_t>(reader.offset), bytes.end());
        std::vector<std::uint8_t> payload;
        if (payloadEncoding == kPayloadEncodingRaw) {
            payload = storedPayload;
            if (payload.size() != uncompressedPayloadSize) {
                return std::nullopt;
            }
        } else if (payloadEncoding == kPayloadEncodingXpressHuff) {
            const auto decompressedPayload = DecompressPayload(storedPayload, uncompressedPayloadSize);
            if (!decompressedPayload.has_value()) {
                return std::nullopt;
            }
            payload = std::move(*decompressedPayload);
        } else {
            return std::nullopt;
        }

        ByteReader payloadReader { payload };
        return LoadProjectPayload(payloadReader, version, filePath, pdfBackend);
    }

    return LoadProjectPayload(reader, version, filePath, pdfBackend);
}

std::vector<std::uint8_t> ProjectFile::Serialize(const Document& document) {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> payload = SerializeProjectPayload(document);
    std::vector<std::uint8_t> storedPayload = payload;
    std::uint32_t payloadEncoding = kPayloadEncodingRaw;

    if (const auto compressedPayload = CompressPayload(payload);
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

bool ProjectFile::Save(const Document& document, const std::wstring& filePath) {
    const std::vector<std::uint8_t> bytes = Serialize(document);
    return WriteAllBytes(filePath, bytes);
}

}  // namespace cnote
