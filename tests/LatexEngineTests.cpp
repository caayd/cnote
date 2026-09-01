// LatexEngine wrapper contract tests: init, parse/measure, scaled drawing,
// error behavior, and cache reuse. Run via ctest (latex_engine_tests).

#include <cstdio>
#include <cstring>

#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>

#include "Document.h"
#include "latex/LatexEngine.h"

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}

// Counts pixels darker than mid-gray on a white background.
int CountInk(Gdiplus::Bitmap& bitmap) {
    const int width = static_cast<int>(bitmap.GetWidth());
    const int height = static_cast<int>(bitmap.GetHeight());
    Gdiplus::BitmapData data;
    const Gdiplus::Rect rect(0, 0, width, height);
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        return -1;
    }
    int ink = 0;
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const std::uint8_t*>(data.Scan0) + static_cast<std::size_t>(y) * static_cast<std::size_t>(data.Stride));
        for (int x = 0; x < width; ++x) {
            const std::uint32_t pixel = row[x];
            const int r = static_cast<int>(pixel & 0xFF);
            const int g = static_cast<int>((pixel >> 8) & 0xFF);
            const int b = static_cast<int>((pixel >> 16) & 0xFF);
            if (r + g + b < 384) {
                ++ink;
            }
        }
    }
    bitmap.UnlockBits(&data);
    return ink;
}

int RenderInk(cnote::latex::FormulaHandle formula, float scale, std::uint32_t argb) {
    const cnote::latex::FormulaMetrics metrics = cnote::latex::Measure(formula);
    const int width = static_cast<int>(metrics.width * scale) + 8;
    const int height = static_cast<int>(metrics.height * scale) + 8;
    Gdiplus::Bitmap bitmap(width, height, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.Clear(Gdiplus::Color(255, 255, 255, 255));
    cnote::latex::Draw(formula, graphics, 4.0f, 4.0f, scale, argb);
    return CountInk(bitmap);
}

}  // namespace

int main() {
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        std::printf("FATAL: GdiplusStartup failed\n");
        return 1;
    }

    // LATEX_RES_ROOT is defined by CMake to the vendored res directory.
    Check(cnote::latex::Initialize(LATEX_RES_ROOT), "engine initializes");

    // Parse + measure.
    auto formula = cnote::latex::Parse(L"e^{i\\pi} + 1 = \\frac{a}{b}", 32.0f, true);
    Check(formula != nullptr, "display formula parses");
    const cnote::latex::FormulaMetrics metrics = cnote::latex::Measure(formula);
    std::printf(
        "  metrics: width=%.1f height=%.1f depth=%.1f baseline=%.3f\n",
        static_cast<double>(metrics.width), static_cast<double>(metrics.height),
        static_cast<double>(metrics.depth), static_cast<double>(metrics.baselineFraction));
    Check(metrics.width > 100.0f && metrics.height > 30.0f, "metrics are plausible page units");
    Check(metrics.depth > 0.0f && metrics.depth < metrics.height, "depth sits inside total height");
    Check(metrics.baselineFraction > 0.0f && metrics.baselineFraction < 1.0f, "baseline fraction valid");

    // Cache: same source returns the same shared formula.
    auto again = cnote::latex::Parse(L"e^{i\\pi} + 1 = \\frac{a}{b}", 32.0f, true);
    Check(again == formula, "identical parse is served from cache");

    // Drawing at 1x and 3x paints ink, and 3x paints substantially more area.
    const int ink1 = RenderInk(formula, 1.0f, 0xFF000000U);
    const int ink3 = RenderInk(formula, 3.0f, 0xFF000000U);
    std::printf("  ink at 1x: %d, at 3x: %d\n", ink1, ink3);
    Check(ink1 > 300, "1x draw paints ink");
    Check(ink3 > ink1 * 4, "3x draw scales vector-crisp (not bitmap-stretched)");

    // Display-mode parsing is lenient by design: unbalanced braces and unknown
    // commands still produce a render (errors show as red text in place), so
    // live preview always has something to show. Null is reserved for input
    // that yields no formula at all.
    Check(cnote::latex::Parse(L"x } y", 24.0f, true) != nullptr, "unbalanced brace stays lenient in display mode");
    Check(cnote::latex::Parse(L"a + \\nosuchcmd + b", 24.0f, true) != nullptr, "unknown command stays lenient (renders red)");
    Check(cnote::latex::Parse(L"", 24.0f, true) == nullptr, "empty source returns null");

    // Model-level convenience path.
    cnote::PageLatex block;
    block.id = 7;
    block.x = 40.0f;
    block.y = 60.0f;
    block.fontSize = 28.0f;
    block.content = L"\\sum_{i=1}^{n} i = \\frac{n(n+1)}{2}";
    const cnote::latex::FormulaMetrics blockMetrics = cnote::latex::Measure(block);
    Check(blockMetrics.width > 100.0f, "PageLatex measure works");

    cnote::latex::Shutdown();
    Gdiplus::GdiplusShutdown(gdiplusToken);

    std::printf(failures == 0 ? "ALL CHECKS PASSED\n" : "%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
