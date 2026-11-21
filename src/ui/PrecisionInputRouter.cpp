#include "ui/PrecisionInputRouter.h"

#include <cmath>

namespace cnote::ui {

PrecisionInputRouter::PrecisionInputRouter(PrecisionInputSink& sink) : sink_(sink) {}

void PrecisionInputRouter::SetNativeAdapterAvailable(bool available) {
    if (nativeAvailable_ == available) {
        return;
    }
    nativeAvailable_ = available;
    if (!available) {
        // Without the native adapter there are no native frames to duplicate,
        // and any in-flight native gesture state is meaningless.
        FinishGesture();
    }
}

void PrecisionInputRouter::NoteNativeFrameConsumed() {
    if (nativeAvailable_) {
        suppression_.armed = true;
        suppression_.since.start();
    }
}

bool PrecisionInputRouter::SuppressionFresh() const {
    return suppression_.armed
        && suppression_.since.isValid()
        && suppression_.since.elapsed() <= kDuplicateSuppressionWindowMs;
}

bool PrecisionInputRouter::NativeGestureIsDuplicate() const {
    return nativeAvailable_ && SuppressionFresh();
}

bool PrecisionInputRouter::LiveGestureDuplicate() const {
    return nativeAvailable_ && phase_ != GesturePhase::Idle && SuppressionFresh();
}

bool PrecisionInputRouter::ShouldDropWheelDuplicate() const {
    return LiveGestureDuplicate();
}

bool PrecisionInputRouter::BeginGesture(GestureSource source, const QPoint& globalPosition) {
    if (phase_ != GesturePhase::Idle) {
        // A begin while any gesture is live, whatever its source, is not a new
        // gesture: the native touchpad must not steal Qt's touch gesture and a
        // repeated native Begin must not emit a second BeginPrecisionGesture.
        return false;
    }
    source_ = source;
    phase_ = GesturePhase::Tracking;
    gestureScale_ = 1.0;
    lastTouchCentroid_ = QPointF();
    touchStartDistance_ = 0.0;
    // A new gesture never inherits a trailing frame's suppression.
    suppression_ = Suppression {};
    return AcceptGestureAt(globalPosition);
}

bool PrecisionInputRouter::AcceptGestureAt(const QPoint& globalPosition) {
    if (phase_ == GesturePhase::Accepted) {
        return true;
    }
    if (phase_ == GesturePhase::Idle) {
        return false;
    }
    if (sink_.RoutePrecisionInput(globalPosition) != PrecisionInputRoute::Canvas) {
        return false;
    }
    phase_ = GesturePhase::Accepted;
    gestureScale_ = 1.0;
    sink_.BeginPrecisionGesture();
    return true;
}

void PrecisionInputRouter::FinishGesture() {
    if (phase_ == GesturePhase::Accepted) {
        sink_.EndPrecisionGesture();
    }
    phase_ = GesturePhase::Idle;
    source_ = GestureSource::None;
    inertiaActive_ = false;
    gestureScale_ = 1.0;
    lastTouchCentroid_ = QPointF();
    touchStartDistance_ = 0.0;
    suppression_ = Suppression {};
}

void PrecisionInputRouter::ApplyZoom(qreal cumulativeScale, const QPoint& globalAnchor) {
    if (std::abs(cumulativeScale - 1.0) > static_cast<qreal>(kScaleEpsilon)) {
        gestureScale_ = cumulativeScale;
        sink_.ZoomAtAnchor(cumulativeScale, globalAnchor);
    }
}

void PrecisionInputRouter::ApplyPan(qreal deltaX, qreal deltaY) {
    if (std::abs(deltaX) > static_cast<qreal>(kTranslationEpsilon)
        || std::abs(deltaY) > static_cast<qreal>(kTranslationEpsilon)) {
        sink_.PanBy(deltaX, deltaY);
    }
}

void PrecisionInputRouter::HandleTouchpadSample(const TouchpadGestureSample& sample) {
    if (!nativeAvailable_) {
        // Availability fallback: the Qt wheel/gesture path owns the input.
        return;
    }
    switch (sample.phase) {
    case TouchpadGestureSample::Phase::Begin:
        if (!BeginGesture(GestureSource::NativeTouchpad, sample.screenAnchor)) {
            return;
        }
        if (sample.inertiaActive) {
            SetInertiaActive(true);
        }
        return;
    case TouchpadGestureSample::Phase::Update:
        if (phase_ == GesturePhase::Idle) {
            // Inertia frames keep arriving after the manipulation ended; they
            // pan the canvas exactly like the reference front end.  Without an
            // active manipulation (and without inertia) nothing is applied.
            if (!sample.inertiaActive || !RoutesToCanvas(sample.screenAnchor)) {
                return;
            }
            SetInertiaActive(true);
            ApplyPan(static_cast<qreal>(sample.translationX), static_cast<qreal>(sample.translationY));
            return;
        }
        if (source_ != GestureSource::NativeTouchpad) {
            return;
        }
        if (!AcceptGestureAt(sample.screenAnchor)) {
            return;
        }
        ApplyZoom(static_cast<qreal>(sample.cumulativeScale), sample.screenAnchor);
        ApplyPan(static_cast<qreal>(sample.translationX), static_cast<qreal>(sample.translationY));
        SetInertiaActive(sample.inertiaActive);
        return;
    case TouchpadGestureSample::Phase::End: {
        if (phase_ != GesturePhase::Idle && source_ != GestureSource::NativeTouchpad) {
            // A native touchpad End must not finish a Qt gesture.
            return;
        }
        const bool inertia = sample.inertiaActive;
        FinishGesture();
        // Inertia deliberately outlives the manipulation: FinishGesture resets
        // the gesture bookkeeping, then the native frame's inertia state is
        // re-applied for the coasting phase.
        SetInertiaActive(inertia);
        return;
    }
    case TouchpadGestureSample::Phase::Cancel:
        if (phase_ != GesturePhase::Idle && source_ != GestureSource::NativeTouchpad) {
            return;
        }
        SetInertiaActive(false);
        FinishGesture();
        return;
    }
}

bool PrecisionInputRouter::HandleNativeGestureBegin(const QPoint& globalPosition) {
    if (LiveGestureDuplicate()) {
        // A begin that is a duplicate of the adapter's live gesture.
        return true;
    }
    if (!BeginGesture(GestureSource::NativeGesture, globalPosition)) {
        return false;
    }
    return true;
}

bool PrecisionInputRouter::HandleNativeGestureZoom(qreal value, const QPoint& globalPosition) {
    if (NativeGestureIsDuplicate()) {
        return true;
    }
    if (phase_ == GesturePhase::Idle || source_ != GestureSource::NativeGesture) {
        return false;
    }
    if (!AcceptGestureAt(globalPosition)) {
        return false;
    }
    const qreal cumulative = gestureScale_ * (1.0 + value);
    ApplyZoom(cumulative, globalPosition);
    return true;
}

bool PrecisionInputRouter::HandleNativeGesturePan(const QPointF& delta, const QPoint& globalPosition) {
    if (NativeGestureIsDuplicate()) {
        return true;
    }
    if (phase_ == GesturePhase::Idle || source_ != GestureSource::NativeGesture) {
        return false;
    }
    if (!AcceptGestureAt(globalPosition)) {
        return false;
    }
    ApplyPan(delta.x(), delta.y());
    return true;
}

bool PrecisionInputRouter::HandleNativeGestureEnd() {
    if (NativeGestureIsDuplicate()) {
        // Qt's copy of a gesture the native adapter already owns: consume it so
        // nothing applies a second pan/zoom, and stop suppressing.  The native
        // side finishes its own bookkeeping from the adapter's end callback.
        suppression_ = Suppression {};
        return true;
    }
    if (phase_ == GesturePhase::Idle || source_ != GestureSource::NativeGesture) {
        return false;
    }
    const bool accepted = phase_ == GesturePhase::Accepted;
    FinishGesture();
    return accepted;
}

PrecisionInputRouter::TouchMetrics PrecisionInputRouter::ComputeTouchMetrics(const QList<QPointF>& points) {
    TouchMetrics metrics;
    if (points.isEmpty()) {
        return metrics;
    }

    QPointF sum;
    for (const QPointF& point : points) {
        sum += point;
    }
    metrics.centroid = sum / static_cast<qreal>(points.size());
    if (points.size() < 2) {
        return metrics;
    }

    // Two-finger distance drives the pinch scale; extra fingers do not change
    // the reference behaviour.
    metrics.distance = std::hypot(points[0].x() - points[1].x(), points[0].y() - points[1].y());
    return metrics;
}

bool PrecisionInputRouter::HandleTouchBegin(const TouchMetrics& metrics) {
    if (phase_ != GesturePhase::Idle) {
        return false;
    }
    BeginGesture(GestureSource::Touch, metrics.centroid.toPoint());
    lastTouchCentroid_ = metrics.centroid;
    touchStartDistance_ = metrics.distance;
    return phase_ == GesturePhase::Accepted;
}

bool PrecisionInputRouter::HandleTouchUpdate(const TouchMetrics& metrics) {
    if (phase_ == GesturePhase::Idle || source_ != GestureSource::Touch) {
        return false;
    }
    const QPoint globalCentroid = metrics.centroid.toPoint();
    if (!AcceptGestureAt(globalCentroid)) {
        return false;
    }
    const QPointF delta = metrics.centroid - lastTouchCentroid_;
    lastTouchCentroid_ = metrics.centroid;
    ApplyPan(delta.x(), delta.y());
    const qreal scale = touchStartDistance_ > 0.0 ? metrics.distance / touchStartDistance_ : 1.0;
    ApplyZoom(scale, globalCentroid);
    return true;
}

bool PrecisionInputRouter::HandleTouchEnd() {
    if (phase_ == GesturePhase::Idle || source_ != GestureSource::Touch) {
        return false;
    }
    const bool accepted = phase_ == GesturePhase::Accepted;
    FinishGesture();
    return accepted;
}

bool PrecisionInputRouter::HandleTouchCancel() {
    if (phase_ == GesturePhase::Idle || source_ != GestureSource::Touch) {
        return false;
    }
    // Cancel ends the touch gesture and leaves the router idle: no source or
    // touch metric may survive into the next TouchBegin.
    FinishGesture();
    return true;
}

void PrecisionInputRouter::SetInertiaActive(bool active) {
    inertiaActive_ = active;
    if (!active) {
        // Inertia ended: Qt's own events are authoritative again, so stale
        // suppression must not outlive it (also when it coasted past the
        // manipulation's End).
        suppression_ = Suppression {};
    }
}

bool PrecisionInputRouter::RoutesToCanvas(const QPoint& globalPosition) const {
    return sink_.RoutePrecisionInput(globalPosition) == PrecisionInputRoute::Canvas;
}

}  // namespace cnote::ui
