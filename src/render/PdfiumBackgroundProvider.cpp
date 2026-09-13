#include "render/PdfiumBackgroundProvider.h"

#include <algorithm>
#include <cmath>

#include "Document.h"
#include "PdfBackend.h"
#include "render/PageRenderer.h"

namespace cnote::render {

PdfiumBackgroundProvider::PdfiumBackgroundProvider(const PdfiumBackend& backend, const Document& document, std::size_t capacity)
    : backend_(backend),
      document_(&document),
      capacity_(std::max<std::size_t>(1U, capacity)) {}

void PdfiumBackgroundProvider::SetDocument(const Document& document) {
    document_ = &document;
    Clear();
}

void PdfiumBackgroundProvider::Clear() {
    cache_.clear();
}

QImage PdfiumBackgroundProvider::PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale) {
    if (document_ == nullptr || sourceIndex < 0 || pageIndex < 0 || rasterScale <= 0.0f) {
        return {};
    }

    const Key key { sourceIndex, pageIndex, static_cast<int>(std::lround(rasterScale * 1000.0f)) };
    const auto existing = std::find_if(cache_.begin(), cache_.end(), [&](const Entry& entry) {
        return entry.key == key;
    });
    if (existing != cache_.end()) {
        Entry entry = std::move(*existing);
        cache_.erase(existing);
        cache_.push_back(std::move(entry));
        return cache_.back().image;
    }

    const Document::PdfSource* source = document_->PdfSourceAtIndex(sourceIndex);
    if (source == nullptr || !source->pdfData || source->pdfData->empty()) {
        return {};
    }

    const auto raster = backend_.RenderPage(source->pdfData, static_cast<std::size_t>(pageIndex), rasterScale);
    if (!raster.has_value() || raster->pixels.empty() || raster->width <= 0 || raster->height <= 0) {
        return {};
    }

    ++renderCount_;
    QImage image = WrapBgraImage(raster->pixels.data(), raster->width, raster->height, raster->stride).copy();
    cache_.push_back({ key, image });
    while (cache_.size() > capacity_) {
        cache_.pop_front();
    }
    return image;
}

}  // namespace cnote::render
