#pragma once

#include <QPoint>

#include <Qt>

namespace cnote::ui {

// Routing decision for a precision-input gesture.  `PassThrough` means the
// gesture does not belong to the canvas (the PDF sidebar or other chrome owns
// that region) and the platform integration must not consume it.
enum class PrecisionInputRoute {
    Canvas,
    PassThrough,
};

// Small seam between the platform precision-input integrations (Windows
// precision touchpad, Qt native gestures and touch) and the canvas
// (plan steps 5.3/5.4).  The integration classifies and normalises input in
// screen coordinates; the canvas maps it to its viewport and applies it, so the
// integrations never own viewport, model or widget state.
//
// The canvas must implement this by reusing the same operations the pointer
// router already drives (ui::InteractionHost, src/ui/InteractionRouter.h) rather
// than forking viewport logic:
//   RoutePrecisionInput  -> IsPointInCanvas(logical), the sidebar/canvas
//                           exclusion the router itself uses;
//   PanBy                -> PanBy(logicalDelta), the same call as middle-drag
//                           and wheel panning;
//   ZoomAtAnchor         -> the canvas zoom-at-anchor primitive behind
//                           ZoomByWheelSteps, applied with the gesture's
//                           cumulative scale;
//   SetPrecisionGestureCursor -> SetCursor(...).
// Pointer samples, wheel policy (ctrl-zoom steps, angle/pixel scrolling) and
// tool routing stay in ui::InteractionRouter; this slice only adds native
// precision gestures and the duplicate-suppression decision for events Qt
// synthesises on top of a consumed native touchpad frame.
class PrecisionInputSink {
public:
    virtual ~PrecisionInputSink() = default;

    // True when a gesture anchored at this screen position belongs to the
    // canvas; the canvas excludes its sidebar/chrome here.
    [[nodiscard]] virtual PrecisionInputRoute RoutePrecisionInput(const QPoint& globalPosition) const = 0;

    // Gesture lifecycle.  The sink captures its own start zoom on begin.
    virtual void BeginPrecisionGesture() = 0;
    // Cursor shape hint: true while a pan/zoom gesture owns the canvas.
    virtual void SetPrecisionGestureCursor(bool active) = 0;
    // Multiplicative scale relative to the gesture start, anchored at the
    // screen position (cursor-anchored zoom).
    virtual void ZoomAtAnchor(qreal cumulativeScale, const QPoint& globalAnchor) = 0;
    // Screen-space pan delta; applied directly, without extra smoothing, because
    // the OS already provides momentum.
    virtual void PanBy(qreal deltaX, qreal deltaY) = 0;
    virtual void EndPrecisionGesture() = 0;
};

}  // namespace cnote::ui
