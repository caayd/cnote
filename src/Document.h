#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cnote {

enum class TextAlignment : std::uint8_t {
    Left = 0,
    Center = 1,
    Right = 2,
};

enum class TextFontFamily : std::uint8_t {
    Sans = 0,
    Serif = 1,
    Monospace = 2,
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct StrokeStyle {
    std::uint32_t color = 0xFF1F2937;
    float width = 3.0f;
    bool dotted = false;
};

struct Stroke {
    StrokeStyle style {};
    float smoothing = 0.55f;
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

struct PageText {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float fontSize = 24.0f;
    std::uint32_t color = 0xFF111827;
    TextAlignment alignment = TextAlignment::Left;
    TextFontFamily fontFamily = TextFontFamily::Sans;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::wstring content {};
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
    std::int32_t sourcePdfSourceIndex = -1;
    std::int32_t sourcePdfPageIndex = -1;
    RasterImage raster {};
    std::vector<PageImage> images;
    std::vector<PageText> texts;
    std::vector<Stroke> strokes;
};

class Document {
public:
    struct PdfSource {
        std::wstring sourcePath {};
        std::shared_ptr<std::vector<std::uint8_t>> pdfData {};
    };

    struct PageDefaults {
        float width = 612.0f;
        float height = 792.0f;
        std::uint32_t backgroundColor = 0xFFFFFFFFU;
    };

    static Document CreateBlank();
    static Document CreateBlank(PageDefaults pageDefaults);
    static Document CreateEditable(
        std::wstring filePath,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);
    static Document CreatePdfDocument(
        std::wstring sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfData,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);
    static Document CreateProjectDocument(
        std::wstring filePath,
        std::vector<PdfSource> pdfSources,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);

    [[nodiscard]] bool IsPdfDocument() const noexcept;
    [[nodiscard]] const std::wstring& FilePath() const noexcept;
    [[nodiscard]] const std::wstring& SourcePath() const noexcept;
    [[nodiscard]] const std::shared_ptr<std::vector<std::uint8_t>>& PdfData() const noexcept;
    [[nodiscard]] bool HasPdfData() const noexcept;
    [[nodiscard]] std::size_t PdfSourceCount() const noexcept;
    [[nodiscard]] const PdfSource& PdfSourceAt(std::size_t index) const noexcept;
    [[nodiscard]] const PdfSource* PdfSourceAtIndex(std::int32_t index) const noexcept;
    [[nodiscard]] std::size_t PageCount() const noexcept;
    [[nodiscard]] std::size_t CurrentPageIndex() const noexcept;
    [[nodiscard]] Page& CurrentPage() noexcept;
    [[nodiscard]] const Page& CurrentPage() const noexcept;
    [[nodiscard]] Page& PageAt(std::size_t index) noexcept;
    [[nodiscard]] const Page& PageAt(std::size_t index) const noexcept;
    [[nodiscard]] const PageDefaults& NewPageDefaults() const noexcept;

    bool SetCurrentPageIndex(std::size_t index);
    std::size_t InsertPage(std::size_t index, Page page);
    std::size_t RemovePage(std::size_t index);
    void SetFilePath(std::wstring filePath);
    void SetNewPageDefaults(PageDefaults pageDefaults);
    std::int32_t AppendPdfSource(std::wstring sourcePath, std::shared_ptr<std::vector<std::uint8_t>> pdfData);
    void ResetToBlank();
    void ReplaceWithEditable(
        std::wstring filePath,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);
    void ReplaceWithProject(
        std::wstring filePath,
        std::vector<PdfSource> pdfSources,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);

private:
    std::wstring filePath_;
    bool pdfDocument_ = false;
    std::vector<PdfSource> pdfSources_ {};
    std::vector<Page> pages_ { Page {} };
    std::size_t currentPageIndex_ = 0;
    PageDefaults newPageDefaults_ {};
};

}  // namespace cnote
