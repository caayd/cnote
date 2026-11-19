#include "shell/CaptureDriver.h"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <utility>

#include <QApplication>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMainWindow>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <private/qguiapplication_p.h>
#include <private/qhighdpiscaling_p.h>
#include <qpa/qwindowsysteminterface.h>

#include "Unicode.h"
#include "ui/PointerPositionQt.h"

namespace cnote::shell {

namespace {

using Sync = QWindowSystemInterface::SynchronousDelivery;

std::string_view Trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1U);
}

std::vector<std::string_view> Tokens(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t start = text.find_first_not_of(" \t\r", position);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = std::min(text.find_first_of(" \t\r", start), text.size());
        tokens.push_back(text.substr(start, end - start));
        position = end;
    }
    return tokens;
}

bool ParseNumber(std::string_view token, double* value) {
    bool ok = false;
    *value = QString::fromUtf8(token.data(), static_cast<qsizetype>(token.size())).toDouble(&ok);
    return ok;
}

bool ParseInt(std::string_view token, int* value) {
    bool ok = false;
    *value = QString::fromUtf8(token.data(), static_cast<qsizetype>(token.size())).toInt(&ok);
    return ok;
}

bool ParseButton(std::string_view token, Qt::MouseButton* button) {
    if (token == "left") {
        *button = Qt::LeftButton;
    } else if (token == "right") {
        *button = Qt::RightButton;
    } else if (token == "middle") {
        *button = Qt::MiddleButton;
    } else {
        return false;
    }
    return true;
}

bool ParseModifiers(std::string_view token, Qt::KeyboardModifiers* modifiers) {
    Qt::KeyboardModifiers parsed = Qt::NoModifier;
    std::size_t position = 0;
    while (position <= token.size()) {
        const std::size_t end = std::min(token.find('+', position), token.size());
        const std::string_view name = token.substr(position, end - position);
        if (name == "ctrl") {
            parsed |= Qt::ControlModifier;
        } else if (name == "shift") {
            parsed |= Qt::ShiftModifier;
        } else if (name == "alt") {
            parsed |= Qt::AltModifier;
        } else if (name == "meta") {
            parsed |= Qt::MetaModifier;
        } else {
            return false;
        }
        position = end + 1U;
    }
    *modifiers = parsed;
    return true;
}

// The text a keyboard would produce for a key press: printable keys without a
// command modifier, plus the control characters editors rely on.
QString KeyText(int key, Qt::KeyboardModifiers modifiers) {
    if ((modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != 0) {
        return QString();
    }
    switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter: return QStringLiteral("\r");
        case Qt::Key_Tab: return QStringLiteral("\t");
        case Qt::Key_Backspace: return QStringLiteral("\b");
        case Qt::Key_Escape: return QStringLiteral("\x1b");
        case Qt::Key_Delete: return QStringLiteral("\x7f");
        default: break;
    }
    if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
        const QChar character(key);
        return QString(modifiers.testFlag(Qt::ShiftModifier) ? character : character.toLower());
    }
    return QString();
}

bool IsPopup(const QWidget* widget) {
    const Qt::WindowType type = widget->windowType();
    return type == Qt::Popup || type == Qt::ToolTip;
}

}  // namespace

bool ParseCaptureScript(std::string_view text, std::vector<CaptureStep>* steps, std::string* error) {
    steps->clear();
    int lineNumber = 0;
    std::size_t position = 0;
    while (position <= text.size()) {
        const std::size_t end = std::min(text.find('\n', position), text.size());
        const std::string_view raw = text.substr(position, end - position);
        position = end + 1U;
        ++lineNumber;

        const std::string_view line = Trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::vector<std::string_view> tokens = Tokens(line);
        const std::string_view command = tokens.front();
        // Everything after the command word, for the free-text steps.
        const std::string_view rest = Trim(line.substr(command.size()));
        auto fail = [&](const std::string& message) {
            *error = "line " + std::to_string(lineNumber) + ": " + message;
            return false;
        };

        CaptureStep step;
        step.line = lineNumber;
        // Reads `count` numbers starting at tokens[1].
        auto numbers = [&](std::size_t count, double* values) {
            if (tokens.size() < count + 1U) {
                return false;
            }
            for (std::size_t index = 0; index < count; ++index) {
                if (!ParseNumber(tokens[index + 1U], &values[index])) {
                    return false;
                }
            }
            return true;
        };
        // Optional trailing tokens: [STEPS] [BUTTON] [MODS].
        auto options = [&](std::size_t first, bool allowMoves, bool allowButton) {
            for (std::size_t index = first; index < tokens.size(); ++index) {
                int moves = 0;
                if (allowMoves && ParseInt(tokens[index], &moves) && moves > 0) {
                    step.moves = moves;
                    continue;
                }
                if (allowButton && ParseButton(tokens[index], &step.button)) {
                    continue;
                }
                if (!ParseModifiers(tokens[index], &step.modifiers)) {
                    return false;
                }
            }
            return true;
        };

        if (command == "size") {
            int width = 0;
            int height = 0;
            if (tokens.size() != 3U || !ParseInt(tokens[1], &width) || !ParseInt(tokens[2], &height)
                || width <= 0 || height <= 0) {
                return fail("expected: size W H");
            }
            step.kind = CaptureStepKind::Size;
            step.size = QSize(width, height);
        } else if (command == "wait") {
            if (tokens.size() != 2U || !ParseInt(tokens[1], &step.milliseconds) || step.milliseconds < 0) {
                return fail("expected: wait MS");
            }
            step.kind = CaptureStepKind::Wait;
        } else if (command == "move" || command == "click" || command == "doubleclick") {
            double values[2] {};
            const bool pointer = command != "move";
            if (!numbers(2, values) || !options(3, false, pointer) || (!pointer && tokens.size() != 3U)) {
                return fail(pointer ? "expected: " + std::string(command) + " X Y [left|right|middle] [MODS]"
                                    : "expected: move X Y");
            }
            step.kind = command == "move" ? CaptureStepKind::Move
                : (command == "click" ? CaptureStepKind::Click : CaptureStepKind::DoubleClick);
            step.from = QPointF(values[0], values[1]);
        } else if (command == "drag") {
            double values[4] {};
            if (!numbers(4, values) || !options(5, true, true)) {
                return fail("expected: drag X1 Y1 X2 Y2 [STEPS] [left|right|middle] [MODS]");
            }
            step.kind = CaptureStepKind::Drag;
            step.from = QPointF(values[0], values[1]);
            step.to = QPointF(values[2], values[3]);
        } else if (command == "wheel") {
            double values[2] {};
            if (!numbers(2, values) || tokens.size() < 4U || !ParseInt(tokens[3], &step.notches) || !options(4, false, false)) {
                return fail("expected: wheel X Y NOTCHES [MODS]");
            }
            step.kind = CaptureStepKind::Wheel;
            step.from = QPointF(values[0], values[1]);
        } else if (command == "key") {
            step.kind = CaptureStepKind::Key;
            step.text = QString::fromUtf8(rest.data(), static_cast<qsizetype>(rest.size()));
            const QKeySequence sequence = QKeySequence::fromString(step.text, QKeySequence::PortableText);
            if (rest.empty() || sequence.isEmpty() || sequence[0].key() == Qt::Key_unknown) {
                return fail("expected: key SEQUENCE (for example Ctrl+W or Escape)");
            }
        } else if (command == "type") {
            // The text starts after the single separator, so leading spaces
            // beyond it are typed.
            const std::string_view typed = raw.substr(raw.find(command) + command.size());
            if (typed.size() < 2U) {
                return fail("expected: type TEXT");
            }
            step.kind = CaptureStepKind::Type;
            std::string_view body = typed.substr(1);
            if (!body.empty() && body.back() == '\r') {
                body.remove_suffix(1);
            }
            step.text = QString::fromUtf8(body.data(), static_cast<qsizetype>(body.size()));
        } else if (command == "capture") {
            if (rest.empty()) {
                return fail("expected: capture PATH");
            }
            step.kind = CaptureStepKind::Capture;
            step.path = Utf8ToPath(std::string(rest));
        } else {
            return fail("unknown step '" + std::string(command) + "'");
        }
        steps->push_back(std::move(step));
    }
    return true;
}

OffscreenWindows::OffscreenWindows(QObject* parent) : QObject(parent) {}

void OffscreenWindows::SetPrimaryWindow(QWidget* window) {
    primary_ = window;
}

bool OffscreenWindows::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if (type != QEvent::Show && type != QEvent::Hide && type != QEvent::Move && type != QEvent::Resize) {
        return false;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr || !widget->isWindow()) {
        return false;
    }
    if (type == QEvent::Show) {
        // Qt sends the Show event before it shows the native window, so the
        // attribute is in place when it decides whether to map it.
        widget->setAttribute(Qt::WA_DontShowOnScreen);
    }
    if (!widget->testAttribute(Qt::WA_DontShowOnScreen)) {
        return false;
    }
    // Qt never moves or resizes the native window of an off-screen widget, but
    // pointer mapping (mapToGlobal, popup forwarding) reads the native
    // geometry: keep it where a shown window would be.
    if (QWindow* handle = widget->windowHandle(); handle != nullptr && handle->geometry() != widget->geometry()) {
        handle->setGeometry(widget->geometry());
    }
    if (type == QEvent::Show) {
        if (widget->windowType() == Qt::Popup) {
            RegisterPopup(widget);
        } else if (!IsPopup(widget)) {
            Activate(widget);
        }
    } else if (type == QEvent::Hide && !IsPopup(widget) && widget != primary_ && primary_ != nullptr
               && QGuiApplication::focusWindow() == widget->windowHandle()) {
        Activate(primary_);
    }
    return false;
}

void OffscreenWindows::Activate(QWidget* window) {
    // Queued: the window finishes showing (or hiding) first.  The change is
    // delivered inside Qt only; the platform window is never activated.
    QMetaObject::invokeMethod(this, [target = QPointer<QWidget>(window)]() {
        if (target != nullptr && target->isVisible() && target->windowHandle() != nullptr) {
            QWindowSystemInterface::handleFocusWindowChanged<Sync>(target->windowHandle(), Qt::ActiveWindowFocusReason);
        }
    }, Qt::QueuedConnection);
}

void OffscreenWindows::RegisterPopup(QWidget* popup) {
    // Qt only enters popup mode (outside clicks close the popup, keys and
    // pointer events go to it) once the native window is visible, which an
    // off-screen popup never is; add it to the popup stack once shown
    // instead.  Queued after the show, so Qt does not try to grab the mouse
    // and keyboard for a window that is not on screen.  Hiding the popup
    // removes it through Qt's own close path.
    QMetaObject::invokeMethod(this, [target = QPointer<QWidget>(popup)]() {
        if (target == nullptr || !target->isVisible() || target->windowHandle() == nullptr) {
            return;
        }
        QWindowList& popups = QGuiApplicationPrivate::popup_list;
        popups.removeOne(target->windowHandle());
        popups.append(target->windowHandle());
    }, Qt::QueuedConnection);
}

void SetScreenScale(qreal factor) {
    // A per-screen factor replaces the platform's DPI-derived one (it does not
    // multiply it), while logical DPI stays 96, so text keeps its size.
    for (QScreen* screen : QGuiApplication::screens()) {
        QHighDpiScaling::setScreenFactor(screen, factor);
    }
}

QImage GrabComposite(QWidget& window) {
    const QPixmap base = window.grab();
    QImage image = base.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(base.devicePixelRatio());

    // Other main windows are separate views, not part of this one.  Dialogs go
    // below popups, matching the stacking a window manager gives them.
    std::vector<QWidget*> layers;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget != &window && widget->isVisible() && qobject_cast<QMainWindow*>(widget) == nullptr) {
            layers.push_back(widget);
        }
    }
    std::stable_partition(layers.begin(), layers.end(), [](const QWidget* widget) { return !IsPopup(widget); });

    QPainter painter(&image);
    // Widget geometry is the client rectangle in global coordinates, the
    // position a shown window would have on screen.
    const QPoint origin = window.geometry().topLeft();
    for (QWidget* layer : layers) {
        painter.drawPixmap(layer->geometry().topLeft() - origin, layer->grab());
    }
    return image;
}

CaptureDriver::CaptureDriver(QWidget& window, std::vector<CaptureStep> steps, std::function<void(int)> finished)
    : window_(&window), steps_(std::move(steps)), finished_(std::move(finished)) {}

CaptureDriver::~CaptureDriver() {
    ui::SetSyntheticCursorPos(std::nullopt);
}

void CaptureDriver::Start() {
    QWindow* handle = window_->windowHandle();
    // The pointer starts outside the window until a step moves it.
    ui::SetSyntheticCursorPos(window_->geometry().topLeft() - QPoint(1, 1));
    QWindowSystemInterface::handleFocusWindowChanged<Sync>(handle, Qt::ActiveWindowFocusReason);
    QTimer::singleShot(0, this, &CaptureDriver::RunNext);
}

void CaptureDriver::RunNext() {
    if (window_ == nullptr) {
        std::fprintf(stderr, "capture: the window was destroyed before the script finished\n");
        Finish(4);
        return;
    }
    if (done_ || next_ >= steps_.size()) {
        Finish(0);
        return;
    }
    const CaptureStep& step = steps_[next_++];
    // Scheduled before the step runs: a step that opens a nested event loop
    // (QMenu::exec) keeps the script going from inside that loop.
    const int delay = step.kind == CaptureStepKind::Wait ? step.milliseconds : 0;
    QTimer::singleShot(delay, this, &CaptureDriver::RunNext);
    if (!Execute(step)) {
        Finish(4);
    }
}

bool CaptureDriver::Execute(const CaptureStep& step) {
    switch (step.kind) {
        case CaptureStepKind::Size:
            window_->resize(step.size);
            if (window_->size() != step.size) {
                std::fprintf(stderr, "capture: line %d: window is %dx%d, not the requested %dx%d (screen limit)\n",
                    step.line, window_->width(), window_->height(), step.size.width(), step.size.height());
            }
            return true;
        case CaptureStepKind::Wait:
            return true;
        case CaptureStepKind::Move:
            SendMouse(QEvent::MouseMove, step.from, Qt::NoButton, step.modifiers);
            return true;
        case CaptureStepKind::Click:
        case CaptureStepKind::DoubleClick: {
            const int clicks = step.kind == CaptureStepKind::Click ? 1 : 2;
            SendMouse(QEvent::MouseMove, step.from, Qt::NoButton, step.modifiers);
            for (int click = 0; click < clicks; ++click) {
                SendMouse(QEvent::MouseButtonPress, step.from, step.button, step.modifiers);
                SendMouse(QEvent::MouseButtonRelease, step.from, step.button, step.modifiers);
            }
            return true;
        }
        case CaptureStepKind::Drag:
            SendMouse(QEvent::MouseMove, step.from, Qt::NoButton, step.modifiers);
            SendMouse(QEvent::MouseButtonPress, step.from, step.button, step.modifiers);
            for (int move = 1; move <= step.moves; ++move) {
                const qreal t = static_cast<qreal>(move) / static_cast<qreal>(step.moves);
                SendMouse(QEvent::MouseMove, step.from + (step.to - step.from) * t, Qt::NoButton, step.modifiers);
            }
            SendMouse(QEvent::MouseButtonRelease, step.to, step.button, step.modifiers);
            return true;
        case CaptureStepKind::Wheel: {
            SendMouse(QEvent::MouseMove, step.from, Qt::NoButton, step.modifiers);
            const QPointF global = Global(step.from);
            QWidget* target = WindowAt(global);
            QWindow* handle = target->windowHandle();
            QWindowSystemInterface::handleWheelEvent(handle,
                QHighDpi::toNativeLocalPosition(global - target->geometry().topLeft(), handle),
                QHighDpi::toNativeGlobalPosition(global, handle), QPoint(), QPoint(0, step.notches * 120), step.modifiers);
            QWindowSystemInterface::flushWindowSystemEvents();
            return true;
        }
        case CaptureStepKind::Key: {
            const QKeySequence sequence = QKeySequence::fromString(step.text, QKeySequence::PortableText);
            for (int index = 0; index < sequence.count(); ++index) {
                const QKeyCombination combination = sequence[index];
                const int key = static_cast<int>(combination.key());
                SendKey(key, combination.keyboardModifiers(), KeyText(key, combination.keyboardModifiers()));
            }
            return true;
        }
        case CaptureStepKind::Type: {
            const QList<uint> codePoints = step.text.toUcs4();
            for (const uint codePoint : codePoints) {
                const QString text = QString::fromUcs4(reinterpret_cast<const char32_t*>(&codePoint), 1);
                const bool upper = text.toLower() != text;
                const int key = codePoint < 0x10000U ? static_cast<int>(QChar(static_cast<char16_t>(codePoint)).toUpper().unicode()) : 0;
                SendKey(key, upper ? Qt::ShiftModifier : Qt::NoModifier, text);
            }
            return true;
        }
        case CaptureStepKind::Capture: {
            const QImage image = GrabComposite(*window_);
            std::error_code error;
            if (step.path.has_parent_path()) {
                std::filesystem::create_directories(step.path.parent_path(), error);
            }
            const QString path = QString::fromStdString(PathToUtf8(step.path));
            if (image.isNull() || !image.save(path, "PNG")) {
                std::fprintf(stderr, "capture: line %d: cannot write %s\n", step.line, PathToUtf8(step.path).c_str());
                return false;
            }
            std::printf("capture: wrote %s (%dx%d px, scale %.2f)\n", PathToUtf8(step.path).c_str(),
                image.width(), image.height(), image.devicePixelRatio());
            std::fflush(stdout);
            return true;
        }
    }
    return true;
}

QPointF CaptureDriver::Global(const QPointF& position) const {
    return position + window_->geometry().topLeft();
}

QWidget* CaptureDriver::WindowAt(const QPointF& global) const {
    // While buttons are down the pressed window keeps the pointer (the
    // platform's implicit grab).  An open popup needs no lookup: Qt forwards
    // every pointer event to it.
    if (pressed_ != nullptr) {
        return pressed_;
    }
    if (QApplication::activePopupWidget() == nullptr) {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget != window_ && widget->isVisible() && !IsPopup(widget)
                && qobject_cast<QMainWindow*>(widget) == nullptr && widget->geometry().contains(global.toPoint())) {
                return widget;
            }
        }
    }
    return window_;
}

void CaptureDriver::SendMouse(QEvent::Type type, const QPointF& position, Qt::MouseButton button,
                              Qt::KeyboardModifiers modifiers) {
    const QPointF global = Global(position);
    QWidget* target = WindowAt(global);
    if (type == QEvent::MouseButtonPress) {
        if (buttons_ == Qt::NoButton) {
            pressed_ = target;
            // Pressing in an inactive window activates it, as a window manager does.
            if (QApplication::activePopupWidget() == nullptr && QGuiApplication::focusWindow() != target->windowHandle()) {
                QWindowSystemInterface::handleFocusWindowChanged<Sync>(target->windowHandle(), Qt::ActiveWindowFocusReason);
            }
        }
        buttons_ |= button;
    } else if (type == QEvent::MouseButtonRelease) {
        buttons_ &= ~Qt::MouseButtons(button);
    }
    ui::SetSyntheticCursorPos(global.toPoint());
    // Platform input arrives in native (device) pixels; Qt scales it back.
    QWindow* handle = target->windowHandle();
    QWindowSystemInterface::handleMouseEvent<Sync>(handle,
        QHighDpi::toNativeLocalPosition(global - target->geometry().topLeft(), handle),
        QHighDpi::toNativeGlobalPosition(global, handle), buttons_, button, type, modifiers);
    if (buttons_ == Qt::NoButton) {
        pressed_ = nullptr;
    }
}

void CaptureDriver::SendKey(int key, Qt::KeyboardModifiers modifiers, const QString& text) {
    QWindow* target = KeyTarget();
    QWindowSystemInterface::handleKeyEvent<Sync>(target, QEvent::KeyPress, key, modifiers, text);
    QWindowSystemInterface::handleKeyEvent<Sync>(KeyTarget(), QEvent::KeyRelease, key, modifiers, text);
}

QWindow* CaptureDriver::KeyTarget() const {
    QWindow* focus = QGuiApplication::focusWindow();
    return focus != nullptr ? focus : window_->windowHandle();
}

void CaptureDriver::Finish(int exitCode) {
    if (done_) {
        return;
    }
    done_ = true;
    if (finished_) {
        finished_(exitCode);
    }
}

}  // namespace cnote::shell
