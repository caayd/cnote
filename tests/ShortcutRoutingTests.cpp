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
//   * a selection change enables Copy/Cut immediately (no unrelated UpdateChrome
//     needed) and Ctrl+C then fires;
//   * the unshifted Ctrl+'=' key aliases the Zoom In accelerator;
//   * a binding remapped through the settings store is the one that fires.
//
// Acceptance is observed through the public seam the shell exposes: the canvas
// router's active tool, ToolbarWidget's popup-visibility queries and
// SidebarWidget's IsOpen().  Every wait is bounded; the CTest TIMEOUT bounds the
// process.

#include <algorithm>
#include <array>
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
#include <QKeySequence>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QString>
#include <QTextEdit>
#include <QWindow>
#include <QtTest/QTest>

#include "Unicode.h"
#include "qt/QtMainWindow.h"
#include "settings/KeyBindingQt.h"
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/InteractionRouter.h"
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

void SendMouse(QWidget& widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, position, widget.mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
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

    // Ctrl+W (brush quick popup) belongs to the pen tools; a non-pen tool is
    // inert, exactly like the reference IsToolbarBrushTool gate.
    for (const cnote::settings::Tool tool : tools) {
        const bool brushTool = tool == cnote::settings::Tool::Draw || tool == cnote::settings::Tool::Erase;
        for (const bool toolbarFocus : { false, true }) {
            Harness harness;
            SelectTool(harness, tool);
            toolbarFocus ? harness.FocusToolbar() : harness.FocusCanvas();
            const bool before = harness.toolbar->QuickSizePopupVisible();
            harness.Press(Qt::Key_W, Qt::ControlModifier);
            const bool changed = harness.toolbar->QuickSizePopupVisible() != before;
            std::printf("Ctrl+W tool=%s focus=%s -> quick=%d (brush=%d)\n",
                ToolName(tool), toolbarFocus ? "toolbar" : "canvas",
                harness.toolbar->QuickSizePopupVisible() ? 1 : 0, brushTool ? 1 : 0);
            Check(changed == brushTool, std::string("Ctrl+W ") + (brushTool ? "fires" : "is inert")
                    + std::string(" with ") + ToolName(tool)
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

void EditorKeepsCtrlBWhileEditingText() {
    // Reference precedence (TextEditProc, MessageLoop.cpp): ctrl+B inside a text
    // edit is the editor's bold command, so the sidebar toggle must not steal it.
    Harness harness;
    SelectTool(harness, cnote::settings::Tool::Draw);
    harness.window->AddTextObject();
    QApplication::processEvents();
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
    Check(boldAfter != boldBefore, "Ctrl+B while editing text toggles bold (reference TextEditProc)");

    // In the LaTeX editor the reference leaves Ctrl+B inert.
    Harness latex;
    SelectTool(latex, cnote::settings::Tool::Draw);
    latex.window->AddLatexObject();
    QApplication::processEvents();
    const bool latexSidebarBefore = latex.sidebar->IsOpen();
    latex.Press(Qt::Key_B, Qt::ControlModifier);
    Check(latex.sidebar->IsOpen() == latexSidebarBefore, "Ctrl+B while editing LaTeX is inert");
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

    // Select through the canvas host path (the same path click/lasso selection
    // uses) and read the action state immediately - no viewport/zoom action in
    // between, which is where the enablement used to be refreshed.
    harness.canvas->AddObjectToSelection(cnote::ui::ObjectRef { 0, cnote::ui::ObjectKind::Text, 0 });
    const bool copyEnabled = copy != nullptr && copy->isEnabled();
    const bool cutEnabled = cut != nullptr && cut->isEnabled();
    std::printf("after AddObjectToSelection: copy=%d cut=%d\n", copyEnabled ? 1 : 0, cutEnabled ? 1 : 0);
    Check(copyEnabled, "selecting an object enables Copy immediately");
    Check(cutEnabled, "selecting an object enables Cut immediately");

    ShortcutProbe probe;
    probe.Install(*harness.window);
    harness.FocusCanvas();
    probe.Reset();
    harness.PressSequence(copySequence);
    Check(probe.Last() == copy, "Ctrl+C fires immediately after a selection");

    harness.canvas->ClearSelection();
    Check(copy != nullptr && !copy->isEnabled(), "clearing the selection disables Copy again");
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

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    RedirectSettingsStore();

    ToolKeysSwitchToolsWhileTextToolActive();
    EditorOwnsToolKeys();
    ToolKeysRecoverAfterEditorCloses();
    PageNavigationIsGatedWhileEditing();
    QuickActionsFireEverywhere();
    QuickPopupIsTextFormattingWhileEditing();
    EditorKeepsCtrlBWhileEditingText();
    SelectionEnablesCopyAndCutImmediately();
    ZoomInKeysAreNotDead();
    SettingsTableShortcutsAreBoundAndFire();
    UndoRedoFireWhenApplicable();
    RemappedBindingIsHonoured();

    if (gFailures != 0) {
        std::printf("%d shortcut-routing check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all shortcut-routing checks passed\n");
    return 0;
}
