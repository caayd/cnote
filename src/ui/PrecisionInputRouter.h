#pragma once

#include <QList>
#include <QPoint>
#include <QPointF>

#include <cstddef>

#include "ui/PrecisionInputSink.h"

namespace cnote::ui {

// One precision-touchpad frame, mirroring PrecisionTouchpadGesture without the
// Windows headers so the decision core stays portable and testable.
struct TouchpadGestureSample {
    enum class Phase {
        Begin,
        Update,
        End,
        Cancel,
    };

    Phase phase = Phase::Update;
    QPoint screenAnchor {};       // screen coordinates (native gesture anchor)
    float cumulativeScale = 1.0f; // relative to the gesture start
    float translationX = 0.0f;    // screen-space frame delta
    float translationY = 0.0f;
    bool inertiaActive = false;   // the native context is producing inertia frames
};

// Portable decision core shared by the Windows precision-touchpad filter and the
// Qt native-gesture/touch path (plan steps 5.3/5.4).
//
// Responsibilities: availability fallback, canvas/sidebar routing, duplicate
// suppression for events Qt synthesises on top of a consumed native gesture,
// gesture lifecycle and inertia bookkeeping, and the reference thresholds for
// applying scale/translation.  Plain Qt wheel events are never re-implemented
// here: unless a duplicate must be dropped the event is left to the widget's own
// wheel path, which keeps the fallback behaviour identical by construction.
class PrecisionInputRouter {
public:
    // Legacy App::HandlePrecisionTouchpadGesture thresholds.
    static constexpr float kScaleEpsilon = 0.0005f;
    static constexpr float kTranslationEpsilon = 0.001f;

    explicit PrecisionInputRouter(PrecisionInputSink& sink);

    // --- availability ------------------------------------------------------
    // False when the Windows adapter could not load/initialize its dynamically
    // loaded APIs (or on platforms without one): every message then passes
    // through to Qt's own wheel/gesture path.
    void SetNativeAdapterAvailable(bool available);
    [[nodiscard]] bool NativeAdapterAvailable() const { return nativeAvailable_; }

    // --- duplicate suppression ---------------------------------------------
    // The native adapter consumed a pointer frame for the active gesture; every
    // event Qt synthesises from that same physical input must be dropped
    // instead of being applied a second time: the duplicate wheel
    // (ShouldDropWheelDuplicate) and the duplicate QNativeGestureEvent
    // pan/zoom/begin/end, which the HandleNativeGesture* entry points consume
    // without applying while this flag is set.  Suppression lasts until the
    // gesture ends or is cancelled.
    void NoteNativeFrameConsumed();
    [[nodiscard]] bool ShouldDropWheelDuplicate() const;
    [[nodiscard]] bool SuppressingDuplicates() const { return suppressDuplicates_; }

    // --- Windows precision-touchpad frames ---------------------------------
    void HandleTouchpadSample(const TouchpadGestureSample& sample);

    // --- Qt native gestures (QNativeGestureEvent) --------------------------
    [[nodiscard]] bool HandleNativeGestureBegin(const QPoint& globalPosition);
    [[nodiscard]] bool HandleNativeGestureZoom(qreal value, const QPoint& globalPosition);
    [[nodiscard]] bool HandleNativeGesturePan(const QPointF& delta, const QPoint& globalPosition);
    [[nodiscard]] bool HandleNativeGestureEnd();

    // --- Qt touch (two-finger pinch/pan where Qt delivers touch) -----------
    // `cumulativeScale` is relative to the first two-finger frame.
    [[nodiscard]] bool HandleTouchBegin(const QPoint& globalCentroid);
    [[nodiscard]] bool HandleTouchUpdate(const QPoint& globalCentroid, qreal cumulativeScale);
    [[nodiscard]] bool HandleTouchEnd();

    // --- state -------------------------------------------------------------
    // Route query without starting a gesture: the native adapter asks this
    // before it processes a pointer frame (canvas/sidebar exclusion).
    [[nodiscard]] bool RoutesToCanvas(const QPoint& globalPosition) const;

    void SetInertiaActive(bool active);
    [[nodiscard]] bool InertiaActive() const { return inertiaActive_; }
    [[nodiscard]] bool GestureActive() const { return gestureActive_; }
    [[nodiscard]] bool GestureIgnored() const { return gestureIgnored_; }
    [[nodiscard]] qreal CumulativeGestureScale() const { return gestureScale_; }

    // Pure two-finger metrics (centroid and finger distance) for the touch path.
    struct TouchMetrics {
        QPointF centroid {};
        qreal distance = 0.0;
    };
    [[nodiscard]] static TouchMetrics ComputeTouchMetrics(const QList<QPointF>& points);

private:
    // Returns true when the gesture is accepted by the canvas.  When the route
    // says otherwise the gesture stays ignored but tracked, so a later frame can
    // still be accepted exactly like the legacy WM_POINTER behaviour.
    bool AcceptGestureAt(const QPoint& globalPosition);
    // True while the adapter is consuming frames for the current gesture: a Qt
    // native gesture arriving now is that gesture's duplicate.
    [[nodiscard]] bool NativeGestureIsDuplicate() const { return nativeAvailable_ && suppressDuplicates_; }
    void FinishGesture();
    void ApplyZoom(qreal cumulativeScale, const QPoint& globalAnchor);
    void ApplyPan(qreal deltaX, qreal deltaY);

    PrecisionInputSink& sink_;
    bool nativeAvailable_ = false;
    bool suppressDuplicates_ = false;
    bool gestureActive_ = false;
    bool gestureIgnored_ = false;
    bool gestureAccepted_ = false;
    bool inertiaActive_ = false;
    qreal gestureScale_ = 1.0;
    QPointF lastTouchCentroid_ {};
    bool touchActive_ = false;
};

}  // namespace cnote::ui
