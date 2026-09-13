#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

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

    // Reopens each stored PDF source and fills page geometry/rasters from it.
    // Missing or invalid sources fall back to a solid page raster. Model decode
    // stays usable without calling this.
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
#ifdef _WIN32
    [[nodiscard]] bool RenderPageToHdc(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex,
        HDC hdc,
        int x,
        int y,
        int width,
        int height) const;
#endif
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
