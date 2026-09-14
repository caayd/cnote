#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <QMainWindow>
#include <QPoint>
#include <QTextCharFormat>

#include "qt/QtDocumentController.h"
#include "settings/SettingsStoreQt.h"

class QAction;
class QMenu;
class QVBoxLayout;
class QWidget;

namespace cnote {
class AppManager;
}

namespace cnote::ui {
class CanvasWidget;
class NewPageDialogQt;
class SettingsDialogQt;
class SidebarWidget;
class ToolbarWidget;
}

namespace cnote::qt {

// What the close-confirmation seam answers for an unsaved document.
enum class CloseDecision {
    Save,
    Discard,
    Cancel,
};

// Shell for the runnable Qt slice: window, canvas, session registration with the
// AppManager and the file/editing commands the canvas gate exercises.  Chrome,
// settings dialogs and session/AppManager parity for the full app are later
// slices (plan steps 4-7).
class QtMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit QtMainWindow(cnote::AppManager& manager, QWidget* parent = nullptr);
    ~QtMainWindow() override;

    // Loads a project or PDF, joining an already-open session for the same file.
    // Returns false without touching the current document when it cannot load.
    [[nodiscard]] bool OpenPath(const std::filesystem::path& path);

    // Close-confirmation policy seam: the default shows the real
    // Save/Don't Save/Cancel dialog, while tests/harness runs inject a
    // deterministic answer so a headless run can never construct a modal
    // dialog (plan 6.6 close semantics are covered by tests either way).
    void SetCloseDecisionProvider(std::function<CloseDecision()> provider);

    // Headless/test runs (CNOTE_HEADLESS=1) must never block on a modal dialog:
    // informational modals are logged to stderr instead, and the close prompt
    // falls back to Discard unless a test injected a CloseDecisionProvider.
    [[nodiscard]] static bool HeadlessModals();
    // Non-interactive entry points (the packaging --self-check, harnesses) must
    // never construct a modal dialog even without CNOTE_HEADLESS: this forces
    // HeadlessModals() on for the whole process before the window exists.
    static void SetHeadlessModals(bool headless);

    [[nodiscard]] ui::CanvasWidget* Canvas() const { return canvas_; }
    [[nodiscard]] QtDocumentController& Controller() { return controller_; }

public slots:
    void OpenFileDialog();
    // Selection clipboard (plan 6.2: Ctrl+C/X/V through SelectionClipboardQt).
    void CopySelection();
    void CutSelection();
    void PasteSelection();
    // Chrome commands routed from the toolbar widget.
    void ToggleSettingsDialog();
    void ToggleSidebar();
    void AddPage();
    void OpenNewPageSettings();
    void ShowTitleMenu();
    void NewDocument();
    void Save();
    bool SaveAs();
    bool ExportPdf(bool embedProject);
    void Undo();
    void Redo();
    void FitToWindow();
    void ZoomIn();
    void ZoomOut();
    void AddTextObject();
    void AddLatexObject();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    // Windows custom caption (see kUsesCustomCaption): removes the native title
    // bar from the frame and restores the reference's hit testing.
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    // The reference frontend has no native caption on Windows: the band carries
    // the app's own minimize/maximize/close cluster and the title menu, and the
    // whole band hides with the toolbar.  Platforms that keep native decorations
    // must not draw a second set of window buttons inside the client area.
#if defined(Q_OS_WIN)
    static constexpr bool kUsesCustomCaption = true;
#else
    static constexpr bool kUsesCustomCaption = false;
#endif
    // Handles WM_NCCALCSIZE/WM_NCHITTEST; returns true when the message was
    // consumed.  Defined on Windows only.
    bool HandleNativeChromeMessage(void* message, qintptr* result);
    // Maximized state that drives the band layout.  Maximizing never goes
    // through Qt on Windows - double-clicking the band and Aero snap are system
    // paths - so this asks the reference's own source of truth (IsZoomed) first
    // and only falls back to Qt's cached window state.
    [[nodiscard]] bool IsWindowMaximized() const;
    // Feeds the toolbar the DPI-scaled native frame width (LayoutQt is logical,
    // the frame metrics are physical): while maximized the reference insets the
    // caption cluster from the window's right edge by that width because the
    // maximized client area extends past the work area by the frame.
    void UpdateNativeFrameThickness();
    void UpdateChrome();
    // Chrome geometry: the toolbar band and the sidebar host rect (both widgets
    // position themselves; the window only supplies their contract inputs).
    void UpdateChromeGeometry();
    void ApplySettingsToShell();
    // Applies a mutation to the active text editor's char format (the toolbar's
    // format commands; no-op unless a text edit is open).
    void ApplyTextFormat(const std::function<void(QTextCharFormat&)>& apply);
    void ApplyToolToCanvas();
    void ApplyStrokeStyle();
    void RefreshShortcutBindings();
    void RefreshRecentMenu();
    void RecordRecentPath(const std::filesystem::path& path);
    void OpenRecentPath(const std::filesystem::path& path);
    void CreateChrome();
    [[nodiscard]] settings::Tool ActiveTool() const { return activeTool_; }
    void SetActiveTool(settings::Tool tool);
    [[nodiscard]] ui::NewPageDraft CurrentPageDraft() const;

    cnote::AppManager& manager_;
    QtDocumentController controller_;
    ui::CanvasWidget* canvas_ = nullptr;
    // The canvas lives below the reserved toolbar band, exactly like the
    // reference client layout (App::CanvasRect starts under the chrome); the
    // toolbar and sidebar are overlays on the window.
    QWidget* content_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    int reservedTop_ = -1;
    QAction* saveAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* exportFlatAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* addTextAction_ = nullptr;
    QAction* addLatexAction_ = nullptr;
    QAction* settingsAction_ = nullptr;
    QAction* sidebarAction_ = nullptr;
    QAction* quickSizeAction_ = nullptr;
    QAction* wheelAction_ = nullptr;
    QAction* addPageAction_ = nullptr;
    QAction* newPageSettingsAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* pasteAction_ = nullptr;

    settings::SettingsStoreQt settingsStore_;
    settings::Settings settings_ {};
    settings::Tool activeTool_ = settings::Tool::Draw;
    std::vector<std::uint32_t> recentPageColors_ {};
    std::vector<QAction*> toolShortcutActions_ {};

    ui::ToolbarWidget* toolbar_ = nullptr;
    ui::SidebarWidget* sidebar_ = nullptr;
    ui::SettingsDialogQt* settingsDialog_ = nullptr;
    ui::NewPageDialogQt* newPageDialog_ = nullptr;
    QMenu* titleMenu_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    QAction* recentMenuAction_ = nullptr;
    std::function<CloseDecision()> closeDecision_;
    bool startSizeApplied_ = false;

    void ReportError(const QString& text);
};

}  // namespace cnote::qt
