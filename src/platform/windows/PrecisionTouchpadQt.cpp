#include "platform/windows/PrecisionTouchpadQt.h"

#include <QCoreApplication>
#include <QPoint>

namespace cnote::ui {

namespace {

// Reference front end used a 15 ms Win32 timer for native inertia frames.
constexpr int kInertiaTimerIntervalMs = 15;

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
    if (msg == nullptr) {
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
    sample.screenAnchor = QPoint(gesture.screenAnchor.x, gesture.screenAnchor.y);
    sample.cumulativeScale = gesture.cumulativeScale;
    sample.translationX = gesture.translationX;
    sample.translationY = gesture.translationY;
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
    return self->router_.RoutesToCanvas(QPoint(screenPoint.x, screenPoint.y));
}

}  // namespace cnote::ui
