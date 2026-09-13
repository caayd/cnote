#include "latex/LatexEngine.h"

#include <cstdint>

#include <QPainter>

#include "platform/qt/graphic_qt.h"

// Qt drawing backend for the MicroTeX wrapper, compiled into the targets that
// link `microtex_qt` (BUILD_QT). The GDI backend was removed at cutover; the
// two platform backends must never be linked into one target (plan step 1.5).

namespace cnote::latex {

void Draw(const FormulaHandle& formula, QPainter& painter, float x, float y, float scale, std::uint32_t argb) {
    if (formula == nullptr || scale <= 0.0f) {
        return;
    }
    painter.save();
    // Graphics2D_qt captures the painter transform at construction, so build it
    // before applying the formula placement; its reset() then returns to the
    // caller's page/DPR transform instead of erasing it.
    tex::Graphics2D_qt g2(&painter);
    painter.translate(x, y);
    painter.scale(scale, scale);
    detail::DrawRender(formula, g2, argb);
    painter.restore();
}

void Draw(const PageLatex& latex, QPainter& painter, float x, float y, float scale) {
    Draw(Parse(latex), painter, x, y, scale, latex.color);
}

}  // namespace cnote::latex
