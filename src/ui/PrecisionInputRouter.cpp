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
        suppressDuplicates_ = false;
        gestureActive_ = false;
        gestureAccepted_ = false;
        gestureIgnored_ = false;
        inertiaActive_ = false;
    }
}

void PrecisionInputRouter::NoteNativeFrameConsumed() {
    if (nativeAvailable_) {
        suppressDuplicates_ = true;
        lastConsumedFrame_.start();
    }
}

bool PrecisionInputRouter::SuppressionFresh() const {
    return suppressDuplicates_
        && lastConsumedFrame_.isValid()
        && lastConsumedFrame_.elapsed() <= kDuplicateSuppressionWindowMs;
}

bool PrecisionInputRouter::ShouldDropWheelDuplicate() const {
    return nativeAvailable_ && SuppressionFresh();
}

bool PrecisionInputRouter::NativeGestureIsDuplicate() const {
    return nativeAvailable_ && SuppressionFresh();
}

bool PrecisionInputRouter::AcceptGestureAt(const QPoint& globalPosition) {
    if (gestureAccepted_) {
        return true;
    }
    if (sink_.RoutePrecisionInput(globalPosition) != PrecisionInputRoute::Canvas) {
        gestureIgnored_ = true;
        return false;
    }
    gestureAccepted_ = true;
    gestureIgnored_ = false;
    gestureScale_ = 1.0;
    sink_.BeginPrecisionGesture();
    return true;
}

void PrecisionInputRouter::FinishGesture() {
    if (gestureAccepted_) {
        sink_.EndPrecisionGesture();
    }
    gestureActive_ = false;
    gestureIgnored_ = false;
    gestureAccepted_ = false;
    gestureScale_ = 1.0;
    // The native touchpad gesture is over; Qt's own events are in charge again
    // until the next consumed frame starts suppressing duplicates.
    suppressDuplicates_ = false;
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
        gestureActive_ = true;
        gestureAccepted_ = false;
        gestureIgnored_ = false;
        gestureScale_ = 1.0;
        if (sample.inertiaActive) {
            SetInertiaActive(true);
        }
        AcceptGestureAt(sample.screenAnchor);
        return;
    case TouchpadGestureSample::Phase::Update:
        if (!gestureActive_) {
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
        if (!AcceptGestureAt(sample.screenAnchor)) {
            return;
        }
        ApplyZoom(static_cast<qreal>(sample.cumulativeScale), sample.screenAnchor);
        ApplyPan(static_cast<qreal>(sample.translationX), static_cast<qreal>(sample.translationY));
        SetInertiaActive(sample.inertiaActive);
        return;
    case TouchpadGestureSample::Phase::End:
        SetInertiaActive(sample.inertiaActive);
        FinishGesture();
        return;
    case TouchpadGestureSample::Phase::Cancel:
        SetInertiaActive(false);
        FinishGesture();
        return;
    }
}

bool PrecisionInputRouter::HandleNativeGestureBegin(const QPoint& globalPosition) {
    if (NativeGestureIsDuplicate()) {
        return true;
    }
    if (gestureActive_) {
        return false;
    }
    gestureActive_ = true;
    gestureAccepted_ = false;
    gestureIgnored_ = false;
    gestureScale_ = 1.0;
    return AcceptGestureAt(globalPosition);
}

bool PrecisionInputRouter::HandleNativeGestureZoom(qreal value, const QPoint& globalPosition) {
    if (NativeGestureIsDuplicate()) {
        return true;
    }
    if (!gestureActive_ || !AcceptGestureAt(globalPosition)) {
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
    if (!gestureActive_ || !AcceptGestureAt(globalPosition)) {
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
        suppressDuplicates_ = false;
        return true;
    }
    if (!gestureActive_) {
        return false;
    }
    const bool accepted = gestureAccepted_;
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

bool PrecisionInputRouter::HandleTouchBegin(const QPoint& globalCentroid) {
    if (gestureActive_) {
        return false;
    }
    gestureActive_ = true;
    touchActive_ = true;
    lastTouchCentroid_ = QPointF(globalCentroid);
    gestureAccepted_ = false;
    gestureIgnored_ = false;
    gestureScale_ = 1.0;
    return AcceptGestureAt(globalCentroid);
}

bool PrecisionInputRouter::HandleTouchUpdate(const QPoint& globalCentroid, qreal cumulativeScale) {
    if (!gestureActive_ || !touchActive_) {
        return false;
    }
    if (!AcceptGestureAt(globalCentroid)) {
        return false;
    }
    const QPointF centroid(globalCentroid);
    const QPointF delta = centroid - lastTouchCentroid_;
    lastTouchCentroid_ = centroid;
    ApplyPan(delta.x(), delta.y());
    ApplyZoom(cumulativeScale, globalCentroid);
    return true;
}

bool PrecisionInputRouter::HandleTouchEnd() {
    if (!gestureActive_ || !touchActive_) {
        return false;
    }
    const bool accepted = gestureAccepted_;
    touchActive_ = false;
    FinishGesture();
    return accepted;
}

void PrecisionInputRouter::SetInertiaActive(bool active) {
    inertiaActive_ = active;
    if (!active && !gestureActive_) {
        // Inertia ended with no live gesture: Qt's own events are authoritative
        // again, so stale suppression must not outlive it.
        suppressDuplicates_ = false;
    }
}

bool PrecisionInputRouter::RoutesToCanvas(const QPoint& globalPosition) const {
    return sink_.RoutePrecisionInput(globalPosition) == PrecisionInputRoute::Canvas;
}

}  // namespace cnote::ui
