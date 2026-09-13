#include "qt/QtMainWindow.h"

#include <optional>
#include <utility>

#include <QAction>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>

#include "qt/QtDocumentController.h"
#include "ui/CanvasWidget.h"

namespace cnote::qt {

QtMainWindow::QtMainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("cnote"));

    canvas_ = new ui::CanvasWidget(controller_, this);
    setCentralWidget(canvas_);

    QToolBar* toolbar = addToolBar(QStringLiteral("cnote"));
    toolbar->setMovable(false);

    QAction* openAction = toolbar->addAction(QStringLiteral("Open"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &QtMainWindow::OpenFileDialog);

    undoAction_ = toolbar->addAction(QStringLiteral("Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, &QtMainWindow::Undo);

    redoAction_ = toolbar->addAction(QStringLiteral("Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, &QtMainWindow::Redo);

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
    UpdateChrome();
}

QtMainWindow::~QtMainWindow() = default;

bool QtMainWindow::OpenPath(const std::filesystem::path& path) {
    canvas_->CancelStroke();

    std::string error;
    if (!controller_.OpenFile(path, &error)) {
        QMessageBox::critical(
            this,
            QStringLiteral("cnote"),
            QStringLiteral("Could not open %1\n%2")
                .arg(QString::fromStdString(path.string()), QString::fromStdString(error)));
        return false;
    }

    // PDF page backgrounds are rendered lazily by the controller's provider.
    canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
    canvas_->ResetView();
    canvas_->NotifyDocumentChanged();
    canvas_->CenterOnPage(controller_.DocumentRef().CurrentPageIndex());
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
        OpenPath(std::filesystem::path(selected.toStdString()));
    }
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

void QtMainWindow::UpdateChrome() {
    const Document& document = controller_.DocumentRef();
    const QString name = controller_.FilePath().empty()
        ? QStringLiteral("untitled")
        : QFileInfo(QString::fromStdString(controller_.FilePath().string())).fileName();
    const QString dirty = controller_.Dirty() ? QStringLiteral("*") : QString();

    undoAction_->setEnabled(controller_.CanUndo());
    redoAction_->setEnabled(controller_.CanRedo());

    statusLabel_->setText(
        QStringLiteral("%1%2  |  %3 page(s)  |  %4%")
            .arg(dirty, name)
            .arg(document.PageCount())
            .arg(qRound(canvas_->Zoom() * 100.0)));
}

}  // namespace cnote::qt
