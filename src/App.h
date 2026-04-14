#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <array>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "Document.h"
#include "PdfBackend.h"
#include "ProjectFile.h"

namespace cnote {

class SettingsWindow;
class NewPageSettingsWindow;

class App {
public:
    static constexpr std::size_t kZoomPresetCount = 5;

    explicit App(HINSTANCE instance);
    ~App();

    int Run(int showCommand, std::optional<std::wstring> startupPath = std::nullopt);
    enum class Tool {
        Select,
        Move,
        Text,
        Draw,
        Pan,
        Erase,
    };

    enum class SelectionMode {
        Box,
        Lasso,
    };

    enum class HistoryActionType {
        AddStroke,
        RemoveStroke,
        AddPages,
        RemovePages,
        ReplacePages,
    };

    enum class ShortcutAction {
        ToolDraw,
        ToolSelect,
        ToolMove,
        ToolText,
        ToolPan,
        ToolErase,
        New,
        Undo,
        Redo,
        Open,
        Save,
        ExportPdf,
        Copy,
        Cut,
        Paste,
        ResetView,
        ZoomPreset1,
        ZoomPreset2,
        ZoomPreset3,
        ZoomPreset4,
        ZoomPreset5,
        ToggleSettings,
        TogglePdfSidebar,
        ToggleQuickSize,
        ToggleWheel,
        PreviousPage,
        NextPage,
    };

    enum class ImageTransformHandle {
        None,
        ResizeNorthWest,
        ResizeNorth,
        ResizeNorthEast,
        ResizeEast,
        ResizeSouthEast,
        ResizeSouth,
        ResizeSouthWest,
        ResizeWest,
        Rotate,
    };

    struct PageView {
        RECT rect {};
        std::size_t pageIndex = 0;
        float scale = 1.0f;
        bool valid = false;
    };

    struct HistoryAction {
        HistoryActionType type = HistoryActionType::AddStroke;
        Stroke stroke {};
        std::vector<Page> pages {};
        std::vector<Page> replacementPages {};
        std::vector<std::size_t> pageIndices {};
        std::size_t index = 0;
        std::size_t pageIndex = 0;
        std::size_t focusPageIndex = 0;
        std::size_t previousPageIndex = 0;
    };

    struct PageSurfaceCache {
        int width = 0;
        int height = 0;
        int stride = 0;
        std::size_t pageIndex = 0;
        std::size_t strokeCount = 0;
        std::size_t imageCount = 0;
        std::size_t textCount = 0;
        int rasterWidth = 0;
        int rasterHeight = 0;
        float rasterScale = 0.0f;
        float pageScale = 0.0f;
        bool valid = false;
        std::vector<std::uint8_t> pixels {};
    };

    struct SidebarThumbnailCache {
        int width = 0;
        int height = 0;
        int stride = 0;
        int sourceWidth = 0;
        int sourceHeight = 0;
        float sourceScale = 0.0f;
        bool valid = false;
        std::vector<std::uint8_t> pixels {};
    };

    struct ZoomPageSnapshot {
        int width = 0;
        int height = 0;
        int stride = 0;
        std::size_t pageIndex = 0;
        bool valid = false;
        std::vector<std::uint8_t> pixels {};
    };

    enum class InteractionPreviewMode {
        None,
        MoveSelection,
        ImageTransform,
        StrokeTransform,
        TextTransform,
    };

    struct InteractionPreviewStroke {
        std::size_t sourcePageIndex = 0;
        std::size_t sourceIndex = 0;
        Stroke stroke {};
    };

    struct InteractionPreviewImage {
        std::size_t sourcePageIndex = 0;
        std::size_t sourceIndex = 0;
        PageImage image {};
    };

    struct InteractionPreviewText {
        std::size_t sourcePageIndex = 0;
        std::size_t sourceIndex = 0;
        PageText text {};
        float boundsWidth = 0.0f;
        float boundsHeight = 0.0f;
    };

    struct MoveSelectionTextCacheEntry {
        std::size_t textIndex = 0;
        float centerX = 0.0f;
        float centerY = 0.0f;
        float boundsWidth = 0.0f;
        float boundsHeight = 0.0f;
    };

    struct RenderRequest {
        std::shared_ptr<std::vector<std::uint8_t>> pdfData {};
        std::size_t pageIndex = 0;
        std::int32_t sourcePdfPageIndex = -1;
        float renderScale = 0.0f;
        std::uint64_t generation = 0;
    };

    struct RenderResult {
        std::size_t pageIndex = 0;
        float renderScale = 0.0f;
        std::uint64_t generation = 0;
        std::optional<RasterImage> raster {};
    };

    struct KeyBinding {
        UINT key = 0;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
    };

    struct ShortcutState {
        std::vector<std::pair<ShortcutAction, KeyBinding>> bindings {};
    };

    struct WheelState {
        bool paletteMenuOpen = false;
        bool paletteWheelOpen = false;
        POINT center {};
        std::vector<Tool> tools {};
    };

    struct SettingsState {
        SettingsWindow* window = nullptr;
        std::size_t paletteIndex = 0;
        float drawBrushSize = 3.0f;
        float eraseBrushSize = 3.0f;
        float inputSpacing = 0.85f;
        float inputSmoothing = 0.3f;
        float strokeSmoothing = 0.55f;
        unsigned int autoSaveIntervalSeconds = 60;
        bool autoHideToolbar = false;
        std::array<float, kZoomPresetCount> zoomPresets {};
    };

    struct NamedPalette {
        std::wstring name {};
        std::vector<std::uint32_t> colors {};
    };

    struct NewPageSettingsState {
        NewPageSettingsWindow* window = nullptr;
        float width = 612.0f;
        float height = 792.0f;
        std::uint32_t backgroundColor = 0xFFFFFFFFU;
        std::vector<std::uint32_t> recentColors {};
    };

private:
    friend class SettingsWindow;
    friend class NewPageSettingsWindow;

    bool CreateMainWindow(int showCommand);
    void StartRenderWorker();
    void StopRenderWorker();
    void CreateBlankDocument();
    void ClearHistory();
    void InvalidateCanvas();
    void InvalidateViewport();
    void InvalidatePageSurfaceCache(std::size_t pageIndex);
    void InvalidateAllPageSurfaceCaches();
    void InvalidateVisiblePageSurfaceCaches();
    void InvalidateStrokeRegion(const Stroke& stroke, std::size_t pageIndex, bool smoothLiveStroke);
    void ResetView();
    void SetActiveTool(Tool tool);
    void PreviousPage();
    void NextPage();
    void SetCurrentPageIndex(std::size_t index);
    void FocusPage(std::size_t index);
    void AddBlankPageAfterCurrent();
    void CopyCurrentPageToSidebarBuffer();
    bool CutCurrentPageToSidebarBuffer();
    bool PasteSidebarPageAfterCurrent();
    bool DeleteCurrentPage();
    void ToggleBrushSettings();
    void TogglePdfSidebar();
    void Undo();
    void Redo();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    [[nodiscard]] bool CanGoToPreviousPage() const;
    [[nodiscard]] bool CanGoToNextPage() const;
    void NotifyDocumentMutated();
    void MarkDocumentSaved();
    void UpdateAutoSaveTimer();
    [[nodiscard]] bool HasSavedCnotePath() const;
    [[nodiscard]] bool CanAutoSaveCurrentDocument() const;
    [[nodiscard]] bool IsDocumentInteractionActive() const;
    void RestoreAfterPageHistoryChange(std::size_t focusPageIndex);
    void RestoreAfterContentHistoryChange(const std::vector<std::size_t>& pageIndices, std::size_t focusPageIndex);
    void PushPageHistoryAction(HistoryActionType type, std::size_t index, std::vector<Page> pages, std::size_t focusPageIndex, std::size_t previousPageIndex);
    void PushReplacePagesHistoryAction(std::vector<std::size_t> pageIndices, std::vector<Page> beforePages, std::size_t focusPageIndex);
    void AddPaletteColor();
    void RemovePaletteColor();
    [[nodiscard]] bool ApplyHexColorFromClipboard();
    [[nodiscard]] bool SaveNamedPalette(const std::wstring& name);
    [[nodiscard]] bool ApplyNamedPalette(std::size_t index);
    [[nodiscard]] bool DeleteNamedPalette(std::size_t index);
    void UpdateWindowTitle();
    void BeginStroke(POINT cursor);
    void ExtendStroke(POINT cursor);
    void EndStroke();
    void StartStrokeSegment(std::size_t pageIndex, Vec2 pagePoint);
    void BeginSelection(POINT cursor, bool boxOverride = false);
    void ExtendSelection(POINT cursor);
    void EndSelection();
    void BeginMoveSelection(POINT cursor);
    void UpdateMoveSelection(POINT cursor);
    void EndMoveSelection();
    void BeginTextTransform(std::size_t pageIndex, std::size_t textIndex, ImageTransformHandle handle, POINT cursor);
    void UpdateTextTransform(POINT cursor);
    void EndTextTransform();
    void BeginTextToolInteraction(POINT cursor);
    void BeginTextEdit(std::size_t pageIndex, std::size_t textIndex, Page beforePage);
    void CommitTextEdit();
    void CancelTextEdit();
    void EndTextEditControl();
    bool EnsureTextEditWindow(TextAlignment alignment);
    void RefreshTextEditControl();
    void InvalidateEditingTextRegion();
    void ReleaseTextEditBackgroundBrush();
    void RefreshTextEditBackgroundBrush(const RECT& editRect);
    void BeginImageTransform(std::size_t pageIndex, std::size_t imageIndex, ImageTransformHandle handle, POINT cursor);
    void UpdateImageTransform(POINT cursor);
    void EndImageTransform();
    void BeginStrokeTransform(std::size_t pageIndex, ImageTransformHandle handle, POINT cursor);
    void UpdateStrokeTransform(POINT cursor);
    void EndStrokeTransform();
    void BeginErase(POINT cursor);
    void ExtendErase(POINT cursor);
    [[nodiscard]] bool EraseAt(POINT cursor);
    void BeginPan(POINT cursor, bool fromMiddleButton);
    void UpdatePan(POINT cursor);
    void EndPan();
    void UpdateCaptureState();
    void ExecuteCommand(unsigned int commandId);
    [[nodiscard]] bool HandleSidebarClick(POINT cursor);
    [[nodiscard]] bool HandleToolbarClick(POINT cursor);
    [[nodiscard]] bool HandleQuickSizePopupClick(POINT cursor);
    [[nodiscard]] bool HandleToolbarRightClick(POINT cursor);
    [[nodiscard]] bool HandlePaletteClick(POINT cursor);
    void TogglePaletteWheel();
    void ToggleQuickSizePopup();
    void CancelQuickSizePopup(bool cancelInput = true);
    void ClosePaletteOverlays();
    void ToggleToolInWheel(Tool tool);
    [[nodiscard]] bool IsToolInWheel(Tool tool) const;
    [[nodiscard]] bool HandleShortcutKey(UINT key, bool ctrl, bool shift, bool alt);
    void ExecuteShortcutAction(ShortcutAction action);
    [[nodiscard]] std::wstring FormatKeyBinding(const KeyBinding& binding) const;
    void LoadAppSettings();
    void SaveAppSettings() const;
    void RefreshSettingsWindow();
    [[nodiscard]] bool IsSettingsWindowOpen() const;
    void ToggleNewPageSettings();
    void RefreshNewPageSettingsWindow();
    void ResetNewPageSettings();
    [[nodiscard]] Page BuildConfiguredNewPage() const;
    void ApplyConfiguredSettingsToCurrentPage(float width, float height, std::uint32_t backgroundColor);
    void ApplyConfiguredSettingsToAllPages(float width, float height, std::uint32_t backgroundColor);
    [[nodiscard]] bool AppendPagesFromFile();
    [[nodiscard]] bool AppendPagesFromDocumentPath(const std::wstring& filePath);
    void PanBy(float dx, float dy);
    void ZoomAt(int delta, POINT cursor);
    void SetZoom(float zoom, std::optional<POINT> anchor = std::nullopt);
    [[nodiscard]] bool ApplyZoomPreset(std::size_t presetIndex);
    void BeginZoomSession(POINT cursor);
    void EndZoomSession();
    void ScheduleDeferredRasterRefresh();
    void CancelDeferredRasterRefresh(bool clearPending = true);
    void HandleDeferredRasterRefresh();
    void HandleCompletedRenderResults();
    void QueuePageRasterRequest(const PageView& pageView);
    void ResetPendingRasterWork();
    [[nodiscard]] bool SidebarOwnsKeyboardShortcuts() const;
    void ClearSelection();
    [[nodiscard]] bool HasSelection() const;
    [[nodiscard]] bool HasSelectedStrokes() const;
    [[nodiscard]] std::optional<std::size_t> FirstSelectedPageIndex() const;
    void DeleteSelection(std::optional<std::size_t> pageIndex = std::nullopt);
    void AdjustSelectionForRemoval(std::size_t pageIndex, std::size_t strokeIndex);
    void AdjustSelectionForInsertion(std::size_t pageIndex, std::size_t strokeIndex);
    [[nodiscard]] bool IsImageSelected(std::size_t pageIndex, std::size_t imageIndex) const;
    [[nodiscard]] bool IsStrokeSelected(std::size_t pageIndex, std::size_t strokeIndex) const;
    [[nodiscard]] bool IsTextSelected(std::size_t pageIndex, std::size_t textIndex) const;
    [[nodiscard]] bool IsEditingText(std::size_t pageIndex, std::size_t textIndex) const;
    [[nodiscard]] std::optional<PageText> CurrentEditingTextLayout() const;
    [[nodiscard]] std::optional<ImageTransformHandle> HitTestSelectedImageHandle(POINT cursor, std::size_t* pageIndex = nullptr, std::size_t* imageIndex = nullptr) const;
    [[nodiscard]] std::optional<ImageTransformHandle> HitTestSelectedStrokeHandle(POINT cursor, std::size_t* pageIndex = nullptr) const;
    [[nodiscard]] std::optional<ImageTransformHandle> HitTestSelectedTextHandle(POINT cursor, std::size_t* pageIndex = nullptr, std::size_t* textIndex = nullptr) const;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> HitTestText(POINT cursor) const;
    [[nodiscard]] std::vector<Vec2> CurrentSelectionLassoPolygon() const;
    [[nodiscard]] bool CursorHitsSelectionMoveGrip(POINT cursor) const;
    [[nodiscard]] bool CursorHitsSelectedContent(POINT cursor) const;
    [[nodiscard]] bool CursorInteractsWithSelection(POINT cursor) const;
    bool CopySelectionToClipboard();
    bool CutSelectionToClipboard();
    bool PasteSelectionFromClipboard();
    bool PasteImageFromClipboard();
    [[nodiscard]] std::optional<Vec2> DefaultPasteOrigin(std::size_t pageIndex) const;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> SingleSelectedImage() const;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> SingleSelectedText() const;
    [[nodiscard]] std::optional<std::size_t> SingleSelectedStrokePage() const;
    void TranslateSelectedContent(std::size_t pageIndex, Vec2 delta, bool clampToPage = true, bool notify = true);
    bool RehomeSelectedContentAcrossPages(bool clampToPage);
    void ChangeBrushSize(float delta);
    [[nodiscard]] float BrushSizeForTool(Tool tool) const;
    void SetBrushSizeForTool(Tool tool, float size, bool refreshUi = true);
    void SyncBrushWidthFromActiveTool();
    void CycleColor();
    void SetColorIndex(std::size_t index);
    void OpenDocument();
    [[nodiscard]] bool OpenDocumentPath(const std::wstring& filePath);
    [[nodiscard]] bool OpenProject(const std::wstring& filePath);
    [[nodiscard]] bool OpenPdf(const std::wstring& filePath);
    [[nodiscard]] bool SaveCurrentCnote();
    [[nodiscard]] bool SaveCnoteToPath(const std::wstring& filePath, bool showError);
    [[nodiscard]] bool SaveAsCnote();
    [[nodiscard]] bool ExportAsPdf();
    void PrepareForDocumentSwitch();
    void ApplyDocument(Document document, bool resetBrushWidth = false);
    [[nodiscard]] std::wstring SuggestedCnotePath() const;
    [[nodiscard]] std::wstring SuggestedPdfExportPath() const;
    void EnsureVisiblePageRasterQuality();
    void EnsurePageRasterQuality(const PageView& pageView);

    [[nodiscard]] bool IsCursorInsidePage(POINT cursor) const;
    [[nodiscard]] std::optional<PageView> HitTestPage(POINT cursor) const;
    [[nodiscard]] std::optional<PageView> ResolveZoomAnchorPage(RECT clientRect, POINT anchorPoint) const;
    [[nodiscard]] Vec2 ScreenToPage(POINT cursor, std::size_t* pageIndex = nullptr) const;
    [[nodiscard]] Vec2 ScreenToPage(const PageView& pageView, POINT cursor, bool clampToPage) const;
    [[nodiscard]] POINT PageToScreen(std::size_t pageIndex, Vec2 point) const;
    [[nodiscard]] PageView CalculatePageView(RECT clientRect, std::size_t pageIndex) const;
    [[nodiscard]] std::vector<PageView> CalculatePageViews(RECT clientRect) const;
    [[nodiscard]] std::optional<std::size_t> ViewedPageIndex(RECT clientRect) const;
    [[nodiscard]] float DesiredRenderScale(const PageView& pageView) const;
    [[nodiscard]] COLORREF ToColorRef(std::uint32_t argb) const;
    [[nodiscard]] std::uint32_t ComposeColor(unsigned char red, unsigned char green, unsigned char blue) const;
    [[nodiscard]] unsigned char GetColorChannel(std::uint32_t color, int shift) const;
    [[nodiscard]] std::wstring CurrentWindowTitle() const;
    [[nodiscard]] std::wstring ZoomIndicatorLabel() const;
    [[nodiscard]] std::wstring PageIndicatorLabel() const;
    [[nodiscard]] bool IsPdfSidebarVisible() const;
    [[nodiscard]] int PdfSidebarWidth() const;
    [[nodiscard]] RECT PdfSidebarRect(RECT clientRect) const;
    void StartPdfSidebarSlideAnimation(float targetProgress);
    void StopPdfSidebarSlideAnimationTimer();
    void HandlePdfSidebarSlideAnimationTimer();
    void InvalidatePdfSidebarAnimationRegion();
    [[nodiscard]] RECT CanvasRect(RECT clientRect) const;
    [[nodiscard]] int FullToolbarHeight() const;
    [[nodiscard]] int ReservedToolbarHeight() const;
    [[nodiscard]] int VisibleToolbarHeight() const;
    [[nodiscard]] bool IsToolbarVisible() const;
    void SetToolbarRevealed(bool revealed);
    void SetToolbarAutoHideEnabled(bool enabled);
    void UpdateToolbarAutoHide(POINT cursor);
    void StartToolbarRevealAnimation(float targetProgress);
    void StopToolbarRevealAnimationTimer();
    void HandleToolbarRevealAnimationTimer();
    [[nodiscard]] std::optional<std::size_t> HitTestSidebarPage(POINT cursor) const;
    void EnsureSidebarCurrentPageVisible();
    void SyncCurrentPageToViewport();
    void InvalidateSidebarThumbnailCache(std::size_t pageIndex);
    void EnsureSidebarThumbnailCache(std::size_t pageIndex, int width, int height);
    void CancelPageJumpInput();
    void BeginPageJumpInput();
    void ApplyPageJumpInput();
    [[nodiscard]] std::optional<Tool> ToolbarBrushTool() const;
    [[nodiscard]] std::wstring ToolbarBrushValueLabel() const;
    void CancelToolbarBrushSizeInput();
    void BeginToolbarBrushSizeInput();
    void ApplyToolbarBrushSizeInput();
    void CancelToolbarZoomInput();
    void BeginToolbarZoomInput();
    void ApplyToolbarZoomInput();
    void BeginToolbarBrushSliderDrag(POINT cursor);
    void UpdateToolbarBrushSliderDrag(POINT cursor);
    void EndToolbarBrushSliderDrag();
    void ToggleTextFormatPopup();
    void CancelTextFormatPopup();
    [[nodiscard]] bool HandleTextFormatPopupClick(POINT cursor);
    [[nodiscard]] std::wstring TextFormatFontSizeLabel() const;
    void CancelTextFormatSizeInput(bool restoreFocus = true);
    void BeginTextFormatSizeInput();
    void ApplyTextFormatSizeInput();
    void SetEditingTextAlignment(TextAlignment alignment);
    void CycleEditingTextAlignment();
    void SetEditingTextFontSize(float fontSize);
    void SetEditingTextFontFamily(TextFontFamily family);
    void ToggleEditingTextBold();
    void ToggleEditingTextItalic();
    void ToggleEditingTextUnderline();

    void Paint();
    void PaintToolbar(HDC hdc, RECT clientRect) const;
    void PaintPdfSidebar(HDC hdc, RECT clientRect);
    void PaintQuickSizePopup(HDC hdc, RECT clientRect) const;
    void PaintTextFormatPopup(HDC hdc, RECT clientRect) const;
    void PaintLiveTextEditOverlay(HDC hdc) const;
    void PaintPaletteMenu(HDC hdc, RECT clientRect) const;
    void PaintPaletteWheel(HDC hdc, RECT clientRect) const;
    void PaintCanvas(HDC hdc, RECT clientRect);
    void PaintPage(HDC hdc, const PageView& pageView);
    void PaintSelectionOverlay(HDC hdc, RECT canvasRect) const;
    void PaintInteractionPreview(HDC hdc, const PageView& pageView, bool showSelectToolSelectionAffordances) const;
    void PaintStroke(HDC hdc, const Stroke& stroke, const PageView& pageView, bool smoothLiveStroke, bool fastPreview = false) const;
    void PaintText(HDC hdc, const PageText& text, const PageView& pageView) const;
    bool TryAppendStrokeToPageSurfaceCache(std::size_t pageIndex, const Stroke& stroke);
    bool TryRefreshPageSurfaceCacheForStroke(std::size_t pageIndex, const Stroke& stroke);
    bool TryRefreshPageSurfaceCacheRegion(std::size_t pageIndex, RECT dirtyRect);
    void EnsurePageSurfaceCache(const PageView& pageView);
    void ClearInteractionPreview();
    void EnsureInteractionPreviewStorage();
    void BuildInteractionPageSnapshot(const PageView& pageView);
    [[nodiscard]] bool ShouldSkipStrokeInInteractionSnapshot(std::size_t pageIndex, std::size_t strokeIndex) const;
    [[nodiscard]] bool ShouldSkipImageInInteractionSnapshot(std::size_t pageIndex, std::size_t imageIndex) const;
    [[nodiscard]] bool ShouldSkipTextInInteractionSnapshot(std::size_t pageIndex, std::size_t textIndex) const;
    void RebuildMoveSelectionTextCache();
    void RebuildMoveSelectionPreview(POINT cursor);
    void RebuildImageTransformPreview(POINT cursor);
    void RebuildStrokeTransformPreview(POINT cursor);
    void CommitMoveSelectionPreview();
    void CommitImageTransformPreview();
    void CommitStrokeTransformPreview();
    [[nodiscard]] std::optional<RECT> InteractionPreviewBoundsForPage(std::size_t pageIndex) const;
    void InvalidateInteractionPreviewDelta(const std::vector<std::optional<RECT>>& previousBounds);
    void EnsurePaintBackbuffer(HDC referenceDc, int width, int height);
    void ReleasePaintBackbuffer();
    void CaptureZoomSessionSnapshots();
    void CaptureZoomPageSnapshot(const PageView& pageView);
    void ClearZoomSessionSnapshots();

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TextEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    Document document_ = Document::CreateBlank();
    PdfiumBackend pdfBackend_ {};

    float zoom_ = 1.0f;
    float targetZoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    float brushWidth_ = 3.0f;
    Tool activeTool_ = Tool::Draw;
    std::size_t colorIndex_ = 0;
    bool isDrawing_ = false;
    bool isErasing_ = false;
    bool isPanning_ = false;
    bool panFromMiddleButton_ = false;
    bool isSelecting_ = false;
    bool isMovingSelection_ = false;
    bool isTransformingImage_ = false;
    bool isTransformingStroke_ = false;
    bool isTransformingText_ = false;
    std::optional<Stroke> activeStroke_ {};
    std::optional<std::size_t> activeStrokePageIndex_ {};
    std::vector<HistoryAction> undoActions_ {};
    std::vector<HistoryAction> redoActions_ {};
    POINT lastPanCursor_ {};
    ULONG_PTR gdiplusToken_ = 0;
    SelectionMode selectionMode_ = SelectionMode::Lasso;
    std::optional<std::size_t> moveSelectionPageIndex_ {};
    std::optional<std::size_t> transformingImagePageIndex_ {};
    std::optional<std::size_t> transformingImageIndex_ {};
    std::optional<std::size_t> transformingStrokePageIndex_ {};
    std::optional<std::size_t> transformingTextPageIndex_ {};
    std::optional<std::size_t> transformingTextIndex_ {};
    POINT selectionAnchorCursor_ {};
    POINT selectionCurrentCursor_ {};
    POINT lastMoveCursor_ {};
    float moveSelectionScale_ = 1.0f;
    Vec2 imageTransformStartPagePoint_ {};
    Vec2 strokeTransformStartPagePoint_ {};
    Vec2 textTransformStartPagePoint_ {};
    PageImage imageTransformInitial_ {};
    PageText textTransformInitial_ {};
    ImageTransformHandle imageTransformHandle_ = ImageTransformHandle::None;
    ImageTransformHandle strokeTransformHandle_ = ImageTransformHandle::None;
    ImageTransformHandle textTransformHandle_ = ImageTransformHandle::None;
    float imageTransformStartAngleDegrees_ = 0.0f;
    float strokeTransformStartAngleDegrees_ = 0.0f;
    Vec2 strokeTransformBoundsMin_ {};
    Vec2 strokeTransformBoundsMax_ {};
    Page textTransformBeforePage_ {};
    std::vector<std::pair<std::size_t, Stroke>> strokeTransformInitials_ {};
    std::vector<POINT> selectionLassoCursorPoints_ {};
    std::vector<std::vector<std::size_t>> selectedImageIndices_ {};
    std::vector<std::vector<std::size_t>> selectedTextIndices_ {};
    std::vector<std::vector<std::size_t>> selectedStrokeIndices_ {};
    bool selectionBoxOverride_ = false;
    bool rasterRefreshPending_ = false;
    bool zoomSessionActive_ = false;
    bool zoomAnchorValid_ = false;
    std::size_t zoomAnchorPageIndex_ = 0;
    Vec2 zoomAnchorPage_ {};
    POINT zoomAnchorCursor_ {};
    std::vector<PageSurfaceCache> pageSurfaceCaches_ {};
    InteractionPreviewMode interactionPreviewMode_ = InteractionPreviewMode::None;
    POINT interactionPreviewStartCursor_ {};
    std::vector<PageSurfaceCache> interactionPageSnapshots_ {};
    std::vector<std::vector<InteractionPreviewStroke>> interactionPreviewStrokes_ {};
    std::vector<std::vector<InteractionPreviewImage>> interactionPreviewImages_ {};
    std::vector<std::vector<InteractionPreviewText>> interactionPreviewTexts_ {};
    std::vector<std::vector<MoveSelectionTextCacheEntry>> moveSelectionTextCache_ {};
    std::vector<SidebarThumbnailCache> sidebarThumbnailCaches_ {};
    std::vector<ZoomPageSnapshot> zoomPageSnapshots_ {};
    std::uint64_t renderGeneration_ = 0;
    std::unordered_map<std::size_t, float> pendingRenderScales_ {};
    std::mutex renderMutex_ {};
    std::condition_variable renderCv_ {};
    std::deque<RenderRequest> renderRequests_ {};
    std::deque<RenderResult> renderResults_ {};
    std::thread renderThread_ {};
    bool renderWorkerRunning_ = false;

    std::vector<std::uint32_t> palette_ {
        0xFF000000,
        0xFF2563EB,
        0xFFDC2626,
        0xFF059669,
        0xFFF59E0B,
    };
    std::vector<NamedPalette> savedPalettes_ {};
    ShortcutState shortcuts_ {};
    WheelState wheel_ {};
    SettingsState settings_ {};
    NewPageSettingsState newPageSettings_ {};
    HCURSOR eraserCursor_ = nullptr;
    bool pdfSidebarOpen_ = false;
    float pdfSidebarSlideProgress_ = 0.0f;
    float pdfSidebarSlideAnimationStartProgress_ = 0.0f;
    float pdfSidebarSlideAnimationTargetProgress_ = 0.0f;
    ULONGLONG pdfSidebarSlideAnimationStartMs_ = 0;
    bool pdfSidebarSlideTimerResolutionActive_ = false;
    int pdfSidebarScrollY_ = 0;
    std::optional<Page> copiedSidebarPage_ {};
    bool pageJumpEditing_ = false;
    bool pageJumpReplaceOnType_ = false;
    std::wstring pageJumpText_ {};
    std::optional<Tool> quickSizePopupTool_ {};
    POINT quickSizePopupAnchor_ {};
    bool toolbarZoomEditing_ = false;
    bool toolbarZoomReplaceOnType_ = false;
    std::wstring toolbarZoomText_ {};
    bool toolbarBrushSizeEditing_ = false;
    bool toolbarBrushSizeReplaceOnType_ = false;
    std::optional<Tool> toolbarBrushSizeEditTool_ {};
    std::wstring toolbarBrushSizeText_ {};
    bool toolbarBrushSliderDragging_ = false;
    bool toolbarRevealed_ = true;
    float toolbarRevealProgress_ = 1.0f;
    float toolbarRevealAnimationStartProgress_ = 1.0f;
    float toolbarRevealAnimationTargetProgress_ = 1.0f;
    ULONGLONG toolbarRevealAnimationStartMs_ = 0;
    bool toolbarRevealTimerResolutionActive_ = false;
    bool pendingTextEditClick_ = false;
    POINT pendingTextEditCursor_ {};
    std::optional<std::size_t> pendingTextEditPageIndex_ {};
    std::optional<std::size_t> pendingTextEditIndex_ {};
    HWND textEditWindow_ = nullptr;
    HFONT textEditFont_ = nullptr;
    WNDPROC textEditOriginalProc_ = nullptr;
    HBITMAP textEditBackgroundBitmap_ = nullptr;
    HBRUSH textEditBackgroundBrush_ = nullptr;
    bool textEditWindowNeedsLayout_ = false;
    RECT textEditBounds_ {};
    int textEditFontPixelHeight_ = 0;
    std::optional<std::size_t> editingTextPageIndex_ {};
    std::optional<std::size_t> editingTextIndex_ {};
    std::optional<Page> editingTextBeforePage_ {};
    bool textFormatPopupOpen_ = false;
    bool textFormatFontMenuOpen_ = false;
    bool textFormatSizeEditing_ = false;
    bool textFormatSizeReplaceOnType_ = false;
    std::wstring textFormatSizeText_ {};
    TextAlignment textEditAlignment_ = TextAlignment::Left;
    TextFontFamily textEditFontFamily_ = TextFontFamily::Sans;
    bool textEditBold_ = false;
    bool textEditItalic_ = false;
    bool textEditUnderline_ = false;
    bool hoveredTitleMenu_ = false;
    bool titleMenuActive_ = false;
    int hoveredWindowButton_ = -1;
    bool trackingMouseLeave_ = false;
    bool documentDirty_ = false;
    HDC paintBackbufferDc_ = nullptr;
    HBITMAP paintBackbufferBitmap_ = nullptr;
    HGDIOBJ paintBackbufferPreviousBitmap_ = nullptr;
    int paintBackbufferWidth_ = 0;
    int paintBackbufferHeight_ = 0;

    static constexpr int kToolbarHeight = 45;
    static constexpr int kSettingsPanelWidth = 320;
    static constexpr int kPageGap = 26;
    static constexpr UINT kRenderResultsMessage = WM_APP + 1;
    static constexpr UINT_PTR kRasterRefreshTimerId = 1;
    static constexpr UINT_PTR kAutoSaveTimerId = 2;
    static constexpr UINT_PTR kZoomSessionTimerId = 3;
    static constexpr UINT_PTR kToolbarRevealTimerId = 4;
    static constexpr UINT_PTR kPdfSidebarSlideTimerId = 5;
    static constexpr UINT kRasterRefreshDelayMs = 120;
    static constexpr UINT kZoomSessionDelayMs = 100;
    static constexpr UINT kToolbarRevealFrameMs = 1;
    static constexpr UINT kToolbarRevealDurationMs = 120;
    static constexpr UINT kPdfSidebarSlideFrameMs = 1;
    static constexpr UINT kPdfSidebarSlideDurationMs = 150;
    static constexpr float kMinBrushSize = 0.5f;
    static constexpr float kMaxBrushSize = 20.0f;
    static constexpr float kBrushSizeStep = 0.25f;
    static constexpr float kWheelScrollPixels = 72.0f;
    static constexpr float kMinZoom = 0.35f;
    static constexpr float kMaxZoom = 4.0f;
    float sidebarWheelScrollRemainder_ = 0.0f;
};

}  // namespace cnote
