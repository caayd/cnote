#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "Document.h"

namespace cnote {

struct PdfTextCharBox {
    std::uint32_t unicode = 0;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

class IPdfBackend {
public:
    virtual ~IPdfBackend() = default;

    [[nodiscard]] virtual std::optional<Document> Open(const std::wstring& filePath) = 0;
};

class PdfiumBackend final : public IPdfBackend {
public:
    PdfiumBackend();
    ~PdfiumBackend() override;

    [[nodiscard]] std::optional<Document> Open(const std::wstring& filePath) override;
    [[nodiscard]] std::optional<Document> OpenFromMemory(
        const std::wstring& sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfBytes) const;
    [[nodiscard]] std::optional<RasterImage> RenderPage(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex,
        float renderScale) const;
    [[nodiscard]] std::optional<std::vector<PdfTextCharBox>> ExtractTextCharBoxes(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex) const;
    [[nodiscard]] bool RenderPageToHdc(
        const std::shared_ptr<std::vector<std::uint8_t>>& pdfBytes,
        std::size_t pageIndex,
        HDC hdc,
        int x,
        int y,
        int width,
        int height) const;
    [[nodiscard]] bool ExportPdf(
        const Document& document,
        const std::wstring& filePath,
        float renderScale,
        float strokeSmoothing,
        float dashLength,
        float dashGap,
        bool embedProject) const;

private:
    bool initialized_ = false;
};

}  // namespace cnote
