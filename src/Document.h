#pragma once

#include <cstdint>
#include <filesystem>
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
    // Bundled Shantell Sans (SIL OFL 1.1, third_party/shantell-sans); the
    // default for new text.  Serialized as 3; readers predating it clamp to
    // Monospace.
    ShantellSans = 3,
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

struct PageTextStyle {
    float fontSize = 24.0f;
    std::uint32_t color = 0xFF111827;
    TextFontFamily fontFamily = TextFontFamily::ShantellSans;
    bool bold = false;
    bool italic = false;
    bool underline = false;
};

struct PageTextStyleSpan {
    std::uint32_t start = 0;
    std::uint32_t length = 0;
    PageTextStyle style {};
};

struct PageText {
    std::uint64_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float fontSize = 24.0f;
    std::uint32_t color = 0xFF111827;
    TextAlignment alignment = TextAlignment::Left;
    TextFontFamily fontFamily = TextFontFamily::ShantellSans;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    // Clockwise degrees the text is rotated about its own bounds centre; 0 is
    // upright.  Page rotation adds multiples of 90.
    float rotationDegrees = 0.0f;
    std::u16string content {};
    std::vector<PageTextStyleSpan> styleSpans {};
};

// A rendered LaTeX equation block. The source is the single source of truth:
// rendered output is derived (and cached) by cnote::latex, never stored here.
// x/y is the top-left of the equation's bounding box in page units; the box
// size itself comes from latex::Measure. Resizing scales fontSize.
struct PageLatex {
    std::uint64_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 24.0f;
    std::uint32_t color = 0xFF111827;
    std::u16string content {};
    // Clockwise degrees the formula is rotated about its bounds centre.
    float rotationDegrees = 0.0f;
};

struct RasterImage {
    int width = 0;
    int height = 0;
    int stride = 0;
    float scale = 0.0f;
    std::vector<std::uint8_t> pixels;
};

struct Page;

[[nodiscard]] bool RasterImageEqual(const RasterImage& a, const RasterImage& b);
[[nodiscard]] RasterImage CreateSolidRaster(float pageWidth, float pageHeight, float renderScale, std::uint32_t argb);
[[nodiscard]] bool Vec2Equal(const Vec2& a, const Vec2& b);
[[nodiscard]] bool StrokeStyleEqual(const StrokeStyle& a, const StrokeStyle& b);
[[nodiscard]] bool StrokeEqual(const Stroke& a, const Stroke& b);
[[nodiscard]] bool PageImageEqual(const PageImage& a, const PageImage& b);
[[nodiscard]] bool PageTextEqual(const PageText& a, const PageText& b);
[[nodiscard]] bool PageLatexEqual(const PageLatex& a, const PageLatex& b);
[[nodiscard]] bool PageTextStyleEqual(const PageTextStyle& a, const PageTextStyle& b);
[[nodiscard]] bool PageEqual(const Page& a, const Page& b);

// Clockwise rotation of a point inside a W×H frame for a quarter turn; the
// result lives inside the rotated frame (H×W for odd turns).  `quarterTurns`
// is normalized to 0-3.
[[nodiscard]] Vec2 RotatePointQuarterTurn(Vec2 point, float width, float height, int quarterTurns);

// Rotates a BGRA raster clockwise by quarter turns, swapping dimensions and
// rebasing the stride to width*4; a no-op (input returned) for 0 turns or an
// empty raster.
[[nodiscard]] RasterImage RotateRasterQuarterTurn(const RasterImage& raster, int quarterTurns);

// Rotates every page coordinate-bearing object clockwise by quarter turns
// about the page frame and swaps width/height for odd turns.  Strokes,
// images (rotation accumulated), texts (box rotated, rotation accumulated),
// LaTeX (bounds rotated, rotation accumulated) and the stored raster move
// with the page; Page::rotationDegrees accumulates.  PDF source pages keep
// their source indices - their background rotates at draw time.
void RotatePageContents(Page& page, int quarterTurns);

struct Page {
    float width = 595.0f;
    float height = 842.0f;
    // Clockwise degrees the page was rotated (sidebar Rotate actions); always
    // a multiple of 90 in this UI.  Object coordinates and the stored raster
    // are already transformed; PDF source backgrounds and PDF text boxes are
    // rendered/extracted unrotated and rotated by this value at use sites.
    float rotationDegrees = 0.0f;
    std::int32_t sourcePdfSourceIndex = -1;
    std::int32_t sourcePdfPageIndex = -1;
    std::vector<std::uint32_t> paletteProfile {};
    RasterImage raster {};
    std::vector<PageImage> images;
    std::vector<PageText> texts;
    std::vector<PageLatex> latexes;
    std::vector<Stroke> strokes;
};

class Document {
public:
    struct PdfSource {
        std::filesystem::path sourcePath {};
        std::shared_ptr<std::vector<std::uint8_t>> pdfData {};
    };

    struct PageDefaults {
        float width = 595.0f;
        float height = 842.0f;
        std::uint32_t backgroundColor = 0xFFFFFFFFU;
        std::vector<std::uint32_t> paletteProfile {};
        // False follows the application page-size preset for future pages.
        // Explicit project page settings pin width/height instead.
        bool sizeOverridden = false;
    };

    static Document CreateBlank();
    static Document CreateBlank(PageDefaults pageDefaults);
    static Document CreateEditable(
        std::filesystem::path filePath,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);
    static Document CreatePdfDocument(
        std::filesystem::path sourcePath,
        std::shared_ptr<std::vector<std::uint8_t>> pdfData,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);
    static Document CreateProjectDocument(
        std::filesystem::path filePath,
        std::vector<PdfSource> pdfSources,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);

    [[nodiscard]] const std::filesystem::path& FilePath() const noexcept;
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

    // Per-page content epoch, consumed by renderer-side caches (page surface
    // rasters, sidebar thumbnails).  Every committed mutation of a page's
    // contents must bump it via TouchPage; insert/remove/replace maintain the
    // per-page slots.  Epochs come from a process-wide counter, so a fresh
    // document can never collide with a dead page's cached signature.
    [[nodiscard]] std::uint64_t PageEpoch(std::size_t index) const noexcept;
    void TouchPage(std::size_t index) noexcept;
    bool SetCurrentPageIndex(std::size_t index);
    std::size_t InsertPage(std::size_t index, Page page);
    std::size_t RemovePage(std::size_t index);
    void SetFilePath(std::filesystem::path filePath);
    void SetNewPageDefaults(PageDefaults pageDefaults);
    void ResetToBlank();
    void ReplaceWithEditable(
        std::filesystem::path filePath,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);
    void ReplaceWithProject(
        std::filesystem::path filePath,
        std::vector<PdfSource> pdfSources,
        std::vector<Page> pages,
        std::size_t currentPageIndex,
        PageDefaults pageDefaults);

private:
    std::filesystem::path filePath_;
    std::vector<PdfSource> pdfSources_ {};
    std::vector<Page> pages_ { Page {} };
    // Parallel to pages_: each slot starts at a fresh process-wide epoch and
    // is bumped by TouchPage; epochs never serialize.
    std::vector<std::uint64_t> pageEpochs_ { NextPageEpoch() };
    static std::uint64_t NextPageEpoch() noexcept;
    std::size_t currentPageIndex_ = 0;
    PageDefaults newPageDefaults_ {};
};

}  // namespace cnote
