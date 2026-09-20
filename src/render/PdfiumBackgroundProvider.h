#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "render/PdfBackgroundProvider.h"

namespace cnote {
class Document;
class PdfiumBackend;
}  // namespace cnote

namespace cnote::render {

// PdfiumBackend-backed background provider with a bounded LRU cache keyed by
// (source index, page index, raster scale). Only the pages the controller asks
// for are rendered, so a 50-page PDF never allocates 50 full pages, and a zoom
// or DPR change re-renders at the new scale instead of scaling a stale bitmap.
class PdfiumBackgroundProvider final : public PdfBackgroundProvider {
public:
    PdfiumBackgroundProvider(const PdfiumBackend& backend, const Document& document, std::size_t capacity = 8);

    [[nodiscard]] QImage PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale) override;

    // Diagnostics for the render evidence.
    [[nodiscard]] std::size_t RenderCount() const noexcept { return renderCount_; }
    [[nodiscard]] std::size_t CachedPageCount() const noexcept { return cache_.size(); }

private:
    struct Key {
        std::int32_t source = -1;
        std::int32_t page = -1;
        int scaleMilli = 0;

        [[nodiscard]] bool operator==(const Key& other) const noexcept {
            return source == other.source && page == other.page && scaleMilli == other.scaleMilli;
        }
    };

    struct Entry {
        Key key {};
        QImage image;
    };

    const PdfiumBackend& backend_;
    const Document* document_ = nullptr;
    std::size_t capacity_ = 8;
    std::size_t renderCount_ = 0;
    std::deque<Entry> cache_;
};

}  // namespace cnote::render
