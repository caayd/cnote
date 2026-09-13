#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <QMainWindow>

#include "qt/QtDocumentController.h"

class QAction;
class QLabel;

namespace cnote::ui {
class CanvasWidget;
}

namespace cnote::qt {

// Minimal shell for the migration slice: window, canvas, open/undo/redo/fit
// actions and a status line.  Chrome, dialogs, settings, editing overlays and
// export are later slices (plan steps 5-7).
class QtMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit QtMainWindow(QWidget* parent = nullptr);
    ~QtMainWindow() override;

    // Loads a project or PDF into the session.  Returns false without touching
    // the current document when the file cannot be loaded.
    [[nodiscard]] bool OpenPath(const std::filesystem::path& path);

    [[nodiscard]] ui::CanvasWidget* Canvas() const { return canvas_; }
    [[nodiscard]] QtDocumentController& Controller() { return controller_; }

public slots:
    void OpenFileDialog();
    void Undo();
    void Redo();
    void FitToWindow();
    void ZoomIn();
    void ZoomOut();

private:
    void UpdateChrome();

    QtDocumentController controller_;
    ui::CanvasWidget* canvas_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
};

}  // namespace cnote::qt
