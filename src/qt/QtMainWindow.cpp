#include "qt/QtMainWindow.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScreen>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWindow>

#include "Unicode.h"
#include "document/SelectionClipboardQt.h"
#include "settings/KeyBindingQt.h"
#include "shell/AppManager.h"
#include "text/TextEditingQt.h"
#include "ui/CanvasWidget.h"
#include "ui/LayoutQt.h"
#include "ui/NewPageDialogQt.h"
#include "ui/SettingsDialogQt.h"
#include "ui/SidebarWidget.h"
#include "ui/ToolbarWidget.h"
#include "ui/WindowBehaviorQt.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#include <windowsx.h>

// windows.h only declares the per-DPI metrics/DPI surface when WINVER >= 0x0605,
// and cnote_ui pins _WIN32_WINNT=0x0602 for its Windows 8 API surface.  Qt 6.11
// requires Windows 10, where both of these user32 exports exist on every
// supported system, so they are declared here for this translation unit only
// instead of raising the target version for the whole library.
extern "C" {
WINUSERAPI UINT WINAPI GetDpiForWindow(HWND hwnd);
WINUSERAPI int WINAPI GetSystemMetricsForDpi(int nIndex, UINT dpi);
}
#endif

namespace cnote::qt {

namespace {

#if defined(Q_OS_WIN)
// Reference WindowFrameThicknessX/Y (Layout.cpp:110-113, MessageLoop.cpp:68-70):
// the resize border plus the padded border at the window's DPI.
int NativeFrameThicknessX(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

int NativeFrameThicknessY(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT HitCodeFor(ui::WindowChromeHit hit) {
    switch (hit) {
        case ui::WindowChromeHit::Caption: return HTCAPTION;
        case ui::WindowChromeHit::MinButton: return HTMINBUTTON;
        case ui::WindowChromeHit::MaxButton: return HTMAXBUTTON;
        case ui::WindowChromeHit::CloseButton: return HTCLOSE;
        case ui::WindowChromeHit::Top: return HTTOP;
        case ui::WindowChromeHit::Bottom: return HTBOTTOM;
        case ui::WindowChromeHit::Left: return HTLEFT;
        case ui::WindowChromeHit::Right: return HTRIGHT;
        case ui::WindowChromeHit::TopLeft: return HTTOPLEFT;
        case ui::WindowChromeHit::TopRight: return HTTOPRIGHT;
        case ui::WindowChromeHit::BottomLeft: return HTBOTTOMLEFT;
        case ui::WindowChromeHit::BottomRight: return HTBOTTOMRIGHT;
        case ui::WindowChromeHit::Client: break;
    }
    return HTCLIENT;
}

ui::ToolbarControl HitCodeControl(LRESULT code) {
    switch (code) {
        case HTMINBUTTON: return ui::ToolbarControl::Minimize;
        case HTMAXBUTTON: return ui::ToolbarControl::Maximize;
        case HTCLOSE: return ui::ToolbarControl::Close;
        default: return ui::ToolbarControl::None;
    }
}
#endif

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
    // fontFamily + the weight flags come from the shared capture mapping (its
    // page-unit fontSize is overwritten below: the popup shows the editor's
    // screen point size, same as the legacy size box).
    style = text::StyleForCharFormat(format, style, 1.0f);
    if (format.hasProperty(QTextFormat::FontPointSize)) {
        style.fontSize = static_cast<float>(format.fontPointSize());
    }
    return style;
}

}  // namespace

QtMainWindow::QtMainWindow(cnote::AppManager& manager, QWidget* parent)
    : QMainWindow(parent),
      manager_(manager),
      controller_(manager) {
    setWindowTitle(QStringLiteral("cnote"));
    settings_ = settingsStore_.Load();

    content_ = new QWidget(this);
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);
    canvas_ = new ui::CanvasWidget(controller_, content_);
    canvas_->setMouseTracking(true);
    contentLayout_->addWidget(canvas_);
    setCentralWidget(content_);

    CreateChrome();
    ApplySettingsToShell();

#if defined(Q_OS_WIN)
    // Precision touchpad (plan 5.4): the Windows adapter consumes WM_POINTER
    // frames for the canvas and applies its pan/inertia frames 1:1, while this
    // filter drops the wheel/native-gesture events Qt synthesises on top of a
    // consumed frame.  Both must share ONE router (the adapter's), otherwise
    // suppression state is invisible to the filter and one gesture pans twice.
    precisionTouchpad_ = std::make_unique<ui::PrecisionTouchpadQt>(*canvas_);
    precisionFilter_ = std::make_unique<ui::PrecisionInputFilterQt>(precisionTouchpad_->Router());
    canvas_->installEventFilter(precisionFilter_.get());
    precisionTouchpad_->Install(winId());
#endif

    connect(canvas_, &ui::CanvasWidget::ViewportChanged, this, &QtMainWindow::UpdateChrome);
    // Refresh the selection-gated actions (copy/cut) on every selection change,
    // whatever its origin (a canvas gesture, a cut, or a paste that selects what
    // it inserted): the controller owns the selection state.
    connect(&controller_, &QtDocumentController::SelectionChanged, this, &QtMainWindow::UpdateChrome);
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
    newAction_ = new QAction(QStringLiteral("New"), this);
    connect(newAction_, &QAction::triggered, this, &QtMainWindow::NewDocument);

    openAction_ = new QAction(QStringLiteral("Open"), this);
    connect(openAction_, &QAction::triggered, this, &QtMainWindow::OpenFileDialog);

    saveAction_ = new QAction(QStringLiteral("Save"), this);
    connect(saveAction_, &QAction::triggered, this, &QtMainWindow::Save);

    QAction* saveAsAction = new QAction(QStringLiteral("Save As"), this);
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this]() { SaveAs(); });

    // Whether the project embeds in the export is the settings toggle
    // (embed_project_in_exported_pdfs), read at export time.
    exportAction_ = new QAction(QStringLiteral("Export PDF"), this);
    connect(exportAction_, &QAction::triggered, this, [this]() { ExportPdf(settings_.embedProjectInExportedPdfs); });

    undoAction_ = new QAction(QStringLiteral("Undo"), this);
    connect(undoAction_, &QAction::triggered, this, &QtMainWindow::Undo);

    redoAction_ = new QAction(QStringLiteral("Redo"), this);
    connect(redoAction_, &QAction::triggered, this, &QtMainWindow::Redo);

    copyAction_ = new QAction(QStringLiteral("Copy"), this);
    connect(copyAction_, &QAction::triggered, this, &QtMainWindow::CopySelection);

    cutAction_ = new QAction(QStringLiteral("Cut"), this);
    connect(cutAction_, &QAction::triggered, this, &QtMainWindow::CutSelection);

    pasteAction_ = new QAction(QStringLiteral("Paste"), this);
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
    connect(quickSizeAction_, &QAction::triggered, this, [this]() { ToggleQuickPopup(); });

    wheelAction_ = new QAction(QStringLiteral("Tool Wheel"), this);
    connect(wheelAction_, &QAction::triggered, this, [this]() {
        if (toolbar_ != nullptr) {
            // App::TogglePaletteWheel (ToolbarState.cpp:127-159): the wheel
            // anchors at the cursor; the toolbar computes the fallback.
            toolbar_->TogglePaletteWheel();
        }
    });

    QAction* fitAction = new QAction(QStringLiteral("Fit"), this);
    fitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(fitAction, &QAction::triggered, this, &QtMainWindow::FitToWindow);

    QAction* zoomInAction = new QAction(QStringLiteral("Zoom In"), this);
    // QKeySequence::ZoomIn resolves to Ctrl+'+' (Qt::Key_Plus) only, so the
    // unshifted Ctrl+'=' key on a US layout would be dead; bind it as an alias
    // of the same action.
    zoomInAction->setShortcuts({ QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+=")) });
    connect(zoomInAction, &QAction::triggered, this, &QtMainWindow::ZoomIn);

    QAction* zoomOutAction = new QAction(QStringLiteral("Zoom Out"), this);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, &QtMainWindow::ZoomOut);

    QAction* quitAction = new QAction(QStringLiteral("Quit"), this);
    connect(quitAction, &QAction::triggered, this, [this]() { close(); });

    // Commands that exist as settings bindings without a chrome control of their
    // own (the reference table is authoritative: Reset View, the five zoom
    // presets and the page keys).  They are window actions so the settings
    // dialog can remap them like every other command.
    resetViewAction_ = new QAction(QStringLiteral("Reset View"), this);
    connect(resetViewAction_, &QAction::triggered, this, &QtMainWindow::ResetView);

    for (std::size_t index = 0; index < zoomPresetActions_.size(); ++index) {
        zoomPresetActions_[index] = new QAction(QStringLiteral("Zoom Preset %1").arg(index + 1U), this);
        connect(zoomPresetActions_[index], &QAction::triggered, this, [this, index]() { ApplyZoomPreset(index); });
    }

    previousPageAction_ = new QAction(QStringLiteral("Previous Page"), this);
    connect(previousPageAction_, &QAction::triggered, this, &QtMainWindow::PreviousPage);

    nextPageAction_ = new QAction(QStringLiteral("Next Page"), this);
    connect(nextPageAction_, &QAction::triggered, this, &QtMainWindow::NextPage);

    std::vector<QAction*> actions {
        newAction_, openAction_, saveAction_, saveAsAction, exportAction_,
        undoAction_, redoAction_, copyAction_, cutAction_, pasteAction_,
        addTextAction_, addLatexAction_, addPageAction_, newPageSettingsAction_,
        settingsAction_, sidebarAction_, quickSizeAction_, wheelAction_,
        fitAction, zoomInAction, zoomOutAction, quitAction,
        resetViewAction_, previousPageAction_, nextPageAction_,
    };
    actions.insert(actions.end(), zoomPresetActions_.begin(), zoomPresetActions_.end());
    for (QAction* action : actions) {
        addAction(action);
    }

    // Title menu: project/file management only.  Tool selection, editing and
    // zoom commands stay bound to their shortcuts (the actions are still
    // registered on the window above) but no longer appear here.
    titleMenu_ = new QMenu(this);
    titleMenu_->addAction(newAction_);
    titleMenu_->addAction(openAction_);
    recentMenu_ = titleMenu_->addMenu(QStringLiteral("Open Recent"));
    recentMenuAction_ = recentMenu_->menuAction();
    // Reference title menu order (App.cpp:5954-5957): New, New window, New view
    // of this project, then the save/export entries.
    newWindowAction_ = titleMenu_->addAction(QStringLiteral("New Window"));
    connect(newWindowAction_, &QAction::triggered, this, &QtMainWindow::OpenNewWindow);
    newViewAction_ = titleMenu_->addAction(QStringLiteral("New View of This Project"));
    connect(newViewAction_, &QAction::triggered, this, &QtMainWindow::OpenNewView);
    titleMenu_->addSeparator();
    titleMenu_->addAction(saveAction_);
    titleMenu_->addAction(saveAsAction);
    titleMenu_->addAction(exportAction_);
    titleMenu_->addSeparator();
    titleMenu_->addAction(settingsAction_);
    titleMenu_->addSeparator();
    titleMenu_->addAction(quitAction);
    RefreshRecentMenu();

    toolbar_ = new ui::ToolbarWidget(this);
    // The band carries the app's own window buttons where the native caption is
    // removed (Windows); platforms with native decorations keep only the reserve.
    toolbar_->SetCaptionButtonsVisible(kUsesCustomCaption);
    toolbar_->SetSettings(settings_);
    toolbar_->SetActiveTool(activeTool_);
    toolbar_->SetMaximized(IsWindowMaximized());
    connect(toolbar_, &ui::ToolbarWidget::CommandTriggered, this, [this](ui::ToolbarCommand command) {
        switch (command) {
            case ui::ToolbarCommand::Undo: Undo(); break;
            case ui::ToolbarCommand::Redo: Redo(); break;
            case ui::ToolbarCommand::Minimize: showMinimized(); break;
            case ui::ToolbarCommand::Maximize: IsWindowMaximized() ? showNormal() : showMaximized(); UpdateChrome(); break;
            case ui::ToolbarCommand::Close: close(); break;
            case ui::ToolbarCommand::OpenTitleMenu: ShowTitleMenu(); break;
            case ui::ToolbarCommand::OpenPalettePicker: ToggleSettingsDialog(); break;
        }
    });
    connect(toolbar_, &ui::ToolbarWidget::ToolSelected, this, [this](settings::Tool tool) {
        SetActiveTool(tool);
    });
    connect(toolbar_, &ui::ToolbarWidget::ColorSelected, this, [this](std::size_t index) {
        settings_.colorIndex = index;
        ApplyStrokeStyle();
        settingsStore_.Save(settings_);
        // Reference App::SetColorIndex (ToolbarState.cpp:520-535): a palette
        // pick recolors the current selection as well as the active slot.
        if (!settings_.palette.empty()) {
            controller_.RecolorSelection(settings_.palette[index % settings_.palette.size()]);
        }
        UpdateChrome();
    });
    connect(toolbar_, &ui::ToolbarWidget::WheelToolToggled, this, [this](settings::Tool tool) {
        // App::ToggleToolInWheel (ToolbarState.cpp:87-99): membership toggles
        // in place and persists immediately.
        const auto existing = std::find(settings_.wheelTools.begin(), settings_.wheelTools.end(), tool);
        if (existing != settings_.wheelTools.end()) {
            settings_.wheelTools.erase(existing);
        } else {
            settings_.wheelTools.push_back(tool);
        }
        settingsStore_.Save(settings_);
        ApplySettingsToShell();
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
        canvas_->SetZoom(zoom, ZoomAnchorPoint());
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
    // Text-format popup (Ctrl+W): while a text edit is open the commands apply
    // to the editor's char format and the result is mirrored into the pending
    // style; without an editor they edit settings_ directly.  Either way the
    // pending style persists like the other tools' settings.
    const auto persistPendingTextStyle = [this]() {
        SyncPendingTextStyle();
        ApplyPendingTextStyle();
        settingsStore_.Save(settings_);
        UpdateChrome();
    };
    connect(toolbar_, &ui::ToolbarWidget::TextBoldToggleRequested, this, [this, persistPendingTextStyle]() {
        if (controller_.EditingText()) {
            ApplyTextFormat([](QTextCharFormat& format) {
                format.setFontWeight(format.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
            });
        } else {
            settings_.textBold = !settings_.textBold;
        }
        persistPendingTextStyle();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextItalicToggleRequested, this, [this, persistPendingTextStyle]() {
        if (controller_.EditingText()) {
            ApplyTextFormat([](QTextCharFormat& format) { format.setFontItalic(!format.fontItalic()); });
        } else {
            settings_.textItalic = !settings_.textItalic;
        }
        persistPendingTextStyle();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextUnderlineToggleRequested, this, [this, persistPendingTextStyle]() {
        if (controller_.EditingText()) {
            ApplyTextFormat([](QTextCharFormat& format) { format.setFontUnderline(!format.fontUnderline()); });
        } else {
            settings_.textUnderline = !settings_.textUnderline;
        }
        persistPendingTextStyle();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextAlignCycleRequested, this, [this, persistPendingTextStyle]() {
        QTextEdit* editor = canvas_->TextEditor();
        if (editor != nullptr && controller_.EditingText()) {
            const Qt::Alignment current = editor->alignment();
            const Qt::Alignment next = (current & Qt::AlignHCenter)
                ? Qt::AlignRight
                : ((current & Qt::AlignRight) ? Qt::AlignLeft : Qt::AlignHCenter);
            editor->setAlignment(next);
        } else {
            settings_.textAlignment = settings_.textAlignment == TextAlignment::Left
                ? TextAlignment::Center
                : (settings_.textAlignment == TextAlignment::Center ? TextAlignment::Right : TextAlignment::Left);
        }
        persistPendingTextStyle();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextSizePresetChanged, this, [this, persistPendingTextStyle](std::size_t index) {
        if (index >= settings_.textSizePresets.size()) {
            return;
        }
        // Static size: the preset is a page-unit value and goes straight into
        // the pending style; while editing, the live editor wants the same
        // size in screen points (FontForStyle's displayScale convention).
        settings_.textSizePreset = index;
        if (controller_.EditingText()) {
            const float points = settings_.textSizePresets[index]
                * std::max(0.01f, canvas_->TextEditScale()) * text::kPointsPerPageUnit;
            ApplyTextFormat([points](QTextCharFormat& format) {
                format.setFontPointSize(points);
            });
        }
        persistPendingTextStyle();
    });
    connect(toolbar_, &ui::ToolbarWidget::TextFontFamilyChanged, this, [this, persistPendingTextStyle](TextFontFamily family) {
        settings_.textFontFamily = family;
        if (controller_.EditingText()) {
            ApplyTextFormat([family](QTextCharFormat& format) {
                format.setFontFamilies({ text::TextFontFamilyName(family) });
            });
        }
        persistPendingTextStyle();
    });
    connect(toolbar_, &ui::ToolbarWidget::ToolbarGeometryChanged, this, [this]() { UpdateChromeGeometry(); });
    connect(toolbar_, &ui::ToolbarWidget::RevealProgressChanged, this, [this](qreal) { UpdateChromeGeometry(); });
    connect(toolbar_, &ui::ToolbarWidget::PaletteOverlayToggled, this, [this](bool) { UpdateChrome(); });

    sidebar_ = new ui::SidebarWidget(this);
    sidebar_->SetDocument(&controller_.DocumentRef(), controller_.PdfBackground());
    connect(sidebar_, &ui::SidebarWidget::PageActivated, this, [this](std::size_t pageIndex) {
        FocusPage(pageIndex);
    });
    connect(sidebar_, &ui::SidebarWidget::PageJumpRequested, this, [this](std::size_t pageIndex) {
        FocusPage(pageIndex);
    });
    connect(sidebar_, &ui::SidebarWidget::AddPageRequested, this, &QtMainWindow::AddPage);
    connect(sidebar_, &ui::SidebarWidget::NewPageSettingsRequested, this, &QtMainWindow::OpenNewPageSettings);
    connect(sidebar_, &ui::SidebarWidget::PageContextMenuRequested, this,
        [this](std::size_t pageIndex, const QPoint& globalPosition) {
            QMenu menu(this);
            QAction* copy = menu.addAction(QStringLiteral("Copy Page"));
            QAction* cut = menu.addAction(QStringLiteral("Cut Page"));
            QAction* paste = menu.addAction(QStringLiteral("Paste Page"));
            QAction* deletePage = menu.addAction(QStringLiteral("Delete Page"));
            menu.addSeparator();
            QAction* rotateCw = menu.addAction(QStringLiteral("Rotate 90° Clockwise"));
            QAction* rotateCcw = menu.addAction(QStringLiteral("Rotate 90° Counterclockwise"));
            QAction* rotate180 = menu.addAction(QStringLiteral("Rotate 180°"));
            paste->setEnabled(manager_.HasClipboardPage());
            deletePage->setEnabled(controller_.DocumentRef().PageCount() > 1U);
            QAction* chosen = menu.exec(globalPosition);
            if (chosen == copy) {
                static_cast<void>(controller_.CopyPage(pageIndex));
            } else if (chosen == cut) {
                static_cast<void>(controller_.CutPage(pageIndex));
            } else if (chosen == paste) {
                static_cast<void>(controller_.PastePage(pageIndex));
            } else if (chosen == deletePage) {
                static_cast<void>(controller_.RemovePage(pageIndex));
            } else if (chosen == rotateCw) {
                static_cast<void>(controller_.RotatePage(pageIndex, 1));
            } else if (chosen == rotateCcw) {
                static_cast<void>(controller_.RotatePage(pageIndex, 3));
            } else if (chosen == rotate180) {
                static_cast<void>(controller_.RotatePage(pageIndex, 2));
            }
        });
    connect(sidebar_, &ui::SidebarWidget::OpenChanged, this, [this](bool open) {
        if (sidebarAction_ != nullptr) {
            sidebarAction_->setChecked(open);
        }
        UpdateChrome();
    });
    connect(sidebar_, &ui::SidebarWidget::ScrollChanged, this, [this](int) { UpdateChrome(); });
    sidebar_->installEventFilter(this);

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

void QtMainWindow::SyncPendingTextStyle() {
    QTextEdit* editor = canvas_ != nullptr ? canvas_->TextEditor() : nullptr;
    if (editor == nullptr || !controller_.EditingText()) {
        return;
    }
    // The editor's current char format is the last-applied style; the pending
    // popup state mirrors it so the next created text inherits it (the per-tool
    // persistence pattern: last use wins).  Color is deliberately not part of
    // the pending style - new objects take the active palette slot - and the
    // size comes from the preset index alone (the editor's point size is a
    // view-scale conversion, not a pending choice).
    TextAlignment alignment = TextAlignment::Left;
    const PageTextStyle style = TextStyleFromEditor(editor, alignment);
    settings_.textFontFamily = style.fontFamily;
    settings_.textBold = style.bold;
    settings_.textItalic = style.italic;
    settings_.textUnderline = style.underline;
    settings_.textAlignment = alignment;
}

void QtMainWindow::ApplyPendingTextStyle() {
    ui::CanvasInputSettings input = controller_.InputSettings();
    input.textFontFamily = settings_.textFontFamily;
    input.textAlignment = settings_.textAlignment;
    input.textFontSize = settings_.textSizePresets[
        std::min(settings_.textSizePreset, settings_.textSizePresets.size() - 1U)];
    input.textBold = settings_.textBold;
    input.textItalic = settings_.textItalic;
    input.textUnderline = settings_.textUnderline;
    controller_.SetInputSettings(input);
}

void QtMainWindow::ApplySettingsToShell() {
    ui::CanvasInputSettings input {};
    input.inputSpacing = settings_.inputSpacing;
    input.inputSmoothing = settings_.inputSmoothing;
    input.strokeSmoothing = settings_.strokeSmoothing;
    input.dashLength = settings_.dashLength;
    input.dashGap = settings_.dashGap;
    controller_.SetInputSettings(input);
    ApplyPendingTextStyle();
    ApplyStrokeStyle();
    ApplyToolToCanvas();
    if (toolbar_ != nullptr) {
        toolbar_->SetSettings(settings_);
        toolbar_->SetActiveTool(activeTool_);
        toolbar_->SetColorIndex(settings_.colorIndex);
        toolbar_->SetCaptionButtonsVisible(kUsesCustomCaption);
        toolbar_->SetAutoHideEnabled(settings_.autoHideToolbar);
        if (!settings_.autoHideToolbar) {
            toolbar_->SetRevealed(true);
        }
    }
    if (sidebar_ != nullptr) {
        sidebar_->SetMaximized(IsWindowMaximized());
    }
}

void QtMainWindow::ApplyStrokeStyle() {
    const StrokeStyle style = StrokeStyleFor(settings_, activeTool_, 0.0f);
    controller_.SetActiveStrokeStyle(style);
    // New text/LaTeX objects take the same active palette colour.
    ui::CanvasInputSettings input = controller_.InputSettings();
    input.objectColor = style.color;
    controller_.SetInputSettings(input);
    // The cursors track the same settings: the dot fills with the active
    // palette colour (App::DrawCursorForCurrentSize, App.cpp:7471-7489) and the
    // eraser square sizes from eraseBrushSize (App::EraserCursorForCurrentSize,
    // App.cpp:7459-7468).  Both setters early-out when nothing changed.
    canvas_->SetDrawCursorSettings(settings_.drawCursorKind, settings_.drawCursorSize, style.color);
    canvas_->SetEraseBrushSize(settings_.eraseBrushSize);
}

void QtMainWindow::ApplyToolToCanvas() {
    canvas_->SetTool(ToCanvasTool(activeTool_));
    canvas_->Router().SetTuning(ui::InteractionTuning {
        settings_.inputSpacing,
        settings_.inputSmoothing,
    });
}

// Whether a text/LaTeX editor owns the keyboard right now.
//
// This asks the controller, which is the authoritative source at the moment its
// EditStateChanged is delivered: BeginTextEdit sets its index *before* emitting,
// and Commit/CancelTextEdit reset it before emitting, so the answer is correct in
// both directions.  CanvasWidget::EditOverlayActive() is not: it is a view-state
// cache that opens only when SyncEditOverlay/ShowTextOverlay runs *after*
// BeginTextEdit has already emitted, so a gate keyed on it would read "no editor"
// while one is opening (and it used to read "editor" while one was closing -
// CommitActiveEdit/CancelActiveEdit cleared it after the controller call, which
// left the tool actions disabled after the editor had closed).  That close-side
// ordering is now fixed in CanvasWidget too, but the controller remains the
// single state that is never stale at signal time.
bool QtMainWindow::EditorOwnsKeyboard() const {
    return controller_.EditingText() || controller_.EditingLatex();
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
    // KeyBindingQt is the reference shortcut table and the settings store is the
    // only source of a runtime binding: every action below takes its sequence
    // from settings_.shortcuts, so a remap reaches all of them.  The tool
    // actions are the ones that must stand down while a text/LaTeX editor owns
    // the keyboard (editor precedence, App::ShortcutAction::Tool*), which is the
    // editor-overlay state - never the active tool.
    //
    // The previous tool actions must be destroyed, not just forgotten: a QAction
    // stays in its window's action list while it lives, and Qt treats two live
    // actions with the same sequence as an *ambiguous* shortcut and fires
    // neither - so dropping the vector silently killed every tool key as soon as
    // the settings dialog re-ran this (it does so on each edit, e.g. the
    // auto-hide toggle).
    for (QAction* stale : toolShortcutActions_) {
        delete stale;
    }
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
    toolAction(settings::ShortcutAction::ToolLatex, settings::Tool::Latex);
    toolAction(settings::ShortcutAction::ToolErase, settings::Tool::Erase);
    toolAction(settings::ShortcutAction::ToolPan, settings::Tool::Pan);

    const auto bind = [this](settings::ShortcutAction action, QAction* target) {
        if (target != nullptr) {
            target->setShortcut(settings_.shortcuts[static_cast<std::size_t>(action)].sequence);
        }
    };
    bind(settings::ShortcutAction::New, newAction_);
    bind(settings::ShortcutAction::Open, openAction_);
    bind(settings::ShortcutAction::Save, saveAction_);
    bind(settings::ShortcutAction::ExportPdf, exportAction_);
    bind(settings::ShortcutAction::Undo, undoAction_);
    bind(settings::ShortcutAction::Redo, redoAction_);
    bind(settings::ShortcutAction::Copy, copyAction_);
    bind(settings::ShortcutAction::Cut, cutAction_);
    bind(settings::ShortcutAction::Paste, pasteAction_);
    bind(settings::ShortcutAction::ResetView, resetViewAction_);
    bind(settings::ShortcutAction::PreviousPage, previousPageAction_);
    bind(settings::ShortcutAction::NextPage, nextPageAction_);
    bind(settings::ShortcutAction::ToggleSettings, settingsAction_);
    bind(settings::ShortcutAction::TogglePdfSidebar, sidebarAction_);
    bind(settings::ShortcutAction::ToggleQuickSize, quickSizeAction_);
    bind(settings::ShortcutAction::ToggleWheel, wheelAction_);
    constexpr std::array<settings::ShortcutAction, settings::kZoomPresetCount> kZoomPresetActions {
        settings::ShortcutAction::ZoomPreset1,
        settings::ShortcutAction::ZoomPreset2,
        settings::ShortcutAction::ZoomPreset3,
        settings::ShortcutAction::ZoomPreset4,
        settings::ShortcutAction::ZoomPreset5,
    };
    for (std::size_t index = 0; index < zoomPresetActions_.size(); ++index) {
        bind(kZoomPresetActions[index], zoomPresetActions_[index]);
    }

    for (QAction* action : toolShortcutActions_) {
        action->setEnabled(!EditorOwnsKeyboard());
    }
}

void QtMainWindow::ResetView() {
    canvas_->ResetView();
    UpdateChrome();
}

void QtMainWindow::ApplyZoomPreset(std::size_t index) {
    if (index >= settings_.zoomPresets.size()) {
        return;
    }
    const float preset = settings_.zoomPresets[index];
    if (preset <= 0.0f) {
        return;
    }
    canvas_->SetZoom(std::clamp(preset, settings::kMinZoom, settings::kMaxZoom), ZoomAnchorPoint());
    UpdateChrome();
}

QPointF QtMainWindow::ZoomAnchorPoint() const {
    // Reference App::ApplyZoomPreset (Viewport.cpp:607-631): the wheel's centre
    // wins while the wheel is open (MessageLoop.cpp:427 passes wheel_.center),
    // then the cursor when it is inside the client, then the client centre.
    if (toolbar_ != nullptr && toolbar_->PaletteWheelVisible()) {
        const QPointF anchor = canvas_->mapFrom(this, toolbar_->PaletteWheelCenter());
        if (canvas_->rect().contains(anchor.toPoint())) {
            return anchor;
        }
    }
    const QPoint cursor = canvas_->mapFromGlobal(QCursor::pos());
    if (canvas_->rect().contains(cursor)) {
        return QPointF(cursor);
    }
    return QPointF(canvas_->rect().center());
}

void QtMainWindow::FocusPage(std::size_t pageIndex) {
    if (pageIndex >= controller_.DocumentRef().PageCount()) {
        return;
    }
    canvas_->CenterOnPage(pageIndex);
    controller_.SetCurrentPageIndex(pageIndex);
    if (sidebar_ != nullptr) {
        sidebar_->SetCurrentPage(pageIndex);
    }
    UpdateChrome();
}

void QtMainWindow::PreviousPage() {
    const std::size_t current = controller_.CurrentPageIndex();
    if (current > 0U) {
        FocusPage(current - 1U);
    }
}

void QtMainWindow::NextPage() {
    const std::size_t current = controller_.CurrentPageIndex();
    FocusPage(current + 1U);  // FocusPage ignores an out-of-range page
}

void QtMainWindow::ToggleQuickPopup() {
    if (toolbar_ == nullptr) {
        return;
    }
    // Reference TextEditProc WM_KEYDOWN ctrl+W (MessageLoop.cpp): a text edit
    // owns Ctrl+W and toggles the text-format popup.  A LaTeX edit does not, and
    // the brush popup only belongs to the pen tools (IsToolbarBrushTool); every
    // other state is inert, exactly like App::ToggleQuickSizePopup.
    if (controller_.EditingText()) {
        if (toolbar_->TextFormatPopupVisible()) {
            toolbar_->HideTextFormatPopup();
            return;
        }
        QTextEdit* editor = canvas_->TextEditor();
        const QRectF editRect = editor != nullptr
            ? QRectF(canvas_->mapTo(this, editor->pos()), QSizeF(editor->size()))
            : QRectF();
        toolbar_->ShowTextFormatPopup(editRect);
        return;
    }
    // The Text tool owns Ctrl+W before any edit exists: the same text-format
    // popup edits the pending style that AddTextObject seeds.  Anchored at the
    // cursor like the brush popup (there is no edit rect yet).
    if (activeTool_ == settings::Tool::Text) {
        if (toolbar_->TextFormatPopupVisible()) {
            toolbar_->HideTextFormatPopup();
            return;
        }
        const QPointF anchor = QuickPopupCursorAnchor();
        toolbar_->ShowTextFormatPopup(QRectF(anchor, anchor));
        return;
    }
    if (activeTool_ != settings::Tool::Draw && activeTool_ != settings::Tool::Erase) {
        return;
    }
    if (toolbar_->QuickSizePopupVisible() && toolbar_->QuickSizeTool() == activeTool_) {
        toolbar_->HideQuickSizePopup();
        return;
    }
    toolbar_->ShowQuickSizePopup(activeTool_, QuickPopupCursorAnchor());
}

QPointF QtMainWindow::QuickPopupCursorAnchor() const {
    // App::ToggleQuickSizePopup (ToolbarState.cpp:246-258): the popup anchors at
    // the cursor in client coordinates; when the cursor is outside the canvas
    // (exclusive right/bottom edges, like the reference PtInRect-style check)
    // the anchor falls back to the canvas centre 120 px below its top.  Qt
    // works in device-independent pixels, so the offset is used as-is.
    const QPointF cursor = mapFromGlobal(QCursor::pos());
    const QRectF canvas = toolbar_ != nullptr ? toolbar_->HostCanvasRect() : QRectF();
    const bool inside = cursor.x() >= canvas.left() && cursor.x() < canvas.right()
        && cursor.y() >= canvas.top() && cursor.y() < canvas.bottom();
    if (inside) {
        return cursor;
    }
    return QPointF((canvas.left() + canvas.right()) / 2.0, canvas.top() + 120.0);
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
    static_cast<void>(OpenPath(path));
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
    // Clipboard.cpp:120-121 CopySelectionToClipboard: selected PDF text copies
    // as plain text before any object payload is considered.
    if (controller_.HasSelectedPdfText()) {
        static_cast<void>(cnote::clipboard::WritePlainText(controller_.SelectedPdfText()));
        return;
    }
    cnote::SelectionClipboardData data;
    if (controller_.BuildSelectionPayload(data)) {
        static_cast<void>(cnote::clipboard::WriteSelection(data));
    }
}

void QtMainWindow::CutSelection() {
    // CutSelectionToClipboard (Clipboard.cpp:225-235): the pdf branch still
    // copies; the "cut" only clears the selection since the source text is
    // baked into the page.
    if (controller_.HasSelectedPdfText()) {
        if (cnote::clipboard::WritePlainText(controller_.SelectedPdfText())) {
            controller_.ClearSelection();
            canvas_->NotifyDocumentChanged();
            UpdateChrome();
        }
        return;
    }
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
            PageImage image = cnote::clipboard::PageImageFromQImage(content.image);
            if (image.pixelWidth <= 0 || image.pixelHeight <= 0) {
                return;
            }

            // Reference Clipboard.cpp:391-401 PasteImageFromClipboard: the
            // pasted image keeps its on-screen pixel size (pixels / view
            // scale), capped at 75% of the page width, aspect preserved.
            const std::size_t pageIndex = controller_.CurrentPageIndex();
            const Page& page = controller_.DocumentRef().PageAt(pageIndex);
            const ui::CanvasPageView view = canvas_->CalculatePageView(pageIndex);
            const float pageScale = std::max(0.1f, view.valid ? view.scale : 1.0f);
            image.width = std::min(page.width * 0.75f,
                static_cast<float>(image.pixelWidth) / pageScale);
            image.height = image.width
                * (static_cast<float>(image.pixelHeight)
                    / std::max(1.0f, static_cast<float>(image.pixelWidth)));

            cnote::SelectionClipboardData payload;
            payload.images.push_back(image);
            payload.minX = 0.0f;
            payload.minY = 0.0f;
            payload.maxX = image.width;
            payload.maxY = image.height;
            const Vec2 target = canvas_->ScreenToPage(canvas_->rect().center(), true);
            if (controller_.PastePayload(pageIndex, target, payload)) {
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

void QtMainWindow::OpenNewWindow() {
    // Reference App::CreateAdditionalWindow(false) (Startup.cpp:49-79): an
    // independent window on a fresh untitled session.  Qt owns the lifetime:
    // WA_DeleteOnClose frees the window and its controller unregisters the
    // view, and quitOnLastWindowClosed ends the app when the last one goes.
    canvas_->CancelStroke();
    canvas_->CancelActiveEdit();
    auto* window = new QtMainWindow(manager_);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(size());
    window->show();
}

void QtMainWindow::OpenNewView() {
    // Reference App::CreateAdditionalWindow(true): a second view attached to
    // the same ProjectSession, so edits propagate live through the manager's
    // view registry.  The new view inherits the current page, zoom, pan and
    // sidebar state (Startup.cpp:56-66).
    canvas_->CancelStroke();
    canvas_->CancelActiveEdit();
    const std::shared_ptr<ProjectSession> session = controller_.Session();
    auto* window = new QtMainWindow(manager_);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->resize(size());
    window->controller_.AttachSession(session);
    window->canvas_->SetPdfBackgroundProvider(window->controller_.PdfBackground());
    window->controller_.SetCurrentPageIndex(controller_.CurrentPageIndex());
    window->canvas_->SetZoom(canvas_->Zoom());
    window->canvas_->PanBy(QPointF(
        static_cast<qreal>(canvas_->ViewportPanX() - window->canvas_->ViewportPanX()),
        static_cast<qreal>(canvas_->ViewportPanY() - window->canvas_->ViewportPanY())));
    if (sidebar_ != nullptr && sidebar_->IsOpen() && window->sidebar_ != nullptr) {
        window->sidebar_->SetOpen(true);
    }
    if (window->sidebar_ != nullptr) {
        window->sidebar_->SetDocument(&window->controller_.DocumentRef(), window->controller_.PdfBackground());
        window->sidebar_->NotifyDocumentChanged();
        window->sidebar_->SetCurrentPage(window->controller_.CurrentPageIndex());
    }
    window->UpdateChrome();
    window->show();
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
    canvas_->ZoomBySteps(1, ZoomAnchorPoint());
    UpdateChrome();
}

void QtMainWindow::AddTextObject() {
    const std::optional<ui::CanvasPageView> hit = canvas_->HitTestPageView(canvas_->rect().center());
    const std::size_t pageIndex = hit.has_value() ? hit->pageIndex : controller_.CurrentPageIndex();
    const Vec2 point = hit.has_value()
        ? canvas_->ScreenToPage(*hit, canvas_->rect().center(), false)
        : Vec2 { 60.0f, 80.0f };
    // Static text size like the text tool's CreateTextAt: the pending preset
    // (page units) is stored verbatim, no zoom scaling.
    controller_.AddTextObject(pageIndex, point,
        std::max(1.0f, controller_.InputSettings().textFontSize));
    canvas_->SyncEditOverlay();
    UpdateChrome();
}

void QtMainWindow::ZoomOut() {
    canvas_->ZoomBySteps(-1, ZoomAnchorPoint());
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

namespace {

bool& ForcedHeadlessModals() {
    static bool forced = false;
    return forced;
}

}  // namespace

bool QtMainWindow::HeadlessModals() {
    static const bool fromEnvironment = qEnvironmentVariableIsSet("CNOTE_HEADLESS");
    return fromEnvironment || ForcedHeadlessModals();
}

void QtMainWindow::SetHeadlessModals(bool headless) {
    ForcedHeadlessModals() = headless;
}

void QtMainWindow::ReportError(const QString& text) {
    if (HeadlessModals()) {
        std::fprintf(stderr, "MODAL cnote: %s\n", text.toUtf8().constData());
        return;
    }
    QMessageBox::critical(this, QStringLiteral("cnote"), text);
}

bool QtMainWindow::IsWindowMaximized() const {
#if defined(Q_OS_WIN)
    const QWindow* handle = windowHandle();
    if (handle != nullptr) {
        return IsZoomed(reinterpret_cast<HWND>(handle->winId())) != FALSE;
    }
#endif
    return isMaximized();
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

bool QtMainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#if defined(Q_OS_WIN)
    static const QByteArray kWindowsMessage = QByteArrayLiteral("windows_generic_MSG");
    if (eventType == kWindowsMessage && HandleNativeChromeMessage(message, result)) {
        return true;
    }
#else
    static_cast<void>(eventType);
    static_cast<void>(message);
    static_cast<void>(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

#if defined(Q_OS_WIN)
bool QtMainWindow::HandleNativeChromeMessage(void* message, qintptr* result) {
    auto* msg = static_cast<MSG*>(message);
    switch (msg->message) {
    case WM_NCCALCSIZE:
        // App::HandleMessage (MessageLoop.cpp:87): the whole window rectangle is
        // client area, so the caption is never drawn.  The WS_OVERLAPPEDWINDOW
        // style stays in place - that is what keeps the DWM shadow, Aero snap,
        // double-click maximize/restore, the resize edges and the work-area
        // maximize clamp, all of which Qt::FramelessWindowHint gives up.
        *result = 0;
        return true;
    case WM_NCACTIVATE:
        // App::HandleMessage (MessageLoop.cpp:85-86) answers TRUE: the frame has
        // no non-client area left to repaint on activation, so the default
        // handler is never allowed to draw one.
        *result = TRUE;
        return true;
    case WM_NCHITTEST: {
        const HWND window = reinterpret_cast<HWND>(winId());
        RECT windowRect {};
        GetWindowRect(window, &windowRect);
        const QPoint screenCursor(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));

        POINT clientCursor { screenCursor.x(), screenCursor.y() };
        ScreenToClient(window, &clientCursor);
        const qreal ratio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
        const QPointF clientPosition(
            static_cast<qreal>(clientCursor.x) / ratio,
            static_cast<qreal>(clientCursor.y) / ratio);

        if (toolbar_ != nullptr) {
            const bool interactionActive = canvas_ != nullptr
                && canvas_->ActiveInteraction() != ui::InteractionKind::None;
            // App::HandleMessage WM_NCHITTEST (MessageLoop.cpp:141-152): the hit
            // test is what reveals an auto-hidden band.  It also carries the band
            // hover state, because the caption strip's mouse moves arrive as
            // non-client messages the widget never sees.
            toolbar_->UpdateAutoHideForCursor(clientPosition, interactionActive);
            toolbar_->UpdateHoverAtHostPosition(clientPosition);
        }

        ui::WindowChromeHitTest hit;
        hit.windowRect = QRect(
            QPoint(windowRect.left, windowRect.top),
            QSize(windowRect.right - windowRect.left, windowRect.bottom - windowRect.top));
        hit.cursor = screenCursor;
        hit.frameThicknessX = NativeFrameThicknessX(window);
        hit.frameThicknessY = NativeFrameThicknessY(window);
        hit.maximized = IsZoomed(window) != FALSE;
        hit.cursorInBand = toolbar_ != nullptr
            && clientPosition.y() >= 0.0
            && clientPosition.y() < toolbar_->VisibleToolbarHeight();
        const ui::ToolbarControl control = hit.cursorInBand
            ? toolbar_->ControlAtHostPosition(clientPosition)
            : ui::ToolbarControl::None;
        hit.cursorOverToolbarControl = control != ui::ToolbarControl::None;
        hit.cursorOverMinButton = control == ui::ToolbarControl::Minimize;
        hit.cursorOverMaxButton = control == ui::ToolbarControl::Maximize;
        hit.cursorOverCloseButton = control == ui::ToolbarControl::Close;
        *result = static_cast<qintptr>(HitCodeFor(ui::ResolveWindowChromeHit(hit)));
        return true;
    }
    case WM_NCLBUTTONDOWN: {
        // The HT*BUTTON hit codes make the cluster real non-client buttons
        // (Windows 11 uses them for the snap-layouts flyout).  DefWindowProc
        // cannot complete the click - the WM_NCCALCSIZE override leaves it no
        // caption rect - so the press is armed here and completed on release.
        // SetCapture routes the release to the client mouse path; consuming
        // the message keeps DefWindowProc out of its own dead-end tracking.
        const ui::ToolbarControl armed = HitCodeControl(static_cast<LRESULT>(msg->wParam));
        if (armed == ui::ToolbarControl::None) {
            // Not a caption button: DefWindowProc still needs the press (title
            // bar drag, resize, system menu).
            return false;
        }
        ncButtonArmed_ = armed;
        SetCapture(reinterpret_cast<HWND>(winId()));
        if (toolbar_ != nullptr) {
            toolbar_->SetPressedControl(armed);
        }
        *result = 0;
        return true;
    }
    case WM_CAPTURECHANGED:
        // Another window stealing the capture mid-press abandons the cycle.
        ncButtonArmed_ = ui::ToolbarControl::None;
        if (toolbar_ != nullptr) {
            toolbar_->SetPressedControl(ui::ToolbarControl::None);
        }
        return false;
    case WM_MOUSEMOVE:
        // Drag off/on the armed button mirrors the native push feel.
        if (ncButtonArmed_ != ui::ToolbarControl::None && (msg->wParam & MK_LBUTTON) != 0 && toolbar_ != nullptr) {
            const ui::ToolbarControl over = (ncButtonArmed_ == CaptionControlAtClientPosition(msg->lParam))
                ? ncButtonArmed_
                : ui::ToolbarControl::None;
            toolbar_->SetPressedControl(over);
        }
        return false;
    case WM_LBUTTONUP:
        // The release lands here while the press is captured; complete the
        // cycle only when it still lands on the armed button.
        if (ncButtonArmed_ != ui::ToolbarControl::None) {
            // Complete before ReleaseCapture: its WM_CAPTURECHANGED clears the
            // armed state.
            CompleteCaptionButtonCycle(CaptionControlAtClientPosition(msg->lParam));
            ReleaseCapture();
            *result = 0;
            return true;
        }
        return false;
    case WM_NCLBUTTONUP:
        // Defensive: a press armed without capture still completes here.
        CompleteCaptionButtonCycle(HitCodeControl(static_cast<LRESULT>(msg->wParam)));
        return false;
    case WM_NCMOUSELEAVE:
        // The pressed visual and any armed press clear when the non-client
        // area loses the cursor.
        ncButtonArmed_ = ui::ToolbarControl::None;
        if (toolbar_ != nullptr) {
            toolbar_->SetPressedControl(ui::ToolbarControl::None);
        }
        return false;
    default:
        break;
    }
    return false;
}

void QtMainWindow::CompleteCaptionButtonCycle(ui::ToolbarControl released) {
    const ui::ToolbarControl armed = ncButtonArmed_;
    ncButtonArmed_ = ui::ToolbarControl::None;
    if (toolbar_ != nullptr) {
        toolbar_->SetPressedControl(ui::ToolbarControl::None);
    }
    if (armed == ui::ToolbarControl::None || released != armed) {
        return;
    }
    switch (armed) {
    case ui::ToolbarControl::Minimize: showMinimized(); break;
    case ui::ToolbarControl::Maximize:
        IsWindowMaximized() ? showNormal() : showMaximized();
        UpdateChrome();
        break;
    case ui::ToolbarControl::Close: close(); break;
    default: break;
    }
}

ui::ToolbarControl QtMainWindow::CaptionControlAtClientPosition(qintptr lParam) const {
    if (toolbar_ == nullptr) {
        return ui::ToolbarControl::None;
    }
    const qreal ratio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const QPointF clientPosition(
        static_cast<qreal>(GET_X_LPARAM(static_cast<LPARAM>(lParam))) / ratio,
        static_cast<qreal>(GET_Y_LPARAM(static_cast<LPARAM>(lParam))) / ratio);
    if (clientPosition.y() < 0.0 || clientPosition.y() >= toolbar_->VisibleToolbarHeight()) {
        return ui::ToolbarControl::None;
    }
    return toolbar_->ControlAtHostPosition(clientPosition);
}
#endif  // Q_OS_WIN

void QtMainWindow::UpdateNativeFrameThickness() {
    if (toolbar_ == nullptr) {
        return;
    }
#if defined(Q_OS_WIN)
    const QWindow* handle = windowHandle();
    if (handle == nullptr) {
        toolbar_->SetFrameThicknessX(0.0);
        return;
    }
    const qreal ratio = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    toolbar_->SetFrameThicknessX(static_cast<qreal>(NativeFrameThicknessX(reinterpret_cast<HWND>(handle->winId()))) / ratio);
#else
    // Only the Windows chrome removes the caption, so only Windows needs the
    // frame inset the maximized layout subtracts from the caption cluster.
    toolbar_->SetFrameThicknessX(0.0);
#endif
}

void QtMainWindow::UpdateChromeGeometry() {
    const bool maximized = IsWindowMaximized();
    const int band = qRound(cnote::appui::qt::ToolbarHeight(maximized));
    const int reserved = qRound(cnote::appui::qt::ReservedToolbarHeight(
        toolbar_ != nullptr && toolbar_->AutoHideEnabled(), maximized));
    if (sidebar_ != nullptr) {
        sidebar_->SetMaximized(maximized);
        sidebar_->SetHostClientRect(rect(), reserved);
        // The sidebar overlays the canvas but stays below the toolbar band:
        // raise it first so the toolbar's raise keeps it on top.
        sidebar_->raise();
    }
    if (toolbar_ != nullptr) {
        UpdateNativeFrameThickness();
        toolbar_->SetMaximized(maximized);
        // The widget owns the 120 ms reveal offset; the window only supplies the
        // band's width and the maximized state.
        toolbar_->setGeometry(0, qRound(toolbar_->ToolbarOffsetY()), width(), band);
        toolbar_->raise();
    }
    if (contentLayout_ != nullptr && reservedTop_ != reserved) {
        reservedTop_ = reserved;
        contentLayout_->setContentsMargins(0, reserved, 0, 0);
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

    // Reference SidebarOwnsKeyboardShortcuts (App.cpp:4069-4081): while the
    // pages panel owns the keyboard - the cursor is over it, or the sidebar
    // widget itself has focus (it is ClickFocus) - clipboard keys and Delete
    // act on the current page instead of the canvas selection.  The claim
    // happens on ShortcutOverride and the action on the key press itself.
    if ((watched == canvas_ || watched == sidebar_) && sidebar_ != nullptr && sidebar_->SidebarVisible()
        && !sidebar_->PageJumpEditing()
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool ctrl = key->modifiers().testFlag(Qt::ControlModifier);
        const bool pageKey = (ctrl && (key->key() == Qt::Key_C || key->key() == Qt::Key_X || key->key() == Qt::Key_V))
            || key->key() == Qt::Key_Delete;
        const bool sidebarOwnsKeys = watched == sidebar_
            || sidebar_->rect().contains(sidebar_->mapFromGlobal(QCursor::pos()));
        if (pageKey && sidebarOwnsKeys) {
            const std::size_t page = controller_.CurrentPageIndex();
            if (event->type() == QEvent::ShortcutOverride) {
                // Claim C/X/Delete unconditionally (copy/cut/delete always own
                // the key); V only when a page is actually on the clipboard,
                // otherwise an empty page clipboard would swallow a selection
                // paste.
                if (key->key() != Qt::Key_V || manager_.HasClipboardPage()) {
                    event->accept();
                    return true;
                }
            } else {
                bool handled = false;
                if (key->key() == Qt::Key_C) {
                    handled = controller_.CopyPage(page);
                } else if (key->key() == Qt::Key_X) {
                    handled = controller_.CutPage(page);
                } else if (key->key() == Qt::Key_V) {
                    handled = controller_.PastePage(page);
                } else {
                    handled = controller_.RemovePage(page);
                }
                if (handled || key->key() != Qt::Key_V) {
                    event->accept();
                    return true;
                }
            }
        }
    }

    // Reference TextEditProc (MessageLoop.cpp): inside a text edit Ctrl+B is the
    // editor's bold command, and in a LaTeX edit it does nothing - in both cases
    // it never reaches the window's sidebar binding.  QTextEdit with
    // acceptRichText(false) does not claim Ctrl+B itself, so the editor accepts
    // the ShortcutOverride here and the reference action runs on the key press.
    // The LaTeX source box is left inert: QPlainTextEdit owns Ctrl+B as its
    // standard bold key (as the reference's edit control did), so the window
    // stands down rather than stealing it for the sidebar.
    const bool inTextEditor = canvas_ != nullptr && watched == canvas_->TextEditor();
    const bool inLatexEditor = canvas_ != nullptr && watched == canvas_->LatexEditor();
    if ((inTextEditor || inLatexEditor)
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_B && key->modifiers() == Qt::ControlModifier) {
            if (event->type() == QEvent::ShortcutOverride) {
                // Neither editor claims Ctrl+B themselves, so claim the window
                // shortcut back for them; otherwise the sidebar binding fires.
                event->accept();
                return true;
            }
            if (inTextEditor) {
                ApplyTextFormat([](QTextCharFormat& format) {
                    format.setFontWeight(format.fontWeight() >= QFont::Bold ? QFont::Normal : QFont::Bold);
                });
                // Same last-use-wins rule as the popup toggles.
                SyncPendingTextStyle();
                ApplyPendingTextStyle();
                settingsStore_.Save(settings_);
                event->accept();
                return true;
            }
            // LaTeX source box: leave the key to the editor (its own standard
            // bold key), which is what the reference's edit control did.
            return false;
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
    (void)pages;
    // The reference frontend has no status bar: file/zoom live in the toolbar
    // band and the page indicator lives in the sidebar, so the canvas keeps the
    // full client height above its chrome.
    const bool editorOwnsKeyboard = EditorOwnsKeyboard();
    if (saveAction_ != nullptr) {
        saveAction_->setEnabled(controller_.Dirty() && !editorOwnsKeyboard);
    }
    if (undoAction_ != nullptr) {
        undoAction_->setEnabled(controller_.CanUndo() && !editorOwnsKeyboard);
    }
    if (redoAction_ != nullptr) {
        redoAction_->setEnabled(controller_.CanRedo() && !editorOwnsKeyboard);
    }
    if (copyAction_ != nullptr) {
        const bool hasSelection = controller_.HasSelection() && !editorOwnsKeyboard;
        copyAction_->setEnabled(hasSelection);
        cutAction_->setEnabled(hasSelection);
    }
    if (toolbar_ != nullptr) {
        toolbar_->SetUndoRedoEnabled(controller_.CanUndo(), controller_.CanRedo());
        toolbar_->SetTitle(fileLabel);
        toolbar_->SetZoomLabel(QStringLiteral("%1%").arg(canvas_ != nullptr ? qRound(canvas_->Zoom() * 100.0) : 100));
        toolbar_->SetMaximized(IsWindowMaximized());
        toolbar_->SetPageScale(canvas_ != nullptr ? canvas_->Zoom() : 1.0);
        toolbar_->SetColorIndex(settings_.colorIndex);
        // The popup belongs to the text tool as a whole now: while a text edit
        // is open it shows that editor's format, otherwise the pending style
        // the next created text will inherit.
        toolbar_->SetTextFormatAvailable(controller_.EditingText() || activeTool_ == settings::Tool::Text);
        if (controller_.EditingText()) {
            TextAlignment alignment = TextAlignment::Left;
            const PageTextStyle style = TextStyleFromEditor(canvas_ != nullptr ? canvas_->TextEditor() : nullptr, alignment);
            toolbar_->SetTextFormatState(style, alignment, settings_.textSizePreset);
        } else {
            toolbar_->SetTextFormatState(
                PageTextStyle {
                    settings_.textSizePresets[
                        std::min(settings_.textSizePreset, settings_.textSizePresets.size() - 1U)],
                    0xFF111827U,
                    settings_.textFontFamily,
                    settings_.textBold,
                    settings_.textItalic,
                    settings_.textUnderline,
                },
                settings_.textAlignment,
                settings_.textSizePreset);
        }
    }
    // The overlay editors are created lazily by the canvas; their Ctrl+B
    // precedence (reference TextEditProc) needs this window as a filter.  Qt
    // deduplicates repeated installs.
    if (QTextEdit* textEditor = canvas_ != nullptr ? canvas_->TextEditor() : nullptr; textEditor != nullptr) {
        textEditor->installEventFilter(this);
    }
    if (QPlainTextEdit* latexEditor = canvas_ != nullptr ? canvas_->LatexEditor() : nullptr; latexEditor != nullptr) {
        latexEditor->installEventFilter(this);
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
    // a text/LaTeX editor owns the keyboard (App::ShortcutAction::Tool*), and so
    // do the page keys - the legacy editor child window consumed PgUp/PgDn for
    // caret/scroll movement, so page navigation was only reachable from the
    // canvas.  The gate follows the controller's edit state, which is already
    // cleared by the time its EditStateChanged reaches this function (see
    // EditorOwnsKeyboard).
    // Editor focus precedence: every window shortcut stands down while a
    // text/LaTeX editor owns the keyboard - the editor's own bindings (Ctrl+B
    // bold, Ctrl+C/X/V clipboard, caret keys) take the keys instead, and
    // command shortcuts like zoom presets or tool keys must not fire mid-edit.
    // Command-state actions (save/undo/redo/copy/cut above) combine the gate
    // with their own applicability; the rest are simply disabled.
    for (QAction* action : actions()) {
        if (action == saveAction_ || action == undoAction_ || action == redoAction_
            || action == copyAction_ || action == cutAction_) {
            continue;  // gated above with their command state
        }
        if (action == quickSizeAction_) {
            continue;  // Ctrl+W is context-sensitive: it opens the text-format
                       // popup while editing, so it must stay enabled.
        }
        action->setEnabled(!editorOwnsKeyboard);
    }
    if (toolbar_ != nullptr) {
        toolbar_->SetAutoHideEnabled(settings_.autoHideToolbar);
    }
}

}  // namespace cnote::qt
