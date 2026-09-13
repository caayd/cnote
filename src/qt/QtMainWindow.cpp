#include "qt/QtMainWindow.h"

#include <optional>
#include <utility>

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/LayoutQt.h"

namespace cnote::qt {

QtMainWindow::QtMainWindow(cnote::AppManager& manager, QWidget* parent)
    : QMainWindow(parent),
      manager_(manager),
      controller_(manager) {
    setWindowTitle(QStringLiteral("cnote"));

    canvas_ = new ui::CanvasWidget(controller_, this);
    setCentralWidget(canvas_);

    QToolBar* toolbar = addToolBar(QStringLiteral("cnote"));
    toolbar->setMovable(false);
    // Reference chrome band (plan 6.1): the legacy 34 logical-pixel toolbar.
    toolbar->setFixedHeight(qRound(cnote::appui::qt::ToolbarHeight(isMaximized())));

    QAction* newAction = toolbar->addAction(QStringLiteral("New"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &QtMainWindow::NewDocument);

    QAction* openAction = toolbar->addAction(QStringLiteral("Open"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &QtMainWindow::OpenFileDialog);

    saveAction_ = toolbar->addAction(QStringLiteral("Save"));
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &QtMainWindow::Save);

    QAction* saveAsAction = toolbar->addAction(QStringLiteral("Save As"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this]() { SaveAs(); });

    exportAction_ = toolbar->addAction(QStringLiteral("Export PDF"));
    connect(exportAction_, &QAction::triggered, this, [this]() { ExportPdf(true); });

    exportFlatAction_ = toolbar->addAction(QStringLiteral("Export PDF (no project)"));
    connect(exportFlatAction_, &QAction::triggered, this, [this]() { ExportPdf(false); });

    toolbar->addSeparator();
    undoAction_ = toolbar->addAction(QStringLiteral("Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, &QtMainWindow::Undo);

    redoAction_ = toolbar->addAction(QStringLiteral("Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, &QtMainWindow::Redo);

    toolbar->addSeparator();
    addTextAction_ = toolbar->addAction(QStringLiteral("Add Text"));
    connect(addTextAction_, &QAction::triggered, this, &QtMainWindow::AddTextObject);

    addLatexAction_ = toolbar->addAction(QStringLiteral("Add Equation"));
    connect(addLatexAction_, &QAction::triggered, this, &QtMainWindow::AddLatexObject);

    toolbar->addSeparator();
    QAction* fitAction = toolbar->addAction(QStringLiteral("Fit"));
    fitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(fitAction, &QAction::triggered, this, &QtMainWindow::FitToWindow);

    QAction* zoomInAction = toolbar->addAction(QStringLiteral("Zoom In"));
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, this, &QtMainWindow::ZoomIn);

    QAction* zoomOutAction = toolbar->addAction(QStringLiteral("Zoom Out"));
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, &QtMainWindow::ZoomOut);

    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);

    connect(canvas_, &ui::CanvasWidget::ViewportChanged, this, &QtMainWindow::UpdateChrome);
    connect(&controller_, &QtDocumentController::SessionChanged, this, [this]() {
        canvas_->NotifyDocumentChanged();
        UpdateChrome();
    });
    connect(&controller_, &QtDocumentController::EditStateChanged, this, [this]() { UpdateChrome(); });

    controller_.NewBlankDocument();
    canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
    UpdateChrome();
}

QtMainWindow::~QtMainWindow() = default;

bool QtMainWindow::OpenPath(const std::filesystem::path& path) {
    canvas_->CancelStroke();
    canvas_->CancelActiveEdit();

    // Same-file joins: reuse the shared session instead of decoding twice.
    if (const std::shared_ptr<ProjectSession> existing = manager_.FindOpenSession(path); existing != nullptr) {
        controller_.AttachSession(existing);
        canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
        canvas_->ResetView();  // reference open: zoom 1.0 with panY_ 0
        UpdateChrome();
        return true;
    }

    std::string error;
    if (!controller_.OpenFile(path, &error)) {
        QMessageBox::critical(
            this,
            QStringLiteral("cnote"),
            QStringLiteral("Could not open %1\n%2")
                .arg(QString::fromStdString(path.string()), QString::fromStdString(error)));
        return false;
    }

    canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
    // Reference open: zoom 1.0 and panY_ 0, i.e. the current page's top sits
    // kTopInset below the canvas top; the view is not centred on open.
    canvas_->ResetView();
    UpdateChrome();
    return true;
}

void QtMainWindow::OpenFileDialog() {
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open"),
        QString(),
        QStringLiteral("cnote projects and PDFs (*.cnote *.pdf);;All files (*)"));
    if (!selected.isEmpty()) {
        static_cast<void>(OpenPath(std::filesystem::path(selected.toStdString())));
    }
}

void QtMainWindow::NewDocument() {
    canvas_->CancelStroke();
    canvas_->CancelActiveEdit();
    controller_.NewBlankDocument();
    canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
    canvas_->ResetView();
    UpdateChrome();
}

void QtMainWindow::Save() {
    if (controller_.HasSavedPath()) {
        std::string error;
        if (!controller_.SaveTo(controller_.FilePath(), &error)) {
            QMessageBox::critical(
                this, QStringLiteral("cnote"),
                QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
        }
        UpdateChrome();
        return;
    }
    SaveAs();
}

bool QtMainWindow::SaveAs() {
    const QString selected = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save As"),
        QString(),
        QStringLiteral("cnote project (*.cnote)"));
    if (selected.isEmpty()) {
        return false;
    }
    std::string error;
    if (!controller_.SaveTo(std::filesystem::path(selected.toStdString()), &error)) {
        QMessageBox::critical(
            this, QStringLiteral("cnote"),
            QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
        UpdateChrome();
        return false;
    }
    UpdateChrome();
    return true;
}

bool QtMainWindow::ExportPdf(bool embedProject) {
    canvas_->CommitActiveEdit();
    const QString selected = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export PDF"),
        QString(),
        QStringLiteral("PDF (*.pdf)"));
    if (selected.isEmpty()) {
        return false;
    }
    std::string error;
    if (!controller_.ExportPdf(std::filesystem::path(selected.toStdString()), embedProject, &error)) {
        QMessageBox::critical(
            this, QStringLiteral("cnote"),
            QStringLiteral("Could not export: %1").arg(QString::fromStdString(error)));
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Exported %1").arg(selected), 4000);
    return true;
}

void QtMainWindow::Undo() {
    canvas_->CancelStroke();
    if (controller_.Undo()) {
        canvas_->NotifyDocumentChanged();
        UpdateChrome();
    }
}

void QtMainWindow::Redo() {
    canvas_->CancelStroke();
    if (controller_.Redo()) {
        canvas_->NotifyDocumentChanged();
        UpdateChrome();
    }
}

void QtMainWindow::FitToWindow() {
    canvas_->FitToWindow();
    UpdateChrome();
}

void QtMainWindow::ZoomIn() {
    canvas_->ZoomBySteps(1, canvas_->rect().center());
    UpdateChrome();
}

void QtMainWindow::ZoomOut() {
    canvas_->ZoomBySteps(-1, canvas_->rect().center());
    UpdateChrome();
}

void QtMainWindow::AddTextObject() {
    const std::optional<ui::CanvasPageView> hit = canvas_->HitTestPageView(canvas_->rect().center());
    const std::size_t pageIndex = hit.has_value() ? hit->pageIndex : controller_.CurrentPageIndex();
    const Vec2 point = hit.has_value()
        ? canvas_->ScreenToPage(*hit, canvas_->rect().center(), false)
        : Vec2 { 60.0f, 80.0f };
    controller_.AddTextObject(pageIndex, point, 24.0f);
    canvas_->SyncEditOverlay();
    UpdateChrome();
}

void QtMainWindow::AddLatexObject() {
    const std::optional<ui::CanvasPageView> hit = canvas_->HitTestPageView(canvas_->rect().center());
    const std::size_t pageIndex = hit.has_value() ? hit->pageIndex : controller_.CurrentPageIndex();
    const Vec2 point = hit.has_value()
        ? canvas_->ScreenToPage(*hit, canvas_->rect().center(), false)
        : Vec2 { 60.0f, 120.0f };
    controller_.AddLatexObject(pageIndex, point, 28.0f);
    canvas_->SyncEditOverlay();
    UpdateChrome();
}

void QtMainWindow::closeEvent(QCloseEvent* event) {
    canvas_->CancelStroke();
    if (canvas_->EditOverlayActive()) {
        canvas_->CommitActiveEdit();
    }

    if (controller_.Dirty()) {
        const std::shared_ptr<ProjectSession>& session = controller_.Session();
        const std::size_t viewCount = manager_.ViewsForSession(session).size();
        const bool lastView = viewCount <= 1U;

        std::string error;
        if (lastView && controller_.HasSavedPath()) {
            // Plan 4.5: the last view auto-saves an already-saved project.
            if (!controller_.SaveTo(controller_.FilePath(), &error)) {
                QMessageBox::critical(
                    this, QStringLiteral("cnote"),
                    QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
                event->ignore();
                return;
            }
        } else {
            // Reference WM_CLOSE -> ConfirmCloseWithUnsavedChanges
            // (MessageLoop.cpp:1083): Save / Don't Save / Cancel, and cancelling
            // keeps this window (and the process) alive.  Applies to untitled
            // projects and to closing one view while others stay open.
            const QMessageBox::StandardButton choice = QMessageBox::warning(
                this,
                QStringLiteral("cnote"),
                QStringLiteral("Save this cnote?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                QMessageBox::Save);
            if (choice == QMessageBox::Cancel) {
                event->ignore();
                return;
            }
            if (choice == QMessageBox::Save) {
                const bool saved = controller_.HasSavedPath()
                    ? controller_.SaveTo(controller_.FilePath(), &error)
                    : SaveAs();
                if (!saved) {
                    if (!error.empty()) {
                        QMessageBox::critical(
                            this, QStringLiteral("cnote"),
                            QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
                    }
                    event->ignore();
                    return;
                }
            }
        }
    }

    QMainWindow::closeEvent(event);
}

void QtMainWindow::UpdateChrome() {
    const Document& document = controller_.DocumentRef();
    const QString name = controller_.FilePath().empty()
        ? QStringLiteral("untitled")
        : QFileInfo(QString::fromStdString(controller_.FilePath().string())).fileName();
    const QString dirty = controller_.Dirty() ? QStringLiteral("*") : QString();

    const bool editing = canvas_->EditOverlayActive();
    // Native undo inside the active editor stays local until commit, so the
    // window-level Undo/Redo must not steal Ctrl+Z while an overlay is open.
    undoAction_->setEnabled(!editing && controller_.CanUndo());
    redoAction_->setEnabled(!editing && controller_.CanRedo());
    saveAction_->setEnabled(!editing);
    exportAction_->setEnabled(!editing);
    exportFlatAction_->setEnabled(!editing);
    addTextAction_->setEnabled(!editing);
    addLatexAction_->setEnabled(!editing);

    statusLabel_->setText(
        QStringLiteral("%1%2  |  %3 page(s)  |  %4%5")
            .arg(dirty, name)
            .arg(document.PageCount())
            .arg(qRound(canvas_->Zoom() * 100.0))
            .arg(editing ? QStringLiteral("  |  editing") : QString()));
}

}  // namespace cnote::qt
