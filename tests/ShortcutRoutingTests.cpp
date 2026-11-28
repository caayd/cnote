// Deterministic offscreen contract test for keyboard-shortcut routing in the
// Qt shell (QtMainWindow).
//
// The window owns one QAction per command; the settings store is the single
// source of every runtime binding (KeyBindingQt is the portable restatement of
// the removed reference table, src/ShortcutConfig.cpp).  This test drives real
// key events through the real window (QTest::keyClick on the QWindow, i.e. the
// same QWindowSystemInterface -> QWidgetWindow -> QShortcutMap path a physical
// keyboard takes) and asserts the observable state the user sees:
//
//   * D/E/S/M/H/L/T switch tools while the Text tool is active with no editor
//     open, whichever widget inside the window has focus;
//   * while a text editor or a LaTeX editor owns the keyboard, typing those
//     letters reaches the editor and neither switches tools nor loses a
//     character (the reference precedence: the editor's child window consumed
//     every key; App::TextEditProc, MessageLoop.cpp);
//   * every non-empty shortcut in the settings table is bound on exactly one
//     window action and that action is the one Qt dispatches the key to - from
//     canvas focus and from toolbar focus;
//   * Ctrl+W (quick popup / text formatting), Ctrl+Q (tool wheel) and Ctrl+B
//     (PDF sidebar) fire from both focus hosts and in any tool state;
//   * Ctrl+W while a text edit is open routes to the text-format popup (the
//     reference TextEditProc WM_KEYDOWN ctrl+W branch) rather than the brush
//     popup, and Ctrl+B inside the editors stays the editor's key;
//   * PgUp/PgDn page navigation stands down while an editor owns the keyboard
//     (the reference editor child window consumed them for caret/scroll);
//   * a selection change enables Copy/Cut immediately, including one set through
//     the controller or by a paste (no unrelated UpdateChrome needed);
//   * the unshifted Ctrl+'=' key aliases the Zoom In accelerator;
//   * a binding remapped through the settings store is the one that fires.
//
// Acceptance is observed through the public seam the shell exposes: the canvas
// router's active tool, ToolbarWidget's popup-visibility queries and
// SidebarWidget's IsOpen().  Every wait is bounded; the CTest TIMEOUT bounds the
// process.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSlider>
#include <QString>
#include <QTextEdit>
#include <QWindow>
#include <QtTest/QTest>

#include "Unicode.h"
#include "document/SelectionClipboardCodec.h"
#include "qt/QtMainWindow.h"
#include "settings/KeyBindingQt.h"
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"
#include "text/TextEditingQt.h"
#include "latex/LatexEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/InteractionRouter.h"
#include "ui/SettingsDialogQt.h"
#include "ui/SidebarWidget.h"
#include "ui/ToolbarWidget.h"

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("ok   %s\n", what.c_str());
    } else {
        std::printf("FAIL %s\n", what.c_str());
        ++gFailures;
    }
}

const char* ToolName(cnote::settings::Tool tool) {
    switch (tool) {
        case cnote::settings::Tool::Select: return "Select";
        case cnote::settings::Tool::Move: return "Move";
        case cnote::settings::Tool::Text: return "Text";
        case cnote::settings::Tool::Latex: return "Latex";
        case cnote::settings::Tool::Draw: return "Draw";
        case cnote::settings::Tool::Pan: return "Pan";
        case cnote::settings::Tool::Erase: return "Erase";
        case cnote::settings::Tool::Zoom: return "Zoom";
    }
    return "?";
}

std::string Describe(const cnote::settings::ShortcutAction action) {
    const cnote::settings::ShortcutDefinition* definition = cnote::settings::FindShortcutDefinition(action);
    return definition != nullptr ? std::string(definition->label) : std::string("?");
}

// A session view with no widgets, registered on the shell's manager so the
// auto-save deadline can be observed independently of the window's document.
class AutoSaveStubView : public QObject, public cnote::SessionView {
public:
    bool interactionActive = false;

    [[nodiscard]] bool IsInteractionActive() const override { return interactionActive; }
    void FinishInteraction() override {}
    void OnSessionChanged(const cnote::ProjectChange&, bool) override {}
    [[nodiscard]] std::size_t CurrentPageIndex() const override { return 0; }
    void SetCurrentPageIndex(std::size_t) override {}
};

// The shell's live state, reached through the same public seam the app uses.
struct Harness {
    cnote::AppManager manager;
    std::unique_ptr<cnote::qt::QtMainWindow> window;
    cnote::ui::ToolbarWidget* toolbar = nullptr;
    cnote::ui::SidebarWidget* sidebar = nullptr;
    cnote::ui::CanvasWidget* canvas = nullptr;

    Harness() {
        window = std::make_unique<cnote::qt::QtMainWindow>(manager);
        window->show();
        window->activateWindow();
        // Qt::WindowShortcut matching consults QApplication::activeWindow(); the
        // offscreen platform plugins do not all honour requestActivate, so set it
        // explicitly for a deterministic run on every platform.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        QApplication::setActiveWindow(window.get());
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        QApplication::processEvents();
        toolbar = window->findChild<cnote::ui::ToolbarWidget*>();
        sidebar = window->findChild<cnote::ui::SidebarWidget*>();
        canvas = window->Canvas();
    }

    ~Harness() {
        if (window != nullptr) {
            window->hide();
        }
    }

    [[nodiscard]] cnote::settings::Tool ActiveTool() const {
        return static_cast<cnote::settings::Tool>(canvas->Router().ActiveTool());
    }

    void FocusCanvas() {
        canvas->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
    }

    void FocusToolbar() {
        toolbar->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
    }

    // The real keyboard path: the platform event enters through the window.
    void Press(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(window->windowHandle(), key, modifiers);
        QApplication::processEvents();
    }

    void PressSequence(const QKeySequence& sequence) {
        QTest::keySequence(window->windowHandle(), sequence);
        QApplication::processEvents();
    }
};

void SendMouse(QWidget& widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, position, widget.mapToGlobal(position.toPoint()), button, buttons, modifiers);
    QApplication::sendEvent(&widget, &event);
}

// Clicks a toolbar control the way a user does, so the shell's active tool is
// set through the widget's real intent signal (independent of shortcuts).
void ClickToolbarControl(Harness& harness, cnote::ui::ToolbarControl control) {
    const QPointF center = harness.toolbar->ControlRect(control).center();
    SendMouse(*harness.toolbar, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    SendMouse(*harness.toolbar, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}

void SelectTool(Harness& harness, cnote::settings::Tool tool) {
    ClickToolbarControl(harness, cnote::ui::ToolbarWidget::ControlForTool(tool));
}

// Commits a non-empty text object so the session gets a real undo step (an
// empty create is removed again and records no history).
void MakeUndoableEdit(Harness& harness) {
    harness.window->AddTextObject();
    if (QTextEdit* editor = harness.canvas->TextEditor(); editor != nullptr) {
        editor->setPlainText(QStringLiteral("x"));
    }
    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();
}

// Text typed after the editor has been emptied must keep the object's style.
// The editor palette is the dark chrome's (white text), so a character that
// lost its explicit colour drew white on the white page: invisible.  The
// common trigger was a Ctrl+Backspace pressed with nothing left to delete.
void TypingAfterEmptyingKeepsObjectStyle() {
    struct Route {
        const char* name;
        std::vector<std::pair<Qt::Key, Qt::KeyboardModifiers>> keys;
    };
    const std::vector<Route> routes {
        { "Ctrl+Backspace pressed past empty", { { Qt::Key_Backspace, Qt::ControlModifier },
              { Qt::Key_Backspace, Qt::ControlModifier }, { Qt::Key_Backspace, Qt::ControlModifier } } },
        { "Backspace pressed past empty", std::vector<std::pair<Qt::Key, Qt::KeyboardModifiers>>(13, { Qt::Key_Backspace, Qt::NoModifier }) },
        { "select all, Delete twice", { { Qt::Key_A, Qt::ControlModifier }, { Qt::Key_Delete, Qt::NoModifier },
              { Qt::Key_Delete, Qt::NoModifier } } },
        { "select all, cut, Backspace", { { Qt::Key_A, Qt::ControlModifier }, { Qt::Key_X, Qt::ControlModifier },
              { Qt::Key_Backspace, Qt::NoModifier } } },
    };
    for (const Route& route : routes) {
        Harness harness;
        harness.window->AddTextObject();
        QApplication::processEvents();
        QTextEdit* editor = harness.canvas->TextEditor();
        const cnote::PageText* object = harness.window->Controller().EditingTextObject();
        Check(editor != nullptr && object != nullptr, std::string("text overlay opens for ") + route.name);
        if (editor == nullptr || object == nullptr) {
            continue;
        }
        const QColor objectColor = QColor::fromRgba(object->color);
        editor->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        for (const char character : std::string("hello world")) {
            QTest::keyClick(harness.window->windowHandle(), character);
        }
        for (const auto& [key, modifiers] : route.keys) {
            harness.Press(key, modifiers);
        }
        Check(editor->toPlainText().isEmpty(), std::string("the editor is empty after ") + route.name);
        for (const char character : std::string("abc")) {
            QTest::keyClick(harness.window->windowHandle(), character);
        }
        QApplication::processEvents();

        bool styled = editor->toPlainText() == QStringLiteral("abc");
        QTextCursor probe(editor->document());
        for (int position = 1; position < editor->document()->characterCount(); ++position) {
            probe.setPosition(position);
            const QTextCharFormat format = probe.charFormat();
            styled = styled && format.hasProperty(QTextFormat::ForegroundBrush)
                && format.foreground().color() == objectColor
                && format.fontPointSize() > 0.0 && !format.fontFamilies().toStringList().isEmpty();
        }
        Check(styled, std::string("text typed after ") + route.name + " keeps the object's colour, size and family");
        // And it draws: pixels in the object's colour appear in the editor.
        const QImage shot = editor->grab().toImage();
        int ink = 0;
        for (int y = 0; y < shot.height(); ++y) {
            for (int x = 0; x < shot.width(); ++x) {
                const QRgb pixel = shot.pixel(x, y);
                ink += qAlpha(pixel) > 128 && std::abs(qGray(pixel) - qGray(objectColor.rgb())) < 64 ? 1 : 0;
            }
        }
        std::printf("typed after %s: '%s' ink=%d\n", route.name, editor->toPlainText().toUtf8().constData(), ink);
        Check(ink > 0, std::string("text typed after ") + route.name + " is visible");
    }
}

// Finds the single window action carrying `sequence` (empty when none, exactly
// one for every wired settings binding).
QAction* FindActionBySequence(cnote::qt::QtMainWindow& window, const QKeySequence& sequence) {
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->shortcut() == sequence) {
            return action;
        }
    }
    return nullptr;
}

// Records which QAction Qt's QShortcutMap dispatches a key to, without letting
// the action's own command run (Open/Save/Export would open modal dialogs).
// QShortcutMap sends QEvent::Shortcut to the matched QAction, so a filter on the
// action is the exact routing probe.
class ShortcutProbe : public QObject {
public:
    void Install(cnote::qt::QtMainWindow& window) {
        for (QAction* action : window.findChildren<QAction*>()) {
            action->installEventFilter(this);
        }
    }

    void Reset() {
        last_ = nullptr;
        count_ = 0;
    }

    [[nodiscard]] QAction* Last() const { return last_; }
    [[nodiscard]] int Count() const { return count_; }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Shortcut) {
            last_ = qobject_cast<QAction*>(watched);
            ++count_;
            return true;
        }
        return false;
    }

private:
    QAction* last_ = nullptr;
    int count_ = 0;
};

std::filesystem::path MakeTempDir(const std::string& name) {
    std::error_code error;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(error) / cnote::Utf8ToPath("cnote-shortcut-" + name);
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

std::string ReadText(const std::filesystem::path& path) {
    std::string text;
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return text;
    }
    char buffer[256] = {};
    for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
        text.append(buffer, read);
    }
    std::fclose(file);
    return text;
}

// --- 1. tool keys with the Text tool active and no editor --------------------

void ToolKeysSwitchToolsWhileTextToolActive() {
    Harness harness;
    const std::array<std::pair<Qt::Key, cnote::settings::Tool>, 7> cases {{
        { Qt::Key_D, cnote::settings::Tool::Draw },
        { Qt::Key_E, cnote::settings::Tool::Erase },
        { Qt::Key_S, cnote::settings::Tool::Select },
        { Qt::Key_M, cnote::settings::Tool::Move },
        { Qt::Key_H, cnote::settings::Tool::Pan },
        { Qt::Key_L, cnote::settings::Tool::Latex },
        { Qt::Key_T, cnote::settings::Tool::Text },
    }};
    for (const auto& [key, expected] : cases) {
        for (const bool toolbarFocus : { false, true }) {
            SelectTool(harness, cnote::settings::Tool::Text);
            Check(harness.ActiveTool() == cnote::settings::Tool::Text, "baseline: Text tool active");
            if (toolbarFocus) {
                harness.FocusToolbar();
            } else {
                harness.FocusCanvas();
            }
            harness.Press(key);
            const bool switched = harness.ActiveTool() == expected;
            std::printf("key=%c focus=%s editor=%d -> tool=%s (expected %s)\n",
                static_cast<char>(key), toolbarFocus ? "toolbar" : "canvas",
                harness.canvas->EditOverlayActive() ? 1 : 0,
                ToolName(harness.ActiveTool()), ToolName(expected));
            Check(switched, std::string("tool key switches to ") + ToolName(expected)
                    + (toolbarFocus ? " from toolbar focus" : " from canvas focus"));
        }
    }
}

// --- 2. an open editor owns the keyboard -------------------------------------

void EditorOwnsToolKeys() {
    {
        Harness harness;
        SelectTool(harness, cnote::settings::Tool::Draw);
        harness.window->AddTextObject();
        QApplication::processEvents();
        QTextEdit* editor = harness.canvas->TextEditor();
        Check(editor != nullptr && editor->isVisible(), "AddTextObject opens the text overlay");
        Check(harness.canvas->EditOverlayActive(), "the text overlay owns the keyboard");
        if (editor != nullptr) {
            editor->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
        }
        for (const Qt::Key key : { Qt::Key_D, Qt::Key_E, Qt::Key_S, Qt::Key_M, Qt::Key_H, Qt::Key_L }) {
            harness.Press(key);
        }
        const QString typed = editor != nullptr ? editor->toPlainText() : QString();
        std::printf("text editor content after typing desmhl: '%s' tool=%s\n",
            typed.toUtf8().constData(), ToolName(harness.ActiveTool()));
        Check(harness.ActiveTool() == cnote::settings::Tool::Draw, "typing D/E/S/M/H/L in the text editor does not switch tools");
        Check(typed == QStringLiteral("desmhl"), "every typed character reaches the text editor");

        // Ctrl+Z / Ctrl+Y through the window reach the editor's own history
        // (the window's Undo action stands down while the editor is open).
        harness.Press(Qt::Key_Z, Qt::ControlModifier);
        const QString undone = editor != nullptr ? editor->toPlainText() : QString();
        harness.Press(Qt::Key_Y, Qt::ControlModifier);
        const QString redone = editor != nullptr ? editor->toPlainText() : QString();
        std::printf("text editor undo -> '%s' redo -> '%s'\n", undone.toUtf8().constData(), redone.toUtf8().constData());
        Check(undone.isEmpty(), "Ctrl+Z in the text editor undoes the typed word");
        Check(redone == QStringLiteral("desmhl"), "Ctrl+Y in the text editor redoes it");

        // A typed $...$ formula renders once the caret moves on and commits
        // as its source.
        for (const char character : std::string(" $x^2$ ")) {
            QTest::keyClick(harness.window->windowHandle(), character);
            QApplication::processEvents();
        }
        const std::size_t rendered = editor != nullptr
            ? cnote::text::ReadDocumentContent(*editor->document()).map.math.size()
            : 0U;
        Check(rendered == 1U, "a typed formula renders in the text editor");
        harness.canvas->CommitActiveEdit();
        QApplication::processEvents();
        const auto& texts = harness.window->Controller().DocumentRef().PageAt(0U).texts;
        const std::u16string committed = texts.empty() ? std::u16string() : texts.back().content;
        std::printf("committed text with formula: '%s'\n", cnote::Utf16ToUtf8(committed).c_str());
        Check(committed == u"desmhl $x^2$ ", "the committed text stores the formula source");
    }
    {
        Harness harness;
        SelectTool(harness, cnote::settings::Tool::Draw);
        harness.window->AddLatexObject();
        QApplication::processEvents();
        QPlainTextEdit* editor = harness.canvas->LatexEditor();
        Check(editor != nullptr && editor->isVisible(), "AddLatexObject opens the LaTeX overlay");
        Check(harness.canvas->EditOverlayActive(), "the LaTeX overlay owns the keyboard");
        if (editor != nullptr) {
            editor->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
        }
        for (const Qt::Key key : { Qt::Key_D, Qt::Key_E, Qt::Key_S, Qt::Key_M, Qt::Key_H, Qt::Key_L }) {
            harness.Press(key);
        }
        const QString typed = editor != nullptr ? editor->toPlainText() : QString();
        std::printf("latex editor content after typing desmhl: '%s' tool=%s\n",
            typed.toUtf8().constData(), ToolName(harness.ActiveTool()));
        Check(harness.ActiveTool() == cnote::settings::Tool::Draw, "typing D/E/S/M/H/L in the LaTeX editor does not switch tools");
        // The LaTeX overlay lowercases? No: the exact source must survive.
        Check(typed == QStringLiteral("desmhl"), "every typed character reaches the LaTeX editor");
    }
}

// --- 3. quick popup / wheel / sidebar ----------------------------------------

void QuickActionsFireEverywhere() {
    const std::array<cnote::settings::Tool, 4> tools {{
        cnote::settings::Tool::Draw, cnote::settings::Tool::Text,
        cnote::settings::Tool::Latex, cnote::settings::Tool::Pan,
    }};
    // Ctrl+Q (tool wheel) and Ctrl+B (PDF sidebar) are window commands: they
    // fire from either focus host and in every tool state.
    for (const cnote::settings::Tool tool : tools) {
        for (const bool toolbarFocus : { false, true }) {
            for (const Qt::Key key : { Qt::Key_Q, Qt::Key_B }) {
                Harness harness;
                SelectTool(harness, tool);
                toolbarFocus ? harness.FocusToolbar() : harness.FocusCanvas();
                const bool sidebarBefore = harness.sidebar->IsOpen();
                const bool wheelBefore = harness.toolbar->PaletteWheelVisible();
                harness.Press(key, Qt::ControlModifier);
                const bool changed = key == Qt::Key_Q
                    ? harness.toolbar->PaletteWheelVisible() != wheelBefore
                    : harness.sidebar->IsOpen() != sidebarBefore;
                std::printf("Ctrl+%c tool=%s focus=%s -> sidebar=%d wheel=%d\n",
                    static_cast<char>(key), ToolName(tool), toolbarFocus ? "toolbar" : "canvas",
                    harness.sidebar->IsOpen() ? 1 : 0,
                    harness.toolbar->PaletteWheelVisible() ? 1 : 0);
                Check(changed, std::string("Ctrl+") + static_cast<char>(key) + " fires with " + ToolName(tool)
                        + (toolbarFocus ? " from toolbar focus" : " from canvas focus"));
            }
        }
    }

    // Ctrl+W is context-sensitive: the brush quick popup for the pen tools,
    // the text-format popup for the Text tool (pre-placement pending style),
    // inert for the rest - the reference IsToolbarBrushTool gate extended by
    // the text tool's pending-format branch.
    for (const cnote::settings::Tool tool : tools) {
        for (const bool toolbarFocus : { false, true }) {
            Harness harness;
            SelectTool(harness, tool);
            const cnote::settings::Tool active = harness.ActiveTool();
            Check(active == tool, std::string("the toolbar button selects ") + ToolName(tool));
            const bool brushTool = active == cnote::settings::Tool::Draw || active == cnote::settings::Tool::Erase;
            const bool textTool = active == cnote::settings::Tool::Text;
            toolbarFocus ? harness.FocusToolbar() : harness.FocusCanvas();
            const bool before = harness.toolbar->QuickSizePopupVisible();
            harness.Press(Qt::Key_W, Qt::ControlModifier);
            const bool changed = harness.toolbar->QuickSizePopupVisible() != before;
            const bool textPopup = harness.toolbar->TextFormatPopupVisible();
            std::printf("Ctrl+W tool=%s focus=%s -> quick=%d textFormat=%d (brush=%d)\n",
                ToolName(tool), toolbarFocus ? "toolbar" : "canvas",
                harness.toolbar->QuickSizePopupVisible() ? 1 : 0,
                textPopup ? 1 : 0, brushTool ? 1 : 0);
            Check(changed == brushTool, std::string("Ctrl+W ") + (brushTool ? "fires" : "is inert")
                    + std::string(" with ") + ToolName(tool)
                    + (toolbarFocus ? " from toolbar focus" : " from canvas focus"));
            Check(textPopup == textTool, std::string("Ctrl+W ") + (textTool ? "opens" : "skips")
                    + " the text-format popup with " + ToolName(tool)
                    + (toolbarFocus ? " from toolbar focus" : " from canvas focus"));
        }
    }
}

void QuickPopupIsTextFormattingWhileEditing() {
    Harness harness;
    SelectTool(harness, cnote::settings::Tool::Draw);
    harness.window->AddTextObject();
    QApplication::processEvents();
    Check(harness.canvas->EditOverlayActive(), "the text edit is open before Ctrl+W");
    harness.Press(Qt::Key_W, Qt::ControlModifier);
    std::printf("editing Ctrl+W -> quick=%d textFormat=%d\n",
        harness.toolbar->QuickSizePopupVisible() ? 1 : 0,
        harness.toolbar->TextFormatPopupVisible() ? 1 : 0);
    Check(harness.toolbar->TextFormatPopupVisible(), "Ctrl+W while editing text opens the text-format popup");
    Check(!harness.toolbar->QuickSizePopupVisible(), "Ctrl+W while editing text does not open the brush popup");
}

// Clicking the text-format popup used to move the keyboard focus to the
// popup, which then hid with Ctrl+W and left focus nowhere: typing,
// Ctrl+Enter and Escape did nothing until the text was clicked again.
void TextFormatPopupLeavesFocusWithEditor() {
    Harness harness;
    harness.window->AddTextObject();
    QApplication::processEvents();
    QTextEdit* editor = harness.canvas->TextEditor();
    Check(editor != nullptr, "AddTextObject opens the editor for the popup focus check");
    if (editor == nullptr) {
        return;
    }
    editor->setFocus(Qt::OtherFocusReason);
    harness.Press(Qt::Key_A);
    harness.Press(Qt::Key_W, Qt::ControlModifier);
    Check(harness.toolbar->TextFormatPopupVisible(), "Ctrl+W opens the text-format popup over the edit");

    QTest::mouseClick(harness.window->windowHandle(), Qt::LeftButton, Qt::NoModifier,
        harness.toolbar->TextFormatPopupBounds().center());
    QApplication::processEvents();
    Check(QApplication::focusWidget() == editor, "clicking the popup leaves the keyboard with the editor");

    harness.Press(Qt::Key_W, Qt::ControlModifier);
    Check(!harness.toolbar->TextFormatPopupVisible(), "Ctrl+W closes the popup again");
    Check(QApplication::focusWidget() == editor, "the editor keeps the keyboard once the popup closes");
    harness.Press(Qt::Key_B);
    Check(harness.canvas->CaptureEditedText().content == u"ab", "typing after the popup goes into the text");
    harness.Press(Qt::Key_Return, Qt::ControlModifier);
    Check(!harness.canvas->EditOverlayActive(), "Ctrl+Enter after the popup commits the edit");
}

// Escape finishes a text or LaTeX edit and keeps what was typed, like
// Excalidraw's editor; it used to discard the whole edit.  A keyboard commit
// (Escape or Ctrl+Enter) leaves the object selected, as Excalidraw does.
void EscapeCommitsTheEdit() {
    Harness harness;
    SelectTool(harness, cnote::settings::Tool::Text);
    auto& controller = harness.window->Controller();
    const auto& page = controller.DocumentRef().PageAt(0U);
    const auto selectedTexts = [&]() { return controller.SelectionFor(0U).texts; };
    const std::size_t textsBefore = page.texts.size();
    harness.window->AddTextObject();
    QApplication::processEvents();
    if (QTextEdit* editor = harness.canvas->TextEditor(); editor != nullptr) {
        editor->setFocus(Qt::OtherFocusReason);
    }
    harness.Press(Qt::Key_A);
    harness.Press(Qt::Key_B);
    harness.Press(Qt::Key_Escape);
    Check(!harness.canvas->EditOverlayActive(), "Escape closes the text editor");
    Check(page.texts.size() == textsBefore + 1U && page.texts.back().content == u"ab",
        "Escape keeps the typed text");
    Check(controller.CanUndo(), "the kept text is one undoable step");
    Check(selectedTexts() == std::vector<std::size_t> { textsBefore },
        "Escape leaves the committed text selected");
    Check(harness.ActiveTool() == cnote::settings::Tool::Select,
        "a keyboard commit switches to the Select tool so the text can be moved or resized");

    harness.window->AddTextObject();
    QApplication::processEvents();
    harness.Press(Qt::Key_C);
    harness.Press(Qt::Key_Return, Qt::ControlModifier);
    Check(selectedTexts() == std::vector<std::size_t> { textsBefore + 1U },
        "Ctrl+Enter leaves the committed text selected");

    controller.ClearSelection();
    harness.window->AddTextObject();
    QApplication::processEvents();
    harness.Press(Qt::Key_Escape);
    Check(page.texts.size() == textsBefore + 2U, "Escape on a new, empty text leaves no object");
    Check(!controller.HasSelection(), "an emptied text leaves nothing selected");

    const std::size_t latexesBefore = page.latexes.size();
    harness.window->AddLatexObject();
    QApplication::processEvents();
    if (QPlainTextEdit* editor = harness.canvas->LatexEditor(); editor != nullptr) {
        editor->setFocus(Qt::OtherFocusReason);
        editor->setPlainText(QStringLiteral("x^2"));
    }
    QApplication::processEvents();
    harness.Press(Qt::Key_Escape);
    Check(!harness.canvas->EditOverlayActive(), "Escape closes the LaTeX editor");
    Check(page.latexes.size() == latexesBefore + 1U && page.latexes.back().content == u"x^2",
        "Escape keeps the typed LaTeX");
    Check(controller.SelectionFor(0U).latexes == std::vector<std::size_t> { latexesBefore },
        "Escape leaves the committed formula selected");
}

void EditorKeepsCtrlBWhileEditingText() {
    // Reference precedence (TextEditProc, MessageLoop.cpp): ctrl+B inside a text
    // edit is the editor's bold command, so the sidebar toggle must not steal it.
    // The edit is the window's first, opened by a Text-tool click: that path
    // creates the editor after the window last refreshed its chrome, which
    // left Ctrl+B dead until some later UpdateChrome.
    Harness harness;
    SelectTool(harness, cnote::settings::Tool::Text);
    const QPointF center = QRectF(harness.canvas->rect()).center();
    SendMouse(*harness.canvas, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    SendMouse(*harness.canvas, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    Check(harness.canvas->EditOverlayActive(), "a Text-tool click opens the editor");
    const bool sidebarBefore = harness.sidebar->IsOpen();
    const bool boldBefore = harness.canvas->TextEditor() != nullptr
        && harness.canvas->TextEditor()->currentCharFormat().fontWeight() >= QFont::Bold;
    harness.Press(Qt::Key_B, Qt::ControlModifier);
    const bool sidebarAfter = harness.sidebar->IsOpen();
    const bool boldAfter = harness.canvas->TextEditor() != nullptr
        && harness.canvas->TextEditor()->currentCharFormat().fontWeight() >= QFont::Bold;
    std::printf("editing Ctrl+B -> sidebar %d->%d bold %d->%d\n",
        sidebarBefore ? 1 : 0, sidebarAfter ? 1 : 0, boldBefore ? 1 : 0, boldAfter ? 1 : 0);
    Check(sidebarAfter == sidebarBefore, "Ctrl+B while editing text does not toggle the sidebar");
    Check(boldAfter != boldBefore, "Ctrl+B in the first, click-opened edit toggles bold (reference TextEditProc)");

    // In the LaTeX editor the reference leaves Ctrl+B inert.
    Harness latex;
    SelectTool(latex, cnote::settings::Tool::Draw);
    latex.window->AddLatexObject();
    QApplication::processEvents();
    const bool latexSidebarBefore = latex.sidebar->IsOpen();
    latex.Press(Qt::Key_B, Qt::ControlModifier);
    Check(latex.sidebar->IsOpen() == latexSidebarBefore, "Ctrl+B while editing LaTeX is inert");
}

// The format commands change only their own property, decide a toggle from the
// whole selection, and typing after reopening continues the text's style.
// They used to merge the caret's complete format over the selection: bolding
// text of mixed sizes resized all of it, and for a selection made leftwards
// the caret's format belonged to the character before the selection.
void FormatCommandsChangeOnlyTheirProperty() {
    Harness harness;
    harness.window->AddTextObject();
    QApplication::processEvents();
    QTextEdit* editor = harness.canvas->TextEditor();
    Check(editor != nullptr, "AddTextObject opens the editor for the format checks");
    if (editor == nullptr) {
        return;
    }
    editor->setFocus(Qt::OtherFocusReason);
    for (const char character : std::string("aa bb")) {
        QTest::keyClick(harness.window->windowHandle(), character);
    }
    QApplication::processEvents();
    const std::size_t pageIndex = harness.window->Controller().EditingPageIndex();
    const std::size_t textIndex = harness.window->Controller().EditingTextIndex().value_or(0U);
    const float baseSize = harness.window->Controller().EditingTextObject()->fontSize;
    const float largeSize = harness.toolbar->Settings().textSizePresets[2];

    const auto styleAt = [&](std::uint32_t offset) {
        const cnote::PageText text = harness.canvas->CaptureEditedText();
        for (const cnote::PageTextStyleSpan& span : text.styleSpans) {
            if (span.start <= offset && offset < span.start + span.length) {
                return span.style;
            }
        }
        return cnote::PageTextStyle { text.fontSize, text.color, text.fontFamily, text.bold, text.italic, text.underline };
    };

    // "aa" large, then bold over everything, selected right to left.
    harness.Press(Qt::Key_Home);
    harness.Press(Qt::Key_Right, Qt::ShiftModifier);
    harness.Press(Qt::Key_Right, Qt::ShiftModifier);
    emit harness.toolbar->TextSizePresetChanged(2U);
    harness.Press(Qt::Key_End);
    harness.Press(Qt::Key_Home, Qt::ShiftModifier);
    emit harness.toolbar->TextBoldToggleRequested();
    QApplication::processEvents();
    Check(styleAt(0).bold && styleAt(4).bold, "bold reaches the whole selection made leftwards");
    Check(std::abs(styleAt(0).fontSize - largeSize) < 0.05f && std::abs(styleAt(4).fontSize - baseSize) < 0.05f,
        "bolding text of mixed sizes keeps every run's size");

    // Everything bold: the toggle turns it off everywhere, sizes untouched.
    emit harness.toolbar->TextBoldToggleRequested();
    QApplication::processEvents();
    Check(!styleAt(0).bold && !styleAt(4).bold, "the toggle on an all-bold selection removes bold");
    Check(std::abs(styleAt(0).fontSize - largeSize) < 0.05f, "removing bold keeps the run's size");

    // Italic and a family change over "bb" leave "aa" alone.
    harness.Press(Qt::Key_End);
    harness.Press(Qt::Key_Left, Qt::ShiftModifier);
    harness.Press(Qt::Key_Left, Qt::ShiftModifier);
    emit harness.toolbar->TextItalicToggleRequested();
    emit harness.toolbar->TextFontFamilyChanged(cnote::TextFontFamily::Serif);
    QApplication::processEvents();
    const cnote::PageTextStyle tail = styleAt(4);
    Check(tail.italic && tail.fontFamily == cnote::TextFontFamily::Serif && !styleAt(0).italic
            && styleAt(0).fontFamily != cnote::TextFontFamily::Serif,
        "italic and family apply to the selection only");

    // Reopened, typing at the end continues the last run's style.
    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();
    Check(harness.window->Controller().BeginTextEdit(pageIndex, textIndex), "the text reopens");
    harness.canvas->SyncEditOverlay();
    QApplication::processEvents();
    harness.canvas->TextEditor()->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(harness.window->windowHandle(), 'c');
    QApplication::processEvents();
    const cnote::PageTextStyle typed = styleAt(5);
    Check(harness.canvas->CaptureEditedText().content == u"aa bbc" && typed.italic
            && typed.fontFamily == cnote::TextFontFamily::Serif,
        "typing after reopening continues the style before the caret");
    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();

    // Restore the pending style the format commands persisted.
    emit harness.toolbar->TextItalicToggleRequested();
    emit harness.toolbar->TextFontFamilyChanged(cnote::TextFontFamily::ShantellSans);
    emit harness.toolbar->TextSizePresetChanged(1U);
    QApplication::processEvents();
}

// --- 4. every settings-table shortcut fires ----------------------------------

void SettingsTableShortcutsAreBoundAndFire() {
    Harness harness;
    // Make the document dirty with an undo step so Undo/Save are applicable;
    // Redo, Copy and Cut stay disabled by command state and must stay inert.
    MakeUndoableEdit(harness);
    ShortcutProbe probe;
    probe.Install(*harness.window);

    const std::span<const cnote::settings::ShortcutDefinition> definitions = cnote::settings::ShortcutDefinitions();
    for (const cnote::settings::ShortcutDefinition& definition : definitions) {
        const QKeySequence sequence = cnote::settings::DefaultKeySequence(definition.action);
        if (sequence.isEmpty()) {
            continue;
        }
        std::vector<QAction*> matches;
        for (QAction* action : harness.window->findChildren<QAction*>()) {
            if (action->shortcut() == sequence) {
                matches.push_back(action);
            }
        }
        if (matches.size() != 1) {
            Check(false, "settings binding " + Describe(definition.action) + " ("
                    + sequence.toString(QKeySequence::PortableText).toStdString() + ") is bound exactly once (found "
                    + std::to_string(matches.size()) + ")");
            continue;
        }
        QAction* const target = matches.front();
        for (const bool toolbarFocus : { false, true }) {
            toolbarFocus ? harness.FocusToolbar() : harness.FocusCanvas();
            probe.Reset();
            harness.PressSequence(sequence);
            if (target->isEnabled()) {
                const bool routed = probe.Count() == 1 && probe.Last() == target;
                if (!routed) {
                    std::printf("FAIL %s (%s) focus=%s dispatched=%d target=%s\n",
                        Describe(definition.action).c_str(),
                        sequence.toString(QKeySequence::PortableText).toUtf8().constData(),
                        toolbarFocus ? "toolbar" : "canvas", probe.Count(),
                        probe.Last() != nullptr ? probe.Last()->text().toUtf8().constData() : "<none>");
                }
                Check(routed, "settings shortcut " + Describe(definition.action)
                        + (toolbarFocus ? " fires from toolbar focus" : " fires from canvas focus"));
            } else {
                std::printf("skip %s: disabled by command state\n", Describe(definition.action).c_str());
                Check(probe.Count() == 0, "disabled settings shortcut " + Describe(definition.action) + " stays inert");
            }
        }
    }
}

void UndoRedoFireWhenApplicable() {
    Harness harness;
    MakeUndoableEdit(harness);
    ShortcutProbe probe;
    probe.Install(*harness.window);

    const QKeySequence redoSequence = cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Redo);
    QAction* redoAction = nullptr;
    for (QAction* action : harness.window->findChildren<QAction*>()) {
        if (action->shortcut() == redoSequence) {
            redoAction = action;
        }
    }
    Check(redoAction != nullptr, "the Redo sequence is bound");
    Check(redoAction != nullptr && !redoAction->isEnabled(), "Redo is disabled with an empty redo stack");
    harness.window->Undo();  // performs the undo directly, filling the redo stack
    Check(redoAction != nullptr && redoAction->isEnabled(), "Redo becomes enabled after an undo");
    harness.FocusCanvas();
    probe.Reset();
    harness.PressSequence(redoSequence);
    Check(probe.Count() == 1 && probe.Last() == redoAction, "Ctrl+Y fires Redo once the redo stack is non-empty");
}

void SelectionEnablesCopyAndCutImmediately() {
    Harness harness;
    MakeUndoableEdit(harness);  // one text object exists at page 0, index 0
    const QKeySequence copySequence = cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Copy);
    const QKeySequence cutSequence = cnote::settings::DefaultKeySequence(cnote::settings::ShortcutAction::Cut);
    QAction* copy = FindActionBySequence(*harness.window, copySequence);
    QAction* cut = FindActionBySequence(*harness.window, cutSequence);
    Check(copy != nullptr && cut != nullptr, "copy and cut actions exist");
    Check(copy != nullptr && !copy->isEnabled(), "Copy is disabled with no selection");
    Check(cut != nullptr && !cut->isEnabled(), "Cut is disabled with no selection");

    // The exact case an external harness cannot observe: a selection set through
    // the controller (not a canvas gesture), read back immediately with no
    // intervening viewport/zoom action.
    cnote::ui::PageSelection selection;
    selection.texts.push_back(0U);
    harness.window->Controller().SelectObjects(0U, selection);
    const bool copyEnabled = copy != nullptr && copy->isEnabled();
    const bool cutEnabled = cut != nullptr && cut->isEnabled();
    std::printf("after Controller::SelectObjects: copy=%d cut=%d\n", copyEnabled ? 1 : 0, cutEnabled ? 1 : 0);
    Check(copyEnabled, "a controller-level selection enables Copy immediately");
    Check(cutEnabled, "a controller-level selection enables Cut immediately");

    ShortcutProbe probe;
    probe.Install(*harness.window);
    harness.FocusCanvas();
    probe.Reset();
    harness.PressSequence(copySequence);
    Check(probe.Last() == copy, "Ctrl+C fires immediately after a controller-level selection");

    // Paste selects what it inserted (QtDocumentController::PastePayload), so it
    // must refresh Copy/Cut the same way.
    harness.window->Controller().ClearSelection();
    Check(copy != nullptr && !copy->isEnabled(), "clearing the selection disables Copy again");
    cnote::SelectionClipboardData payload;
    cnote::PageText pastedText;
    pastedText.content = u"p";
    pastedText.x = 10.0f;
    pastedText.y = 20.0f;
    payload.texts.push_back(pastedText);
    payload.minX = 10.0f;
    payload.minY = 20.0f;
    payload.maxX = 30.0f;
    payload.maxY = 40.0f;
    const bool pasted = harness.window->Controller().PastePayload(0U, cnote::Vec2 { 100.0f, 100.0f }, payload);
    std::printf("after PastePayload: pasted=%d copy=%d\n", pasted ? 1 : 0,
        copy != nullptr && copy->isEnabled() ? 1 : 0);
    Check(pasted, "PastePayload inserts the selection payload");
    Check(copy != nullptr && copy->isEnabled(), "a paste that selects what it inserted enables Copy immediately");
}

void TextObjectsCopyPasteThroughShortcuts() {
    Harness harness;
    cnote::SelectionClipboardData payload;
    cnote::PageText text;
    text.content = u"Copy \U0001F600\nrich text";
    text.width = 180.0f;
    text.fontSize = 23.5f;
    text.rotationDegrees = 17.0f;
    text.color = 0xFF123456U;
    text.alignment = cnote::TextAlignment::Center;
    text.fontFamily = cnote::TextFontFamily::Serif;
    text.bold = true;
    text.italic = true;
    text.underline = true;
    text.styleSpans.push_back({ 8U, 4U, { 31.0f, 0xFFABCDEFU,
        cnote::TextFontFamily::Sans, false, true, false } });
    payload.texts.push_back(text);
    payload.maxX = text.width;
    payload.maxY = 70.0f;
    Check(harness.window->Controller().PastePayload(0U, { 200.0f, 200.0f }, payload),
        "the source rich-text object is inserted and selected");
    const cnote::PageText source = harness.window->Controller().DocumentRef().PageAt(0U).texts.front();

    harness.FocusCanvas();
    harness.Press(Qt::Key_C, Qt::ControlModifier);
    harness.Press(Qt::Key_V, Qt::ControlModifier);
    const auto& texts = harness.window->Controller().DocumentRef().PageAt(0U).texts;
    Check(texts.size() == 2U, "Ctrl+C then Ctrl+V duplicates the selected text object");
    if (texts.size() != 2U) {
        return;
    }
    const cnote::PageText& pasted = texts.back();
    Check(pasted.id != source.id && pasted.content == source.content,
        "pasted text has a fresh identity and preserves Unicode and line breaks");
    Check(pasted.width == source.width && pasted.rotationDegrees == source.rotationDegrees,
        "pasted text preserves its width and rotation");
    Check(pasted.fontSize == source.fontSize && pasted.color == source.color
            && pasted.alignment == source.alignment && pasted.fontFamily == source.fontFamily
            && pasted.bold == source.bold && pasted.italic == source.italic
            && pasted.underline == source.underline
            && std::equal(pasted.styleSpans.begin(), pasted.styleSpans.end(),
                source.styleSpans.begin(), source.styleSpans.end(),
                [](const cnote::PageTextStyleSpan& a, const cnote::PageTextStyleSpan& b) {
                    return a.start == b.start && a.length == b.length
                        && a.style.fontSize == b.style.fontSize && a.style.color == b.style.color
                        && a.style.fontFamily == b.style.fontFamily && a.style.bold == b.style.bold
                        && a.style.italic == b.style.italic && a.style.underline == b.style.underline;
                }),
        "pasted text preserves base formatting and rich-text spans");
    harness.window->Undo();
    Check(harness.window->Controller().DocumentRef().PageAt(0U).texts.size() == 1U,
        "undo removes only the pasted text");
    harness.window->Redo();
    Check(harness.window->Controller().DocumentRef().PageAt(0U).texts.size() == 2U,
        "redo restores the pasted text");
}

void ZoomInKeysAreNotDead() {
    Harness harness;
    harness.FocusCanvas();
    const float before = harness.canvas->Zoom();
    harness.Press(Qt::Key_Equal, Qt::ControlModifier);
    const float afterEqual = harness.canvas->Zoom();
    std::printf("zoom %.2f -> Ctrl+= -> %.2f\n", before, afterEqual);
    Check(afterEqual != before, "Ctrl+= (unshifted) zooms in");
    harness.Press(Qt::Key_Plus, Qt::ControlModifier);
    std::printf("Ctrl++ -> %.2f\n", harness.canvas->Zoom());
    Check(harness.canvas->Zoom() != afterEqual, "Ctrl++ still zooms in");
}

void ToolKeysRecoverAfterEditorCloses() {
    Harness harness;
    // The reported scenario: the Text tool is selected, an editor is opened and
    // then closed, and the other tool keys must work again.  No SelectTool call
    // may follow the close, so a stale disabled-gate regression is visible.
    SelectTool(harness, cnote::settings::Tool::Text);
    harness.window->AddTextObject();
    QApplication::processEvents();
    Check(harness.canvas->EditOverlayActive(), "the editor is open");
    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();
    Check(!harness.canvas->EditOverlayActive(), "the committed editor no longer owns the keyboard");
    Check(harness.ActiveTool() == cnote::settings::Tool::Text, "the Text tool is still selected");
    harness.FocusCanvas();
    harness.Press(Qt::Key_D);
    std::printf("after committing the editor, D -> tool=%s\n", ToolName(harness.ActiveTool()));
    Check(harness.ActiveTool() == cnote::settings::Tool::Draw, "tool keys work again after the editor closes");
}

void PageNavigationIsGatedWhileEditing() {
    // Reference: the legacy editor child window consumed PgUp/PgDn (caret/scroll
    // movement), so the page-navigation shortcuts were only reachable from the
    // canvas.  The Qt page actions must stand down the same way while a text or
    // LaTeX editor owns the keyboard.
    Harness harness;
    harness.window->AddPage();  // two pages; current is the inserted page
    QApplication::processEvents();
    const std::size_t secondPage = harness.window->Controller().CurrentPageIndex();
    Check(secondPage == 1U, "the new page is current");

    harness.FocusCanvas();
    harness.Press(Qt::Key_PageUp);
    const std::size_t afterUp = harness.window->Controller().CurrentPageIndex();
    std::printf("canvas PgUp: page %zu -> %zu\n", secondPage, afterUp);
    Check(afterUp == secondPage - 1U, "PgUp navigates from the canvas");
    harness.Press(Qt::Key_PageDown);
    std::printf("canvas PgDn: page %zu\n", harness.window->Controller().CurrentPageIndex());
    Check(harness.window->Controller().CurrentPageIndex() == secondPage, "PgDn navigates from the canvas");

    harness.window->AddTextObject();
    QApplication::processEvents();
    Check(harness.canvas->EditOverlayActive(), "the text edit owns the keyboard");
    const std::size_t beforeTextUp = harness.window->Controller().CurrentPageIndex();
    harness.Press(Qt::Key_PageUp);
    harness.Press(Qt::Key_PageDown);
    std::printf("text-editing PgUp/PgDn: page %zu -> %zu\n", beforeTextUp,
        harness.window->Controller().CurrentPageIndex());
    Check(harness.window->Controller().CurrentPageIndex() == beforeTextUp, "PgUp/PgDn stay inert while editing text");
    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();

    harness.window->AddLatexObject();
    QApplication::processEvents();
    Check(harness.canvas->EditOverlayActive(), "the LaTeX edit owns the keyboard");
    const std::size_t beforeLatexUp = harness.window->Controller().CurrentPageIndex();
    harness.Press(Qt::Key_PageUp);
    harness.Press(Qt::Key_PageDown);
    std::printf("latex-editing PgUp/PgDn: page %zu -> %zu\n", beforeLatexUp,
        harness.window->Controller().CurrentPageIndex());
    Check(harness.window->Controller().CurrentPageIndex() == beforeLatexUp, "PgUp/PgDn stay inert while editing LaTeX");
    harness.canvas->CancelActiveEdit();
    QApplication::processEvents();

    harness.FocusCanvas();
    harness.Press(Qt::Key_PageUp);
    Check(harness.window->Controller().CurrentPageIndex() < secondPage, "PgUp works again after the editor closes");
}

void FocusedPageFollowsViewport() {
    // The focused page is the one filling most of the canvas: scrolling moves
    // the focus (and the sidebar's highlight), drawing on a partly visible
    // neighbour does not.
    Harness harness;
    harness.window->AddPage();
    harness.window->AddPage();
    QApplication::processEvents();
    const auto& controller = harness.window->Controller();
    const QRectF canvasRect(harness.canvas->rect());
    // Pans so page 0's bottom edge sits at `fraction` of the canvas height.
    const auto scrollPageZeroBottomTo = [&](qreal fraction) {
        const qreal bottom = harness.canvas->CalculatePageView(0).rect.bottom();
        harness.canvas->PanBy(QPointF(0.0, canvasRect.top() + (canvasRect.height() * fraction) - bottom));
        QApplication::processEvents();
    };
    Check(harness.canvas->CalculatePageView(0).rect.height() > canvasRect.height(),
        "a page is taller than the canvas at the default zoom");

    scrollPageZeroBottomTo(0.65);
    std::printf("page 0 bottom at 65%%: focused page %zu\n", controller.CurrentPageIndex());
    Check(controller.CurrentPageIndex() == 0U, "scrolling page 0 over most of the canvas focuses it");
    Check(harness.sidebar->CurrentPage() == 0U, "the sidebar highlights the viewport's page");

    SelectTool(harness, cnote::settings::Tool::Draw);
    const QPointF onPageOne(canvasRect.center().x(), canvasRect.top() + (canvasRect.height() * 0.9));
    SendMouse(*harness.canvas, QEvent::MouseButtonPress, onPageOne, Qt::LeftButton, Qt::LeftButton);
    SendMouse(*harness.canvas, QEvent::MouseMove, onPageOne + QPointF(30.0, 0.0), Qt::NoButton, Qt::LeftButton);
    SendMouse(*harness.canvas, QEvent::MouseButtonRelease, onPageOne + QPointF(30.0, 0.0), Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
    Check(controller.DocumentRef().PageAt(1).strokes.size() == 1U, "the stroke lands on the partly visible page 1");
    std::printf("stroke on page 1: focused page %zu\n", controller.CurrentPageIndex());
    Check(controller.CurrentPageIndex() == 0U, "drawing on a minority page keeps the viewport's page focused");

    scrollPageZeroBottomTo(0.35);
    std::printf("page 0 bottom at 35%%: focused page %zu\n", controller.CurrentPageIndex());
    Check(controller.CurrentPageIndex() == 1U, "scrolling page 1 over most of the canvas focuses it");
    Check(harness.sidebar->CurrentPage() == 1U, "the sidebar follows the scroll");
}

void ShiftBuildsAnObjectSelection() {
    // Shift multi-select on the canvas, through real mouse events: Shift-click
    // toggles an object, Shift-drag adds a marquee's objects, and a plain drag
    // on a selected object then moves every selected object.
    Harness harness;
    auto& controller = harness.window->Controller();
    const QRectF canvasRect(harness.canvas->rect());
    const qreal x = canvasRect.center().x();
    const std::array<qreal, 3> rows {
        canvasRect.top() + (canvasRect.height() * 0.3),
        canvasRect.top() + (canvasRect.height() * 0.5),
        canvasRect.top() + (canvasRect.height() * 0.7),
    };
    const auto drag = [&](QPointF from, QPointF to, Qt::KeyboardModifiers modifiers) {
        SendMouse(*harness.canvas, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, modifiers);
        SendMouse(*harness.canvas, QEvent::MouseMove, (from + to) / 2.0, Qt::NoButton, Qt::LeftButton, modifiers);
        SendMouse(*harness.canvas, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, modifiers);
        SendMouse(*harness.canvas, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::processEvents();
    };
    // Clicks land on a stroke but off the selection frame's corner handles,
    // which take precedence over the object under them.
    const auto click = [&](qreal y, Qt::KeyboardModifiers modifiers) {
        drag(QPointF(x - 20.0, y), QPointF(x - 20.0, y), modifiers);
    };
    using Indices = std::vector<std::size_t>;
    const auto selected = [&]() { return controller.SelectionFor(0).strokes; };

    SelectTool(harness, cnote::settings::Tool::Draw);
    for (const qreal y : rows) {
        drag(QPointF(x - 40.0, y), QPointF(x + 40.0, y), Qt::NoModifier);
    }
    Check(controller.DocumentRef().PageAt(0).strokes.size() == 3U, "three strokes to select");

    SelectTool(harness, cnote::settings::Tool::Select);
    harness.canvas->SetSelectionMode(cnote::ui::SelectionMode::Box);
    click(rows[0], Qt::NoModifier);
    Check(selected() == Indices { 0 }, "a plain click selects one stroke");
    click(rows[2], Qt::ShiftModifier);
    std::printf("shift-click stroke 3: %zu selected\n", selected().size());
    Check(selected() == Indices { 0, 2 }, "Shift-click adds a stroke");
    click(rows[0], Qt::ShiftModifier);
    Check(selected() == Indices { 2 }, "Shift-click on a selected stroke removes it");
    drag(QPointF(x - 80.0, rows[1] - 30.0), QPointF(x + 80.0, rows[1] + 30.0), Qt::ShiftModifier);
    std::printf("shift-marquee stroke 2: %zu selected\n", selected().size());
    Check(selected() == Indices { 1, 2 }, "a Shift-marquee adds its strokes");
    click(rows[0], Qt::ShiftModifier);
    Check(selected() == Indices { 0, 1, 2 }, "every stroke is selected");

    // A plain drag on a selected stroke moves the whole selection.
    const auto firstY = [&](std::size_t index) { return controller.DocumentRef().PageAt(0).strokes[index].points.front().y; };
    const std::array<float, 3> before { firstY(0), firstY(1), firstY(2) };
    drag(QPointF(x, rows[1]), QPointF(x, rows[1] + 60.0), Qt::NoModifier);
    bool allMoved = true;
    for (std::size_t index = 0; index < 3U; ++index) {
        allMoved = allMoved && firstY(index) > before[index] + 1.0f;
    }
    Check(allMoved, "dragging one selected stroke moves every selected stroke");

    click(canvasRect.top() + (canvasRect.height() * 0.9), Qt::NoModifier);
    Check(selected().empty(), "a plain click on empty space still replaces the selection");
}

void SidebarPageKeysActOnTheSelection() {
    // With the sidebar owning the keyboard, Delete/Ctrl+C/Ctrl+V act on its
    // multi-page selection, not just the current page.
    Harness harness;
    harness.window->AddPage();
    harness.window->AddPage();
    harness.window->AddPage();
    harness.sidebar->SetOpen(true);
    QApplication::processEvents();
    auto& controller = harness.window->Controller();
    Check(controller.DocumentRef().PageCount() == 4U, "four pages to work on");
    harness.sidebar->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();

    harness.sidebar->SelectPages(1, 2);
    harness.Press(Qt::Key_Delete);
    std::printf("Delete with pages 2-3 selected: %zu pages\n", controller.DocumentRef().PageCount());
    Check(controller.DocumentRef().PageCount() == 2U, "Delete removes every selected page");
    Check(controller.Undo(), "the delete undoes in one step");
    Check(controller.DocumentRef().PageCount() == 4U, "undo restores both pages");

    harness.sidebar->SelectPages(1, 2);
    harness.Press(Qt::Key_C, Qt::ControlModifier);
    harness.Press(Qt::Key_V, Qt::ControlModifier);
    std::printf("Ctrl+C, Ctrl+V with pages 2-3 selected: %zu pages\n", controller.DocumentRef().PageCount());
    Check(controller.DocumentRef().PageCount() == 6U, "Ctrl+C then Ctrl+V duplicates both selected pages");
    Check(harness.sidebar->SelectedPages() == std::vector<std::size_t> { 3, 4 },
        "the pasted pages land after the selection and stay selected");

    harness.Press(Qt::Key_X, Qt::ControlModifier);
    Check(controller.DocumentRef().PageCount() == 4U, "Ctrl+X cuts the selected (pasted) pages");
}

// A quick-size drag applies every pointer sample live; only the release writes
// the settings file, exactly once (PERF-008).
void QuickSizeDragWritesOnRelease() {
    Harness harness;
    const std::filesystem::path path = cnote::settings::SettingsStoreQt::ResolveSettingsPath();

    emit harness.toolbar->BrushSizeChanged(cnote::settings::Tool::Draw, 7.5f);
    QApplication::processEvents();
    {
        FILE* file = std::fopen(path.string().c_str(), "wb");
        Check(file != nullptr, "the scratch settings file can be replaced");
        if (file == nullptr) {
            return;
        }
        std::fputs("sentinel\n", file);
        std::fclose(file);
    }

    // A repeated sample and a changed sample are both still inside the drag:
    // the handler updates the live model but must not touch the file.
    emit harness.toolbar->BrushSizeChanged(cnote::settings::Tool::Draw, 7.5f);
    QApplication::processEvents();
    Check(ReadText(path) == "sentinel\n", "a repeated quick-size tick writes nothing");
    emit harness.toolbar->BrushSizeChanged(cnote::settings::Tool::Draw, 9.0f);
    QApplication::processEvents();
    Check(ReadText(path) == "sentinel\n", "a changed quick-size drag tick writes nothing");

    emit harness.toolbar->BrushSizeCommitted(cnote::settings::Tool::Draw, 9.0f);
    QApplication::processEvents();
    Check(cnote::settings::SettingsStoreQt(path).Load().drawBrushSize == 9.0f,
        "the quick-size release persists the final size");
}

// Closing the window while a settings slider is still held writes the live
// value before the window goes away (PERF-008).
void WindowCloseFlushesHeldSettingsDrag() {
    Harness harness;
    harness.window->SetCloseDecisionProvider([] { return cnote::qt::CloseDecision::Discard; });
    QAction* settingsAction = nullptr;
    for (QAction* action : harness.window->findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("Settings...")) {
            settingsAction = action;
            break;
        }
    }
    Check(settingsAction != nullptr, "the Settings command exists for the close test");
    if (settingsAction == nullptr) {
        return;
    }
    settingsAction->trigger();
    QApplication::processEvents();
    auto* dialog = harness.window->findChild<cnote::ui::SettingsDialogQt*>();
    Check(dialog != nullptr, "the settings dialog opens for the close test");
    if (dialog == nullptr) {
        return;
    }
    auto* spacing = dialog->findChild<QSlider*>(QStringLiteral("spacingSlider"));
    Check(spacing != nullptr, "the spacing slider exists for the close test");
    if (spacing == nullptr) {
        return;
    }

    spacing->setSliderDown(true);
    spacing->setValue(290);
    Check(dialog->settings().inputSpacing == 2.9f, "the settings drag is live before the window closes");

    harness.window->close();
    QApplication::processEvents();
    Check(cnote::settings::SettingsStoreQt(cnote::settings::SettingsStoreQt::ResolveSettingsPath())
                  .Load()
                  .inputSpacing
            == 2.9f,
        "a window close persists a held settings drag");
    spacing->setSliderDown(false);
}

// A transient auto-save bucket must not re-derive the manager's maximum dirty
// age mid-drag: an already-overdue session would auto-save (and report) for a
// value the user is only passing through.  Only the committed release may
// re-arm the deadline (PERF-008, #61).
void AutoSaveDragDoesNotReArmTheManager() {
    Harness harness;
    // Open the dialog through the title menu, as the user does, and start from
    // a known 60 s committed interval regardless of earlier scratch settings.
    QAction* settingsAction = nullptr;
    for (QAction* action : harness.window->findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("Settings...")) {
            settingsAction = action;
            break;
        }
    }
    Check(settingsAction != nullptr, "the Settings command exists for the auto-save test");
    if (settingsAction == nullptr) {
        return;
    }
    settingsAction->trigger();
    QApplication::processEvents();
    auto* dialog = harness.window->findChild<cnote::ui::SettingsDialogQt*>();
    Check(dialog != nullptr, "the settings dialog opens for the auto-save test");
    if (dialog == nullptr) {
        return;
    }
    auto* autoSave = dialog->findChild<QSlider*>(QStringLiteral("autoSaveSlider"));
    Check(autoSave != nullptr, "the auto-save slider exists for the auto-save test");
    if (autoSave == nullptr) {
        return;
    }
    autoSave->setValue(60);
    QApplication::processEvents();
    Check(dialog->settings().autoSaveIntervalSeconds == 60U, "the auto-save test starts from 60 s");

    // A dirty session whose deadline is already overdue: 20 s of dirty age
    // against a 60 s committed interval.  A live 15 s bucket would make the
    // remaining age 0 and fire immediately.
    auto session = std::make_shared<cnote::ProjectSession>();
    session->document.SetFilePath(
        cnote::settings::SettingsStoreQt::ResolveSettingsPath().parent_path() / "autosave-drag-stub.cnote");
    session->dirty = true;
    AutoSaveStubView view;
    harness.manager.RegisterView(&view, &view, session);
    harness.manager.BackdateAutoSaveDirtySinceForTesting(session, std::chrono::seconds(20));
    Check(harness.manager.IsAutoSaveTimerActive(session), "the overdue dirty session is armed");
    int autoSaves = 0;
    harness.manager.SetAutoSaveHandler(
        [&](const std::shared_ptr<cnote::ProjectSession>&, QObject*) { ++autoSaves; });

    autoSave->setSliderDown(true);
    autoSave->setValue(0);
    QApplication::processEvents();
    Check(dialog->settings().autoSaveIntervalSeconds == 60U, "Off is only previewed while dragging");
    Check(harness.manager.AutoSaveInterval() == 60U, "dragging through Off does not push a transient interval");
    Check(harness.manager.IsAutoSaveTimerActive(session), "dragging through Off does not disarm the deadline");
    autoSave->setValue(15);
    QTest::qWait(40);
    Check(dialog->settings().autoSaveIntervalSeconds == 60U, "the transient bucket stays out of the live model");
    Check(harness.manager.AutoSaveInterval() == 60U, "the transient bucket never reaches the manager");
    Check(autoSaves == 0, "a transient 15 s bucket does not auto-save the overdue session");

    autoSave->setSliderDown(false);
    QTest::qWait(60);
    Check(harness.manager.AutoSaveInterval() == 15U, "release pushes the committed bucket to the manager");
    Check(autoSaves == 1, "the committed due bucket auto-saves after release");

    autoSave->setSliderDown(true);
    autoSave->setValue(45);
    autoSave->setSliderDown(false);
    QTest::qWait(60);
    Check(harness.manager.AutoSaveInterval() == 45U, "a longer release commits 45 s");
    Check(autoSaves == 1, "a committed bucket later than the dirty age is not due");

    autoSave->setValue(60);
    harness.manager.UnregisterView(&view);
}

// --- 4b. settings edits only rebuild what they touch -------------------------

void SettingsEditsKeepUnrelatedActions() {
    Harness harness;
    // Open the modeless settings dialog through the title menu's action, the
    // same entry point the user has (ToggleSettings has no default chord).
    QAction* settingsAction = nullptr;
    for (QAction* action : harness.window->findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("Settings...")) {
            settingsAction = action;
            break;
        }
    }
    Check(settingsAction != nullptr, "the shell still exposes the Settings command");
    if (settingsAction == nullptr) {
        return;
    }
    settingsAction->trigger();
    QApplication::processEvents();
    auto* dialog = harness.window->findChild<cnote::ui::SettingsDialogQt*>();
    Check(dialog != nullptr, "the Settings command opens the settings dialog");
    if (dialog == nullptr) {
        return;
    }

    std::vector<QPointer<QAction>> actions;
    for (QAction* action : harness.window->findChildren<QAction*>()) {
        actions.emplace_back(action);
    }
    const auto destroyedActions = [&actions] {
        return std::count_if(actions.begin(), actions.end(), [](const QPointer<QAction>& action) {
            return action.isNull();
        });
    };
    Check(destroyedActions() == 0, "the window starts with one live action set");

    // A drawing-slider tick is not a shortcut change: every action object,
    // including the seven tool actions, must survive it.
    auto* spacing = dialog->findChild<QSlider*>(QStringLiteral("spacingSlider"));
    Check(spacing != nullptr, "the settings dialog exposes the spacing slider");
    if (spacing != nullptr) {
        spacing->setValue(spacing->value() == spacing->maximum() ? spacing->maximum() - 1 : spacing->value() + 1);
        QApplication::processEvents();
        Check(destroyedActions() == 0, "a non-shortcut settings edit keeps the existing shortcut actions");
    }

    // A captured chord still rebinds: the delete/recreate path must run so no
    // stale action keeps the old sequence and makes Qt call it ambiguous.
    dialog->beginShortcutCapture(cnote::settings::ShortcutAction::ToolDraw);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
    QApplication::sendEvent(dialog, &key);
    QApplication::processEvents();
    Check(!dialog->capturingShortcut(), "the captured key finishes capture");
    Check(dialog->settings().shortcuts[static_cast<std::size_t>(cnote::settings::ShortcutAction::ToolDraw)].sequence
            == QKeySequence(Qt::Key_K),
        "the captured chord reaches the live settings model");
    Check(destroyedActions() > 0, "a shortcut edit rebuilds the tool actions");
    Check(FindActionBySequence(*harness.window, QKeySequence(Qt::Key_K)) != nullptr,
        "the remapped chord is bound on the rebuilt tool action");
    dialog->hide();
}

// --- 5. a remapped binding from the settings store ---------------------------

void RemappedBindingIsHonoured() {
    const std::filesystem::path path = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    cnote::settings::Settings settings;
    settings.shortcuts[static_cast<std::size_t>(cnote::settings::ShortcutAction::ToggleWheel)].sequence =
        QKeySequence(QStringLiteral("Ctrl+Shift+U"));
    settings.shortcuts[static_cast<std::size_t>(cnote::settings::ShortcutAction::ToolDraw)].sequence =
        QKeySequence(QStringLiteral("Ctrl+Alt+D"));
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    {
        const std::string text = cnote::settings::SettingsStoreQt::Serialize(settings);
        FILE* file = std::fopen(path.string().c_str(), "wb");
        if (file != nullptr) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
    }

    {
        Harness harness;
        SelectTool(harness, cnote::settings::Tool::Text);
        harness.FocusCanvas();

        // The old default must no longer toggle the wheel...
        harness.Press(Qt::Key_Q, Qt::ControlModifier);
        Check(!harness.toolbar->PaletteWheelVisible(), "the remapped-away Ctrl+Q no longer toggles the wheel");

        // ...but the remapped sequence does.
        harness.Press(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
        std::printf("remapped Ctrl+Shift+U -> wheel=%d\n", harness.toolbar->PaletteWheelVisible() ? 1 : 0);
        Check(harness.toolbar->PaletteWheelVisible(), "the remapped Ctrl+Shift+U toggles the wheel");

        harness.Press(Qt::Key_D, Qt::ControlModifier | Qt::AltModifier);
        std::printf("remapped Ctrl+Alt+D -> tool=%s\n", ToolName(harness.ActiveTool()));
        Check(harness.ActiveTool() == cnote::settings::Tool::Draw, "the remapped tool binding is honoured");
    }

    std::filesystem::remove(path, error);
}

// 5b. A hand-edited file with two actions on the same chord loads as one
// unambiguous runtime binding (the later line wins), not two live shortcuts.
void DuplicateFileBindingsStayUnambiguous() {
    const std::filesystem::path path = cnote::settings::SettingsStoreQt::ResolveSettingsPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    {
        const std::string text =
            "palette=FF000000\n"
            "shortcut.ToolDraw=90,1,0,0\n"
            "shortcut.Redo=90,1,0,0\n";
        FILE* file = std::fopen(path.string().c_str(), "wb");
        if (file != nullptr) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
    }

    {
        Harness harness;
        const QKeySequence chord(Qt::CTRL | Qt::Key_Z);
        QAction* matched = nullptr;
        int matches = 0;
        for (QAction* action : harness.window->findChildren<QAction*>()) {
            if (action->shortcut() == chord) {
                matched = action;
                ++matches;
            }
        }
        std::printf("duplicate file chord Ctrl+Z -> %d action(s), last text='%s'\n",
            matches, matched != nullptr ? matched->text().toUtf8().constData() : "<none>");
        Check(matches == 1, "a duplicate settings chord is bound to exactly one action");
        Check(matched != nullptr && matched->text() == QStringLiteral("Redo"),
            "the later duplicate record is the surviving binding");
    }

    std::filesystem::remove(path, error);
}

// Points the settings store at a scratch directory before any window loads it,
// so the test never reads or writes the developer's real settings file.
void RedirectSettingsStore() {
    const std::filesystem::path scratch = MakeTempDir("routing-settings");
    const QByteArray value(reinterpret_cast<const char*>(scratch.u8string().data()),
        static_cast<qsizetype>(scratch.u8string().size()));
#if defined(Q_OS_WIN)
    qputenv("APPDATA", value);
#else
    qputenv("XDG_CONFIG_HOME", value);
#endif
}

void PendingTextFormatSeedsNewObjects() {
    // Ctrl+W on the Text tool (before any edit exists) edits the pending
    // style: the popup's toggles persist into settings_ and AddTextObject
    // seeds them on the created object.
    Harness harness;
    SelectTool(harness, cnote::settings::Tool::Text);
    harness.FocusCanvas();
    harness.Press(Qt::Key_W, Qt::ControlModifier);
    QApplication::processEvents();
    Check(harness.toolbar->TextFormatPopupVisible(),
        "Ctrl+W opens the text-format popup before placement");

    emit harness.toolbar->TextBoldToggleRequested();
    emit harness.toolbar->TextAlignCycleRequested();
    emit harness.toolbar->TextFontFamilyChanged(cnote::TextFontFamily::Serif);
    emit harness.toolbar->TextSizePresetChanged(2U);
    QApplication::processEvents();

    harness.window->AddTextObject();
    QApplication::processEvents();

    const cnote::PageText* object = harness.window->Controller().EditingTextObject();
    Check(object != nullptr, "AddTextObject leaves the editor open on the new object");
    if (object != nullptr) {
        std::printf("created text: bold=%d align=%d family=%d size=%.1f\n",
            object->bold ? 1 : 0, static_cast<int>(object->alignment),
            static_cast<int>(object->fontFamily), object->fontSize);
        Check(object->bold, "the pending bold flag seeds the created text");
        Check(object->alignment == cnote::TextAlignment::Center,
            "the pending alignment seeds the created text");
        Check(object->fontFamily == cnote::TextFontFamily::Serif,
            "the pending font family seeds the created text");
        // Static size: the Large preset (page units) lands verbatim - no zoom
        // compensation on the stored size.
        Check(std::abs(object->fontSize - harness.toolbar->Settings().textSizePresets[2]) < 0.01f,
            "the pending size preset seeds the created text verbatim");
    }
    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();

    // A fresh window reloads the persisted pending style (same channel the
    // brush tools persist through).
    {
        Harness second;
        const cnote::settings::Settings& reloaded = second.toolbar->Settings();
        Check(reloaded.textBold
                && reloaded.textAlignment == cnote::TextAlignment::Center
                && reloaded.textFontFamily == cnote::TextFontFamily::Serif
                && reloaded.textSizePreset == 2U,
            "the pending text style persists across sessions");
        // Restore the defaults so later checks run against the baseline.
        emit second.toolbar->TextBoldToggleRequested();
        emit second.toolbar->TextAlignCycleRequested();
        emit second.toolbar->TextAlignCycleRequested();
        emit second.toolbar->TextFontFamilyChanged(cnote::TextFontFamily::ShantellSans);
        emit second.toolbar->TextSizePresetChanged(1U);
        QApplication::processEvents();
    }
}

// The popup's alignment while a text is edited aligns the whole object and
// is committed with it; it used to align only the editor's paragraph and
// never reach the model.
void AlignmentChangedWhileEditingIsCommitted() {
    Harness harness;
    harness.window->AddTextObject();
    QApplication::processEvents();
    QTextEdit* editor = harness.canvas->TextEditor();
    Check(editor != nullptr, "AddTextObject opens the editor for the alignment check");
    if (editor == nullptr) {
        return;
    }
    editor->setFocus(Qt::OtherFocusReason);
    for (const char character : std::string("ab")) {
        QTest::keyClick(harness.window->windowHandle(), character);
    }
    QTest::keyClick(harness.window->windowHandle(), Qt::Key_Return);
    QTest::keyClick(harness.window->windowHandle(), 'c');
    QApplication::processEvents();
    const std::size_t pageIndex = harness.window->Controller().EditingPageIndex();
    const std::size_t textIndex = harness.window->Controller().EditingTextIndex().value_or(0U);
    const cnote::TextAlignment before = harness.window->Controller().EditingTextObject()->alignment;

    emit harness.toolbar->TextAlignCycleRequested();
    QApplication::processEvents();
    const cnote::TextAlignment expected = before == cnote::TextAlignment::Left ? cnote::TextAlignment::Center
        : (before == cnote::TextAlignment::Center ? cnote::TextAlignment::Right : cnote::TextAlignment::Left);
    Check(harness.window->Controller().EditingTextObject()->alignment == expected,
        "the alignment command aligns the object under edit");
    const Qt::Alignment shown = editor->document()->defaultTextOption().alignment();
    Check((expected == cnote::TextAlignment::Center && (shown & Qt::AlignHCenter))
            || (expected == cnote::TextAlignment::Right && (shown & Qt::AlignRight))
            || (expected == cnote::TextAlignment::Left && !(shown & (Qt::AlignHCenter | Qt::AlignRight))),
        "the editor shows every paragraph with the new alignment");

    harness.canvas->CommitActiveEdit();
    QApplication::processEvents();
    const cnote::PageText& committed = harness.window->Controller().DocumentRef().PageAt(pageIndex).texts[textIndex];
    Check(committed.alignment == expected && committed.content == u"ab\nc",
        "the commit keeps the alignment chosen while editing");

    // Restore the pending alignment the command persisted (the cycle has three steps).
    for (int step = 0; step < 3 && harness.toolbar->Settings().textAlignment != before; ++step) {
        emit harness.toolbar->TextAlignCycleRequested();
        QApplication::processEvents();
    }
    Check(harness.toolbar->Settings().textAlignment == before, "the pending alignment is restored");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    RedirectSettingsStore();
    // Inline $...$ formulas render only with the engine up (the app's
    // AppManager initializes it from the installed resources).
    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL latex engine did not initialize\n");
        return 1;
    }

    TypingAfterEmptyingKeepsObjectStyle();
    ToolKeysSwitchToolsWhileTextToolActive();
    EditorOwnsToolKeys();
    ToolKeysRecoverAfterEditorCloses();
    PageNavigationIsGatedWhileEditing();
    FocusedPageFollowsViewport();
    SidebarPageKeysActOnTheSelection();
    ShiftBuildsAnObjectSelection();
    QuickActionsFireEverywhere();
    QuickPopupIsTextFormattingWhileEditing();
    TextFormatPopupLeavesFocusWithEditor();
    EscapeCommitsTheEdit();
    PendingTextFormatSeedsNewObjects();
    AlignmentChangedWhileEditingIsCommitted();
    FormatCommandsChangeOnlyTheirProperty();
    EditorKeepsCtrlBWhileEditingText();
    SelectionEnablesCopyAndCutImmediately();
    TextObjectsCopyPasteThroughShortcuts();
    ZoomInKeysAreNotDead();
    SettingsTableShortcutsAreBoundAndFire();
    UndoRedoFireWhenApplicable();
    SettingsEditsKeepUnrelatedActions();
    QuickSizeDragWritesOnRelease();
    WindowCloseFlushesHeldSettingsDrag();
    AutoSaveDragDoesNotReArmTheManager();
    RemappedBindingIsHonoured();
    DuplicateFileBindingsStayUnambiguous();

    if (gFailures != 0) {
        std::printf("%d shortcut-routing check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all shortcut-routing checks passed\n");
    return 0;
}
