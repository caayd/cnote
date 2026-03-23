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

struct Page {
    float width = 612.0f;
    float height = 792.0f;
    std::vector<Stroke> strokes;
};

class Document {
public:
    static Document CreateBlank();
    static Document CreatePdfPlaceholder(std::wstring sourcePath);

    [[nodiscard]] bool IsPdfPlaceholder() const noexcept;
    [[nodiscard]] const std::wstring& SourcePath() const noexcept;
    [[nodiscard]] Page& CurrentPage() noexcept;
    [[nodiscard]] const Page& CurrentPage() const noexcept;

    void ResetToBlank();
    void ReplaceWithPlaceholder(std::wstring sourcePath);

private:
    std::wstring sourcePath_;
    bool pdfPlaceholder_ = false;
    Page page_ {};
};

}  // namespace ctrainer
