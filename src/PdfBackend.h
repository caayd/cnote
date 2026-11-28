#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Document.h"

namespace cnote {

struct PdfTextCharBox {
    std::uint32_t unicode = 0;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// Top-down device-pixel rect in a full-page PDF raster (RenderPage's space).
struct PdfPixelRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// PDFium wrapper. PDFium parses from the caller's byte buffer for as long as a
// document is open, so every operation goes through one process-wide handle
// cache keyed by the exact PdfSource byte buffer (shared ownership, not an
// address): a buffer replaced by a different allocation, or dropped entirely,
// invalidates its handle instead of being reused.
//
// PDFium is not thread-safe: every method here must run on the GUI thread, the
// only thread that ever calls PDFium (the background save worker serializes the
// model and never touches this class).
class PdfiumBackend final {
public:
    PdfiumBackend();
    ~PdfiumBackend();
    PdfiumBackend(const PdfiumBackend&) = delete;
    PdfiumBackend& operator=(const PdfiumBackend&) = delete;

    [[nodiscard]] std::optional<Document> OpenFromMemory(
        const std::filesystem::path& sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfBytes) const;

    // Opens each stored PDF source once (reusing the handle cache) and
    // validates every page that still references it. A valid source keeps its
    // indices and any stored raster; a missing or invalid source (or an
    // unreadable source page) clears the page's indices while keeping its size,
    // rotation and stored raster. A page left without a raster paints as blank
    // paper; no full-page fallback raster is allocated for it. Model decode
    // stays usable without calling this.
    void HydrateProject(Document& document) const;

    // Returns the embedded cnote project attachment bytes, if present.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> ExtractEmbeddedProject(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes) const;

    [[nodiscard]] std::optional<RasterImage> RenderPage(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex,
        float renderScale) const;
    // Renders only `region` (device pixels of the full RenderPage raster) into
    // a bitmap exactly that size.  Lets callers above the full-page raster
    // budget compose a page from bounded excerpts.
    [[nodiscard]] std::optional<RasterImage> RenderPageRegion(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex,
        float renderScale,
        PdfPixelRect region) const;
    [[nodiscard]] std::optional<std::vector<PdfTextCharBox>> ExtractTextCharBoxes(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex) const;
    // Number of FPDF_LoadMemDocument64 calls this process made since PDFium
    // was initialized. Diagnostics only; the handle cache should keep it flat
    // while one source is used repeatedly.
    [[nodiscard]] std::size_t DocumentLoadCount() const;
    [[nodiscard]] bool ExportPdf(
        const Document& document,
        const std::filesystem::path& filePath,
        float renderScale,
        float strokeSmoothing,
        float dashLength,
        float dashGap,
        std::span<const std::uint8_t> embeddedProject) const;

private:
    bool initialized_ = false;
};

}  // namespace cnote
