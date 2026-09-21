#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <QtGui/qwindowdefs.h>

#include <memory>

#include <windows.h>

#include "platform/windows/PrecisionTouchpad.h"
#include "ui/PrecisionInputRouter.h"
#include "ui/PrecisionInputSink.h"

namespace cnote::ui {

// Windows precision-touchpad integration for the Qt frontend (plan step 5.4).
//
// It reuses the native adapter (src/platform/windows/PrecisionTouchpad) as a
// QAbstractNativeEventFilter instead of reimplementing it:
//   * WM_POINTERDOWN/UPDATE/UP frames are fed to the adapter; when it consumes a
//     frame the filter returns true, so Qt does not synthesise a duplicate
//     wheel/gesture event for the same input.
//   * Gesture callbacks are normalised into TouchpadGestureSample and dispatched
//     through the shared PrecisionInputRouter to the canvas sink
//     (zoom-at-anchor / pan-by / cursor hint / route decision).
//   * Native inertia frames are driven by a Qt precise timer instead of the
//     legacy 15 ms Win32 timer.
//   * When the dynamically loaded touchpad APIs are unavailable Install()
//     returns false, the router reports the same, and every message passes
//     through to Qt's own wheel/gesture handling.
class PrecisionTouchpadQt final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit PrecisionTouchpadQt(PrecisionInputSink& sink, QObject* parent = nullptr);
    ~PrecisionTouchpadQt() override;

    // Installs the native filter and initialises the adapter for the top-level
    // window.  Returns false when the touchpad APIs are unavailable; the app
    // then relies on Qt wheel/native-gesture events (the portable filter).
    bool Install(WId windowHandle);
    void Uninstall();

    [[nodiscard]] bool Available() const { return available_; }
    [[nodiscard]] bool Installed() const { return installed_; }
    [[nodiscard]] bool InertiaTimerRunning() const { return inertiaTimer_.isActive(); }
    [[nodiscard]] PrecisionInputRouter& Router() { return router_; }

    // One native message, mirroring what nativeEventFilter does; returns true
    // when the message was consumed.  Public so the headless harness can prove
    // the pass-through and fallback contracts without touchpad hardware.
    bool HandleNativeMessage(unsigned int message, quintptr wParam);

    // One normalised touchpad gesture frame, exactly what the native adapter's
    // callback feeds; public so the harness can drive the inertia timer without
    // physical hardware.
    void ApplyTouchpadSample(const TouchpadGestureSample& sample);

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void availabilityChanged(bool available);
    void gestureConsumed();

private:
    void HandleNativeGesture(const PrecisionTouchpadGesture& gesture);
    void SyncInertiaTimer();

    static void GestureThunk(void* clientData, const PrecisionTouchpadGesture& gesture);
    static bool RouteThunk(void* clientData, POINT clientCursor);

    PrecisionInputRouter router_;
    std::unique_ptr<PrecisionTouchpad> touchpad_;
    QTimer inertiaTimer_;
    HWND window_ = nullptr;
    bool available_ = false;
    bool installed_ = false;
};

}  // namespace cnote::ui
