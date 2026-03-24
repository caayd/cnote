#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cnote {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct StrokeStyle {
    std::uint32_t color = 0xFF1F2937;
    float width = 3.0f;
};

struct Stroke {
    StrokeStyle style {};
    std::vector<Vec2> points;
};

struct PageImage {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float rotationDegrees = 0.0f;
    int pixelWidth = 0;
    int pixelHeight = 0;
    int stride = 0;
    std::vector<std::uint8_t> pixels;
};

struct RasterImage {
    int width = 0;
    int height = 0;
    int stride = 0;
    float scale = 0.0f;
    std::vector<std::uint8_t> pixels;
};

struct Page {
    float width = 612.0f;
    float height = 792.0f;
    std::int32_t sourcePdfPageIndex = -1;
    RasterImage raster {};
    std::vector<PageImage> images;
    std::vector<Stroke> strokes;
};

class Document {
public:
    static Document CreateBlank();
    static Document CreateEditable(std::wstring filePath, std::vector<Page> pages, std::size_t currentPageIndex = 0);
    static Document CreatePdfDocument(
        std::wstring sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfData,
        std::vector<Page> pages,
        std::size_t currentPageIndex = 0);
    static Document CreateProjectDocument(
        std::wstring filePath,
        std::wstring sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfData,
        std::vector<Page> pages,
        std::size_t currentPageIndex = 0);

    [[nodiscard]] bool IsPdfDocument() const noexcept;
    [[nodiscard]] const std::wstring& FilePath() const noexcept;
    [[nodiscard]] const std::wstring& SourcePath() const noexcept;
    [[nodiscard]] const std::shared_ptr<std::vector<std::uint8_t>>& PdfData() const noexcept;
    [[nodiscard]] bool HasPdfData() const noexcept;
    [[nodiscard]] std::size_t PageCount() const noexcept;
    [[nodiscard]] std::size_t CurrentPageIndex() const noexcept;
    [[nodiscard]] Page& CurrentPage() noexcept;
    [[nodiscard]] const Page& CurrentPage() const noexcept;
    [[nodiscard]] Page& PageAt(std::size_t index) noexcept;
    [[nodiscard]] const Page& PageAt(std::size_t index) const noexcept;

    bool SetCurrentPageIndex(std::size_t index);
    std::size_t InsertPage(std::size_t index, Page page);
    std::size_t RemovePage(std::size_t index);
    void SetFilePath(std::wstring filePath);
    void ResetToBlank();
    void ReplaceWithEditable(std::wstring filePath, std::vector<Page> pages, std::size_t currentPageIndex = 0);
    void ReplaceWithPdf(
        std::wstring filePath,
        std::wstring sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfData,
        std::vector<Page> pages,
        std::size_t currentPageIndex = 0);

private:
    std::wstring filePath_;
    std::wstring sourcePath_;
    bool pdfDocument_ = false;
    std::shared_ptr<std::vector<std::uint8_t>> pdfData_ {};
    std::vector<Page> pages_ { Page {} };
    std::size_t currentPageIndex_ = 0;
};

}  // namespace cnote
