#include "latex/LatexEngine.h"

#include <cstdint>

#include <QPainter>

#include "platform/qt/graphic_qt.h"

// Qt drawing backend for the MicroTeX wrapper: the only drawing backend (the
// legacy GDI one was deleted at cutover). Compiled into the targets that link
// `microtex_qt`.

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

}  // namespace cnote::latex
