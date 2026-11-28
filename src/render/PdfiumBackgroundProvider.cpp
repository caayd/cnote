#include "render/PdfiumBackgroundProvider.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "Document.h"
#include "PdfBackend.h"
#include "render/PageRenderer.h"

namespace cnote::render {

PdfiumBackgroundProvider::PdfiumBackgroundProvider(const PdfiumBackend& backend, const Document& document,
    std::size_t capacity, std::size_t maxBytes)
    : backend_(backend),
      document_(&document),
      capacity_(std::max<std::size_t>(1U, capacity)),
      maxBytes_(std::max<std::size_t>(1U, maxBytes)) {}

void PdfiumBackgroundProvider::Invalidate() {
    cache_.clear();
    cachedBytes_ = 0;
}

bool PdfiumBackgroundProvider::Lookup(const Key& key, QImage* image) const {
    const auto existing = std::find_if(cache_.begin(), cache_.end(), [&](const Entry& entry) {
        return entry.key == key;
    });
    if (existing == cache_.end()) {
        return false;
    }
    Entry entry = std::move(*existing);
    cache_.erase(existing);
    cache_.push_back(std::move(entry));
    const EntryStatus status = cache_.back().status;
    if (image != nullptr) {
        *image = status == EntryStatus::Ready ? cache_.back().image : QImage {};
    }
    return true;
}

void PdfiumBackgroundProvider::Store(const Key& key, QImage image, EntryStatus status) {
    cachedBytes_ += static_cast<std::size_t>(image.isNull() ? 0 : image.sizeInBytes());
    cache_.push_back({ key, std::move(image), status });
    while (cache_.size() > 1U
        && (cache_.size() > capacity_ || cachedBytes_ > maxBytes_)) {
        const Entry& oldest = cache_.front();
        if (!oldest.image.isNull()) {
            cachedBytes_ -= static_cast<std::size_t>(oldest.image.sizeInBytes());
        }
        cache_.pop_front();
    }
}

QImage PdfiumBackgroundProvider::PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale) {
    if (document_ == nullptr || sourceIndex < 0 || pageIndex < 0 || rasterScale <= 0.0f) {
        return {};
    }

    // Validate the current source before trusting the cache: a document may
    // have been retargeted or had its sources replaced under the same address.
    const Document::PdfSource* source = document_->PdfSourceAtIndex(sourceIndex);
    if (source == nullptr || !source->pdfData || source->pdfData->empty()) {
        return {};
    }

    const Key key {
        document_,
        source->pdfData.get(),
        sourceIndex,
        pageIndex,
        static_cast<int>(std::lround(rasterScale * 1000.0f)),
        QRect(),
    };
    QImage cached;
    if (Lookup(key, &cached)) {
        return cached;
    }

    const auto raster = backend_.RenderPage(source->pdfData, static_cast<std::size_t>(pageIndex), rasterScale);
    if (!raster.has_value() || raster->pixels.empty() || raster->width <= 0 || raster->height <= 0) {
        ++failedRenderCount_;
        Store(key, QImage {}, EntryStatus::Failed);
        return {};
    }

    ++renderCount_;
    QImage image = WrapBgraImage(raster->pixels.data(), raster->width, raster->height, raster->stride).copy();
    Store(key, image, EntryStatus::Ready);
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
    if (Lookup(key, &cached)) {
        return cached.isNull() ? std::nullopt : std::optional<QImage>(std::move(cached));
    }

    const auto raster = backend_.RenderPageRegion(
        source->pdfData,
        static_cast<std::size_t>(pageIndex),
        rasterScale,
        PdfPixelRect { pixelRect.x(), pixelRect.y(), pixelRect.width(), pixelRect.height() });
    if (!raster.has_value() || raster->pixels.empty() || raster->width <= 0 || raster->height <= 0) {
        ++failedRenderCount_;
        Store(key, QImage {}, EntryStatus::Failed);
        return std::nullopt;
    }

    ++renderCount_;
    QImage image = WrapBgraImage(raster->pixels.data(), raster->width, raster->height, raster->stride).copy();
    Store(key, image, EntryStatus::Ready);
    return image;
}

}  // namespace cnote::render
