#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include <windowsx.h>

#include "ui/Layout.h"
#include "DpiUtil.h"

namespace cnote {

namespace appui = cnote::appui;

namespace {

using appui::BuildQuickSizePopupLayout;
using appui::BuildSidebarHeaderLayout;
using appui::BuildSidebarMetrics;
using appui::BuildToolbarLayout;
using appui::IsToolbarControlRect;
using appui::QuickSizePopupBounds;
using appui::QuickSizePopupLayout;
using appui::ResolveToolbarTitleLayout;
using appui::SidebarHeaderLayout;
using appui::SidebarMetrics;
using appui::ToolbarHeight;
using appui::ToolbarLayout;
using appui::ToolbarTitleLayout;

float WheelScrollPixels(int wheelDelta, UINT dpi, bool horizontal, int viewportSpan = 0) {
    UINT scrollUnits = 0;
    if (horizontal) {
        SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &scrollUnits, 0);
    } else {
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &scrollUnits, 0);
    }

    if (scrollUnits == WHEEL_PAGESCROLL) {
        const int pagePixels = std::max(1, viewportSpan);
        return (static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA)) * static_cast<float>(pagePixels);
    }

    if (scrollUnits == 0U) {
        scrollUnits = 3U;
    }

    const float unitPixels = static_cast<float>(ScaleForDpi(18, dpi));
    return (static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA))
        * static_cast<float>(scrollUnits)
        * unitPixels;
}

int ConsumeWholePixels(float& remainder) {
    const int wholePixels = static_cast<int>(std::trunc(remainder));
    remainder -= static_cast<float>(wholePixels);
    return wholePixels;
}

bool HasRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

int WindowFrameThicknessX(UINT dpi) {
    return SystemMetricForDpi(SM_CXFRAME, dpi) + SystemMetricForDpi(SM_CXPADDEDBORDER, dpi);
}

int WindowFrameThicknessY(UINT dpi) {
    return SystemMetricForDpi(SM_CYFRAME, dpi) + SystemMetricForDpi(SM_CXPADDEDBORDER, dpi);
}

RECT LiveTextEditFrameRect(const RECT& editRect, UINT dpi) {
    RECT frameRect = editRect;
    InflateRect(&frameRect, ScaleForDpi(4, dpi), ScaleForDpi(4, dpi));
    return frameRect;
}

}  // namespace

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCACTIVATE:
        return TRUE;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        if (!IsZoomed(window_)) {
            const UINT dpi = WindowDpi(window_);
            RECT windowRect {};
            GetWindowRect(window_, &windowRect);
            const int frameX = WindowFrameThicknessX(dpi);
            const int frameY = WindowFrameThicknessY(dpi);
            const POINT screenCursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const bool onLeft = screenCursor.x < (windowRect.left + frameX);
            const bool onRight = screenCursor.x >= (windowRect.right - frameX);
            const bool onTop = screenCursor.y < (windowRect.top + frameY);
            const bool onBottom = screenCursor.y >= (windowRect.bottom - frameY);

            if (onTop && onLeft) {
                return HTTOPLEFT;
            }
            if (onTop && onRight) {
                return HTTOPRIGHT;
            }
            if (onBottom && onLeft) {
                return HTBOTTOMLEFT;
            }
            if (onBottom && onRight) {
                return HTBOTTOMRIGHT;
            }
            if (onTop) {
                return HTTOP;
            }
            if (onBottom) {
                return HTBOTTOM;
            }
            if (onLeft) {
                return HTLEFT;
            }
            if (onRight) {
                return HTRIGHT;
            }
        }

        POINT clientCursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window_, &clientCursor);
        const UINT dpi = WindowDpi(window_);
        if (clientCursor.y >= 0 && clientCursor.y < ToolbarHeight(IsZoomed(window_), dpi)) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);
            HDC windowDc = GetDC(window_);
            HFONT bodyFont = CreateFontW(FontHeightForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
            HGDIOBJ previousFont = windowDc != nullptr && bodyFont != nullptr ? SelectObject(windowDc, bodyFont) : nullptr;
            const ToolbarTitleLayout titleLayout = windowDc != nullptr
                ? ResolveToolbarTitleLayout(layout, windowDc, CurrentWindowTitle(), ZoomIndicatorLabel(), dpi)
                : ToolbarTitleLayout {};
            if (windowDc != nullptr && previousFont != nullptr) {
                SelectObject(windowDc, previousFont);
            }
            if (bodyFont != nullptr) {
                DeleteObject(bodyFont);
            }
            if (windowDc != nullptr) {
                ReleaseDC(window_, windowDc);
            }
            return IsToolbarControlRect(layout, titleLayout, clientCursor) ? HTCLIENT : HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (HandleTextFormatPopupClick(cursor)) {
            return 0;
        }
        CommitTextEdit();
        if (HandlePaletteClick(cursor)) {
            return 0;
        }
        if (HandleToolbarClick(cursor)) {
            return 0;
        }
        if (HandleQuickSizePopupClick(cursor)) {
            return 0;
        }
        if (HandleSidebarClick(cursor)) {
            return 0;
        }

        CancelPageJumpInput();
        ApplyToolbarZoomInput();
        CancelToolbarZoomInput();
        CancelQuickSizePopup();

        ClosePaletteOverlays();

        if (activeTool_ == Tool::Pan) {
            BeginPan(cursor, false);
        } else if (activeTool_ == Tool::Move) {
            BeginMoveSelection(cursor);
        } else if (activeTool_ == Tool::Select) {
            if (HasSelection() && CursorInteractsWithSelection(cursor)) {
                BeginMoveSelection(cursor);
            } else {
                if (HasSelection()) {
                    ClearSelection();
                }
                BeginSelection(cursor);
            }
        } else if (activeTool_ == Tool::Text) {
            BeginTextToolInteraction(cursor);
        } else if (activeTool_ == Tool::Erase) {
            BeginErase(cursor);
        } else {
            BeginStroke(cursor);
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BeginPan(cursor, true);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        SetFocus(window_);
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (HandleToolbarRightClick(cursor)) {
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hoveredWindowButton = -1;
        bool hoveredTitleMenu = false;
        const UINT dpi = WindowDpi(window_);
        if (cursor.y >= 0 && cursor.y < ToolbarHeight(IsZoomed(window_), dpi)) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const ToolbarLayout layout = BuildToolbarLayout(clientRect, palette_.size(), IsZoomed(window_), activeTool_, dpi);
            HDC windowDc = GetDC(window_);
            HFONT bodyFont = CreateFontW(FontHeightForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
            HGDIOBJ previousFont = windowDc != nullptr && bodyFont != nullptr ? SelectObject(windowDc, bodyFont) : nullptr;
            const ToolbarTitleLayout titleLayout = windowDc != nullptr
                ? ResolveToolbarTitleLayout(layout, windowDc, CurrentWindowTitle(), ZoomIndicatorLabel(), dpi)
                : ToolbarTitleLayout {};
            if (windowDc != nullptr && previousFont != nullptr) {
                SelectObject(windowDc, previousFont);
            }
            if (bodyFont != nullptr) {
                DeleteObject(bodyFont);
            }
            if (windowDc != nullptr) {
                ReleaseDC(window_, windowDc);
            }
            hoveredTitleMenu = PtInRect(&titleLayout.fileTitleRect, cursor) != 0;
            if (PtInRect(&layout.minimizeRect, cursor) != 0) {
                hoveredWindowButton = 0;
            } else if (PtInRect(&layout.maximizeRect, cursor) != 0) {
                hoveredWindowButton = 1;
            } else if (PtInRect(&layout.closeRect, cursor) != 0) {
                hoveredWindowButton = 2;
            }
        }
        if (hoveredWindowButton_ != hoveredWindowButton) {
            hoveredWindowButton_ = hoveredWindowButton;
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            clientRect.bottom = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
            InvalidateRect(window_, &clientRect, FALSE);
        }
        if (hoveredTitleMenu_ != hoveredTitleMenu) {
            hoveredTitleMenu_ = hoveredTitleMenu;
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            clientRect.bottom = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
            InvalidateRect(window_, &clientRect, FALSE);
        }
        if (!trackingMouseLeave_) {
            TRACKMOUSEEVENT event {};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = window_;
            if (TrackMouseEvent(&event) != FALSE) {
                trackingMouseLeave_ = true;
            }
        }

        if (toolbarBrushSliderDragging_) {
            UpdateToolbarBrushSliderDrag(cursor);
        } else if (isSelecting_) {
            ExtendSelection(cursor);
        } else if (isTransformingStroke_) {
            UpdateStrokeTransform(cursor);
        } else if (isTransformingText_) {
            UpdateTextTransform(cursor);
        } else if (isTransformingImage_) {
            UpdateImageTransform(cursor);
        } else if (isPanning_ || (wParam & MK_MBUTTON) != 0) {
            UpdatePan(cursor);
        } else if (isMovingSelection_ || ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Move)) {
            UpdateMoveSelection(cursor);
        } else if ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Select) {
            ExtendSelection(cursor);
        } else if (isErasing_ || ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Erase)) {
            ExtendErase(cursor);
        } else if ((wParam & MK_LBUTTON) != 0 && activeTool_ == Tool::Draw) {
            ExtendStroke(cursor);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        if (hoveredWindowButton_ != -1 || hoveredTitleMenu_) {
            hoveredWindowButton_ = -1;
            hoveredTitleMenu_ = false;
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            clientRect.bottom = ToolbarHeight(IsZoomed(window_), WindowDpi(window_));
            InvalidateRect(window_, &clientRect, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (toolbarBrushSliderDragging_) {
            EndToolbarBrushSliderDrag();
        } else if (isDrawing_) {
            const auto hitPage = HitTestPage(cursor);
            if (!hitPage.has_value() || (activeStrokePageIndex_.has_value() && hitPage->pageIndex == *activeStrokePageIndex_)) {
                ExtendStroke(cursor);
            }
            EndStroke();
        } else if (isTransformingStroke_) {
            EndStrokeTransform();
        } else if (isTransformingText_) {
            EndTextTransform();
        } else if (isTransformingImage_) {
            EndImageTransform();
        } else if (isMovingSelection_) {
            EndMoveSelection();
        } else if (pendingTextEditClick_) {
            const std::optional<std::size_t> pageIndex = pendingTextEditPageIndex_;
            const std::optional<std::size_t> textIndex = pendingTextEditIndex_;
            pendingTextEditClick_ = false;
            pendingTextEditPageIndex_.reset();
            pendingTextEditIndex_.reset();
            UpdateCaptureState();
            if (pageIndex.has_value() && textIndex.has_value() && *pageIndex < document_.PageCount()
                && *textIndex < document_.PageAt(*pageIndex).texts.size()) {
                BeginTextEdit(*pageIndex, *textIndex, document_.PageAt(*pageIndex));
            }
        } else if (isSelecting_) {
            EndSelection();
        } else if (isErasing_) {
            isErasing_ = false;
            UpdateCaptureState();
            if (rasterRefreshPending_) {
                ScheduleDeferredRasterRefresh();
            }
            InvalidateCanvas();
        } else if (isPanning_ && !panFromMiddleButton_) {
            EndPan();
        }
        return 0;
    }
    case WM_MBUTTONUP:
        if (isPanning_ && panFromMiddleButton_) {
            EndPan();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window_, &cursor);
        const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        const bool ctrlPressed = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
        if (ctrlPressed) {
            ZoomAt(wheelDelta, cursor);
            return 0;
        }
        if (IsPdfSidebarVisible()) {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const RECT sidebarRect = PdfSidebarRect(clientRect);
            if (PtInRect(&sidebarRect, cursor) != 0) {
                const UINT dpi = WindowDpi(window_);
                const SidebarHeaderLayout headerLayout = BuildSidebarHeaderLayout(sidebarRect, dpi);
                const SidebarMetrics metrics = BuildSidebarMetrics(dpi);
                const int itemWidth = std::max(1, static_cast<int>(headerLayout.headerRect.right - headerLayout.headerRect.left) - (metrics.itemInset * 2));
                int contentHeight = metrics.contentPadding;
                for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
                    const Page& page = document_.PageAt(pageIndex);
                    const float aspect = page.width > 0.0f ? (page.height / page.width) : (11.0f / 8.5f);
                    const int thumbHeight = std::max(metrics.minThumbHeight, static_cast<int>(std::lround(static_cast<float>(itemWidth) * aspect)));
                    contentHeight += thumbHeight + metrics.labelHeight + metrics.itemGap;
                }
                const int viewportHeight = std::max(1, static_cast<int>(headerLayout.contentRect.bottom - headerLayout.contentRect.top));
                const int maxScroll = std::max(0, contentHeight - viewportHeight);
                sidebarWheelScrollRemainder_ += WheelScrollPixels(wheelDelta, dpi, false, viewportHeight);
                const int scrollPixels = ConsumeWholePixels(sidebarWheelScrollRemainder_);
                pdfSidebarScrollY_ = std::clamp(pdfSidebarScrollY_ - scrollPixels, 0, maxScroll);
                InvalidateRect(window_, &sidebarRect, FALSE);
                return 0;
            }
        }
        PanBy(0.0f, WheelScrollPixels(wheelDelta, WindowDpi(window_), false));
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        RECT clientRect {};
        GetClientRect(window_, &clientRect);
        const RECT viewportRect = CanvasRect(clientRect);
        const int viewportWidth = std::max(1, static_cast<int>(viewportRect.right - viewportRect.left));
        PanBy(-WheelScrollPixels(wheelDelta, WindowDpi(window_), true, viewportWidth), 0.0f);
        return 0;
    }
    case kRenderResultsMessage:
        HandleCompletedRenderResults();
        return 0;
    case WM_ACTIVATEAPP:
        if (wParam == FALSE) {
            if (CanAutoSaveCurrentDocument() && !IsDocumentInteractionActive()) {
                static_cast<void>(SaveCnoteToPath(document_.FilePath(), false));
            } else {
                UpdateAutoSaveTimer();
            }
        }
        break;
    case WM_TIMER:
        if (wParam == kRasterRefreshTimerId) {
            HandleDeferredRasterRefresh();
            return 0;
        }
        if (wParam == kAutoSaveTimerId) {
            if (CanAutoSaveCurrentDocument() && !IsDocumentInteractionActive()) {
                static_cast<void>(SaveCnoteToPath(document_.FilePath(), false));
            } else {
                UpdateAutoSaveTimer();
            }
            return 0;
        }
        if (wParam == kZoomSessionTimerId) {
            EndZoomSession();
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor {};
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            const auto cursorForHandle = [](ImageTransformHandle handle) -> LPCWSTR {
                switch (handle) {
                case ImageTransformHandle::ResizeNorth:
                case ImageTransformHandle::ResizeSouth:
                    return IDC_SIZENS;
                case ImageTransformHandle::ResizeEast:
                case ImageTransformHandle::ResizeWest:
                    return IDC_SIZEWE;
                case ImageTransformHandle::ResizeNorthWest:
                case ImageTransformHandle::ResizeSouthEast:
                    return IDC_SIZENWSE;
                case ImageTransformHandle::ResizeNorthEast:
                case ImageTransformHandle::ResizeSouthWest:
                    return IDC_SIZENESW;
                case ImageTransformHandle::Rotate:
                    return IDC_HAND;
                case ImageTransformHandle::None:
                default:
                    return IDC_ARROW;
                }
            };
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const RECT sidebarRect = PdfSidebarRect(clientRect);
            const UINT dpi = WindowDpi(window_);
            const QuickSizePopupLayout quickSizePopupLayout = BuildQuickSizePopupLayout(CanvasRect(clientRect), quickSizePopupAnchor_, dpi);
            const RECT quickSizePopupBounds = QuickSizePopupBounds(quickSizePopupLayout, dpi);
            const RECT liveTextFrameRect = LiveTextEditFrameRect(textEditBounds_, dpi);
            if (IsPdfSidebarVisible() && PtInRect(&sidebarRect, cursor) != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (quickSizePopupTool_.has_value()
                && PtInRect(&quickSizePopupBounds, cursor) != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (cursor.y < ToolbarHeight(IsZoomed(window_), dpi)) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            } else if (textEditWindow_ != nullptr
                && IsWindowVisible(textEditWindow_) != FALSE
                && HasRect(textEditBounds_)
                && PtInRect(&liveTextFrameRect, cursor) != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            } else if (isPanning_ || activeTool_ == Tool::Pan) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            } else if (isTransformingStroke_) {
                SetCursor(LoadCursorW(nullptr, cursorForHandle(strokeTransformHandle_)));
            } else if (isTransformingText_) {
                SetCursor(LoadCursorW(nullptr, cursorForHandle(textTransformHandle_)));
            } else if (isTransformingImage_) {
                SetCursor(LoadCursorW(nullptr, cursorForHandle(imageTransformHandle_)));
            } else if (isMovingSelection_) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            } else if (activeTool_ == Tool::Move) {
                if (const auto strokeHandle = HitTestSelectedStrokeHandle(cursor); strokeHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*strokeHandle)));
                } else if (const auto textHandle = HitTestSelectedTextHandle(cursor); textHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*textHandle)));
                } else if (const auto imageHandle = HitTestSelectedImageHandle(cursor); imageHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*imageHandle)));
                } else if (CursorHitsSelectionMoveGrip(cursor) || CursorHitsSelectedContent(cursor)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
            } else if (activeTool_ == Tool::Select) {
                if (const auto strokeHandle = HitTestSelectedStrokeHandle(cursor); strokeHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*strokeHandle)));
                } else if (const auto textHandle = HitTestSelectedTextHandle(cursor); textHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*textHandle)));
                } else if (const auto imageHandle = HitTestSelectedImageHandle(cursor); imageHandle.has_value()) {
                    SetCursor(LoadCursorW(nullptr, cursorForHandle(*imageHandle)));
                } else if (CursorHitsSelectionMoveGrip(cursor) || CursorHitsSelectedContent(cursor)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                }
            } else if (activeTool_ == Tool::Text) {
                SetCursor(LoadCursorW(nullptr, HitTestText(cursor).has_value() ? IDC_IBEAM : IDC_CROSS));
            } else if (activeTool_ == Tool::Erase) {
                SetCursor(eraserCursor_ != nullptr ? eraserCursor_ : LoadCursorW(nullptr, IDC_CROSS));
            } else {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            }
            return TRUE;
        }
        break;
    case WM_CHAR:
        if (textFormatSizeEditing_) {
            if (wParam == VK_BACK) {
                if (textFormatSizeReplaceOnType_) {
                    textFormatSizeText_.clear();
                    textFormatSizeReplaceOnType_ = false;
                }
                if (!textFormatSizeText_.empty()) {
                    textFormatSizeText_.pop_back();
                    ApplyTextFormatSizeInput();
                }
            } else if (wParam >= L'0' && wParam <= L'9') {
                if (textFormatSizeReplaceOnType_) {
                    textFormatSizeText_.clear();
                    textFormatSizeReplaceOnType_ = false;
                }
                if (!(textFormatSizeText_.empty() && wParam == L'0') && textFormatSizeText_.size() < 6) {
                    textFormatSizeText_.push_back(static_cast<wchar_t>(wParam));
                    ApplyTextFormatSizeInput();
                }
            } else if (wParam == L'.') {
                if (textFormatSizeReplaceOnType_) {
                    textFormatSizeText_.clear();
                    textFormatSizeReplaceOnType_ = false;
                }
                if (textFormatSizeText_.find(L'.') == std::wstring::npos) {
                    if (textFormatSizeText_.empty()) {
                        textFormatSizeText_ = L"0.";
                    } else {
                        textFormatSizeText_.push_back(L'.');
                    }
                }
            } else if (wParam == VK_RETURN) {
                ApplyTextFormatSizeInput();
                CancelTextFormatSizeInput();
            } else if (wParam == 27) {
                CancelTextFormatSizeInput();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (pageJumpEditing_) {
            if (wParam == VK_BACK) {
                if (pageJumpReplaceOnType_) {
                    pageJumpText_.clear();
                    pageJumpReplaceOnType_ = false;
                }
                if (!pageJumpText_.empty()) {
                    pageJumpText_.pop_back();
                    ApplyPageJumpInput();
                }
            } else if (wParam >= L'0' && wParam <= L'9') {
                if (pageJumpReplaceOnType_) {
                    pageJumpText_.clear();
                    pageJumpReplaceOnType_ = false;
                }
                if (!(pageJumpText_.empty() && wParam == L'0') && pageJumpText_.size() < 5) {
                    pageJumpText_.push_back(static_cast<wchar_t>(wParam));
                    ApplyPageJumpInput();
                }
            } else if (wParam == VK_RETURN) {
                ApplyPageJumpInput();
                CancelPageJumpInput();
            } else if (wParam == 27) {
                CancelPageJumpInput();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (toolbarBrushSizeEditing_) {
            if (wParam == VK_BACK) {
                if (toolbarBrushSizeReplaceOnType_) {
                    toolbarBrushSizeText_.clear();
                    toolbarBrushSizeReplaceOnType_ = false;
                }
                if (!toolbarBrushSizeText_.empty()) {
                    toolbarBrushSizeText_.pop_back();
                    ApplyToolbarBrushSizeInput();
                }
            } else if (wParam >= L'0' && wParam <= L'9') {
                if (toolbarBrushSizeReplaceOnType_) {
                    toolbarBrushSizeText_.clear();
                    toolbarBrushSizeReplaceOnType_ = false;
                }
                if (!(toolbarBrushSizeText_.empty() && wParam == L'0') && toolbarBrushSizeText_.size() < 5) {
                    toolbarBrushSizeText_.push_back(static_cast<wchar_t>(wParam));
                    ApplyToolbarBrushSizeInput();
                }
            } else if (wParam == L'.') {
                if (toolbarBrushSizeReplaceOnType_) {
                    toolbarBrushSizeText_.clear();
                    toolbarBrushSizeReplaceOnType_ = false;
                }
                if (toolbarBrushSizeText_.find(L'.') == std::wstring::npos) {
                    if (toolbarBrushSizeText_.empty()) {
                        toolbarBrushSizeText_ = L"0.";
                    } else {
                        toolbarBrushSizeText_.push_back(L'.');
                    }
                }
            } else if (wParam == VK_RETURN) {
                ApplyToolbarBrushSizeInput();
                CancelToolbarBrushSizeInput();
            } else if (wParam == 27) {
                CancelToolbarBrushSizeInput();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (toolbarZoomEditing_) {
            if (wParam == VK_BACK) {
                if (toolbarZoomReplaceOnType_) {
                    toolbarZoomText_.clear();
                    toolbarZoomReplaceOnType_ = false;
                }
                if (!toolbarZoomText_.empty()) {
                    toolbarZoomText_.pop_back();
                }
            } else if ((wParam >= L'0' && wParam <= L'9')) {
                if (toolbarZoomReplaceOnType_) {
                    toolbarZoomText_.clear();
                    toolbarZoomReplaceOnType_ = false;
                }
                if (toolbarZoomText_.size() < 6) {
                    toolbarZoomText_.push_back(static_cast<wchar_t>(wParam));
                }
            } else if (wParam == L'.') {
                if (toolbarZoomReplaceOnType_) {
                    toolbarZoomText_.clear();
                    toolbarZoomReplaceOnType_ = false;
                }
                if (toolbarZoomText_.find(L'.') == std::wstring::npos) {
                    if (toolbarZoomText_.empty()) {
                        toolbarZoomText_ = L"0.";
                    } else {
                        toolbarZoomText_.push_back(L'.');
                    }
                }
            } else if (wParam == VK_RETURN) {
                ApplyToolbarZoomInput();
                CancelToolbarZoomInput();
            } else if (wParam == 27) {
                CancelToolbarZoomInput();
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
    {
        const bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shiftPressed = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool altPressed = (GetKeyState(VK_MENU) & 0x8000) != 0;

        if (textFormatSizeEditing_) {
            if (wParam == VK_RETURN) {
                ApplyTextFormatSizeInput();
                CancelTextFormatSizeInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                CancelTextFormatSizeInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        if (pageJumpEditing_) {
            if (wParam == VK_RETURN) {
                ApplyPageJumpInput();
                CancelPageJumpInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                CancelPageJumpInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        if (toolbarBrushSizeEditing_) {
            if (wParam == VK_RETURN) {
                ApplyToolbarBrushSizeInput();
                CancelToolbarBrushSizeInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                CancelToolbarBrushSizeInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        if (toolbarZoomEditing_) {
            if (wParam == VK_RETURN) {
                ApplyToolbarZoomInput();
                CancelToolbarZoomInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                CancelToolbarZoomInput();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            return 0;
        }

        if (ctrlPressed && wParam == 'W') {
            const auto selectedText = SingleSelectedText();
            if (selectedText.has_value()
                && selectedText->first < document_.PageCount()
                && selectedText->second < document_.PageAt(selectedText->first).texts.size()) {
                if (!IsEditingText(selectedText->first, selectedText->second)) {
                    BeginTextEdit(selectedText->first, selectedText->second, document_.PageAt(selectedText->first));
                }
                if (!textFormatPopupOpen_) {
                    ToggleTextFormatPopup();
                } else {
                    InvalidateRect(window_, nullptr, FALSE);
                }
                return 0;
            }
        }

        if (HandleShortcutKey(static_cast<UINT>(wParam), ctrlPressed, shiftPressed, altPressed)) {
            return 0;
        }

        if (wParam == VK_DELETE) {
            if (SidebarOwnsKeyboardShortcuts()) {
                static_cast<void>(DeleteCurrentPage());
            } else {
                DeleteSelection();
            }
            return 0;
        }

        if (wParam == VK_ESCAPE && (wheel_.paletteMenuOpen || wheel_.paletteWheelOpen)) {
            ClosePaletteOverlays();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        if (wParam == VK_ESCAPE && quickSizePopupTool_.has_value()) {
            CancelQuickSizePopup();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }

        switch (wParam) {
        case VK_LEFT:
            PanBy(20.0f, 0.0f);
            return 0;
        case VK_RIGHT:
            PanBy(-20.0f, 0.0f);
            return 0;
        case VK_UP:
            PanBy(0.0f, 20.0f);
            return 0;
        case VK_DOWN:
            PanBy(0.0f, -20.0f);
            return 0;
        case VK_OEM_4:
        case VK_SUBTRACT:
        case VK_OEM_MINUS:
            ChangeBrushSize(-kBrushSizeStep);
            return 0;
        case VK_OEM_6:
        case VK_ADD:
        case VK_OEM_PLUS:
            ChangeBrushSize(kBrushSizeStep);
            return 0;
        case 'C':
            CycleColor();
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == textEditWindow_) {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetBkMode(editDc, TRANSPARENT);
            if (editingTextPageIndex_.has_value() && editingTextIndex_.has_value()
                && *editingTextPageIndex_ < document_.PageCount()) {
                const Page& page = document_.PageAt(*editingTextPageIndex_);
                if (*editingTextIndex_ < page.texts.size()) {
                    SetTextColor(editDc, ToColorRef(page.texts[*editingTextIndex_].color));
                }
            }
            if (textEditBackgroundBrush_ != nullptr) {
                SetBrushOrgEx(editDc, 0, 0, nullptr);
                return reinterpret_cast<INT_PTR>(textEditBackgroundBrush_);
            }
            SetBkColor(editDc, RGB(255, 255, 255));
            return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        CancelDeferredRasterRefresh();
        KillTimer(window_, kZoomSessionTimerId);
        zoomSessionActive_ = false;
        zoomAnchorValid_ = false;
        ClearZoomSessionSnapshots();
        ReleasePaintBackbuffer();
        targetZoom_ = zoom_;
        InvalidateAllPageSurfaceCaches();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggestedRect = reinterpret_cast<const RECT*>(lParam);
        if (suggestedRect != nullptr) {
            SetWindowPos(
                window_,
                nullptr,
                suggestedRect->left,
                suggestedRect->top,
                suggestedRect->right - suggestedRect->left,
                suggestedRect->bottom - suggestedRect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        CancelDeferredRasterRefresh();
        KillTimer(window_, kZoomSessionTimerId);
        zoomSessionActive_ = false;
        zoomAnchorValid_ = false;
        ClearZoomSessionSnapshots();
        ReleasePaintBackbuffer();
        targetZoom_ = zoom_;
        InvalidateAllPageSurfaceCaches();
        EnsureVisiblePageRasterQuality();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        CancelDeferredRasterRefresh();
        KillTimer(window_, kZoomSessionTimerId);
        ClearZoomSessionSnapshots();
        ReleasePaintBackbuffer();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window_ = window;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (app != nullptr) {
        return app->HandleMessage(message, wParam, lParam);
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::TextEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app == nullptr || app->textEditOriginalProc_ == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN: {
        const bool ctrlPressed = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrlPressed && wParam == 'W') {
            app->ToggleTextFormatPopup();
            return 0;
        }
        if (ctrlPressed && wParam == 'B') {
            app->ToggleEditingTextBold();
            return 0;
        }
        if (ctrlPressed && wParam == 'I') {
            app->ToggleEditingTextItalic();
            return 0;
        }
        if (ctrlPressed && wParam == 'U') {
            app->ToggleEditingTextUnderline();
            return 0;
        }
        if (wParam == VK_RETURN) {
            app->CommitTextEdit();
            SetFocus(app->window_);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (app->textFormatPopupOpen_) {
                app->CancelTextFormatPopup();
                InvalidateRect(app->window_, nullptr, FALSE);
                return 0;
            }
            app->CancelTextEdit();
            SetFocus(app->window_);
            return 0;
        }
        if (ctrlPressed && wParam == 'A') {
            SendMessageW(window, EM_SETSEL, 0, -1);
            return 0;
        }
        if (ctrlPressed && wParam == 'C') {
            SendMessageW(window, WM_COPY, 0, 0);
            return 0;
        }
        if (ctrlPressed && wParam == 'X') {
            SendMessageW(window, WM_CUT, 0, 0);
            app->RefreshTextEditControl();
            return 0;
        }
        if (ctrlPressed && wParam == 'V') {
            SendMessageW(window, WM_PASTE, 0, 0);
            app->RefreshTextEditControl();
            return 0;
        }
        if (ctrlPressed && wParam == VK_BACK) {
            DWORD selection = static_cast<DWORD>(SendMessageW(window, EM_GETSEL, 0, 0));
            const int selectionStart = static_cast<int>(LOWORD(selection));
            const int selectionEnd = static_cast<int>(HIWORD(selection));
            const int textLength = GetWindowTextLengthW(window);
            std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
            GetWindowTextW(window, buffer.data(), textLength + 1);
            std::wstring text(buffer.data(), static_cast<std::size_t>(textLength));

            int eraseStart = selectionStart;
            int eraseEnd = selectionEnd;
            if (eraseStart == eraseEnd) {
                eraseEnd = eraseStart;
                while (eraseStart > 0 && std::iswspace(text[static_cast<std::size_t>(eraseStart - 1)])) {
                    --eraseStart;
                }
                while (eraseStart > 0 && !std::iswspace(text[static_cast<std::size_t>(eraseStart - 1)])) {
                    --eraseStart;
                }
            }

            if (eraseStart < eraseEnd) {
                text.erase(static_cast<std::size_t>(eraseStart), static_cast<std::size_t>(eraseEnd - eraseStart));
                SetWindowTextW(window, text.c_str());
                SendMessageW(window, EM_SETSEL, eraseStart, eraseStart);
                app->RefreshTextEditControl();
            }
            return 0;
        }
        break;
    }
    case WM_CHAR:
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wParam < 32 || wParam == 127)) {
            return 0;
        }
        break;
    default:
        break;
    }

    const LRESULT result = CallWindowProcW(app->textEditOriginalProc_, window, message, wParam, lParam);
    if (message == WM_CHAR || message == WM_PASTE || message == WM_CUT || message == WM_CLEAR) {
        app->RefreshTextEditControl();
        InvalidateRect(window, nullptr, TRUE);
        UpdateWindow(window);
        if (app->textFormatPopupOpen_ && app->window_ != nullptr) {
            InvalidateRect(app->window_, nullptr, FALSE);
        }
    }
    return result;
}

}  // namespace cnote
