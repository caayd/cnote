#include "document/SelectionClipboardCodec.h"

#include <bit>
#include <cstring>

namespace cnote {

namespace {

constexpr std::uint32_t kStrokeFlagDotted = 1U << 0U;

void AppendBytes(std::vector<std::uint8_t>& output, const void* data, std::size_t size) {
    const auto* begin = static_cast<const std::uint8_t*>(data);
    output.insert(output.end(), begin, begin + size);
}

void AppendUint32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void AppendFloat(std::vector<std::uint8_t>& output, float value) {
    AppendUint32(output, std::bit_cast<std::uint32_t>(value));
}

void AppendUtf16(std::vector<std::uint8_t>& output, std::u16string_view value) {
    for (char16_t unit : value) {
        output.push_back(static_cast<std::uint8_t>(unit & 0xFFU));
        output.push_back(static_cast<std::uint8_t>((unit >> 8U) & 0xFFU));
    }
}

struct ByteReader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;

    [[nodiscard]] std::size_t Remaining() const noexcept {
        return bytes.size() - offset;
    }

    [[nodiscard]] bool ReadUint32(std::uint32_t& value) {
        if (Remaining() < 4U) {
            return false;
        }
        value = static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
            | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
            | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
        offset += 4U;
        return true;
    }

    [[nodiscard]] bool ReadFloat(float& value) {
        std::uint32_t bits = 0;
        if (!ReadUint32(bits)) {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

    [[nodiscard]] bool ReadUtf16(std::uint32_t charCount, std::u16string& value) {
        const std::size_t byteCount = static_cast<std::size_t>(charCount) * 2U;
        if (byteCount > Remaining()) {
            return false;
        }
        value.resize(charCount);
        for (std::size_t index = 0; index < charCount; ++index) {
            value[index] = static_cast<char16_t>(
                static_cast<std::uint16_t>(bytes[offset + (index * 2U)])
                | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + (index * 2U) + 1U]) << 8U));
        }
        offset += byteCount;
        return true;
    }

    [[nodiscard]] bool ReadBytes(std::vector<std::uint8_t>& value, std::uint32_t size) {
        if (size > Remaining()) {
            return false;
        }
        value.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
        return true;
    }
};

}  // namespace

std::vector<std::uint8_t> EncodeSelectionClipboard(const SelectionClipboardData& data) {
    std::vector<std::uint8_t> bytes;
    const char magic[] = { 'C', 'T', 'S', '9' };
    AppendBytes(bytes, magic, sizeof(magic));
    AppendUint32(bytes, static_cast<std::uint32_t>(data.strokes.size()));
    AppendUint32(bytes, static_cast<std::uint32_t>(data.texts.size()));
    AppendUint32(bytes, static_cast<std::uint32_t>(data.images.size()));
    AppendFloat(bytes, data.minX);
    AppendFloat(bytes, data.minY);
    AppendFloat(bytes, data.maxX);
    AppendFloat(bytes, data.maxY);

    for (const Stroke& stroke : data.strokes) {
        std::uint32_t strokeFlags = 0;
        if (stroke.style.dotted) {
            strokeFlags |= kStrokeFlagDotted;
        }
        AppendUint32(bytes, stroke.style.color);
        AppendFloat(bytes, stroke.style.width);
        AppendUint32(bytes, strokeFlags);
        AppendFloat(bytes, stroke.smoothing);
        AppendUint32(bytes, static_cast<std::uint32_t>(stroke.points.size()));
        for (const Vec2& point : stroke.points) {
            AppendFloat(bytes, point.x);
            AppendFloat(bytes, point.y);
        }
    }

    for (const PageText& text : data.texts) {
        std::uint32_t flags = 0;
        flags |= static_cast<std::uint32_t>(text.alignment);
        flags |= static_cast<std::uint32_t>(text.fontFamily) << 8U;
        if (text.bold) {
            flags |= 1U << 16U;
        }
        if (text.italic) {
            flags |= 1U << 17U;
        }
        if (text.underline) {
            flags |= 1U << 18U;
        }
        AppendFloat(bytes, text.x);
        AppendFloat(bytes, text.y);
        AppendFloat(bytes, text.width);
        AppendFloat(bytes, text.height);
        AppendFloat(bytes, text.rotationDegrees);
        AppendUint32(bytes, text.color);
        AppendUint32(bytes, flags);
        AppendUint32(bytes, static_cast<std::uint32_t>(text.content.size()));
        AppendUtf16(bytes, text.content);
        AppendUint32(bytes, static_cast<std::uint32_t>(text.styleSpans.size()));
        for (const PageTextStyleSpan& span : text.styleSpans) {
            std::uint32_t spanFlags = static_cast<std::uint32_t>(span.style.fontFamily);
            spanFlags |= span.style.bold ? 1U << 8U : 0U;
            spanFlags |= span.style.italic ? 1U << 9U : 0U;
            spanFlags |= span.style.underline ? 1U << 10U : 0U;
            AppendUint32(bytes, span.start);
            AppendUint32(bytes, span.length);
            AppendFloat(bytes, span.style.fontSize);
            AppendUint32(bytes, span.style.color);
            AppendUint32(bytes, spanFlags);
        }
    }

    for (const PageImage& image : data.images) {
        AppendFloat(bytes, image.x);
        AppendFloat(bytes, image.y);
        AppendFloat(bytes, image.width);
        AppendFloat(bytes, image.height);
        AppendFloat(bytes, image.rotationDegrees);
        AppendUint32(bytes, static_cast<std::uint32_t>(image.pixelWidth));
        AppendUint32(bytes, static_cast<std::uint32_t>(image.pixelHeight));
        AppendUint32(bytes, static_cast<std::uint32_t>(image.stride));
        AppendUint32(bytes, static_cast<std::uint32_t>(image.pixels.size()));
        if (!image.pixels.empty()) {
            AppendBytes(bytes, image.pixels.data(), image.pixels.size());
        }
    }

    AppendUint32(bytes, static_cast<std::uint32_t>(data.latexes.size()));
    for (const PageLatex& latex : data.latexes) {
        AppendFloat(bytes, latex.x);
        AppendFloat(bytes, latex.y);
        AppendFloat(bytes, latex.fontSize);
        AppendFloat(bytes, latex.rotationDegrees);
        AppendUint32(bytes, latex.color);
        AppendUint32(bytes, static_cast<std::uint32_t>(latex.content.size()));
        AppendUtf16(bytes, latex.content);
    }

    return bytes;
}

std::optional<SelectionClipboardData> DecodeSelectionClipboard(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4U) {
        return std::nullopt;
    }

    char magic[4] {};
    std::memcpy(magic, bytes.data(), sizeof(magic));
    const bool isCts1 = std::memcmp(magic, "CTS1", 4) == 0;
    const bool isCts2 = std::memcmp(magic, "CTS2", 4) == 0;
    const bool isCts3 = std::memcmp(magic, "CTS3", 4) == 0;
    const bool isCts4 = std::memcmp(magic, "CTS4", 4) == 0;
    const bool isCts5 = std::memcmp(magic, "CTS5", 4) == 0;
    const bool isCts6 = std::memcmp(magic, "CTS6", 4) == 0;
    const bool isCts7 = std::memcmp(magic, "CTS7", 4) == 0;
    const bool isCts8 = std::memcmp(magic, "CTS8", 4) == 0;
    const bool isCts9 = std::memcmp(magic, "CTS9", 4) == 0;
    if (!isCts1 && !isCts2 && !isCts3 && !isCts4 && !isCts5 && !isCts6 && !isCts7 && !isCts8 && !isCts9) {
        return std::nullopt;
    }

    const bool hasTextCount = !isCts1;
    const bool hasTextWidth = isCts3 || isCts4 || isCts5 || isCts6 || isCts7 || isCts8 || isCts9;
    const bool hasTextHeight = isCts4 || isCts5 || isCts6 || isCts7 || isCts8 || isCts9;
    const bool hasTextFlags = isCts5 || isCts6 || isCts7 || isCts8 || isCts9;
    const bool hasStrokeFlags = isCts6 || isCts7 || isCts8 || isCts9;
    const bool hasStrokeSmoothing = isCts7 || isCts8 || isCts9;
    const bool hasObjectRotation = isCts9;

    ByteReader reader { bytes, 4U };
    SelectionClipboardData data;
    std::uint32_t strokeCount = 0;
    std::uint32_t textCount = 0;
    std::uint32_t imageCount = 0;
    bool headerOk = reader.ReadUint32(strokeCount);
    if (headerOk && hasTextCount) {
        headerOk = reader.ReadUint32(textCount);
    }
    headerOk = headerOk && reader.ReadUint32(imageCount)
        && reader.ReadFloat(data.minX) && reader.ReadFloat(data.minY)
        && reader.ReadFloat(data.maxX) && reader.ReadFloat(data.maxY);
    if (!headerOk
        || strokeCount > reader.bytes.size()
        || textCount > reader.bytes.size()
        || imageCount > reader.bytes.size()) {
        return std::nullopt;
    }

    data.strokes.reserve(strokeCount);
    data.texts.reserve(textCount);
    data.images.reserve(imageCount);

    bool ok = true;
    for (std::uint32_t strokeIndex = 0; ok && strokeIndex < strokeCount; ++strokeIndex) {
        Stroke stroke {};
        std::uint32_t pointCount = 0;
        std::uint32_t strokeFlags = 0;
        ok = reader.ReadUint32(stroke.style.color) && reader.ReadFloat(stroke.style.width);
        if (ok && hasStrokeFlags) {
            ok = reader.ReadUint32(strokeFlags);
        }
        stroke.style.dotted = (strokeFlags & kStrokeFlagDotted) != 0U;
        if (ok && hasStrokeSmoothing) {
            ok = reader.ReadFloat(stroke.smoothing);
        }
        ok = ok && reader.ReadUint32(pointCount);
        if (ok && pointCount > reader.Remaining() / 8U) {
            ok = false;
        }
        if (ok) {
            stroke.points.reserve(pointCount);
        }
        for (std::uint32_t pointIndex = 0; ok && pointIndex < pointCount; ++pointIndex) {
            Vec2 point {};
            ok = reader.ReadFloat(point.x) && reader.ReadFloat(point.y);
            if (ok) {
                stroke.points.push_back(point);
            }
        }
        if (ok) {
            data.strokes.push_back(std::move(stroke));
        }
    }

    for (std::uint32_t textIndex = 0; ok && textIndex < textCount; ++textIndex) {
        PageText text {};
        std::uint32_t charCount = 0;
        std::uint32_t flags = 0;
        ok = reader.ReadFloat(text.x) && reader.ReadFloat(text.y);
        if (ok && hasTextWidth) {
            ok = reader.ReadFloat(text.width);
        } else {
            text.width = 0.0f;
        }
        if (ok && hasTextHeight) {
            ok = reader.ReadFloat(text.height);
        } else {
            text.height = 0.0f;
        }
        if (ok && hasObjectRotation) {
            ok = reader.ReadFloat(text.rotationDegrees);
        } else {
            text.rotationDegrees = 0.0f;
        }
        ok = ok && reader.ReadFloat(text.fontSize) && reader.ReadUint32(text.color);
        if (ok && hasTextFlags) {
            ok = reader.ReadUint32(flags);
        }
        ok = ok && reader.ReadUint32(charCount);
        if (ok) {
            ok = reader.ReadUtf16(charCount, text.content);
        }
        text.alignment = static_cast<TextAlignment>(std::min<std::uint32_t>(flags & 0xFFU, 2U));
        text.fontFamily = static_cast<TextFontFamily>(std::min<std::uint32_t>((flags >> 8U) & 0xFFU, 3U));
        text.bold = (flags & (1U << 16U)) != 0;
        text.italic = (flags & (1U << 17U)) != 0;
        text.underline = (flags & (1U << 18U)) != 0;
        if (ok && isCts8) {
            std::uint32_t spanCount = 0;
            ok = reader.ReadUint32(spanCount) && spanCount <= text.content.size() + 1U;
            for (std::uint32_t spanIndex = 0; ok && spanIndex < spanCount; ++spanIndex) {
                PageTextStyleSpan span {};
                std::uint32_t spanFlags = 0;
                ok = reader.ReadUint32(span.start)
                    && reader.ReadUint32(span.length)
                    && reader.ReadFloat(span.style.fontSize)
                    && reader.ReadUint32(span.style.color)
                    && reader.ReadUint32(spanFlags)
                    && span.start <= text.content.size()
                    && span.length <= text.content.size() - span.start;
                span.style.fontFamily = static_cast<TextFontFamily>(std::min<std::uint32_t>(spanFlags & 0xFFU, 3U));
                span.style.bold = (spanFlags & (1U << 8U)) != 0U;
                span.style.italic = (spanFlags & (1U << 9U)) != 0U;
                span.style.underline = (spanFlags & (1U << 10U)) != 0U;
                if (ok) {
                    text.styleSpans.push_back(std::move(span));
                }
            }
        }
        if (ok) {
            data.texts.push_back(std::move(text));
        }
    }

    for (std::uint32_t imageIndex = 0; ok && imageIndex < imageCount; ++imageIndex) {
        PageImage image {};
        std::uint32_t pixelWidth = 0;
        std::uint32_t pixelHeight = 0;
        std::uint32_t stride = 0;
        std::uint32_t dataSize = 0;
        ok = reader.ReadFloat(image.x) && reader.ReadFloat(image.y) && reader.ReadFloat(image.width) && reader.ReadFloat(image.height)
            && reader.ReadFloat(image.rotationDegrees)
            && reader.ReadUint32(pixelWidth) && reader.ReadUint32(pixelHeight) && reader.ReadUint32(stride) && reader.ReadUint32(dataSize);
        image.pixelWidth = static_cast<int>(pixelWidth);
        image.pixelHeight = static_cast<int>(pixelHeight);
        image.stride = static_cast<int>(stride);
        if (ok) {
            ok = reader.ReadBytes(image.pixels, dataSize);
        }
        if (ok) {
            data.images.push_back(std::move(image));
        }
    }

    if (ok && isCts8 && reader.Remaining() >= sizeof(std::uint32_t)) {
        std::uint32_t latexCount = 0;
        ok = reader.ReadUint32(latexCount);
        if (ok && latexCount > reader.Remaining() / 16U) {
            ok = false;
        }
        if (ok) {
            data.latexes.reserve(latexCount);
        }
        for (std::uint32_t latexIndex = 0; ok && latexIndex < latexCount; ++latexIndex) {
            PageLatex latex {};
            std::uint32_t charCount = 0;
            ok = reader.ReadFloat(latex.x)
                && reader.ReadFloat(latex.y)
                && reader.ReadFloat(latex.fontSize)
                && (!hasObjectRotation || reader.ReadFloat(latex.rotationDegrees))
                && reader.ReadUint32(latex.color)
                && reader.ReadUint32(charCount);
            if (ok) {
                ok = reader.ReadUtf16(charCount, latex.content);
            }
            if (ok) {
                data.latexes.push_back(std::move(latex));
            }
        }
    }

    if (!ok) {
        return std::nullopt;
    }
    return data;
}

}  // namespace cnote
