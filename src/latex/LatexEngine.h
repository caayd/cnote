#pragma once

// Wrapper around the vendored MicroTeX engine (third_party/microtex).
// The rest of the app never touches tex:: types directly.
//
// Threading: MicroTeX keeps process-global parser state, so every function
// here must be called on the UI thread.
//
// Error contract: Parse returns nullptr for structural errors (unbalanced
// braces etc.) and incomplete required arguments such as an empty fraction
// slot. Unknown commands do NOT fail — MicroTeX's partial mode renders them
// as red text in place, which doubles as the live-preview error indication.
//
// Backends: the parse/measure core is backend-independent (LatexEngine.cpp).
// Drawing is provided by two mutually exclusive platform translation units,
// selected at compile time so neither backend sees the other's toolkit:
//   * Qt (CNOTE_LATEX_QT_BACKEND):  LatexEngineQt.cpp  — QPainter
//   * legacy Win32 (default):       LatexEngineGdi.cpp — Gdiplus/HDC
// Never link both into the same target (plan step 1.5).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "Document.h"

namespace tex {
class Graphics2D;
}

#if defined(CNOTE_LATEX_QT_BACKEND)
class QPainter;
#else
#include <windows.h>
namespace Gdiplus {
class Graphics;
}
#endif

namespace cnote::latex {

// Measured extents of a parsed formula in page units (same coordinate space
// as PageText::x/y and fontSize).
struct FormulaMetrics {
    float width = 0.0f;             // total width
    float height = 0.0f;            // total height (above + below baseline)
    float depth = 0.0f;             // extent below the baseline
    float baselineFraction = 0.0f;  // baseline offset from the top / height
};

struct Formula;
using FormulaHandle = std::shared_ptr<Formula>;

// Must be called once after the platform graphics stack is up (GdiplusStartup
// in the legacy frontend, QApplication in the Qt frontend), before any
// Parse/Draw call. resRoot is the directory containing MicroTeX's resource
// tree (latex-res) and its `.clatexmath-res_root` marker.
bool Initialize(const std::filesystem::path& resRoot);
void Shutdown();
bool IsInitialized();

// Parses and lays out an equation. fontSize is in page units. display=true
// wraps the source as display math (\[ ... \]) unless it already is; display
// equations take their natural width. Parsed formulae are cached (LRU) keyed
// by source+fontSize+display; color is applied at draw time, not parse time.
[[nodiscard]] FormulaHandle Parse(std::u16string_view source, float fontSize, bool display);

[[nodiscard]] FormulaMetrics Measure(const FormulaHandle& formula);

// Convenience for model objects.
[[nodiscard]] FormulaHandle Parse(const PageLatex& latex);
[[nodiscard]] FormulaMetrics Measure(const PageLatex& latex);

namespace detail {

// Backend glue: paints the parsed formula into a graphics context whose
// coordinate system the backend adapter has already positioned and scaled.
// Defined in LatexEngine.cpp; safe to call with a null handle.
void DrawRender(const FormulaHandle& formula, tex::Graphics2D& graphics, std::uint32_t argb);

}  // namespace detail

#if defined(CNOTE_LATEX_QT_BACKEND)

// Draws with the formula's top-left at (x, y) in the painter's current
// coordinate space, scaled by `scale` (page units -> target units). Vector
// output at any scale. The painter state and the caller's transform are
// preserved. Safe to call with a null handle (no-op).
void Draw(const FormulaHandle& formula, QPainter& painter, float x, float y, float scale, std::uint32_t argb);
void Draw(const PageLatex& latex, QPainter& painter, float x, float y, float scale);

#else

void Draw(const FormulaHandle& formula, Gdiplus::Graphics& graphics, float x, float y, float scale, std::uint32_t argb);
void Draw(const FormulaHandle& formula, HDC dc, float x, float y, float scale, std::uint32_t argb);
void Draw(const PageLatex& latex, HDC dc, float x, float y, float scale);

#endif

}  // namespace cnote::latex
