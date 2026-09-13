#include "qt/QtMainWindow.h"

#include <optional>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolBar>

#include "Unicode.h"
#include "document/SelectionClipboardQt.h"
#include "settings/KeyBindingQt.h"
#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/LayoutQt.h"
#include "ui/NewPageDialogQt.h"
#include "ui/SettingsDialogQt.h"
#include "ui/SidebarWidget.h"
#include "ui/ToolbarWidget.h"
#include "ui/WindowBehaviorQt.h"

namespace cnote::qt {

namespace {

// settings::Tool and ui::Tool mirror each other one for one (settings cannot
// include the router header); the switch keeps a future addition a compile
// error instead of a silent miscast.
ui::Tool ToCanvasTool(settings::Tool tool) {
    switch (tool) {
        case settings::Tool::Select:
            return ui::Tool::Select;
        case settings::Tool::Move:
            return ui::Tool::Move;
        case settings::Tool::Text:
            return ui::Tool::Text;
        case settings::Tool::Latex:
            return ui::Tool::Latex;
        case settings::Tool::Draw:
            return ui::Tool::Draw;
        case settings::Tool::Pan:
            return ui::Tool::Pan;
        case settings::Tool::Erase:
            return ui::Tool::Erase;
        case settings::Tool::Zoom:
            return ui::Tool::Zoom;
    }
    return ui::Tool::Draw;
}

// Strokes take their colour from the active palette slot and their width from
// the tool's brush size (App::CurrentStrokeStyle).
StrokeStyle StrokeStyleFor(
    const settings::Settings& settings, settings::Tool tool, float quickSizeValue) {
    StrokeStyle style {};
    const std::size_t colorIndex = settings.palette.empty()
        ? 0U
        : std::min(settings.colorIndex, settings.palette.size() - 1U);
    if (!settings.palette.empty()) {
        style.color = settings.palette[colorIndex];
    }
    style.width = tool == settings::Tool::Erase ? settings.eraseBrushSize : settings.drawBrushSize;
    if (quickSizeValue > 0.0f && (tool == settings::Tool::Draw || tool == settings::Tool::Erase)) {
        style.width = quickSizeValue;
    }
    style.dotted = false;
    return style;
}

PageTextStyle TextStyleFromEditor(const QTextEdit* editor, TextAlignment& alignment) {
    PageTextStyle style {};
    if (editor == nullptr) {
        alignment = TextAlignment::Left;
        return style;
    }
    const QTextCharFormat format = editor->currentCharFormat();
    const Qt::Alignment current = editor->alignment();
    if (current & Qt::AlignHCenter) {
        alignment = TextAlignment::Center;
    } else if (current & Qt::AlignRight) {
        alignment = TextAlignment::Right;
    } else {
        alignment = TextAlignment::Left;
    }
    if (format.hasProperty(QTextFormat::FontPointSize)) {
        style.fontSize = static_cast<float>(format.fontPointSize());
    }
    if (format.hasProperty(QTextFormat::ForegroundBrush)) {
        style.color = static_cast<std::uint32_t>(format.foreground().color().rgba());
    }
    style.bold = format.fontWeight() >= QFont::Bold;
    style.italic = format.fontItalic();
    style.underline = format.fontUnderline();
    return style;
}

}  // namespace

QtMainWindow::QtMainWindow(cnote::AppManager& manager, QWidget* parent)
    : QMainWindow(parent),
      manager_(manager),
      controller_(manager) {
    setWindowTitle(QStringLiteral("cnote"));
    settings_ = settingsStore_.Load();

    canvas_ = new ui::CanvasWidget(controller_, this);
    setCentralWidget(canvas_);
    canvas_->setMouseTracking(true);

    CreateChrome();
    ApplySettingsToShell();

    statusLabel_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel_);

    connect(canvas_, &ui::CanvasWidget::ViewportChanged, this, &QtMainWindow::UpdateChrome);
    connect(&controller_, &QtDocumentController::SessionChanged, this, [this]() {
        canvas_->NotifyDocumentChanged();
        if (sidebar_ != nullptr) {
            sidebar_->NotifyDocumentChanged();
        }
        UpdateChrome();
    });
    connect(&controller_, &QtDocumentController::EditStateChanged, this, [this]() { UpdateChrome(); });

    canvas_->installEventFilter(this);
    controller_.NewBlankDocument();
    canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
    if (sidebar_ != nullptr) {
        sidebar_->SetDocument(&controller_.DocumentRef(), controller_.PdfBackground());
    }
    UpdateChrome();
}

void QtMainWindow::CreateChrome() {
    // One QAction per command (plan 6.2), shared by the window's shortcuts, the
    // title menu and the ported toolbar widget, which only reports intents.
    QAction* newAction = new QAction(QStringLiteral("New"), this);
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &QtMainWindow::NewDocument);

    QAction* openAction = new QAction(QStringLiteral("Open"), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &QtMainWindow::OpenFileDialog);

    saveAction_ = new QAction(QStringLiteral("Save"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &QtMainWindow::Save);

    QAction* saveAsAction = new QAction(QStringLiteral("Save As"), this);
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this]() { SaveAs(); });

    exportAction_ = new QAction(QStringLiteral("Export PDF"), this);
    connect(exportAction_, &QAction::triggered, this, [this]() { ExportPdf(true); });

    exportFlatAction_ = new QAction(QStringLiteral("Export PDF (no project)"), this);
    connect(exportFlatAction_, &QAction::triggered, this, [this]() { ExportPdf(false); });

    undoAction_ = new QAction(QStringLiteral("Undo"), this);
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, &QtMainWindow::Undo);

    redoAction_ = new QAction(QStringLiteral("Redo"), this);
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, &QtMainWindow::Redo);

    copyAction_ = new QAction(QStringLiteral("Copy"), this);
    copyAction_->setShortcut(QKeySequence::Copy);
    connect(copyAction_, &QAction::triggered, this, &QtMainWindow::CopySelection);

    cutAction_ = new QAction(QStringLiteral("Cut"), this);
    cutAction_->setShortcut(QKeySequence::Cut);
    connect(cutAction_, &QAction::triggered, this, &QtMainWindow::CutSelection);

    pasteAction_ = new QAction(QStringLiteral("Paste"), this);
    pasteAction_->setShortcut(QKeySequence::Paste);
    connect(pasteAction_, &QAction::triggered, this, &QtMainWindow::PasteSelection);

    addTextAction_ = new QAction(QStringLiteral("Add Text"), this);
    connect(addTextAction_, &QAction::triggered, this, &QtMainWindow::AddTextObject);

    addLatexAction_ = new QAction(QStringLiteral("Add Equation"), this);
    connect(addLatexAction_, &QAction::triggered, this, &QtMainWindow::AddLatexObject);

    addPageAction_ = new QAction(QStringLiteral("Add Page"), this);
    connect(addPageAction_, &QAction::triggered, this, &QtMainWindow::AddPage);

    newPageSettingsAction_ = new QAction(QStringLiteral("New Page Settings..."), this);
    connect(newPageSettingsAction_, &QAction::triggered, this, &QtMainWindow::OpenNewPageSettings);

    settingsAction_ = new QAction(QStringLiteral("Settings..."), this);
    connect(settingsAction_, &QAction::triggered, this, &QtMainWindow::ToggleSettingsDialog);

    sidebarAction_ = new QAction(QStringLiteral("Pages Panel"), this);
    sidebarAction_->setCheckable(true);
    connect(sidebarAction_, &QAction::triggered, this, &QtMainWindow::ToggleSidebar);

    quickSizeAction_ = new QAction(QStringLiteral("Brush Size"), this);
    connect(quickSizeAction_, &QAction::triggered, this, [this]() {
        if (toolbar_ != nullptr) {
            toolbar_->ShowQuickSizePopup(activeTool_, QPointF(0.0, 0.0));
        }
    });

    wheelAction_ = new QAction(QStringLiteral("Tool Wheel"), this);
    connect(wheelAction_, &QAction::triggered, this, [this]() {
        if (toolbar_ != nullptr) {
            const QPointF center(canvas_->rect().center());
            toolbar_->TogglePaletteWheel(center);
        }
    });

    QAction* fitAction = new QAction(QStringLiteral("Fit"), this);
    fitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(fitAction, &QAction::triggered, this, &QtMainWindow::FitToWindow);

    QAction* zoomInAction = new QAction(QStringLiteral("Zoom In"), this);
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, this, &QtMainWindow::ZoomIn);

    QAction* zoomOutAction = new QAction(QStringLiteral("Zoom Out"), this);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, &QtMainWindow::ZoomOut);

    QAction* quitAction = new QAction(QStringLiteral("Quit"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, [this]() { close(); });

    const std::vector<QAction*> actions {
        newAction, openAction, saveAction_, saveAsAction, exportAction_, exportFlatAction_,
        undoAction_, redoAction_, copyAction_, cutAction_, pasteAction_,
        addTextAction_, addLatexAction_, addPageAction_, newPageSettingsAction_,
        settingsAction_, sidebarAction_, quickSizeAction_, wheelAction_,
        fitAction, zoomInAction, zoomOutAction, quitAction,
    };
    for (QAction* action : actions) {
        addAction(action);
    }

    // Title menu: the reference caption menu (New/Open/Save/Export/Page/Settings
    // plus the recent documents).
    titleMenu_ = new QMenu(this);
    titleMenu_->addAction(newAction);
    titleMenu_->addAction(openAction);
    recentMenu_ = titleMenu_->addMenu(QStringLiteral("Open Recent"));
    recentMenuAction_ = recentMenu_->menuAction();
    titleMenu_->addSeparator();
    titleMenu_->addAction(saveAction_);
    titleMenu_->addAction(saveAsAction);
    titleMenu_->addAction(exportAction_);
    titleMenu_->addAction(exportFlatAction_);
    titleMenu_->addSeparator();
    titleMenu_->addAction(addPageAction_);
    titleMenu_->addAction(newPageSettingsAction_);
    titleMenu_->addSeparator();
    titleMenu_->addAction(undoAction_);
    titleMenu_->addAction(redoAction_);
    titleMenu_->addAction(copyAction_);
    titleMenu_->addAction(cutAction_);
    titleMenu_->addAction(pasteAction_);
    titleMenu_->addSeparator();
    titleMenu_->addAction(fitAction);
    titleMenu_->addAction(zoomInAction);
    titleMenu_->addAction(zoomOutAction);
    titleMenu_->addAction(sidebarAction_);
    titleMenu_->addAction(quickSizeAction_);
    titleMenu_->addAction(wheelAction_);
    titleMenu_->addSeparator();
    titleMenu_->addAction(settingsAction_);
    titleMenu_->addAction(quitAction);
    RefreshRecentMenu();

    toolbar_ = new ui::ToolbarWidget(this);
    toolbar_->SetCaptionButtonsVisible(false);
    toolbar_->SetSettings(settings_);
    toolbar_->SetActiveTool(activeTool_);
    toolbar_->SetMaximized(isMaximized());
    connect(toolbar_, &ui::ToolbarWidget::CommandTriggered, this, [this](ui::ToolbarCommand command) {
        switch (command) {
            case ui::ToolbarCommand::Undo: Undo(); break;
            case ui::ToolbarCommand::Redo: Redo(); break;
            case ui::ToolbarCommand::Minimize: showMinimized(); break;
            case ui::ToolbarCommand::Maximize: isMaximized() ? showNormal() : showMaximized(); UpdateChrome(); break;
            case ui::ToolbarCommand::Close: close(); break;
            case ui::ToolbarCommand::OpenTitleMenu: ShowTitleMenu(); break;
            case ui::ToolbarCommand::ToggleWheel:
                toolbar_->TogglePaletteWheel(QPointF(canvas_->rect().center()));
                break;
            case ui::ToolbarCommand::OpenPalettePicker: ToggleSettingsDialog(); break;
            case ui::ToolbarCommand::ToggleQuickSize:
                if (toolbar_->QuickSizePopupVisible()) {
                    toolbar_->HideQuickSizePopup();
                } else {
                    toolbar_->ShowQuickSizePopup(activeTool_, toolbar_->ControlRect(ui::ToolbarControl::CurrentColor).center());
                }
                break;
        }
    });
    connect(toolbar_, &ui::ToolbarWidget::ToolSelected, this, [this](settings::Tool tool) {
        SetActiveTool(tool);
    });
    connect(toolbar_, &ui::ToolbarWidget::ColorSelected, this, [this](std::size_t index) {
        settings_.colorIndex = index;
        ApplyStrokeStyle();
        settingsStore_.Save(settings_);
        UpdateChrome();
    });
    connect(toolbar_, &ui::ToolbarWidget::PalettePickerRequested, this, &QtMainWindow::ToggleSettingsDialog);
    connect(toolbar_, &ui::ToolbarWidget::ZoomTextCommitted, this, [this](const QString& text) {
        bool ok = false;
        const double percent = QString(text).remove(QLatin1Char('%')).toDouble(&ok);
        if (ok && percent > 0.0) {
            canvas_->SetZoom(static_cast<float>(percent / 100.0));
        }
        UpdateChrome();
    });
    connect(toolbar_, &ui::ToolbarWidget::ZoomPresetRequested, this, [this](float zoom) {
        canvas_->SetZoom(zoom);
        UpdateChrome();
    });
    connect(toolbar_, &ui::ToolbarWidget::BrushSizeChanged, this, [this](settings::Tool tool, float size) {
        if (tool == settings::Tool::Erase) {
            settings_.eraseBrushSize = size;
        } else {
            settings_.drawBrushSize = size;
        }
        ApplyStrokeStyle();
        settingsStore_.Save(settings_);
        UpdateChrome();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextBoldToggleRequested, this, [this]() {
        ApplyTextFormat([this](QTextCharFormat& format) {
            format.setFontWeight(format.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
        });
    });
    connect(toolbar_, &ui::ToolbarWidget::TextItalicToggleRequested, this, [this]() {
        ApplyTextFormat([](QTextCharFormat& format) { format.setFontItalic(!format.fontItalic()); });
    });
    connect(toolbar_, &ui::ToolbarWidget::TextUnderlineToggleRequested, this, [this]() {
        ApplyTextFormat([](QTextCharFormat& format) { format.setFontUnderline(!format.fontUnderline()); });
    });
    connect(toolbar_, &ui::ToolbarWidget::TextAlignCycleRequested, this, [this]() {
        QTextEdit* editor = canvas_->TextEditor();
        if (editor == nullptr) {
            return;
        }
        const Qt::Alignment current = editor->alignment();
        const Qt::Alignment next = (current & Qt::AlignHCenter)
            ? Qt::AlignRight
            : ((current & Qt::AlignRight) ? Qt::AlignLeft : Qt::AlignHCenter);
        editor->setAlignment(next);
        UpdateChrome();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextSizeChanged, this, [this](float size) {
        ApplyTextFormat([size](QTextCharFormat& format) {
            if (size > 0.0f) {
                format.setFontPointSize(size);
            }
        });
    });
    connect(toolbar_, &ui::ToolbarWidget::TextFontFamilyChanged, this, [this](TextFontFamily family) {
        ApplyTextFormat([family](QTextCharFormat& format) {
            format.setFontFamily(QString::fromStdString(
                family == TextFontFamily::Monospace ? "Consolas"
                                                    : (family == TextFontFamily::Serif ? "Times New Roman" : "Segoe UI")));
        });
    });
    connect(toolbar_, &ui::ToolbarWidget::ToolbarGeometryChanged, this, [this]() { UpdateChromeGeometry(); });
    connect(toolbar_, &ui::ToolbarWidget::RevealProgressChanged, this, [this](qreal) { UpdateChromeGeometry(); });
    connect(toolbar_, &ui::ToolbarWidget::PaletteOverlayToggled, this, [this](bool) { UpdateChrome(); });

    sidebar_ = new ui::SidebarWidget(this);
    sidebar_->SetDocument(&controller_.DocumentRef(), controller_.PdfBackground());
    connect(sidebar_, &ui::SidebarWidget::PageActivated, this, [this](std::size_t pageIndex) {
        canvas_->CenterOnPage(pageIndex);
        controller_.SetCurrentPageIndex(pageIndex);
        sidebar_->SetCurrentPage(pageIndex);
        UpdateChrome();
    });
    connect(sidebar_, &ui::SidebarWidget::PageJumpRequested, this, [this](std::size_t pageIndex) {
        canvas_->CenterOnPage(pageIndex);
        controller_.SetCurrentPageIndex(pageIndex);
        sidebar_->SetCurrentPage(pageIndex);
        UpdateChrome();
    });
    connect(sidebar_, &ui::SidebarWidget::AddPageRequested, this, &QtMainWindow::AddPage);
    connect(sidebar_, &ui::SidebarWidget::NewPageSettingsRequested, this, &QtMainWindow::OpenNewPageSettings);
    connect(sidebar_, &ui::SidebarWidget::OpenChanged, this, [this](bool open) {
        if (sidebarAction_ != nullptr) {
            sidebarAction_->setChecked(open);
        }
        UpdateChrome();
    });
    connect(sidebar_, &ui::SidebarWidget::ScrollChanged, this, [this](int) { UpdateChrome(); });

    RefreshShortcutBindings();
    UpdateChromeGeometry();
}

void QtMainWindow::ApplyTextFormat(const std::function<void(QTextCharFormat&)>& apply) {
    QTextEdit* editor = canvas_->TextEditor();
    if (editor == nullptr || !controller_.EditingText()) {
        return;
    }
    QTextCharFormat format = editor->currentCharFormat();
    apply(format);
    editor->mergeCurrentCharFormat(format);
    UpdateChrome();
}

void QtMainWindow::ApplySettingsToShell() {
    controller_.SetInputSettings(ui::CanvasInputSettings {
        settings_.inputSpacing,
        settings_.inputSmoothing,
        settings_.strokeSmoothing,
    });
    ApplyStrokeStyle();
    ApplyToolToCanvas();
    if (toolbar_ != nullptr) {
        toolbar_->SetSettings(settings_);
        toolbar_->SetActiveTool(activeTool_);
        toolbar_->SetColorIndex(settings_.colorIndex);
        toolbar_->SetCaptionButtonsVisible(false);
        toolbar_->SetAutoHideEnabled(settings_.autoHideToolbar);
        if (!settings_.autoHideToolbar) {
            toolbar_->SetRevealed(true);
        }
    }
    if (sidebar_ != nullptr) {
        sidebar_->SetMaximized(isMaximized());
    }
}

void QtMainWindow::ApplyStrokeStyle() {
    controller_.SetActiveStrokeStyle(StrokeStyleFor(settings_, activeTool_, 0.0f));
}

void QtMainWindow::ApplyToolToCanvas() {
    canvas_->SetTool(ToCanvasTool(activeTool_));
    canvas_->Router().SetTuning(ui::InteractionTuning {
        settings_.inputSpacing,
        settings_.inputSmoothing,
    });
    for (QAction* action : toolShortcutActions_) {
        action->setEnabled(!canvas_->EditOverlayActive());
    }
}

void QtMainWindow::SetActiveTool(settings::Tool tool) {
    activeTool_ = tool;
    ApplyStrokeStyle();
    ApplyToolToCanvas();
    if (toolbar_ != nullptr) {
        toolbar_->SetActiveTool(activeTool_);
    }
    UpdateChrome();
}

void QtMainWindow::RefreshShortcutBindings() {
    // KeyBindingQt is the reference shortcut table; the tool actions are the
    // ones that must stand down while a text/LaTeX editor owns the keyboard
    // (editor precedence, App::ShortcutAction::Tool*).
    toolShortcutActions_.clear();
    const auto toolAction = [this](settings::ShortcutAction action, settings::Tool tool) {
        auto* item = new QAction(this);
        addAction(item);
        item->setShortcut(settings_.shortcuts[static_cast<std::size_t>(action)].sequence);
        connect(item, &QAction::triggered, this, [this, tool]() { SetActiveTool(tool); });
        toolShortcutActions_.push_back(item);
    };
    toolAction(settings::ShortcutAction::ToolDraw, settings::Tool::Draw);
    toolAction(settings::ShortcutAction::ToolSelect, settings::Tool::Select);
    toolAction(settings::ShortcutAction::ToolMove, settings::Tool::Move);
    toolAction(settings::ShortcutAction::ToolText, settings::Tool::Text);
    toolAction(settings::ShortcutAction::ToolErase, settings::Tool::Erase);
    toolAction(settings::ShortcutAction::ToolPan, settings::Tool::Pan);
    for (QAction* action : toolShortcutActions_) {
        action->setEnabled(!canvas_->EditOverlayActive());
    }
}

void QtMainWindow::RecordRecentPath(const std::filesystem::path& path) {
    const bool acceptable = AppManager::HasCnoteExtension(path) || AppManager::HasPdfExtension(path);
    if (!acceptable) {
        return;
    }
    std::error_code absoluteError;
    const std::filesystem::path absolute = std::filesystem::absolute(path, absoluteError);
    const std::filesystem::path normalized = absoluteError ? path : absolute.lexically_normal();

    std::vector<std::filesystem::path>& recents = settings_.recentDocumentPaths;
    recents.erase(
        std::remove_if(recents.begin(), recents.end(), [&](const std::filesystem::path& candidate) {
            return AppManager::PathsReferToSameFile(candidate, normalized);
        }),
        recents.end());
    recents.insert(recents.begin(), normalized);
    constexpr std::size_t kMaxRecentDocuments = 10;
    if (recents.size() > kMaxRecentDocuments) {
        recents.resize(kMaxRecentDocuments);
    }
    settingsStore_.Save(settings_);
    RefreshRecentMenu();
}

void QtMainWindow::RefreshRecentMenu() {
    if (recentMenu_ == nullptr) {
        return;
    }
    recentMenu_->clear();
    for (const std::filesystem::path& path : settings_.recentDocumentPaths) {
        const QString label = QFileInfo(QString::fromStdU16String(path.u16string())).fileName();
        QAction* item = recentMenu_->addAction(label);
        item->setToolTip(QString::fromStdU16String(path.u16string()));
        const std::filesystem::path target = path;
        connect(item, &QAction::triggered, this, [this, target]() { OpenRecentPath(target); });
    }
    recentMenu_->setEnabled(!settings_.recentDocumentPaths.empty());
}

void QtMainWindow::OpenRecentPath(const std::filesystem::path& path) {
    OpenPath(path);
}

ui::NewPageDraft QtMainWindow::CurrentPageDraft() const {
    if (newPageDialog_ != nullptr) {
        return newPageDialog_->draft();
    }
    ui::NewPageDraft draft {};
    const ui::NewPageDefaults defaults = controller_.NewPageDefaults();
    draft.width = defaults.width;
    draft.height = defaults.height;
    draft.backgroundColor = defaults.backgroundColor;
    draft.paletteProfile = defaults.paletteProfile;
    draft.applyPaletteProfileToExistingStrokes = defaults.applyPaletteProfileToExistingStrokes;
    return draft;
}

void QtMainWindow::ToggleSettingsDialog() {
    if (settingsDialog_ == nullptr) {
        settingsDialog_ = new ui::SettingsDialogQt(settings_, settingsStore_, this);
        connect(settingsDialog_, &ui::SettingsDialogQt::settingsChanged, this, [this]() {
            ApplySettingsToShell();
            RefreshShortcutBindings();
            UpdateChrome();
        });
        connect(settingsDialog_, &ui::SettingsDialogQt::settingsSaveFailed, this, [this](const QString& reason) {
            ReportError(QStringLiteral("Could not save settings: %1").arg(reason));
        });
    }
    settingsDialog_->refreshFromSettings();
    settingsDialog_->show();
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}

void QtMainWindow::ToggleSidebar() {
    if (sidebar_ == nullptr) {
        return;
    }
    sidebar_->SetOpen(!sidebar_->IsOpen());
    if (sidebarAction_ != nullptr) {
        sidebarAction_->setChecked(sidebar_->IsOpen());
    }
    UpdateChrome();
}

void QtMainWindow::ShowTitleMenu() {
    if (titleMenu_ == nullptr) {
        return;
    }
    const QPoint anchor = toolbar_ != nullptr
        ? toolbar_->ControlRect(ui::ToolbarControl::TitleMenu).topLeft().toPoint()
        : QPoint(0, 0);
    titleMenu_->popup(mapToGlobal(anchor));
}

void QtMainWindow::AddPage() {
    const std::size_t current = controller_.CurrentPageIndex();
    const std::size_t inserted = controller_.InsertPages(current + 1U, 1U, CurrentPageDraft());
    if (sidebar_ != nullptr) {
        sidebar_->SetDocument(&controller_.DocumentRef(), controller_.PdfBackground());
        sidebar_->NotifyDocumentChanged();
        sidebar_->SetCurrentPage(inserted);
    }
    canvas_->NotifyDocumentChanged();
    canvas_->CenterOnPage(inserted);
    UpdateChrome();
}

void QtMainWindow::OpenNewPageSettings() {
    if (newPageDialog_ == nullptr) {
        newPageDialog_ = new ui::NewPageDialogQt(settings_, this);
        newPageDialog_->setDefaults(controller_.NewPageDefaults(), recentPageColors_);
        connect(newPageDialog_, &ui::NewPageDialogQt::applyRequested, this,
            [this](const ui::NewPageDraft& draft, ui::NewPageApplyScope scope) {
                controller_.ApplyNewPageDraft(draft, scope);
                recentPageColors_ = newPageDialog_->recentColors();
                canvas_->NotifyDocumentChanged();
                UpdateChrome();
            });
        connect(newPageDialog_, &ui::NewPageDialogQt::futureDefaultsChanged, this,
            [this](const ui::NewPageDefaults& defaults, const std::vector<std::uint32_t>& recentColors) {
                controller_.SetNewPageDefaults(defaults);
                recentPageColors_ = recentColors;
            });
        connect(newPageDialog_, &ui::NewPageDialogQt::appendPagesRequested, this, [this]() {
            AddPage();
        });
    }
    newPageDialog_->refreshFromSettings();
    newPageDialog_->setDefaults(controller_.NewPageDefaults(), recentPageColors_);
    newPageDialog_->show();
    newPageDialog_->raise();
    newPageDialog_->activateWindow();
}

void QtMainWindow::CopySelection() {
    cnote::SelectionClipboardData data;
    if (controller_.BuildSelectionPayload(data)) {
        cnote::clipboard::WriteSelection(data);
    }
}

void QtMainWindow::CutSelection() {
    cnote::SelectionClipboardData data;
    if (controller_.BuildSelectionPayload(data) && cnote::clipboard::WriteSelection(data)) {
        controller_.RemoveSelection();
        canvas_->NotifyDocumentChanged();
        UpdateChrome();
    }
}

void QtMainWindow::PasteSelection() {
    const cnote::clipboard::ClipboardContent content = cnote::clipboard::Read();
    switch (content.kind) {
        case cnote::clipboard::ClipboardKind::None:
            return;
        case cnote::clipboard::ClipboardKind::Selection: {
            const Vec2 target = canvas_->ScreenToPage(canvas_->rect().center(), true);
            if (controller_.PastePayload(controller_.CurrentPageIndex(), target, content.selection)) {
                canvas_->NotifyDocumentChanged();
                UpdateChrome();
            }
            return;
        }
        case cnote::clipboard::ClipboardKind::Image: {
            const PageImage image = cnote::clipboard::PageImageFromQImage(content.image);
            if (image.pixelWidth <= 0 || image.pixelHeight <= 0) {
                return;
            }
            cnote::SelectionClipboardData payload;
            payload.images.push_back(image);
            payload.minX = 0.0f;
            payload.minY = 0.0f;
            payload.maxX = static_cast<float>(image.pixelWidth);
            payload.maxY = static_cast<float>(image.pixelHeight);
            const Vec2 target = canvas_->ScreenToPage(canvas_->rect().center(), true);
            if (controller_.PastePayload(controller_.CurrentPageIndex(), target, payload)) {
                canvas_->NotifyDocumentChanged();
                UpdateChrome();
            }
            return;
        }
        case cnote::clipboard::ClipboardKind::Text: {
            if (content.text.isEmpty()) {
                return;
            }
            const std::size_t page = controller_.CurrentPageIndex();
            const Vec2 target = canvas_->ScreenToPage(canvas_->rect().center(), true);
            const std::size_t index = controller_.AddTextObject(page, target, 24.0f);
            PageText text = *controller_.EditingTextObject();
            text.content = Utf8ToUtf16(content.text.toUtf8().toStdString());
            text.styleSpans.clear();
            controller_.CommitTextEdit(text);
            (void)index;
            canvas_->NotifyDocumentChanged();
            UpdateChrome();
            return;
        }
    }
}

QtMainWindow::~QtMainWindow() = default;

bool QtMainWindow::OpenPath(const std::filesystem::path& path) {
    canvas_->CancelStroke();
    canvas_->CancelActiveEdit();
    const auto recordIfOpened = [this](bool opened, const std::filesystem::path& openedPath) {
        if (opened) {
            RecordRecentPath(openedPath);
        }
        return opened;
    };

    // Same-file joins: reuse the shared session instead of decoding twice.
    if (const std::shared_ptr<ProjectSession> existing = manager_.FindOpenSession(path); existing != nullptr) {
        controller_.AttachSession(existing);
        canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
        canvas_->ResetView();  // reference open: zoom 1.0 with panY_ 0
        if (sidebar_ != nullptr) {
            sidebar_->SetDocument(&controller_.DocumentRef(), controller_.PdfBackground());
            sidebar_->NotifyDocumentChanged();
            sidebar_->SetCurrentPage(controller_.CurrentPageIndex());
        }
        UpdateChrome();
        return recordIfOpened(true, path);
    }

    std::string error;
    if (!controller_.OpenFile(path, &error)) {
        ReportError(QStringLiteral("Could not open %1\n%2")
            .arg(QString::fromStdString(path.string()), QString::fromStdString(error)));
        return false;
    }

    canvas_->SetPdfBackgroundProvider(controller_.PdfBackground());
    // Reference open: zoom 1.0 and panY_ 0, i.e. the current page's top sits
    // kTopInset below the canvas top; the view is not centred on open.
    canvas_->ResetView();
    if (sidebar_ != nullptr) {
        sidebar_->SetDocument(&controller_.DocumentRef(), controller_.PdfBackground());
        sidebar_->NotifyDocumentChanged();
        sidebar_->SetCurrentPage(controller_.CurrentPageIndex());
    }
    UpdateChrome();
    return recordIfOpened(true, path);
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
            ReportError(QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
        } else {
            RecordRecentPath(controller_.FilePath());
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
    const std::filesystem::path destination(selected.toStdString());
    if (!controller_.SaveTo(destination, &error)) {
        ReportError(QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
        UpdateChrome();
        return false;
    }
    RecordRecentPath(destination);
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
    const std::filesystem::path destination(selected.toStdString());
    if (!controller_.ExportPdf(destination, embedProject, &error)) {
        ReportError(QStringLiteral("Could not export: %1").arg(QString::fromStdString(error)));
        return false;
    }
    RecordRecentPath(destination);
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
        if (!lastView) {
            // Closing one view of a shared session neither writes the project
            // nor asks anything: the other views keep the unsaved changes.
        } else if (controller_.HasSavedPath()) {
            // Plan 4.5: the last view auto-saves an already-saved project.
            if (!controller_.SaveTo(controller_.FilePath(), &error)) {
                ReportError(QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
                event->ignore();
                return;
            }
        } else {
            // Reference WM_CLOSE -> ConfirmCloseWithUnsavedChanges
            // (MessageLoop.cpp:1083): Save / Don't Save / Cancel, and cancelling
            // keeps this window (and the process) alive.  Applies to untitled
            // projects and to closing one view while others stay open.
            CloseDecision decision = CloseDecision::Cancel;
            if (closeDecision_) {
                decision = closeDecision_();
            } else if (HeadlessModals()) {
                std::fprintf(stderr, "MODAL cnote: Save this cnote? -> Discard (headless)\n");
                decision = CloseDecision::Discard;
            } else {
                const QMessageBox::StandardButton choice = QMessageBox::warning(
                    this,
                    QStringLiteral("cnote"),
                    QStringLiteral("Save this cnote?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                    QMessageBox::Save);
                decision = choice == QMessageBox::Save
                    ? CloseDecision::Save
                    : (choice == QMessageBox::Discard ? CloseDecision::Discard : CloseDecision::Cancel);
            }
            if (decision == CloseDecision::Cancel) {
                event->ignore();
                return;
            }
            if (decision == CloseDecision::Save) {
                const bool saved = controller_.HasSavedPath()
                    ? controller_.SaveTo(controller_.FilePath(), &error)
                    : SaveAs();
                if (!saved) {
                    if (!error.empty()) {
                        ReportError(QStringLiteral("Could not save: %1").arg(QString::fromStdString(error)));
                    }
                    event->ignore();
                    return;
                }
            }
        }
    }

    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        // The window may be shown again; re-registering on show restores it.
        controller_.DetachForClose();
    }
}

void QtMainWindow::SetCloseDecisionProvider(std::function<CloseDecision()> provider) {
    closeDecision_ = std::move(provider);
}

bool QtMainWindow::HeadlessModals() {
    static const bool headless = qEnvironmentVariableIsSet("CNOTE_HEADLESS");
    return headless;
}

void QtMainWindow::ReportError(const QString& text) {
    if (HeadlessModals()) {
        std::fprintf(stderr, "MODAL cnote: %s\n", text.toUtf8().constData());
        return;
    }
    QMessageBox::critical(this, QStringLiteral("cnote"), text);
}

void QtMainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    controller_.ReattachForShow();
    if (!startSizeApplied_) {
        startSizeApplied_ = true;
        if (const QScreen* windowScreen = screen(); windowScreen != nullptr) {
            const QSize start = ui::PreferredStartSize(windowScreen->availableGeometry());
            if (start.isValid() && start.width() > 0 && start.height() > 0) {
                resize(start);
            }
        }
    }
    UpdateChromeGeometry();
}

void QtMainWindow::UpdateChromeGeometry() {
    const int band = qRound(cnote::appui::qt::ToolbarHeight(isMaximized()));
    const int reserved = qRound(cnote::appui::qt::ReservedToolbarHeight(
        toolbar_ != nullptr && toolbar_->AutoHideEnabled(), isMaximized()));
    if (toolbar_ != nullptr) {
        toolbar_->SetMaximized(isMaximized());
        // The widget owns the 120 ms reveal offset; the window only supplies the
        // band's width and the maximized state.
        toolbar_->setGeometry(0, qRound(toolbar_->ToolbarOffsetY()), width(), band);
        toolbar_->raise();
    }
    if (sidebar_ != nullptr) {
        sidebar_->SetMaximized(isMaximized());
        sidebar_->SetHostClientRect(rect(), reserved);
        sidebar_->raise();
    }
}

void QtMainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    UpdateChromeGeometry();
}

void QtMainWindow::mouseMoveEvent(QMouseEvent* event) {
    QMainWindow::mouseMoveEvent(event);
    if (toolbar_ != nullptr) {
        toolbar_->UpdateAutoHideForCursor(event->position(), canvas_->ActiveInteraction() != ui::InteractionKind::None);
    }
}

bool QtMainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == canvas_ && toolbar_ != nullptr) {
        switch (event->type()) {
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease: {
                auto* mouse = static_cast<QMouseEvent*>(event);
                const bool interactionActive = canvas_->ActiveInteraction() != ui::InteractionKind::None;
                toolbar_->UpdateAutoHideForCursor(
                    canvas_->mapTo(this, mouse->position().toPoint()),
                    interactionActive);
                break;
            }
            case QEvent::Leave:
                toolbar_->UpdateAutoHideForCursor(QPointF(-1.0, -1.0), false);
                break;
            default:
                break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void QtMainWindow::UpdateChrome() {
    const Document& document = controller_.DocumentRef();
    const std::size_t pages = document.PageCount();
    const std::size_t current = controller_.CurrentPageIndex();
    const QString fileLabel = document.FilePath().empty()
        ? QStringLiteral("Untitled")
        : QFileInfo(QString::fromStdU16String(document.FilePath().u16string())).fileName();
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(QStringLiteral("%1  |  %2 page(s)  |  %3%  |  %4")
            .arg(fileLabel)
            .arg(pages)
            .arg(canvas_ != nullptr ? qRound(canvas_->Zoom() * 100.0) : 100)
            .arg(controller_.HasSelection() ? QStringLiteral("selection") : QStringLiteral("no selection")));
    }
    if (saveAction_ != nullptr) {
        saveAction_->setEnabled(controller_.Dirty());
    }
    if (undoAction_ != nullptr) {
        undoAction_->setEnabled(controller_.CanUndo());
    }
    if (redoAction_ != nullptr) {
        redoAction_->setEnabled(controller_.CanRedo());
    }
    if (copyAction_ != nullptr) {
        const bool hasSelection = controller_.HasSelection();
        copyAction_->setEnabled(hasSelection);
        cutAction_->setEnabled(hasSelection);
    }
    if (toolbar_ != nullptr) {
        toolbar_->SetUndoRedoEnabled(controller_.CanUndo(), controller_.CanRedo());
        toolbar_->SetTitle(fileLabel);
        toolbar_->SetZoomLabel(QStringLiteral("%1%").arg(canvas_ != nullptr ? qRound(canvas_->Zoom() * 100.0) : 100));
        toolbar_->SetMaximized(isMaximized());
        toolbar_->SetPageScale(canvas_ != nullptr ? canvas_->Zoom() : 1.0);
        toolbar_->SetColorIndex(settings_.colorIndex);
        toolbar_->SetTextFormatAvailable(controller_.EditingText());
        if (controller_.EditingText()) {
            TextAlignment alignment = TextAlignment::Left;
            const PageTextStyle style = TextStyleFromEditor(canvas_ != nullptr ? canvas_->TextEditor() : nullptr, alignment);
            toolbar_->SetTextFormatState(style, alignment);
        } else {
            toolbar_->SetTextFormatState(std::nullopt, TextAlignment::Left);
        }
    }
    if (sidebar_ != nullptr) {
        sidebar_->SetDocument(&document, controller_.PdfBackground());
        sidebar_->SetCurrentPage(current);
        if (sidebarAction_ != nullptr) {
            sidebarAction_->setChecked(sidebar_->IsOpen());
        }
    }
    if (newPageDialog_ != nullptr) {
        newPageDialog_->setDefaults(controller_.NewPageDefaults(), recentPageColors_);
    }
    // Editor focus precedence: the single-letter tool shortcuts stand down while
    // a text/LaTeX editor owns the keyboard (App::ShortcutAction::Tool*).
    for (QAction* action : toolShortcutActions_) {
        action->setEnabled(!canvas_->EditOverlayActive());
    }
    if (toolbar_ != nullptr) {
        toolbar_->SetAutoHideEnabled(settings_.autoHideToolbar);
    }
}

}  // namespace cnote::qt
