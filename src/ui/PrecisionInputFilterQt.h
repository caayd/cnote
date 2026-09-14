#pragma once

#include <QObject>
#include <QPointF>

#include <optional>

#include "ui/PrecisionInputRouter.h"
#include "ui/PrecisionInputSink.h"

namespace cnote::ui {

// Portable Qt-side precision-input filter (plan steps 5.3/5.4).
//
// Install it on the canvas widget (`canvas->installEventFilter(filter)`) with
// the canvas as the sink:
//   * QEvent::Wheel  - drops only the duplicate wheel event Qt synthesises on
//                      top of a consumed native touchpad gesture; every other
//                      wheel event is left to the widget's own wheel path, so
//                      ctrl-wheel zoom and ordinary panning keep their existing
//                      behaviour unchanged.
//   * QEvent::NativeGesture - native pinch/pan (macOS trackpads and other
//                      platforms that deliver them): zoom is cursor-anchored
//                      exactly like ctrl-wheel, pan deltas are applied directly
//                      (the OS momentum is not smoothed a second time).
//   * QEvent::TouchBegin/Update/End - two-finger pinch/pan where Qt delivers
//                      touch events instead of native gestures.
//
// The filter must share the Windows adapter's router when both run in one
// process: the adapter is the only side that knows a pointer frame was
// consumed, and the filter is the only side that can drop the wheel event Qt
// synthesises from it.  With two independent routers the adapter pans the
// canvas and the filter would pass Qt's duplicate through, so the same
// physical gesture moves the view twice.  Construct the filter with
// `PrecisionTouchpadQt::Router()` to wire the suppression state together.
class PrecisionInputFilterQt : public QObject {
public:
    explicit PrecisionInputFilterQt(PrecisionInputSink& sink, QObject* parent = nullptr);
    // Shares an existing router (e.g. the Windows precision-touchpad adapter's).
    explicit PrecisionInputFilterQt(PrecisionInputRouter& router, QObject* parent = nullptr);

    [[nodiscard]] PrecisionInputRouter& router() { return *router_; }
    [[nodiscard]] const PrecisionInputRouter& router() const { return *router_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Set only by the owning constructor; the router holds a sink reference and
    // is not assignable, so it is constructed in place.
    std::optional<PrecisionInputRouter> ownedRouter_ {};
    PrecisionInputRouter* router_ = nullptr;
    qreal touchStartDistance_ = 0.0;
};

}  // namespace cnote::ui
