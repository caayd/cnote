// Windows-only precision-touchpad integration test (migration plan step 5.4).
//
// Proves the contracts that do not need touchpad hardware: availability
// fallback, unrelated/non-touchpad messages are never consumed, the native
// filter lifecycle tears down cleanly, and (when the dynamically loaded
// interaction-context APIs are usable on this machine) gesture frames reach the
// sink and drive the Qt inertia timer.  No physical touchpad was exercised;
// real device input, inertia feel and duplicate suppression against Qt's
// synthesised events remain physically unverified.

#include <QApplication>
#include <QPoint>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "platform/windows/PrecisionTouchpadQt.h"
#include "ui/PrecisionInputSink.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

struct FakeSink final : cnote::ui::PrecisionInputSink {
    cnote::ui::PrecisionInputRoute route = cnote::ui::PrecisionInputRoute::Canvas;
    int beginCount = 0;
    int endCount = 0;
    int cursorOnCount = 0;
    int cursorOffCount = 0;
    int zoomCount = 0;
    int panCount = 0;
    qreal lastZoomScale = 0.0;
    QPoint lastZoomAnchor {};
    qreal lastPanX = 0.0;
    qreal lastPanY = 0.0;

    [[nodiscard]] cnote::ui::PrecisionInputRoute RoutePrecisionInput(const QPoint&) const override {
        return route;
    }
    void BeginPrecisionGesture() override { ++beginCount; }
    void SetPrecisionGestureCursor(bool active) override {
        if (active) {
            ++cursorOnCount;
        } else {
            ++cursorOffCount;
        }
    }
    void ZoomAtAnchor(qreal cumulativeScale, const QPoint& globalAnchor) override {
        ++zoomCount;
        lastZoomScale = cumulativeScale;
        lastZoomAnchor = globalAnchor;
    }
    void PanBy(qreal deltaX, qreal deltaY) override {
        ++panCount;
        lastPanX = deltaX;
        lastPanY = deltaY;
    }
    void EndPrecisionGesture() override { ++endCount; }
};

cnote::ui::TouchpadGestureSample MakeSample(
    cnote::ui::TouchpadGestureSample::Phase phase,
    const QPoint& anchor = QPoint(300, 200)) {
    cnote::ui::TouchpadGestureSample sample;
    sample.phase = phase;
    sample.screenAnchor = anchor;
    return sample;
}

// 1. No window / no adapter: install fails and every message passes through.
void UnavailableFallback() {
    FakeSink sink;
    cnote::ui::PrecisionTouchpadQt touchpad(sink);

    Check(!touchpad.Install(0), "installing without a window reports unavailable");
    Check(!touchpad.Available(), "adapter reports unavailable");
    Check(!touchpad.Installed(), "no native filter is installed when unavailable");
    Check(!touchpad.Router().NativeAdapterAvailable(), "router follows the adapter availability");
    Check(!touchpad.InertiaTimerRunning(), "no inertia timer without the adapter");

    Check(!touchpad.HandleNativeMessage(WM_POINTERDOWN, 0), "no pointer message is consumed without the adapter");
    Check(!touchpad.HandleNativeMessage(WM_POINTERUPDATE, 0), "no pointer update is consumed without the adapter");
    Check(!touchpad.HandleNativeMessage(WM_MOUSEWHEEL, 0), "wheel messages are never consumed");
    Check(!touchpad.HandleNativeMessage(WM_KEYDOWN, 0), "key messages are never consumed");
    Check(!touchpad.Router().ShouldDropWheelDuplicate(), "the fallback path suppresses no Qt wheel events");

    touchpad.ApplyTouchpadSample(MakeSample(cnote::ui::TouchpadGestureSample::Phase::Begin));
    Check(sink.beginCount == 0 && !touchpad.Router().GestureActive(),
        "gesture frames are ignored without the adapter (Qt events stay in charge)");
    touchpad.Uninstall();
}

// 2. Lifecycle and pass-through against a real (offscreen) window.
void InstallLifecycle() {
    QWidget window;
    window.resize(800, 600);
    window.show();
    QApplication::processEvents();

    FakeSink sink;
    cnote::ui::PrecisionTouchpadQt touchpad(sink);
    int availabilitySignals = 0;
    QObject::connect(&touchpad, &cnote::ui::PrecisionTouchpadQt::availabilityChanged, [&](bool) { ++availabilitySignals; });

    const bool available = touchpad.Install(window.winId());
    std::printf("precision touchpad adapter available: %s\n", available ? "yes" : "no");
    Check(touchpad.Available() == available, "Available() matches Install()");
    Check(touchpad.Installed() == available, "Installed() matches Install()");
    Check(touchpad.Router().NativeAdapterAvailable() == available, "router availability matches Install()");
    Check(availabilitySignals >= 1, "Install reports availability once");

    Check(!touchpad.HandleNativeMessage(WM_MOUSEWHEEL, 0), "mouse wheel is not consumed");
    Check(!touchpad.HandleNativeMessage(WM_POINTERDOWN, 0), "a non-touchpad pointer id is not consumed");
    Check(!touchpad.HandleNativeMessage(WM_POINTERUPDATE, 0), "a non-touchpad pointer update is not consumed");

    // nativeEventFilter only claims Qt's Windows message streams.
    Check(!touchpad.nativeEventFilter(QByteArrayLiteral("xcb_generic_event_t"), nullptr, nullptr),
        "foreign event streams pass through");
    MSG unrelated {};
    unrelated.message = WM_MOUSEMOVE;
    qintptr result = -1;
    Check(!touchpad.nativeEventFilter(QByteArrayLiteral("windows_generic_MSG"), &unrelated, &result),
        "unrelated Windows messages pass through and keep the default result");
    Check(result == -1, "the pass-through path does not overwrite the result");

    if (available) {
        cnote::ui::PrecisionInputRouter& router = touchpad.Router();
        router.SetNativeAdapterAvailable(true);

        touchpad.ApplyTouchpadSample(MakeSample(cnote::ui::TouchpadGestureSample::Phase::Begin));
        Check(sink.beginCount == 1 && sink.cursorOnCount == 1, "touchpad begin reaches the sink");

        cnote::ui::TouchpadGestureSample update = MakeSample(cnote::ui::TouchpadGestureSample::Phase::Update, QPoint(420, 260));
        update.cumulativeScale = 1.5f;
        update.translationX = 6.0f;
        update.translationY = -4.0f;
        update.inertiaActive = true;
        touchpad.ApplyTouchpadSample(update);
        Check(sink.zoomCount == 1 && std::abs(sink.lastZoomScale - 1.5) < 1e-6, "touchpad pinch reaches the sink");
        Check(sink.lastZoomAnchor == QPoint(420, 260), "touchpad pinch stays anchored at the native anchor");
        Check(sink.panCount == 1 && sink.lastPanX == 6.0 && sink.lastPanY == -4.0, "touchpad pan delta reaches the sink");
        Check(touchpad.InertiaTimerRunning(), "native inertia starts the Qt precise timer");

        cnote::ui::TouchpadGestureSample end = MakeSample(cnote::ui::TouchpadGestureSample::Phase::End, QPoint(420, 260));
        end.inertiaActive = false;
        touchpad.ApplyTouchpadSample(end);
        Check(sink.endCount == 1 && sink.cursorOffCount == 1, "touchpad end reaches the sink");
        Check(!touchpad.InertiaTimerRunning(), "inertia stop stops the timer");

        // Sidebar exclusion uses the same route seam as the portable path.
        sink.route = cnote::ui::PrecisionInputRoute::PassThrough;
        touchpad.ApplyTouchpadSample(MakeSample(cnote::ui::TouchpadGestureSample::Phase::Begin));
        Check(sink.beginCount == 1, "gestures over the sidebar do not reach the canvas");
        sink.route = cnote::ui::PrecisionInputRoute::Canvas;

        touchpad.Uninstall();
        Check(!touchpad.Available() && !touchpad.Installed(), "uninstall tears the adapter down");
        Check(!touchpad.InertiaTimerRunning(), "uninstall stops the inertia timer");
    } else {
        std::printf("note: the dynamically loaded touchpad APIs are unavailable for this window; only the fallback contract is proven here\n");
        touchpad.ApplyTouchpadSample(MakeSample(cnote::ui::TouchpadGestureSample::Phase::Begin));
        Check(sink.beginCount == 0, "unavailable adapter ignores gesture frames");
        touchpad.Uninstall();
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    UnavailableFallback();
    InstallLifecycle();

    std::printf(g_failures == 0 ? "ALL PRECISION TOUCHPAD CHECKS PASSED\n" : "%d PRECISION TOUCHPAD CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
