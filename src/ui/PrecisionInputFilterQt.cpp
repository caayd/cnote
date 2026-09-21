#include "ui/PrecisionInputFilterQt.h"

#include <QEvent>
#include <QList>
#include <QNativeGestureEvent>
#include <QTouchEvent>
#include <QWheelEvent>

namespace cnote::ui {

PrecisionInputFilterQt::PrecisionInputFilterQt(PrecisionInputSink& sink, QObject* parent)
    : QObject(parent) {
    ownedRouter_.emplace(sink);
    router_ = &*ownedRouter_;
}

PrecisionInputFilterQt::PrecisionInputFilterQt(PrecisionInputRouter& router, QObject* parent)
    : QObject(parent),
      router_(&router) {
}

bool PrecisionInputFilterQt::eventFilter(QObject*, QEvent* event) {
    switch (event->type()) {
    case QEvent::Wheel: {
        // Only the duplicate of an already-consumed native touchpad frame is
        // dropped; the widget's own wheel handling runs otherwise.
        if (router_->ShouldDropWheelDuplicate()) {
            event->accept();
            return true;
        }
        return false;
    }
    case QEvent::NativeGesture: {
        auto* gesture = static_cast<QNativeGestureEvent*>(event);
        const QPoint global = gesture->globalPosition().toPoint();
        bool handled = false;
        switch (gesture->gestureType()) {
        case Qt::BeginNativeGesture:
            handled = router_->HandleNativeGestureBegin(global);
            break;
        case Qt::ZoomNativeGesture:
            handled = router_->HandleNativeGestureZoom(gesture->value(), global);
            break;
        case Qt::PanNativeGesture:
            handled = router_->HandleNativeGesturePan(gesture->delta(), global);
            break;
        case Qt::EndNativeGesture:
            handled = router_->HandleNativeGestureEnd();
            break;
        default:
            // Rotate/SmartZoom and anything else stay with the widget.
            handled = false;
            break;
        }
        if (handled) {
            event->accept();
            return true;
        }
        return false;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (event->type() == QEvent::TouchEnd) {
            if (!router_->HandleTouchEnd()) {
                return false;
            }
            event->accept();
            return true;
        }

        const QList<QEventPoint>& points = touch->points();
        if (points.size() < 2) {
            return false;
        }
        QList<QPointF> positions;
        positions.reserve(points.size());
        for (const QEventPoint& point : points) {
            positions.push_back(point.globalPosition());
        }
        const PrecisionInputRouter::TouchMetrics metrics = PrecisionInputRouter::ComputeTouchMetrics(positions);
        bool handled = false;
        if (event->type() == QEvent::TouchBegin) {
            touchStartDistance_ = metrics.distance;
            handled = router_->HandleTouchBegin(metrics.centroid.toPoint());
        } else {
            const qreal scale = touchStartDistance_ > 0.0 ? metrics.distance / touchStartDistance_ : 1.0;
            handled = router_->HandleTouchUpdate(metrics.centroid.toPoint(), scale);
        }
        if (handled) {
            event->accept();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

}  // namespace cnote::ui
