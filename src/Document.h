#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ctrainer {

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
    RasterImage raster {};
    std::vector<Stroke> strokes;
};

class Document {
public:
    static Document CreateBlank();
    static Document CreatePdfDocument(std::wstring sourcePath, std::vector<Page> pages);

    [[nodiscard]] bool IsPdfDocument() const noexcept;
    [[nodiscard]] const std::wstring& SourcePath() const noexcept;
    [[nodiscard]] std::size_t PageCount() const noexcept;
    [[nodiscard]] std::size_t CurrentPageIndex() const noexcept;
    [[nodiscard]] Page& CurrentPage() noexcept;
    [[nodiscard]] const Page& CurrentPage() const noexcept;
    [[nodiscard]] Page& PageAt(std::size_t index) noexcept;
    [[nodiscard]] const Page& PageAt(std::size_t index) const noexcept;

    bool SetCurrentPageIndex(std::size_t index);
    void ResetToBlank();
    void ReplaceWithPdf(std::wstring sourcePath, std::vector<Page> pages);

private:
    std::wstring sourcePath_;
    bool pdfDocument_ = false;
    std::vector<Page> pages_ { Page {} };
    std::size_t currentPageIndex_ = 0;
};

}  // namespace ctrainer
