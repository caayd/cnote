#include "platform/windows/PrecisionTouchpadQt.h"

#include <QCoreApplication>
#include <QPoint>
#include <QWindow>

namespace cnote::ui {

namespace {

// Reference front end used a 15 ms Win32 timer for native inertia frames.
constexpr int kInertiaTimerIntervalMs = 15;

// The native adapter reports physical screen pixels (POINTER_INFO
// ptPixelLocation and INTERACTION_CONTEXT_MEASUREMENT_UNITS_SCREEN), while
// the router and the canvas sink work in Qt device-independent pixels.  On a
// DPR != 1 display an unconverted anchor misses the canvas route check and
// unconverted translations pan DPR-times too far, so every point crosses the
// boundary through the installed window's device pixel ratio.
qreal WindowDevicePixelRatio(const QWindow* window) {
    return window != nullptr ? window->devicePixelRatio() : 1.0;
}

// Physical client pixel -> Qt logical global point.  Dividing in client space
// keeps the conversion correct on multi-monitor setups where a simple
// physical/dpr division of a screen point would misplace secondary screens.
QPoint LogicalGlobalFromClient(const QWindow* window, const QPoint& client, qreal dpr) {
    if (window == nullptr) {
        return client;
    }
    return window->mapToGlobal(QPointF(client.x() / dpr, client.y() / dpr)).toPoint();
}

// Physical screen point -> Qt logical global point.  ScreenToClient lets Win32
// resolve which monitor's pixels these are; the window then maps its own
// logical client space to global space.
QPoint LogicalGlobalFromPhysicalScreen(
    const QWindow* window, HWND hwnd, const QPoint& physical, qreal dpr) {
    if (window == nullptr || hwnd == nullptr) {
        return physical;
    }
    POINT client = { physical.x(), physical.y() };
    if (ScreenToClient(hwnd, &client) == FALSE) {
        return physical;
    }
    return LogicalGlobalFromClient(window, QPoint(client.x, client.y), dpr);
}

}  // namespace

PrecisionTouchpadQt::PrecisionTouchpadQt(PrecisionInputSink& sink, QObject* parent)
    : QObject(parent),
      router_(sink) {
    inertiaTimer_.setInterval(kInertiaTimerIntervalMs);
    // Elapsed-time driven Qt precise timer, not the legacy 1 ms Win32 timer.
    inertiaTimer_.setTimerType(Qt::PreciseTimer);
    connect(&inertiaTimer_, &QTimer::timeout, this, [this] {
        if (touchpad_ == nullptr || !touchpad_->ProcessInertia()) {
            inertiaTimer_.stop();
            router_.SetInertiaActive(false);
        }
    });
}

PrecisionTouchpadQt::~PrecisionTouchpadQt() {
    Uninstall();
}

bool PrecisionTouchpadQt::Install(QWindow* window) {
    Uninstall();

    qtWindow_ = window;
    window_ = window != nullptr ? reinterpret_cast<HWND>(window->winId()) : nullptr;
    touchpad_ = std::make_unique<PrecisionTouchpad>(
        &PrecisionTouchpadQt::GestureThunk,
        &PrecisionTouchpadQt::RouteThunk,
        this);

    available_ = touchpad_->Initialize(window_);
    router_.SetNativeAdapterAvailable(available_);
    if (!available_) {
        touchpad_.reset();
        qtWindow_ = nullptr;
        window_ = nullptr;
        emit availabilityChanged(false);
        return false;
    }

    QCoreApplication::instance()->installNativeEventFilter(this);
    installed_ = true;
    emit availabilityChanged(true);
    return true;
}

void PrecisionTouchpadQt::Uninstall() {
    inertiaTimer_.stop();
    if (installed_ && QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeNativeEventFilter(this);
        installed_ = false;
    }
    if (touchpad_ != nullptr) {
        touchpad_->Shutdown();
        touchpad_.reset();
    }
    qtWindow_ = nullptr;
    window_ = nullptr;
    if (available_) {
        available_ = false;
        router_.SetNativeAdapterAvailable(false);
        emit availabilityChanged(false);
    }
}

bool PrecisionTouchpadQt::HandleNativeMessage(unsigned int message, quintptr wParam) {
    if (!available_ || touchpad_ == nullptr) {
        // Availability fallback: nothing is consumed, Qt's own wheel/gesture
        // events stay in charge.
        return false;
    }

    switch (message) {
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
        break;
    default:
        // Never consume unrelated mouse/keyboard/native messages.
        return false;
    }

    if (!touchpad_->HandlePointerMessage(message, static_cast<WPARAM>(wParam))) {
        return false;
    }

    router_.NoteNativeFrameConsumed();
    emit gestureConsumed();
    return true;
}

bool PrecisionTouchpadQt::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType != QByteArrayLiteral("windows_generic_MSG")
        && eventType != QByteArrayLiteral("windows_dispatcher_MSG")) {
        return false;
    }

    auto* msg = static_cast<MSG*>(message);
    // Native filters are application-wide and run newest-first. Only the
    // addressed window may feed this frame to its interaction context; a
    // cursor hit-test alone also matches overlapping background canvases.
    if (msg == nullptr || window_ == nullptr || msg->hwnd != window_) {
        return false;
    }

    if (!HandleNativeMessage(msg->message, static_cast<quintptr>(msg->wParam))) {
        return false;
    }

    if (result != nullptr) {
        *result = 0;
    }
    return true;
}

void PrecisionTouchpadQt::HandleNativeGesture(const PrecisionTouchpadGesture& gesture) {
    TouchpadGestureSample sample;
    switch (gesture.phase) {
    case PrecisionTouchpadGesture::Phase::Begin:
        sample.phase = TouchpadGestureSample::Phase::Begin;
        break;
    case PrecisionTouchpadGesture::Phase::End:
        sample.phase = TouchpadGestureSample::Phase::End;
        break;
    case PrecisionTouchpadGesture::Phase::Cancel:
        sample.phase = TouchpadGestureSample::Phase::Cancel;
        break;
    case PrecisionTouchpadGesture::Phase::Update:
    default:
        sample.phase = TouchpadGestureSample::Phase::Update;
        break;
    }
    const qreal dpr = WindowDevicePixelRatio(qtWindow_);
    sample.screenAnchor = LogicalGlobalFromPhysicalScreen(
        qtWindow_, window_, QPoint(gesture.screenAnchor.x, gesture.screenAnchor.y), dpr);
    sample.cumulativeScale = gesture.cumulativeScale;
    sample.translationX = static_cast<float>(gesture.translationX / dpr);
    sample.translationY = static_cast<float>(gesture.translationY / dpr);
    sample.inertiaActive = gesture.inertiaActive;

    ApplyTouchpadSample(sample);
}

void PrecisionTouchpadQt::ApplyTouchpadSample(const TouchpadGestureSample& sample) {
    router_.HandleTouchpadSample(sample);
    SyncInertiaTimer();
}

void PrecisionTouchpadQt::SyncInertiaTimer() {
    if (router_.InertiaActive()) {
        if (!inertiaTimer_.isActive()) {
            inertiaTimer_.start();
        }
    } else if (inertiaTimer_.isActive()) {
        inertiaTimer_.stop();
    }
}

void PrecisionTouchpadQt::GestureThunk(void* clientData, const PrecisionTouchpadGesture& gesture) {
    if (clientData == nullptr) {
        return;
    }
    static_cast<PrecisionTouchpadQt*>(clientData)->HandleNativeGesture(gesture);
}

bool PrecisionTouchpadQt::RouteThunk(void* clientData, POINT clientCursor) {
    auto* self = static_cast<PrecisionTouchpadQt*>(clientData);
    if (self == nullptr || self->qtWindow_ == nullptr) {
        return false;
    }
    // The native adapter hands over a physical client-space point; the router
    // routes in Qt logical global coordinates.  Map it once, without the
    // ClientToScreen/ScreenToClient round trip or a per-probe window wrapper.
    const qreal dpr = WindowDevicePixelRatio(self->qtWindow_);
    const QPoint logical = LogicalGlobalFromClient(
        self->qtWindow_, QPoint(clientCursor.x, clientCursor.y), dpr);
    return self->router_.RoutesToCanvas(logical);
}

}  // namespace cnote::ui
