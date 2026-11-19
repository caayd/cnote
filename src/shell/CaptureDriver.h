#pragma once

// Headless capture driver: lets a script (or an agent) run the real app,
// drive it with input and save what it would show, without a window ever
// appearing on screen and without touching the user's cursor or focus.
//
// Windows stay on the native platform - real fonts, DPI and native window
// handles - but every top-level widget is marked Qt::WA_DontShowOnScreen
// before it is shown, so the native window exists and never becomes visible.
// Input enters through QWindowSystemInterface, the same path platform events
// take (shortcut map, popups, focus), with a synthetic pointer position
// (ui/PointerPositionQt.h) instead of the OS cursor.
//
// Script format (one step per line, '#' starts a comment).  Coordinates are
// logical pixels in the main window's client area, the same space a capture
// at scale 1 has.
//
//   size W H                         resize the window
//   wait MS                          let timers/animations run
//   move X Y                         move the pointer (hover)
//   click X Y [BUTTON] [MODS]        press and release
//   doubleclick X Y [BUTTON] [MODS]
//   drag X1 Y1 X2 Y2 [STEPS] [BUTTON] [MODS]   press, STEPS moves (16), release
//   wheel X Y NOTCHES [MODS]         vertical wheel; positive scrolls up
//   key SEQUENCE                     QKeySequence text: Ctrl+W, Escape, "Ctrl+K, Ctrl+C"
//   type TEXT                        types TEXT (rest of the line) as characters
//   capture PATH                     saves the window plus its open popups/dialogs as PNG
//
// BUTTON is left (default), right or middle; MODS is ctrl, shift, alt or meta
// joined with '+', e.g. ctrl+shift.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <QEvent>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QSize>
#include <QString>

class QWidget;
class QWindow;

namespace cnote::shell {

enum class CaptureStepKind {
    Size,
    Wait,
    Move,
    Click,
    DoubleClick,
    Drag,
    Wheel,
    Key,
    Type,
    Capture,
};

struct CaptureStep {
    CaptureStepKind kind = CaptureStepKind::Wait;
    int line = 0;
    QPointF from {};
    QPointF to {};
    QSize size {};
    int milliseconds = 0;
    int moves = 16;
    int notches = 0;
    Qt::MouseButton button = Qt::LeftButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QString text {};
    std::filesystem::path path {};
};

// Parses a script.  On failure returns false and sets *error to "line N: ...".
[[nodiscard]] bool ParseCaptureScript(std::string_view text, std::vector<CaptureStep>* steps, std::string* error);

// Application event filter that keeps every top-level widget off screen: it
// sets Qt::WA_DontShowOnScreen on the Show event, which Qt sends before it
// shows the native window.  Shown dialogs/windows are activated inside Qt (the
// platform never is), and hiding the active one returns activation to the
// primary window, the way a window manager would.
class OffscreenWindows : public QObject {
public:
    explicit OffscreenWindows(QObject* parent = nullptr);
    void SetPrimaryWindow(QWidget* window);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Activate(QWidget* window);
    void RegisterPopup(QWidget* popup);

    QPointer<QWidget> primary_;
};

// Fixes every screen's device pixel ratio, whatever the monitor's own scaling,
// so a capture has a known pixel size.  Call before the first window exists.
void SetScreenScale(qreal factor);

// The window as the user would see it: the window plus every other visible
// top-level widget (menus, popups, dialogs) at its offset.  Device pixels; the
// image carries the window's device pixel ratio.
[[nodiscard]] QImage GrabComposite(QWidget& window);

// Runs steps against the window on the event loop and calls finished(exitCode)
// once: 0 after the last step, 4 when a capture cannot be written.  Each step is
// scheduled before it runs, so a step that enters a nested event loop (a context
// menu's exec) does not stall the script.
class CaptureDriver : public QObject {
public:
    CaptureDriver(QWidget& window, std::vector<CaptureStep> steps, std::function<void(int)> finished);
    ~CaptureDriver() override;

    void Start();

private:
    void RunNext();
    [[nodiscard]] bool Execute(const CaptureStep& step);
    // Main-window client position to global coordinates.
    [[nodiscard]] QPointF Global(const QPointF& position) const;
    // The window a real pointer at `global` would reach: an open dialog over
    // the main window, or the pressed window while a button is down.
    [[nodiscard]] QWidget* WindowAt(const QPointF& global) const;
    void SendMouse(QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void SendKey(int key, Qt::KeyboardModifiers modifiers, const QString& text);
    [[nodiscard]] QWindow* KeyTarget() const;
    void Finish(int exitCode);

    QPointer<QWidget> window_;
    std::vector<CaptureStep> steps_;
    std::size_t next_ = 0;
    Qt::MouseButtons buttons_ = Qt::NoButton;
    QPointer<QWidget> pressed_;
    std::function<void(int)> finished_;
    bool done_ = false;
};

}  // namespace cnote::shell
