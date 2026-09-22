#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include "Document.h"
#include "settings/SettingsStoreQt.h"
#include "ui/LayoutQt.h"

class QTimer;

namespace cnote::ui {

class QuickSizePopup;
class TextFormatPopup;
class PaletteMenuPopup;
class PaletteWheelOverlay;

// One control of the reference compact toolbar.  The geometry of every control
// comes from cnote::appui::qt::BuildToolbarLayout; this enum is the identity used
// for hit-testing, hover state and the per-control click intent.
enum class ToolbarControl {
    None,
    TitleMenu,
    Zoom,
    Minimize,
    Maximize,
    Close,
    Undo,
    Redo,
    SelectTool,
    MoveTool,
    TextTool,
    DrawTool,
    PanTool,
    EraseTool,
    CurrentColor,
};

// Discrete command intents the shell owns (plan step 6.2: one action per
// command, shared with menus/shortcuts; the widget never executes them).
enum class ToolbarCommand {
    Undo,
    Redo,
    Minimize,
    Maximize,
    Close,
    OpenTitleMenu,
    OpenPalettePicker,
};

// The reference compact toolbar (plan steps 6.1/6.3).
//
// Owns the toolbar strip (34 logical pixels, 39 maximized), its hover state,
// its auto-hide 120 ms reveal/hide animation, and the four reference overlays:
// the palette swatch menu, the palette/tool wheel, the quick-size brush popup
// and the text-format popup.  It holds only view state - the settings payload,
// the active tool, undo/redo availability, title/zoom labels and the current
// text format - and reports every command as a signal so the Qt shell can route
// it through its QActions.  No document, App, HWND or HDC state is involved.
//
// Placement contract: the widget occupies the full client width of its host
// window at y == 0 (the shell reserves appui::qt::ReservedToolbarHeight()).
// Overlays are children of window() positioned in host coordinates; the host
// must therefore be the top-level window, and it must not use a QLayout for the
// toolbar or its overlays.
class ToolbarWidget : public QWidget {
    Q_OBJECT

public:
    explicit ToolbarWidget(QWidget* parent = nullptr);
    ~ToolbarWidget() override;

    // --- model / settings payload ----------------------------------------
    void SetSettings(const settings::Settings& settings);
    [[nodiscard]] const settings::Settings& Settings() const { return settings_; }

    void SetActiveTool(settings::Tool tool);
    [[nodiscard]] settings::Tool ActiveTool() const { return activeTool_; }
    void SetUndoRedoEnabled(bool canUndo, bool canRedo);
    void SetTitle(const QString& title);
    [[nodiscard]] QString Title() const { return title_; }
    void SetZoomLabel(const QString& label);
    void SetMaximized(bool maximized);
    [[nodiscard]] bool Maximized() const { return maximized_; }
    // Current canvas page scale, used only for the quick-size preview diameter
    // (App::QuickSizePreviewDiameterForTool).  Defaults to the reference 1.0.
    void SetPageScale(qreal scale);
    [[nodiscard]] qreal PageScale() const { return pageScale_; }
    // The reference frontend draws its own minimize/maximize/close cluster in
    // the band (there is no native caption), so the Windows shell enables it;
    // platforms that keep native decorations leave it off.  The cluster's
    // geometry is always reserved by the layout either way.
    void SetCaptionButtonsVisible(bool visible);
    // Native frame width the reference subtracts from the caption cluster's
    // right edge while maximized (WindowFrameThicknessX, Layout.cpp:211) and the
    // reveal band's top inset (App.cpp:2465).  Logical pixels; the shell supplies
    // it from the window's DPI-scaled frame metrics.
    void SetFrameThicknessX(qreal thickness);

    // Text-format state for the popup's active buttons (filled from the active
    // edit; the popup is only shown while SetTextFormatAvailable(true)).
    void SetTextFormatAvailable(bool available);
    void SetTextFormatState(const std::optional<PageTextStyle>& style, TextAlignment alignment);

    // --- geometry ---------------------------------------------------------
    [[nodiscard]] appui::qt::ToolbarLayout ComputeLayout() const;
    [[nodiscard]] appui::qt::ToolbarTitleLayout ComputeTitleLayout() const;
    [[nodiscard]] QRectF ControlRect(ToolbarControl control) const;
    [[nodiscard]] ToolbarControl ControlAt(const QPointF& widgetPosition) const;
    [[nodiscard]] ToolbarControl HoveredControl() const { return hovered_; }
    // Same hit test in host (window client) coordinates: the widget is offset
    // while the auto-hide reveal animation runs, so the native hit test cannot
    // use raw client positions.  The shell's WM_NCHITTEST and the widget's own
    // mouse routing must agree on where the controls are.
    [[nodiscard]] ToolbarControl ControlAtHostPosition(const QPointF& hostPosition) const;
    // Hover state driven from the native hit test, which also sees the moves
    // Windows routes as non-client (the band's caption strip) and therefore
    // never reaches the widget's own mouse handlers.
    void UpdateHoverAtHostPosition(const QPointF& hostPosition);
    [[nodiscard]] static std::optional<settings::Tool> ToolForControl(ToolbarControl control);
    [[nodiscard]] static ToolbarControl ControlForTool(settings::Tool tool);

    // --- zoom inline edit -------------------------------------------------
    void BeginZoomEdit();
    void CancelZoomEdit();
    void CommitZoomEdit();
    [[nodiscard]] bool ZoomEditing() const { return zoomEditing_; }

    // --- overlays ---------------------------------------------------------
    // The host-space canvas rect (client area below the reserved toolbar band)
    // that anchors and clamps the overlays - the reference App::CanvasRect.
    // The shell needs it to resolve the Ctrl+W cursor anchor's out-of-canvas
    // fallback (App::ToggleQuickSizePopup, ToolbarState.cpp).
    [[nodiscard]] QRectF HostCanvasRect() const;
    void ShowQuickSizePopup(settings::Tool tool, const QPointF& hostAnchor);
    void HideQuickSizePopup();
    [[nodiscard]] bool QuickSizePopupVisible() const;
    [[nodiscard]] QRect QuickSizePopupBounds() const;
    [[nodiscard]] bool QuickSizeSliderDragging() const;
    [[nodiscard]] float QuickSizeValue() const;
    [[nodiscard]] settings::Tool QuickSizeTool() const;

    void ShowTextFormatPopup(const QRectF& editRectInHost);
    void HideTextFormatPopup();
    [[nodiscard]] bool TextFormatPopupVisible() const;
    [[nodiscard]] QRect TextFormatPopupBounds() const;

    void TogglePaletteMenu();
    void HidePaletteMenu();
    [[nodiscard]] bool PaletteMenuVisible() const;
    [[nodiscard]] QRectF PaletteMenuBounds() const;

    // Reference entry (App::TogglePaletteWheel, ToolbarState.cpp:127-159): the
    // wheel opens centred on the cursor, or - when the cursor is outside the
    // host client rect - below the toolbar's current-colour swatch.  The shell
    // uses this overload; the explicit-anchor one stays public for callers and
    // tests that already know the host-space centre.
    void TogglePaletteWheel();
    void TogglePaletteWheel(const QPointF& hostCenter);
    void HidePaletteWheel();
    [[nodiscard]] bool PaletteWheelVisible() const;
    // Host-space centre the open wheel is anchored on (the zoom anchor the
    // reference passes to ApplyZoomPreset, MessageLoop.cpp:427).
    [[nodiscard]] QPointF PaletteWheelCenter() const { return wheelCenter_; }
    [[nodiscard]] std::optional<std::size_t> PaletteWheelHoverIndex() const { return wheelHoverIndex_; }
    [[nodiscard]] std::vector<settings::Tool> PaletteWheelDisplayedTools() const;

    void CloseOverlays();
    [[nodiscard]] bool AnyOverlayVisible() const;

    // Applies/updates the palette color index; mirror of App::SetColorIndex's
    // view state (the shell owns the actual stroke recoloring).
    void SetColorIndex(std::size_t index);

    // --- auto-hide (plan step 6.1, App.cpp:2298-2600) ---------------------
    void SetAutoHideEnabled(bool enabled);
    [[nodiscard]] bool AutoHideEnabled() const { return settings_.autoHideToolbar; }
    void SetRevealed(bool revealed);
    [[nodiscard]] bool Revealed() const { return revealed_; }
    [[nodiscard]] qreal RevealProgress() const { return revealProgress_; }
    // `hostPosition` is in host (window) coordinates, `interactionActive` is the
    // reference guard list of active drags/editors.
    void UpdateAutoHideForCursor(const QPointF& hostPosition, bool interactionActive);
    // Current vertical offset inside the host for the reveal animation
    // (App::ToolbarOffsetYForProgress).
    [[nodiscard]] qreal ToolbarOffsetY() const;
    // App::VisibleToolbarHeight: the band height while the toolbar counts as
    // visible - auto-hide off, revealed, mid-animation, or an editor/overlay
    // owning the band - otherwise 0.  The native hit test uses it to decide
    // whether the band's non-control strip is a caption drag area.
    [[nodiscard]] qreal VisibleToolbarHeight() const;

    [[nodiscard]] QSize sizeHint() const override;

signals:
    void CommandTriggered(ToolbarCommand command);
    void ToolSelected(settings::Tool tool);
    void ColorSelected(std::size_t colorIndex);
    // The reference palette "..." picker (App::OpenToolbarPaletteSwapMenu).
    void PalettePickerRequested();
    void ZoomTextCommitted(const QString& text);
    void BrushSizeChanged(settings::Tool tool, float size);
    void TextAlignCycleRequested();
    void TextBoldToggleRequested();
    void TextItalicToggleRequested();
    void TextUnderlineToggleRequested();
    void TextSizeChanged(float size);
    void TextFontFamilyChanged(TextFontFamily family);
    void PaletteOverlayToggled(bool open);
    void ZoomPresetRequested(float zoom);
    // The reference tool-button context menu (App::HandleToolbarRightClick,
    // App.cpp:6195-6239) toggles a tool's Quick Wheel membership; the shell
    // owns persisting it into settings.
    void WheelToolToggled(settings::Tool tool);
    void RevealProgressChanged(qreal progress);
    void RevealStateChanged(bool revealed);
    void ToolbarGeometryChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void UpdateToolbarGeometry();
    void StartRevealAnimation(qreal target);
    void OnRevealTick();
    void EnsureOverlays();
    void PositionOverlays();
    void OnQuickSizeChanged(settings::Tool tool, float size);
    void OnTextFormatChanged();
    void OnPaletteSelection(std::size_t index);
    void OnPaletteToolSelection(settings::Tool tool);
    // Right-click on a tool button or the zoom indicator shows the "Add/Remove
    // from Quick Wheel" menu (App::HandleToolbarRightClick).  Returns true when
    // the click landed on a tool rect and the menu was shown.
    bool ShowWheelToolMenu(const QPointF& widgetPosition, const QPoint& globalPosition);
    [[nodiscard]] bool IsToolInWheel(settings::Tool tool) const;
    void OnWheelZoomModeChanged(bool zoomPresetMode);
    void SetTextFormatFontMenuOpen(bool open);
    void ToggleTextFormatBold();
    void ToggleTextFormatItalic();
    void ToggleTextFormatUnderline();
    void ZoomCommitIfEditing();
    [[nodiscard]] QRectF HostClientRect() const;

    settings::Settings settings_ {};
    settings::Tool activeTool_ = settings::Tool::Draw;
    bool canUndo_ = false;
    bool canRedo_ = false;
    QString title_ {};
    QString zoomLabel_ {};
    bool maximized_ = false;
    bool captionButtonsVisible_ = false;
    qreal frameThicknessX_ = 0.0;
    qreal pageScale_ = 1.0;
    bool textFormatAvailable_ = false;
    std::optional<PageTextStyle> formatStyle_ {};
    TextAlignment formatAlignment_ = TextAlignment::Left;

    ToolbarControl hovered_ = ToolbarControl::None;
    bool zoomEditing_ = false;
    QString zoomEditText_ {};

    bool revealed_ = true;
    qreal revealProgress_ = 1.0;
    QTimer* revealTimer_ = nullptr;
    QElapsedTimer revealClock_ {};
    qreal revealFrom_ = 1.0;
    qreal revealTo_ = 1.0;

    std::optional<std::size_t> wheelHoverIndex_ {};
    QRectF textFormatEditRect_ {};

    // Overlays are parented to window() and tracked with QPointer semantics via
    // Qt's parent ownership; the raw pointers are cleared on destruction.
    QuickSizePopup* quickSizePopup_ = nullptr;
    TextFormatPopup* textFormatPopup_ = nullptr;
    PaletteMenuPopup* paletteMenuPopup_ = nullptr;
    PaletteWheelOverlay* paletteWheel_ = nullptr;
    QPointF wheelCenter_ {};

    friend class QuickSizePopup;
    friend class TextFormatPopup;
    friend class PaletteMenuPopup;
    friend class PaletteWheelOverlay;
};

}  // namespace cnote::ui
