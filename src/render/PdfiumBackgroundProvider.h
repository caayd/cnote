#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "render/PdfBackgroundProvider.h"
#include "render/RasterBudget.h"

namespace cnote {
class Document;
class PdfiumBackend;
}  // namespace cnote

namespace cnote::render {

// PdfiumBackend-backed background provider with a bounded LRU cache keyed by
// (document, PDF bytes, source index, page index, raster scale, region). Only
// the pages the controller asks for are rendered, so a 50-page PDF never
// allocates 50 full pages, and a zoom or DPR change re-renders at the new scale
// instead of scaling a stale bitmap.  The cache is bounded by entry count and
// by bytes: page surfaces and tiles above the raster budget ask for bounded
// regions, and those region rasters count towards the same budget.
//
// Failed renders are negatively cached too, so a broken page does not retry on
// every paint. The key pins the document and the exact PDF byte buffer, so a
// source replaced with different bytes (or a retargeted page) misses instead of
// serving the failure forever. Invalidate() drops every entry, positive and
// negative.
class PdfiumBackgroundProvider final : public PdfBackgroundProvider {
public:
    PdfiumBackgroundProvider(const PdfiumBackend& backend, const Document& document,
        std::size_t capacity = 8, std::size_t maxBytes = kPdfBackgroundCacheBytes);

    [[nodiscard]] QImage PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale) override;
    // Bounded region of that same background raster (top-down device pixels of
    // the full-page background at `rasterScale`).
    [[nodiscard]] std::optional<QImage> PageBackgroundRegion(
        std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale, const QRect& pixelRect) override;

    // Drops every cached page, including the negative cache.
    void Invalidate();

    // Diagnostics for the render evidence.
    [[nodiscard]] std::size_t RenderCount() const noexcept { return renderCount_; }
    [[nodiscard]] std::size_t FailedRenderCount() const noexcept { return failedRenderCount_; }
    [[nodiscard]] std::size_t CachedPageCount() const noexcept { return cache_.size(); }
    [[nodiscard]] std::size_t CachedBytes() const noexcept { return cachedBytes_; }

private:
    enum class EntryStatus : std::uint8_t {
        Ready,
        Failed,
    };

    struct Key {
        const Document* document = nullptr;
        const std::vector<std::uint8_t>* pdfData = nullptr;
        std::int32_t source = -1;
        std::int32_t page = -1;
        int scaleMilli = 0;
        QRect region {};

        [[nodiscard]] bool operator==(const Key& other) const noexcept {
            return document == other.document
                && pdfData == other.pdfData
                && source == other.source
                && page == other.page
                && scaleMilli == other.scaleMilli
                && region == other.region;
        }
    };

    struct Entry {
        Key key {};
        QImage image;
        EntryStatus status = EntryStatus::Ready;
    };

    // Finds a cached entry and refreshes its LRU position; returns its status
    // and image through `image`.
    [[nodiscard]] bool Lookup(const Key& key, QImage* image) const;
    // Stores (or refreshes) an entry, evicting the oldest while over capacity
    // or over the byte budget (never evicting the entry just stored).
    void Store(const Key& key, QImage image, EntryStatus status);

    const PdfiumBackend& backend_;
    const Document* document_ = nullptr;
    std::size_t capacity_ = 8;
    std::size_t maxBytes_ = kPdfBackgroundCacheBytes;
    std::size_t renderCount_ = 0;
    std::size_t failedRenderCount_ = 0;
    std::size_t cachedBytes_ = 0;
    mutable std::deque<Entry> cache_;
};

}  // namespace cnote::render
