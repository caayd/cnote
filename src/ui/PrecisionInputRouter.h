#pragma once

#include <QElapsedTimer>
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
    // One gesture at a time.  Idle is the only legal starting point; Tracking is
    // a gesture whose first frame was routed away from the canvas (still able to
    // be accepted by a later frame); Accepted is a gesture the canvas owns.
    enum class GesturePhase {
        Idle,
        Tracking,
        Accepted,
    };

    // Which input path owns the active gesture.  Independent sources keep a
    // native touchpad gesture from being finished by a Qt touch End (and vice
    // versa), and make stale cross-path bookkeeping impossible.
    enum class GestureSource {
        None,
        NativeTouchpad,
        NativeGesture,
        Touch,
    };

    // Legacy App::HandlePrecisionTouchpadGesture thresholds.
    static constexpr float kScaleEpsilon = 0.0005f;
    static constexpr float kTranslationEpsilon = 0.001f;

    // Pure two-finger metrics (centroid and finger distance) for the touch path.
    struct TouchMetrics {
        QPointF centroid {};
        qreal distance = 0.0;
    };

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
    // without applying while suppression is fresh.  Suppression is armed by a
    // consumed frame and expires kDuplicateSuppressionWindow after the last
    // consumed frame.  It only takes effect while a gesture is live, so a frame
    // consumed after the gesture ended (the trailing POINTERUP) can never leave
    // real wheel events dropped after the gesture.
    void NoteNativeFrameConsumed();
    [[nodiscard]] bool ShouldDropWheelDuplicate() const;
    [[nodiscard]] bool SuppressingDuplicates() const { return ShouldDropWheelDuplicate(); }
    // How long a consumed native frame keeps suppressing Qt duplicates.
    static constexpr double kDuplicateSuppressionWindowMs = 200.0;

    // --- Windows precision-touchpad frames ---------------------------------
    void HandleTouchpadSample(const TouchpadGestureSample& sample);

    // --- Qt native gestures (QNativeGestureEvent) --------------------------
    [[nodiscard]] bool HandleNativeGestureBegin(const QPoint& globalPosition);
    [[nodiscard]] bool HandleNativeGestureZoom(qreal value, const QPoint& globalPosition);
    [[nodiscard]] bool HandleNativeGesturePan(const QPointF& delta, const QPoint& globalPosition);
    [[nodiscard]] bool HandleNativeGestureEnd();

    // --- Qt touch (two-finger pinch/pan where Qt delivers touch) -----------
    // The router snapshots the start distance on begin and derives the
    // cumulative pinch scale from it, so the filter carries no touch state.
    [[nodiscard]] bool HandleTouchBegin(const TouchMetrics& metrics);
    [[nodiscard]] bool HandleTouchUpdate(const TouchMetrics& metrics);
    [[nodiscard]] bool HandleTouchEnd();
    [[nodiscard]] bool HandleTouchCancel();

    // --- state -------------------------------------------------------------
    // Route query without starting a gesture: the native adapter asks this
    // before it processes a pointer frame (canvas/sidebar exclusion).
    [[nodiscard]] bool RoutesToCanvas(const QPoint& globalPosition) const;

    void SetInertiaActive(bool active);
    [[nodiscard]] bool InertiaActive() const { return inertiaActive_; }
    [[nodiscard]] GesturePhase Phase() const { return phase_; }
    [[nodiscard]] GestureSource Source() const { return source_; }
    [[nodiscard]] bool GestureActive() const { return phase_ != GesturePhase::Idle; }
    [[nodiscard]] bool GestureIgnored() const { return phase_ == GesturePhase::Tracking; }
    [[nodiscard]] qreal CumulativeGestureScale() const { return gestureScale_; }

    [[nodiscard]] static TouchMetrics ComputeTouchMetrics(const QList<QPointF>& points);

private:
    struct Suppression {
        bool armed = false;
        QElapsedTimer since;
    };

    // Route ownership for one coasting run.  Inertia frames have no pointer
    // owner that can move to another window, so the first frame's decision
    // holds until the run ends instead of hit-testing every frame.
    enum class InertiaRoute {
        Unknown,
        Canvas,
        PassThrough,
    };

    // Starts a gesture from Idle for the given source.  Later Begin attempts
    // while any gesture is live are rejected so no source can steal another's
    // active gesture.
    bool BeginGesture(GestureSource source, const QPoint& globalPosition);
    // Returns true when the gesture is accepted by the canvas.  When the route
    // says otherwise the gesture stays Tracking, so a later frame can still be
    // accepted exactly like the legacy WM_POINTER behaviour.
    bool AcceptGestureAt(const QPoint& globalPosition);
    // True while a consumed native frame is still fresh.
    [[nodiscard]] bool SuppressionFresh() const;
    // Qt's copy of the gesture the native adapter owns.  Begin additionally
    // requires a live gesture (below), so a genuine begin arriving with no
    // gesture active is never swallowed.
    [[nodiscard]] bool NativeGestureIsDuplicate() const;
    // True when a duplicate event must be swallowed while a gesture is live:
    // fresh native frames AND a live gesture.  Without the live gesture the
    // trailing POINTERUP would suppress real wheel input after the gesture
    // ended; the same predicate keeps a genuine begin from being consumed.
    [[nodiscard]] bool LiveGestureDuplicate() const;
    // Clears phase, source, inertia, scale, touch bookkeeping and suppression
    // as one reset, ending the sink gesture when the canvas owned it.
    void FinishGesture();
    void ApplyZoom(qreal cumulativeScale, const QPoint& globalAnchor);
    void ApplyPan(qreal deltaX, qreal deltaY);
    // Route query for the idle inertia frames, answered once per run.
    [[nodiscard]] bool InertiaRoutesToCanvas(const QPoint& globalPosition);

    PrecisionInputSink& sink_;
    bool nativeAvailable_ = false;
    Suppression suppression_ {};
    InertiaRoute inertiaRoute_ = InertiaRoute::Unknown;
    GesturePhase phase_ = GesturePhase::Idle;
    GestureSource source_ = GestureSource::None;
    bool inertiaActive_ = false;
    qreal gestureScale_ = 1.0;
    QPointF lastTouchCentroid_ {};
    qreal touchStartDistance_ = 0.0;
};

}  // namespace cnote::ui
