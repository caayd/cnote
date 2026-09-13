#include "document/SelectionClipboardQt.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

namespace cnote::clipboard {

namespace {

bool HasObjects(const SelectionClipboardData& data) {
    return !data.strokes.empty() || !data.texts.empty() || !data.images.empty() || !data.latexes.empty();
}

// Outcome of scanning the structured flavors: absent (no such format),
// valid (decoded) or invalid (present but unusable, which stops the fallback).
enum class StructuredState {
    Absent,
    Valid,
    Invalid,
};

struct StructuredResult {
    StructuredState state = StructuredState::Absent;
    SelectionClipboardData data {};
};

StructuredResult ReadStructured(const QMimeData& mime) {
    const char* const formats[] = { kSelectionMimeType, kWindowsSelectionMimeType };
    for (const char* const format : formats) {
        const QLatin1String name(format);
        if (!mime.hasFormat(name)) {
            continue;
        }
        const QByteArray bytes = mime.data(name);
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
        const std::optional<SelectionClipboardData> decoded = DecodeSelectionClipboard(
            std::span<const std::uint8_t>(begin, static_cast<std::size_t>(bytes.size())));
        if (!decoded.has_value() || !HasObjects(*decoded)) {
            return { StructuredState::Invalid, {} };
        }
        return { StructuredState::Valid, *decoded };
    }
    return {};
}

bool IsSingleImage(const SelectionClipboardData& data) {
    return data.images.size() == 1U && data.strokes.empty() && data.texts.empty() && data.latexes.empty();
}

}  // namespace

bool WriteSelection(const SelectionClipboardData& data) {
    if (!HasObjects(data)) {
        return false;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }

    const std::vector<std::uint8_t> encoded = EncodeSelectionClipboard(data);
    const QByteArray bytes(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<qsizetype>(encoded.size()));

    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kSelectionMimeType), bytes);
#ifdef Q_OS_WIN
    mime->setData(QLatin1String(kWindowsSelectionMimeType), bytes);
#endif
    if (IsSingleImage(data)) {
        const QImage image = QImageFromPageImage(data.images.front());
        if (!image.isNull()) {
            mime->setImageData(image);
        }
    }

    clipboard->setMimeData(mime, QClipboard::Clipboard);
    return true;
}

bool WriteImage(const QImage& image) {
    if (image.isNull()) {
        return false;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    clipboard->setImage(image, QClipboard::Clipboard);
    return true;
}

bool WritePlainText(const QString& text) {
    if (text.isEmpty()) {
        return false;
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    clipboard->setText(text, QClipboard::Clipboard);
    return true;
}

ClipboardContent Read() {
    ClipboardContent content;
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return content;
    }
    const QMimeData* mime = clipboard->mimeData(QClipboard::Clipboard);
    if (mime == nullptr) {
        return content;
    }

    const StructuredResult structured = ReadStructured(*mime);
    if (structured.state == StructuredState::Invalid) {
        return content;
    }
    if (structured.state == StructuredState::Valid) {
        content.kind = ClipboardKind::Selection;
        content.selection = structured.data;
        return content;
    }

    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            content.kind = ClipboardKind::Image;
            content.image = std::move(image);
            return content;
        }
    }

    if (mime->hasText()) {
        QString text = mime->text();
        if (!text.isEmpty()) {
            content.kind = ClipboardKind::Text;
            content.text = std::move(text);
            return content;
        }
    }

    return content;
}

QImage QImageFromPageImage(const PageImage& source) {
    if (source.pixelWidth <= 0 || source.pixelHeight <= 0 || source.pixels.empty()) {
        return {};
    }
    const int rowBytes = source.pixelWidth * 4;
    const int stride = source.stride > 0 ? source.stride : rowBytes;
    if (stride < rowBytes) {
        return {};
    }
    const std::size_t required =
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(source.pixelHeight - 1)
        + static_cast<std::size_t>(rowBytes);
    if (source.pixels.size() < required) {
        return {};
    }

    QImage image(source.pixelWidth, source.pixelHeight, QImage::Format_ARGB32);
    if (image.isNull()) {
        return {};
    }
    for (int y = 0; y < source.pixelHeight; ++y) {
        const std::uint8_t* sourceRow =
            source.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        auto* destinationRow = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < source.pixelWidth; ++x) {
            const std::uint8_t* pixel = sourceRow + static_cast<std::size_t>(x) * 4U;
            // Stored order is BGRA; QImage::Format_ARGB32 keeps straight alpha.
            destinationRow[x] = qRgba(pixel[2], pixel[1], pixel[0], pixel[3]);
        }
    }
    return image;
}

PageImage PageImageFromQImage(const QImage& source) {
    PageImage image;
    if (source.isNull()) {
        return image;
    }
    const QImage converted = source.convertToFormat(QImage::Format_ARGB32);
    if (converted.isNull()) {
        return image;
    }

    image.pixelWidth = converted.width();
    image.pixelHeight = converted.height();
    image.stride = converted.width() * 4;
    image.pixels.resize(
        static_cast<std::size_t>(image.stride) * static_cast<std::size_t>(image.pixelHeight));
    for (int y = 0; y < image.pixelHeight; ++y) {
        const auto* sourceRow = reinterpret_cast<const QRgb*>(converted.constScanLine(y));
        std::uint8_t* destinationRow =
            image.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.stride);
        for (int x = 0; x < image.pixelWidth; ++x) {
            std::uint8_t* pixel = destinationRow + static_cast<std::size_t>(x) * 4U;
            pixel[0] = static_cast<std::uint8_t>(qBlue(sourceRow[x]));
            pixel[1] = static_cast<std::uint8_t>(qGreen(sourceRow[x]));
            pixel[2] = static_cast<std::uint8_t>(qRed(sourceRow[x]));
            pixel[3] = static_cast<std::uint8_t>(qAlpha(sourceRow[x]));
        }
    }
    return image;
}

}  // namespace cnote::clipboard
