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
    ToggleWheel,
    OpenPalettePicker,
    ToggleQuickSize,
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
    [[nodiscard]] bool CanUndo() const { return canUndo_; }
    [[nodiscard]] bool CanRedo() const { return canRedo_; }
    void SetTitle(const QString& title);
    [[nodiscard]] QString Title() const { return title_; }
    void SetZoomLabel(const QString& label);
    void SetMaximized(bool maximized);
    [[nodiscard]] bool Maximized() const { return maximized_; }
    // Current canvas page scale, used only for the quick-size preview diameter
    // (App::QuickSizePreviewDiameterForTool).  Defaults to the reference 1.0.
    void SetPageScale(qreal scale);
    [[nodiscard]] qreal PageScale() const { return pageScale_; }
    // Native OS decorations supply the window buttons (plan step 6.3), so the
    // in-client caption cluster is off by default; the geometry reserve is kept.
    void SetCaptionButtonsVisible(bool visible);
    [[nodiscard]] bool CaptionButtonsVisible() const { return captionButtonsVisible_; }

    // Text-format state for the popup's active buttons (filled from the active
    // edit; the popup is only shown while SetTextFormatAvailable(true)).
    void SetTextFormatAvailable(bool available);
    [[nodiscard]] bool TextFormatAvailable() const { return textFormatAvailable_; }
    void SetTextFormatState(const std::optional<PageTextStyle>& style, TextAlignment alignment);

    // --- geometry ---------------------------------------------------------
    [[nodiscard]] appui::qt::ToolbarLayout ComputeLayout() const;
    [[nodiscard]] appui::qt::ToolbarTitleLayout ComputeTitleLayout() const;
    [[nodiscard]] QRectF ControlRect(ToolbarControl control) const;
    [[nodiscard]] ToolbarControl ControlAt(const QPointF& widgetPosition) const;
    [[nodiscard]] ToolbarControl HoveredControl() const { return hovered_; }
    [[nodiscard]] bool IsControlRect(const QPointF& widgetPosition) const;  // IsToolbarControlRect
    [[nodiscard]] static std::optional<settings::Tool> ToolForControl(ToolbarControl control);
    [[nodiscard]] static ToolbarControl ControlForTool(settings::Tool tool);

    // --- zoom inline edit -------------------------------------------------
    void BeginZoomEdit();
    void CancelZoomEdit();
    void CommitZoomEdit();
    [[nodiscard]] bool ZoomEditing() const { return zoomEditing_; }
    [[nodiscard]] QString ZoomEditText() const { return zoomEditText_; }

    // --- overlays ---------------------------------------------------------
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
    [[nodiscard]] bool TextFormatFontMenuOpen() const;

    void TogglePaletteMenu();
    void HidePaletteMenu();
    [[nodiscard]] bool PaletteMenuVisible() const;
    [[nodiscard]] QRectF PaletteMenuBounds() const;

    void TogglePaletteWheel(const QPointF& hostCenter);
    void HidePaletteWheel();
    [[nodiscard]] bool PaletteWheelVisible() const;
    [[nodiscard]] QRectF PaletteWheelBounds() const;
    [[nodiscard]] std::optional<std::size_t> PaletteWheelHoverIndex() const { return wheelHoverIndex_; }
    [[nodiscard]] std::vector<settings::Tool> PaletteWheelDisplayedTools() const;

    void CloseOverlays();
    [[nodiscard]] bool AnyOverlayVisible() const;

    // Applies/updates the palette color index; mirror of App::SetColorIndex's
    // view state (the shell owns the actual stroke recoloring).
    void SetColorIndex(std::size_t index);
    [[nodiscard]] std::size_t ColorIndex() const { return settings_.colorIndex; }

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
    void OnWheelZoomModeChanged(bool zoomPresetMode);
    void SetTextFormatFontMenuOpen(bool open);
    void ToggleTextFormatBold();
    void ToggleTextFormatItalic();
    void ToggleTextFormatUnderline();
    void ZoomCommitIfEditing();
    [[nodiscard]] QRectF HostCanvasRect() const;
    [[nodiscard]] QRectF HostClientRect() const;

    settings::Settings settings_ {};
    settings::Tool activeTool_ = settings::Tool::Draw;
    bool canUndo_ = false;
    bool canRedo_ = false;
    QString title_ {};
    QString zoomLabel_ {};
    bool maximized_ = false;
    bool captionButtonsVisible_ = false;
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
