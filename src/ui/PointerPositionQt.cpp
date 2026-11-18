#include "ui/PointerPositionQt.h"

#include <QCursor>

namespace cnote::ui {

namespace {

std::optional<QPoint>& SyntheticCursorPos() {
    static std::optional<QPoint> position;
    return position;
}

}  // namespace

QPoint GlobalCursorPos() {
    if (const std::optional<QPoint>& synthetic = SyntheticCursorPos(); synthetic.has_value()) {
        return *synthetic;
    }
    return QCursor::pos();
}

void SetSyntheticCursorPos(std::optional<QPoint> globalPosition) {
    SyntheticCursorPos() = globalPosition;
}

}  // namespace cnote::ui
