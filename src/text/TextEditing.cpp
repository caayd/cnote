#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "ui/Layout.h"
#include "DpiUtil.h"
#include "text/TextGeometry.h"

namespace cnote {

namespace appui = cnote::appui;
namespace textgeom = cnote::text;

namespace {

bool HasRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect { left, top, right, bottom };
    return rect;
}

std::wstring FormatTextFontSizeValue(float size) {
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%.2f", std::max(1.0f, size));
    std::wstring value = buffer;
    while (!value.empty() && value.back() == L'0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == L'.') {
        value.pop_back();
    }
    return value;
}

RECT UnionRectangles(RECT a, RECT b) {
    if (!HasRect(a)) {
        return b;
    }
    if (!HasRect(b)) {
        return a;
    }

    return MakeRect(
        std::min(a.left, b.left),
        std::min(a.top, b.top),
        std::max(a.right, b.right),
        std::max(a.bottom, b.bottom));
}

float DefaultNewTextZoomScale(float zoom) {
    return std::max(0.1f, zoom);
}

float DefaultNewTextSizeScale() {
    return 0.5f;
}

DWORD TextEditAlignmentStyle(TextAlignment alignment) {
    switch (alignment) {
    case TextAlignment::Center:
        return ES_CENTER;
    case TextAlignment::Right:
        return ES_RIGHT;
    case TextAlignment::Left:
    default:
        return ES_LEFT;
    }
}

RECT LiveTextEditFrameRect(const RECT& editRect, UINT dpi) {
    RECT frameRect = editRect;
    InflateRect(&frameRect, ScaleForDpi(4, dpi), ScaleForDpi(4, dpi));
    return frameRect;
}

}  // namespace

std::optional<PageText> App::CurrentEditingTextLayout() const {
    if (textEditWindow_ == nullptr
        || !editingTextPageIndex_.has_value()
        || !editingTextIndex_.has_value()
        || *editingTextPageIndex_ >= document_.PageCount()) {
        return std::nullopt;
    }

    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return std::nullopt;
    }

    PageText layoutText = page.texts[*editingTextIndex_];
    const int editLength = GetWindowTextLengthW(textEditWindow_);
    if (editLength > 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(editLength) + 1U, L'\0');
        GetWindowTextW(textEditWindow_, buffer.data(), editLength + 1);
        layoutText.content.assign(buffer.data(), static_cast<std::size_t>(editLength));
    } else {
        layoutText.content.clear();
    }
    return layoutText;
}

void App::ToggleTextFormatPopup() {
    if (textEditWindow_ == nullptr
        || !editingTextPageIndex_.has_value()
        || !editingTextIndex_.has_value()
        || *editingTextPageIndex_ >= document_.PageCount()
        || *editingTextIndex_ >= document_.PageAt(*editingTextPageIndex_).texts.size()) {
        return;
    }

    if (textFormatPopupOpen_) {
        CancelTextFormatPopup();
    } else {
        CancelTextFormatSizeInput(false);
        textFormatPopupOpen_ = true;
        textFormatFontMenuOpen_ = false;
        RefreshTextEditControl();
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::CancelTextFormatPopup() {
    CancelTextFormatSizeInput(false);
    textFormatPopupOpen_ = false;
    textFormatFontMenuOpen_ = false;
}

std::wstring App::TextFormatFontSizeLabel() const {
    if (textFormatSizeEditing_) {
        return textFormatSizeText_.empty() ? L"" : textFormatSizeText_;
    }
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return L"";
    }
    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return L"";
    }
    return FormatTextFontSizeValue(page.texts[*editingTextIndex_].fontSize);
}

void App::CancelTextFormatSizeInput(bool restoreFocus) {
    textFormatSizeEditing_ = false;
    textFormatSizeReplaceOnType_ = false;
    textFormatSizeText_.clear();
    if (restoreFocus && textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
}

void App::BeginTextFormatSizeInput() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }
    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    textFormatFontMenuOpen_ = false;
    textFormatSizeEditing_ = true;
    textFormatSizeReplaceOnType_ = true;
    textFormatSizeText_ = FormatTextFontSizeValue(page.texts[*editingTextIndex_].fontSize);
    if (window_ != nullptr) {
        SetFocus(window_);
    }
}

void App::ApplyTextFormatSizeInput() {
    if (textFormatSizeText_.empty()) {
        return;
    }

    wchar_t* end = nullptr;
    const float parsed = std::wcstof(textFormatSizeText_.c_str(), &end);
    if (end == textFormatSizeText_.c_str() || *end != L'\0' || parsed <= 0.0f) {
        return;
    }

    SetEditingTextFontSize(parsed);
    textFormatSizeText_ = TextFormatFontSizeLabel();
}

void App::SetEditingTextAlignment(TextAlignment alignment) {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].alignment = alignment;
    textFormatFontMenuOpen_ = false;
    InvalidatePageSurfaceCache(*editingTextPageIndex_);
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::CycleEditingTextAlignment() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    const TextAlignment current = page.texts[*editingTextIndex_].alignment;
    switch (current) {
    case TextAlignment::Left:
        SetEditingTextAlignment(TextAlignment::Center);
        break;
    case TextAlignment::Center:
        SetEditingTextAlignment(TextAlignment::Right);
        break;
    case TextAlignment::Right:
    default:
        SetEditingTextAlignment(TextAlignment::Left);
        break;
    }
}

void App::SetEditingTextFontSize(float fontSize) {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].fontSize = std::max(1.0f, fontSize);
    textFormatFontMenuOpen_ = false;
    InvalidatePageSurfaceCache(*editingTextPageIndex_);
    RefreshTextEditControl();
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::SetEditingTextFontFamily(TextFontFamily family) {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].fontFamily = family;
    textFormatFontMenuOpen_ = false;
    InvalidatePageSurfaceCache(*editingTextPageIndex_);
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::ToggleEditingTextBold() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].bold = !page.texts[*editingTextIndex_].bold;
    textFormatFontMenuOpen_ = false;
    InvalidatePageSurfaceCache(*editingTextPageIndex_);
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::ToggleEditingTextItalic() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].italic = !page.texts[*editingTextIndex_].italic;
    textFormatFontMenuOpen_ = false;
    InvalidatePageSurfaceCache(*editingTextPageIndex_);
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::ToggleEditingTextUnderline() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].underline = !page.texts[*editingTextIndex_].underline;
    textFormatFontMenuOpen_ = false;
    InvalidatePageSurfaceCache(*editingTextPageIndex_);
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

bool App::HandleTextFormatPopupClick(POINT cursor) {
    if (!textFormatPopupOpen_ || window_ == nullptr || !HasRect(textEditBounds_)) {
        return false;
    }
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        CancelTextFormatPopup();
        return false;
    }

    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        CancelTextFormatPopup();
        return false;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    const UINT dpi = WindowDpi(window_);
    const appui::TextFormatPopupLayout layout = appui::BuildTextFormatPopupLayout(canvasRect, textEditBounds_, textFormatFontMenuOpen_, dpi);
    const RECT popupBounds = appui::TextFormatPopupBounds(layout, dpi);
    if (PtInRect(&popupBounds, cursor) == 0) {
        ApplyTextFormatSizeInput();
        CancelTextFormatPopup();
        return false;
    }

    const bool clickedSizeInput = PtInRect(&layout.textSizeInputRect, cursor) != 0;
    if (textFormatSizeEditing_ && !clickedSizeInput) {
        ApplyTextFormatSizeInput();
        CancelTextFormatSizeInput(false);
    }

    if (textFormatFontMenuOpen_) {
        for (std::size_t index = 0; index < layout.fontOptionRects.size(); ++index) {
            if (PtInRect(&layout.fontOptionRects[index], cursor) != 0) {
                switch (index) {
                case 1:
                    SetEditingTextFontFamily(TextFontFamily::Serif);
                    break;
                case 2:
                    SetEditingTextFontFamily(TextFontFamily::Monospace);
                    break;
                case 0:
                default:
                    SetEditingTextFontFamily(TextFontFamily::Sans);
                    break;
                }
                InvalidateRect(window_, nullptr, FALSE);
                return true;
            }
        }
    }

    if (PtInRect(&layout.alignCycleRect, cursor) != 0) {
        CycleEditingTextAlignment();
    } else if (PtInRect(&layout.boldRect, cursor) != 0) {
        ToggleEditingTextBold();
    } else if (PtInRect(&layout.italicRect, cursor) != 0) {
        ToggleEditingTextItalic();
    } else if (PtInRect(&layout.underlineRect, cursor) != 0) {
        ToggleEditingTextUnderline();
    } else if (clickedSizeInput) {
        BeginTextFormatSizeInput();
    } else if (PtInRect(&layout.fontDropdownRect, cursor) != 0) {
        textFormatFontMenuOpen_ = !textFormatFontMenuOpen_;
    }

    if (textEditWindow_ != nullptr && !textFormatSizeEditing_) {
        SetFocus(textEditWindow_);
    }
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool App::EnsureTextEditWindow(TextAlignment alignment) {
    if (window_ == nullptr) {
        return false;
    }

    const DWORD desiredAlignmentStyle = TextEditAlignmentStyle(alignment);
    auto createWindow = [&](const std::wstring& text, DWORD selectionStart, DWORD selectionEnd) -> bool {
        textEditWindow_ = CreateWindowExW(
            0,
            L"EDIT",
            text.c_str(),
            WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL | desiredAlignmentStyle,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(4001),
            instance_,
            nullptr);
        if (textEditWindow_ == nullptr) {
            textEditOriginalProc_ = nullptr;
            return false;
        }

        textEditWindowNeedsLayout_ = true;
        SetWindowLongPtrW(textEditWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        textEditOriginalProc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(textEditWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::TextEditProc)));
        SendMessageW(textEditWindow_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        SendMessageW(textEditWindow_, EM_SETSEL, selectionStart, selectionEnd);
        if (textEditFont_ != nullptr) {
            SendMessageW(textEditWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
        }
        InvalidateRect(textEditWindow_, nullptr, TRUE);
        UpdateWindow(textEditWindow_);
        return true;
    };

    if (textEditWindow_ == nullptr) {
        return createWindow(L"", 0, 0);
    }

    const LONG_PTR currentStyle = GetWindowLongPtrW(textEditWindow_, GWL_STYLE);
    if ((currentStyle & (ES_CENTER | ES_RIGHT)) == desiredAlignmentStyle) {
        return true;
    }

    const int textLength = GetWindowTextLengthW(textEditWindow_);
    std::wstring text;
    if (textLength > 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
        GetWindowTextW(textEditWindow_, buffer.data(), textLength + 1);
        text.assign(buffer.data(), static_cast<std::size_t>(textLength));
    }
    const DWORD selection = static_cast<DWORD>(SendMessageW(textEditWindow_, EM_GETSEL, 0, 0));
    const DWORD selectionStart = static_cast<DWORD>(LOWORD(selection));
    const DWORD selectionEnd = static_cast<DWORD>(HIWORD(selection));

    if (textEditOriginalProc_ != nullptr) {
        SetWindowLongPtrW(textEditWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(textEditOriginalProc_));
    }
    DestroyWindow(textEditWindow_);
    textEditWindow_ = nullptr;
    textEditOriginalProc_ = nullptr;
    return createWindow(text, selectionStart, selectionEnd);
}

void App::EndTextEditControl() {
    const RECT previousBounds = textEditBounds_;
    if (textEditWindow_ != nullptr) {
        if (textEditOriginalProc_ != nullptr) {
            SetWindowLongPtrW(textEditWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(textEditOriginalProc_));
        }
        DestroyWindow(textEditWindow_);
        textEditWindow_ = nullptr;
    }
    textEditOriginalProc_ = nullptr;
    if (textEditFont_ != nullptr) {
        DeleteObject(textEditFont_);
        textEditFont_ = nullptr;
    }
    textEditFontPixelHeight_ = 0;
    textEditAlignment_ = TextAlignment::Left;
    textEditFontFamily_ = TextFontFamily::Sans;
    textEditBold_ = false;
    textEditItalic_ = false;
    textEditUnderline_ = false;
    textEditWindowNeedsLayout_ = false;
    textEditWindowAutoShowSuppressed_ = false;
    CancelTextFormatPopup();
    ReleaseTextEditBackgroundBrush();
    textEditBounds_ = RECT {};
    if (window_ != nullptr && HasRect(previousBounds)) {
        const RECT frameRect = LiveTextEditFrameRect(previousBounds, WindowDpi(window_));
        InvalidateRect(window_, &frameRect, FALSE);
    }
}

void App::InvalidateEditingTextRegion() {
    if (window_ == nullptr || !editingTextPageIndex_.has_value() || *editingTextPageIndex_ >= document_.PageCount()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    if (!pageView.valid) {
        InvalidateCanvas();
        return;
    }
    InvalidateRect(window_, &pageView.rect, FALSE);
}

void App::ReleaseTextEditBackgroundBrush() {
    if (textEditBackgroundBrush_ != nullptr) {
        DeleteObject(textEditBackgroundBrush_);
        textEditBackgroundBrush_ = nullptr;
    }
    if (textEditBackgroundBitmap_ != nullptr) {
        DeleteObject(textEditBackgroundBitmap_);
        textEditBackgroundBitmap_ = nullptr;
    }
}

void App::RefreshTextEditControl() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || window_ == nullptr) {
        return;
    }
    if (*editingTextPageIndex_ >= document_.PageCount()) {
        if (textEditWindow_ != nullptr) {
            ShowWindow(textEditWindow_, SW_HIDE);
        }
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    const Page& page = document_.PageAt(*editingTextPageIndex_);
    if (!pageView.valid || *editingTextIndex_ >= page.texts.size()) {
        if (textEditWindow_ != nullptr) {
            ShowWindow(textEditWindow_, SW_HIDE);
        }
        return;
    }

    const UINT dpi = WindowDpi(window_);
    const PageText& text = page.texts[*editingTextIndex_];
    const PageText layoutText = CurrentEditingTextLayout().value_or(text);
    if (!EnsureTextEditWindow(layoutText.alignment)) {
        return;
    }
    const int minimumEditFontHeight = ScaleForDpi(9, dpi);
    const float editScale = std::max(
        pageView.scale,
        static_cast<float>(minimumEditFontHeight) / std::max(1.0f, layoutText.fontSize));
    const textgeom::TextPixelMetrics metrics = textgeom::MeasurePageTextPixels(layoutText, page.width, editScale);
    const int fontHeight = textgeom::PageTextFontPixelHeight(layoutText, editScale);
    const int minimumAutoWidth = std::max(ScaleForDpi(80, dpi), fontHeight * 3);
    const int contentWidth = (text.width > 0.0f) ? metrics.width : std::max(metrics.width, minimumAutoWidth);
    const int width = std::max(contentWidth + 10, ScaleForDpi(24, dpi));
    const int explicitHeight = std::max(0, static_cast<int>(std::ceil(text.height * editScale)));
    const int height = std::max(std::max(metrics.height, explicitHeight) + 10, fontHeight + 10);
    const int left = pageView.rect.left + static_cast<int>(std::lround(text.x * pageView.scale)) - 3;
    const int top = pageView.rect.top + static_cast<int>(std::lround(text.y * pageView.scale)) - 3;
    const RECT editRect = MakeRect(left, top, left + width, top + height);

    const int editFontHeight = std::max(1, fontHeight);
    const bool fontChanged = textEditFont_ == nullptr
        || textEditFontPixelHeight_ != editFontHeight
        || textEditFontFamily_ != layoutText.fontFamily
        || textEditBold_ != layoutText.bold
        || textEditItalic_ != layoutText.italic
        || textEditUnderline_ != layoutText.underline;
    if (fontChanged) {
        if (textEditFont_ != nullptr) {
            DeleteObject(textEditFont_);
            textEditFont_ = nullptr;
        }
        textEditFont_ = textgeom::CreatePageTextFont(layoutText, editFontHeight, ANTIALIASED_QUALITY);
        textEditFontPixelHeight_ = editFontHeight;
        textEditFontFamily_ = layoutText.fontFamily;
        textEditBold_ = layoutText.bold;
        textEditItalic_ = layoutText.italic;
        textEditUnderline_ = layoutText.underline;
        if (textEditFont_ != nullptr) {
            SendMessageW(textEditWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
        }
    }
    textEditAlignment_ = layoutText.alignment;

    const bool boundsChanged = EqualRect(&textEditBounds_, &editRect) == 0;
    const bool needsLayout = textEditWindowNeedsLayout_;
    const bool wasVisible = IsWindowVisible(textEditWindow_) != FALSE;
    RECT dirtyRect {};
    bool needsParentRedraw = false;
    if (boundsChanged || needsLayout) {
        dirtyRect = LiveTextEditFrameRect(editRect, dpi);
        if (HasRect(textEditBounds_)) {
            const RECT previousFrameRect = LiveTextEditFrameRect(textEditBounds_, dpi);
            dirtyRect = UnionRectangles(previousFrameRect, dirtyRect);
        }
        RefreshTextEditBackgroundBrush(editRect);
        textEditBounds_ = editRect;
        needsParentRedraw = true;
        InvalidateRect(window_, &dirtyRect, FALSE);
        if (textFormatPopupOpen_) {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
    if (boundsChanged || needsLayout) {
        UINT setWindowFlags = SWP_NOACTIVATE | SWP_NOCOPYBITS;
        if (wasVisible) {
            setWindowFlags |= SWP_SHOWWINDOW;
        }
        SetWindowPos(textEditWindow_, HWND_TOP, left, top, width, height, setWindowFlags);
        textEditWindowNeedsLayout_ = false;
    } else if (!wasVisible && !textEditWindowAutoShowSuppressed_) {
        ShowWindow(textEditWindow_, SW_SHOW);
    }
    if (needsParentRedraw) {
        if (!wasVisible) {
            textEditWindowAutoShowSuppressed_ = true;
        }
        RedrawWindow(
            window_,
            &dirtyRect,
            nullptr,
            RDW_INVALIDATE | RDW_NOCHILDREN | (!wasVisible ? RDW_UPDATENOW : 0U));
        if (!wasVisible) {
            textEditWindowAutoShowSuppressed_ = false;
            ShowWindow(textEditWindow_, SW_SHOW);
        }
    }
    if (boundsChanged || needsLayout || fontChanged) {
        InvalidateRect(textEditWindow_, nullptr, TRUE);
    }
}

void App::BeginTextEdit(std::size_t pageIndex, std::size_t textIndex, Page beforePage) {
    CommitTextEdit();
    if (pageIndex >= document_.PageCount() || textIndex >= document_.PageAt(pageIndex).texts.size()) {
        return;
    }

    ClearSelection();
    selectedTextIndices_[pageIndex].push_back(textIndex);
    editingTextPageIndex_ = pageIndex;
    editingTextIndex_ = textIndex;
    editingTextBeforePage_ = std::move(beforePage);

    const PageText& text = document_.PageAt(pageIndex).texts[textIndex];
    if (!EnsureTextEditWindow(text.alignment)) {
        editingTextPageIndex_.reset();
        editingTextIndex_.reset();
        editingTextBeforePage_.reset();
        return;
    }
    SetWindowTextW(textEditWindow_, text.content.c_str());
    InvalidatePageSurfaceCache(pageIndex);
    RefreshTextEditControl();
    ShowWindow(textEditWindow_, SW_SHOW);
    SetFocus(textEditWindow_);
    SendMessageW(textEditWindow_, EM_SETSEL, 0, -1);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::CancelTextEdit() {
    if (!editingTextPageIndex_.has_value() || !editingTextBeforePage_.has_value()) {
        EndTextEditControl();
        return;
    }

    const std::size_t pageIndex = *editingTextPageIndex_;
    const std::size_t textIndex = editingTextIndex_.value_or(0);
    if (pageIndex < document_.PageCount()) {
        document_.PageAt(pageIndex) = *editingTextBeforePage_;
        InvalidatePageSurfaceCache(pageIndex);
    }

    EndTextEditControl();
    editingTextPageIndex_.reset();
    editingTextIndex_.reset();
    editingTextBeforePage_.reset();
    ClearSelection();
    if (pageIndex < document_.PageCount() && textIndex < document_.PageAt(pageIndex).texts.size()) {
        selectedTextIndices_[pageIndex].push_back(textIndex);
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginTextToolInteraction(POINT cursor) {
    const auto hitText = HitTestText(cursor);
    if (hitText.has_value()) {
        document_.SetCurrentPageIndex(hitText->first);
        MarkPdfSidebarCompositionDirty();
        const auto selectedText = SingleSelectedText();
        if (selectedText.has_value() && *selectedText == *hitText) {
            BeginTextEdit(hitText->first, hitText->second, document_.PageAt(hitText->first));
        } else {
            ClearSelection();
            selectedTextIndices_[hitText->first].push_back(hitText->second);
            UpdateCaptureState();
            InvalidateCanvas();
        }
        return;
    }

    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        ClearSelection();
        InvalidateCanvas();
        return;
    }

    Page beforePage = document_.PageAt(hitPage->pageIndex);
    Page& page = document_.PageAt(hitPage->pageIndex);
    PageText text {};
    const PageText defaultText {};
    const float creationZoomScale = DefaultNewTextZoomScale(zoom_);
    const float newTextSizeScale = DefaultNewTextSizeScale();
    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, true);
    text.fontSize = std::max(1.0f, (defaultText.fontSize * newTextSizeScale) / creationZoomScale);
    text.x = pagePoint.x;
    text.y = pagePoint.y;
    text.width = std::max(1.0f, (textgeom::DefaultPageTextBoxWidth(page, defaultText) * newTextSizeScale) / creationZoomScale);
    if (page.width > 0.0f) {
        text.width = std::min(text.width, page.width);
        if (text.x + text.width > page.width) {
            text.x = std::max(0.0f, page.width - text.width);
        }
    }
    if (page.height > 0.0f) {
        const float estimatedInitialHeight = std::max(1.0f, text.fontSize * 1.35f);
        if (text.y + estimatedInitialHeight > page.height) {
            text.y = std::max(0.0f, page.height - estimatedInitialHeight);
        }
    }
    text.color = palette_[colorIndex_];
    page.texts.push_back(text);
    document_.SetCurrentPageIndex(hitPage->pageIndex);
    MarkPdfSidebarCompositionDirty();
    BeginTextEdit(hitPage->pageIndex, page.texts.size() - 1U, std::move(beforePage));
}

}  // namespace cnote
