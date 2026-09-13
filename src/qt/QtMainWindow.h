#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <QMainWindow>

#include "qt/QtDocumentController.h"

class QAction;
class QLabel;

namespace cnote {
class AppManager;
}

namespace cnote::ui {
class CanvasWidget;
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

    [[nodiscard]] ui::CanvasWidget* Canvas() const { return canvas_; }
    [[nodiscard]] QtDocumentController& Controller() { return controller_; }

public slots:
    void OpenFileDialog();
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

private:
    void UpdateChrome();

    cnote::AppManager& manager_;
    QtDocumentController controller_;
    ui::CanvasWidget* canvas_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* exportFlatAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* addTextAction_ = nullptr;
    QAction* addLatexAction_ = nullptr;
    std::function<CloseDecision()> closeDecision_;

    void ReportError(const QString& text);
};

}  // namespace cnote::qt
