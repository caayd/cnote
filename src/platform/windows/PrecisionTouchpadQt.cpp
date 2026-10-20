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
// boundary through the window's device pixel ratio.
qreal WindowDevicePixelRatio(HWND window) {
    if (window == nullptr) {
        return 1.0;
    }
    QWindow* handle = QWindow::fromWinId(reinterpret_cast<WId>(window));
    return handle != nullptr ? handle->devicePixelRatio() : 1.0;
}

// Physical screen point -> Qt logical global point.  Mapping through the
// window's client area keeps the conversion correct on multi-monitor setups
// where a simple physical/dpr division would misplace secondary screens.
QPoint LogicalGlobalFromPhysicalScreen(HWND window, const QPoint& physical) {
    if (window == nullptr) {
        return physical;
    }
    POINT client = { physical.x(), physical.y() };
    if (ScreenToClient(window, &client) == FALSE) {
        return physical;
    }
    QWindow* handle = QWindow::fromWinId(reinterpret_cast<WId>(window));
    if (handle == nullptr) {
        return physical;
    }
    const qreal dpr = handle->devicePixelRatio();
    return handle->mapToGlobal(QPointF(client.x / dpr, client.y / dpr)).toPoint();
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

bool PrecisionTouchpadQt::Install(WId windowHandle) {
    Uninstall();

    window_ = reinterpret_cast<HWND>(windowHandle);
    touchpad_ = std::make_unique<PrecisionTouchpad>(
        &PrecisionTouchpadQt::GestureThunk,
        &PrecisionTouchpadQt::RouteThunk,
        this);

    available_ = touchpad_->Initialize(window_);
    router_.SetNativeAdapterAvailable(available_);
    if (!available_) {
        touchpad_.reset();
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
    const qreal dpr = WindowDevicePixelRatio(window_);
    sample.screenAnchor = LogicalGlobalFromPhysicalScreen(
        window_, QPoint(gesture.screenAnchor.x, gesture.screenAnchor.y));
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
    if (self == nullptr || self->window_ == nullptr) {
        return false;
    }
    POINT screenPoint = clientCursor;
    if (ClientToScreen(self->window_, &screenPoint) == FALSE) {
        return false;
    }
    // The adapter reports physical pixels; the router routes in Qt logical
    // global coordinates.
    const QPoint logical = LogicalGlobalFromPhysicalScreen(
        self->window_, QPoint(screenPoint.x, screenPoint.y));
    return self->router_.RoutesToCanvas(logical);
}

}  // namespace cnote::ui
