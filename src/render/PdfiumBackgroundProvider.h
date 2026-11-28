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

// PdfiumBackend-backed background provider with bounded LRU caches keyed by
// (document, PDF bytes, source index, page index, raster scale, region). Only
// the pages the controller asks for are rendered, so a 50-page PDF never
// allocates 50 full pages, and a zoom or DPR change re-renders at the new scale
// instead of scaling a stale bitmap.  The canvas cache is bounded by entry count
// and by bytes: page surfaces and tiles above the raster budget ask for bounded
// regions, and those region rasters count towards the same budget.  Thumbnails
// use a second, much smaller cache so sidebar churn cannot evict canvas
// surfaces (and vice versa).
//
// Rendered bytes come from the backend as an owned pixel buffer and become the
// QImage's storage directly, so a cache miss does not copy the raster a second
// time.
//
// Failed renders are negatively cached too, so a broken page does not retry on
// every paint. The key pins the document and the exact PDF byte buffer, so a
// source replaced with different bytes (or a retargeted page) misses instead of
// serving the failure forever. Invalidate() drops every entry, positive and
// negative, in both caches.
class PdfiumBackgroundProvider final : public PdfBackgroundProvider {
public:
    PdfiumBackgroundProvider(const PdfiumBackend& backend, const Document& document,
        std::size_t capacity = 8, std::size_t maxBytes = kPdfBackgroundCacheBytes,
        std::size_t thumbnailCapacity = kPdfThumbnailCacheEntries,
        std::size_t thumbnailMaxBytes = kPdfThumbnailCacheBytes);

    [[nodiscard]] QImage PageBackground(std::int32_t sourceIndex, std::int32_t pageIndex,
        float rasterScale, PdfBackgroundUse use = PdfBackgroundUse::Canvas) override;
    // Bounded region of that same background raster (top-down device pixels of
    // the full-page background at `rasterScale`). Regions only ever come from
    // canvas page surfaces and tiles, so they use the canvas cache.
    [[nodiscard]] std::optional<QImage> PageBackgroundRegion(
        std::int32_t sourceIndex, std::int32_t pageIndex, float rasterScale, const QRect& pixelRect,
        QSize fullPagePixelSize = {}) override;

    // Drops every cached page, including the negative cache.
    void Invalidate();

    // Diagnostics for the render evidence (both caches).  RenderCount is the
    // total PDFium render count; FullPageRenderCount counts the full-page
    // renders that every region patch should be an exact crop of.
    [[nodiscard]] std::size_t RenderCount() const noexcept { return renderCount_; }
    [[nodiscard]] std::size_t FullPageRenderCount() const noexcept { return fullPageRenderCount_; }
    [[nodiscard]] std::size_t RegionRenderCount() const noexcept { return regionRenderCount_; }
    [[nodiscard]] std::size_t FailedRenderCount() const noexcept { return failedRenderCount_; }
    [[nodiscard]] std::size_t CachedPageCount() const noexcept {
        return canvas_.entries.size() + thumbnails_.entries.size();
    }
    [[nodiscard]] std::size_t CachedBytes() const noexcept {
        return canvas_.bytes + thumbnails_.bytes;
    }
    // Bytes currently held by one consumer's cache; the two are independent.
    [[nodiscard]] std::size_t CachedBytes(PdfBackgroundUse use) const noexcept {
        return CacheFor(use).bytes;
    }

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

    struct Cache {
        std::deque<Entry> entries;
        std::size_t bytes = 0;
        std::size_t capacity = 1;
        std::size_t maxBytes = 1;
    };

    [[nodiscard]] const Cache& CacheFor(PdfBackgroundUse use) const noexcept {
        return use == PdfBackgroundUse::Thumbnail ? thumbnails_ : canvas_;
    }
    [[nodiscard]] Cache& CacheFor(PdfBackgroundUse use) noexcept {
        return use == PdfBackgroundUse::Thumbnail ? thumbnails_ : canvas_;
    }

    // Finds a cached entry and refreshes its LRU position; returns its status
    // and image through `image`.
    [[nodiscard]] bool Lookup(Cache& cache, const Key& key, QImage* image) const;
    // Stores (or refreshes) an entry, evicting the oldest while over capacity
    // or over the byte budget (never evicting the entry just stored).
    void Store(Cache& cache, const Key& key, QImage image, EntryStatus status) const;

    const PdfiumBackend& backend_;
    const Document* document_ = nullptr;
    std::size_t renderCount_ = 0;
    std::size_t fullPageRenderCount_ = 0;
    std::size_t regionRenderCount_ = 0;
    std::size_t failedRenderCount_ = 0;
    mutable Cache canvas_ {};
    mutable Cache thumbnails_ {};
};

}  // namespace cnote::render
