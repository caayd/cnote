#include "render/PdfiumBackgroundProvider.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Document.h"
#include "PdfBackend.h"
#include "render/PageRenderer.h"

namespace cnote::render {

namespace {

// Hands the backend's owned BGRA pixels to QImage without copying: the heap
// PixelBuffer kept as the cleanup info holds the (shared) buffer alive until
// the last QImage reference is released. The image is read-only, so painting on
// a detached copy cannot write into the shared raster.
QImage WrapOwnedBgraImage(RasterImage raster) {
    if (raster.width <= 0 || raster.height <= 0 || raster.pixels.empty()) {
        return {};
    }
    auto* owner = new PixelBuffer(std::move(raster.pixels));
    return QImage(owner->data(), raster.width, raster.height,
        raster.stride > 0 ? raster.stride : raster.width * 4, QImage::Format_ARGB32,
        [](void* info) { delete static_cast<PixelBuffer*>(info); }, owner);
}

}  // namespace

PdfiumBackgroundProvider::PdfiumBackgroundProvider(const PdfiumBackend& backend, const Document& document,
    std::size_t capacity, std::size_t maxBytes,
    std::size_t thumbnailCapacity, std::size_t thumbnailMaxBytes)
    : backend_(backend),
      document_(&document) {
    canvas_.capacity = std::max<std::size_t>(1U, capacity);
    canvas_.maxBytes = std::max<std::size_t>(1U, maxBytes);
    thumbnails_.capacity = std::max<std::size_t>(1U, thumbnailCapacity);
    thumbnails_.maxBytes = std::max<std::size_t>(1U, thumbnailMaxBytes);
}

void PdfiumBackgroundProvider::Invalidate() {
    canvas_.entries.clear();
    canvas_.bytes = 0;
    thumbnails_.entries.clear();
    thumbnails_.bytes = 0;
}

bool PdfiumBackgroundProvider::Lookup(Cache& cache, const Key& key, QImage* image) const {
    const auto existing = std::find_if(cache.entries.begin(), cache.entries.end(), [&](const Entry& entry) {
        return entry.key == key;
    });
    if (existing == cache.entries.end()) {
        return false;
    }
    Entry entry = std::move(*existing);
    cache.entries.erase(existing);
    cache.entries.push_back(std::move(entry));
    const EntryStatus status = cache.entries.back().status;
    if (image != nullptr) {
        *image = status == EntryStatus::Ready ? cache.entries.back().image : QImage {};
    }
    return true;
}

void PdfiumBackgroundProvider::Store(Cache& cache, const Key& key, QImage image, EntryStatus status) const {
    cache.bytes += static_cast<std::size_t>(image.isNull() ? 0 : image.sizeInBytes());
    cache.entries.push_back({ key, std::move(image), status });
    while (cache.entries.size() > 1U
        && (cache.entries.size() > cache.capacity || cache.bytes > cache.maxBytes)) {
        const Entry& oldest = cache.entries.front();
        if (!oldest.image.isNull()) {
            cache.bytes -= static_cast<std::size_t>(oldest.image.sizeInBytes());
        }
        cache.entries.pop_front();
    }
}

QImage PdfiumBackgroundProvider::PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex,
    float rasterScale, PdfBackgroundUse use) {
    if (document_ == nullptr || sourceIndex < 0 || pageIndex < 0 || rasterScale <= 0.0f) {
        return {};
    }

    // Validate the current source before trusting the cache: a document may
    // have been retargeted or had its sources replaced under the same address.
    const Document::PdfSource* source = document_->PdfSourceAtIndex(sourceIndex);
    if (source == nullptr || !source->pdfData || source->pdfData->empty()) {
        return {};
    }

    Cache& cache = CacheFor(use);
    const Key key {
        document_,
        source->pdfData.get(),
        sourceIndex,
        pageIndex,
        static_cast<int>(std::lround(rasterScale * 1000.0f)),
        QRect(),
    };
    QImage cached;
    if (Lookup(cache, key, &cached)) {
        return cached;
    }

    auto raster = backend_.RenderPage(source->pdfData, static_cast<std::size_t>(pageIndex), rasterScale);
    if (!raster.has_value() || raster->pixels.empty() || raster->width <= 0 || raster->height <= 0) {
        ++failedRenderCount_;
        Store(cache, key, QImage {}, EntryStatus::Failed);
        return {};
    }

    ++renderCount_;
    QImage image = WrapOwnedBgraImage(std::move(*raster));
    Store(cache, key, image, EntryStatus::Ready);
    return image;
}

std::optional<QImage> PdfiumBackgroundProvider::PageBackgroundRegion(
    std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale, const QRect& pixelRect) {
    if (document_ == nullptr || sourceIndex < 0 || pageIndex < 0 || rasterScale <= 0.0f
        || pixelRect.isEmpty()) {
        return std::nullopt;
    }
    const Document::PdfSource* source = document_->PdfSourceAtIndex(sourceIndex);
    if (source == nullptr || !source->pdfData || source->pdfData->empty()) {
        return std::nullopt;
    }

    const Key key {
        document_,
        source->pdfData.get(),
        sourceIndex,
        pageIndex,
        static_cast<int>(std::lround(rasterScale * 1000.0f)),
        pixelRect,
    };
    QImage cached;
    if (Lookup(canvas_, key, &cached)) {
        return cached.isNull() ? std::nullopt : std::optional<QImage>(std::move(cached));
    }

    auto raster = backend_.RenderPageRegion(
        source->pdfData,
        static_cast<std::size_t>(pageIndex),
        rasterScale,
        PdfPixelRect { pixelRect.x(), pixelRect.y(), pixelRect.width(), pixelRect.height() });
    if (!raster.has_value() || raster->pixels.empty() || raster->width <= 0 || raster->height <= 0) {
        ++failedRenderCount_;
        Store(canvas_, key, QImage {}, EntryStatus::Failed);
        return std::nullopt;
    }

    ++renderCount_;
    QImage image = WrapOwnedBgraImage(std::move(*raster));
    Store(canvas_, key, image, EntryStatus::Ready);
    return image;
}

}  // namespace cnote::render
