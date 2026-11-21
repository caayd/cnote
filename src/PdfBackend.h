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

class PdfiumBackend final {
public:
    PdfiumBackend();
    ~PdfiumBackend();
    PdfiumBackend(const PdfiumBackend&) = delete;
    PdfiumBackend& operator=(const PdfiumBackend&) = delete;

    [[nodiscard]] std::optional<Document> OpenFromMemory(
        const std::filesystem::path& sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfBytes) const;

    // Reopens each stored PDF source and validates page backgrounds. A valid
    // source keeps its indices and any stored raster; a missing or invalid
    // source (or an unreadable source page) clears the page's indices so it is
    // raster-only, preserving the stored raster, with a solid fallback when
    // none exists. Model decode stays usable without calling this.
    void HydrateProject(Document& document) const;

    // Returns the embedded cnote project attachment bytes, if present.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> ExtractEmbeddedProject(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes) const;

    [[nodiscard]] std::optional<RasterImage> RenderPage(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex,
        float renderScale) const;
    [[nodiscard]] std::optional<std::vector<PdfTextCharBox>> ExtractTextCharBoxes(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex) const;
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
