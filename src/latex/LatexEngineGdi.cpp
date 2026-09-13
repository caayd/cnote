#include "latex/LatexEngine.h"

#include <cstdint>

#include <objidl.h>
#include <gdiplus.h>

#include "platform/gdi_win/graphic_win32.h"

// Legacy Win32 drawing backend for the MicroTeX wrapper. Compiled only into the
// legacy frontend targets, which link the GDI-flavoured `microtex` target
// (BUILD_WIN32); the Qt migration uses LatexEngineQt.cpp and `microtex_qt`
// instead. Never link both backends into one target (plan step 1.5).

namespace cnote::latex {

void Draw(const FormulaHandle& formula, Gdiplus::Graphics& graphics, float x, float y, float scale, std::uint32_t argb) {
    if (formula == nullptr || scale <= 0.0f) {
        return;
    }
    const Gdiplus::GraphicsState state = graphics.Save();
    // The Graphics2D_win32 constructor resets the transform, so it must wrap
    // the graphics before we apply ours.
    tex::Graphics2D_win32 g2(&graphics);
    graphics.TranslateTransform(x, y);
    graphics.ScaleTransform(scale, scale);
    detail::DrawRender(formula, g2, argb);
    // TeXRender::draw() ends with g2.reset(); restore our caller's state.
    graphics.Restore(state);
}

void Draw(const FormulaHandle& formula, HDC dc, float x, float y, float scale, std::uint32_t argb) {
    if (dc == nullptr) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    Draw(formula, graphics, x, y, scale, argb);
}

void Draw(const PageLatex& latex, HDC dc, float x, float y, float scale) {
    Draw(Parse(latex), dc, x, y, scale, latex.color);
}

}  // namespace cnote::latex
