#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include <richedit.h>

#include "ui/Layout.h"
#include "ui/UiUtil.h"
#include "DpiUtil.h"
#include "text/TextGeometry.h"

namespace cnote {

namespace appui = cnote::appui;
namespace textgeom = cnote::text;
namespace ui = cnote::ui;

namespace {

using cnote::PageTextEqual;
using ui::Utf16FromWide;
using ui::WideFromUtf16;
using ui::HasRect;
using ui::MakeRect;
using ui::UnionRectangles;

HBITMAP CreateTopDown32BitDibSection(HDC dc, int width, int height, void** bits) {
    if (bits == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPINFO bitmapInfo {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, bits, nullptr, 0);
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
        || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return std::nullopt;
    }

    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return std::nullopt;
    }

    PageText layoutText = page.texts[*editingTextIndex_];
    const int editLength = GetWindowTextLengthW(textEditWindow_);
    if (editLength > 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(editLength) + 1U, L'\0');
        GetWindowTextW(textEditWindow_, buffer.data(), editLength + 1);
        layoutText.content = Utf16FromWide(std::wstring_view(buffer.data(), static_cast<std::size_t>(editLength)));
    } else {
        layoutText.content.clear();
    }
    return layoutText;
}

std::optional<PageTextStyle> App::CurrentEditingTextStyle() const {
    if (textEditWindow_ == nullptr
        || !editingTextPageIndex_.has_value()
        || !editingTextIndex_.has_value()
        || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return std::nullopt;
    }
    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return std::nullopt;
    }
    const PageText& text = page.texts[*editingTextIndex_];
    PageTextStyle style {
        text.fontSize,
        text.color,
        text.fontFamily,
        text.bold,
        text.italic,
        text.underline,
    };

    CHARFORMAT2W format {};
    format.cbSize = sizeof(format);
    SendMessageW(textEditWindow_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    if ((format.dwMask & CFM_SIZE) != 0U && pageView.valid) {
        HDC dc = GetDC(textEditWindow_);
        const int dpiY = dc != nullptr ? std::max(1, GetDeviceCaps(dc, LOGPIXELSY)) : 96;
        if (dc != nullptr) {
            ReleaseDC(textEditWindow_, dc);
        }
        const int pixelHeight = MulDiv(std::max<LONG>(1, format.yHeight), dpiY, 72 * 20);
        style.fontSize = std::max(1.0f, static_cast<float>(pixelHeight) / pageView.scale);
    }
    if ((format.dwMask & CFM_COLOR) != 0U) {
        style.color = 0xFF000000U
            | (static_cast<std::uint32_t>(GetRValue(format.crTextColor)) << 16U)
            | (static_cast<std::uint32_t>(GetGValue(format.crTextColor)) << 8U)
            | static_cast<std::uint32_t>(GetBValue(format.crTextColor));
    }
    if ((format.dwMask & CFM_FACE) != 0U) {
        if (_wcsicmp(format.szFaceName, textgeom::PageTextFontFace(TextFontFamily::Serif)) == 0) {
            style.fontFamily = TextFontFamily::Serif;
        } else if (_wcsicmp(format.szFaceName, textgeom::PageTextFontFace(TextFontFamily::Monospace)) == 0) {
            style.fontFamily = TextFontFamily::Monospace;
        } else {
            style.fontFamily = TextFontFamily::Sans;
        }
    }
    style.bold = (format.dwMask & CFM_BOLD) != 0U && (format.dwEffects & CFE_BOLD) != 0U;
    style.italic = (format.dwMask & CFM_ITALIC) != 0U && (format.dwEffects & CFE_ITALIC) != 0U;
    style.underline = (format.dwMask & CFM_UNDERLINE) != 0U && (format.dwEffects & CFE_UNDERLINE) != 0U;
    return style;
}

void App::SyncTextEditModel() {
    if (textEditWindow_ == nullptr
        || !editingTextPageIndex_.has_value()
        || !editingTextIndex_.has_value()
        || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    const int textLength = GetWindowTextLengthW(textEditWindow_);
    std::u16string content;
    if (textLength > 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
        GetWindowTextW(textEditWindow_, buffer.data(), textLength + 1);
        content = Utf16FromWide(std::wstring_view(buffer.data(), static_cast<std::size_t>(textLength)));
    }
    if (page.texts[*editingTextIndex_].content == content) {
        return;
    }

    page.texts[*editingTextIndex_].content = std::move(content);
}

void App::ToggleTextFormatPopup() {
    if (textEditWindow_ == nullptr
        || !editingTextPageIndex_.has_value()
        || !editingTextIndex_.has_value()
        || *editingTextPageIndex_ >= session_->document.PageCount()
        || *editingTextIndex_ >= session_->document.PageAt(*editingTextPageIndex_).texts.size()) {
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
    const auto style = CurrentEditingTextStyle();
    return style.has_value() ? FormatTextFontSizeValue(style->fontSize) : L"";
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
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }
    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
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
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    page.texts[*editingTextIndex_].alignment = alignment;
    textFormatFontMenuOpen_ = false;
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::CycleEditingTextAlignment() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
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

void App::ApplyEditingTextCharacterFormat(
    DWORD mask,
    DWORD effects,
    std::optional<float> fontSize,
    std::optional<TextFontFamily> fontFamily) {
    if (textEditWindow_ == nullptr
        || !editingTextPageIndex_.has_value()
        || !editingTextIndex_.has_value()
        || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }
    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    if (!pageView.valid) {
        return;
    }

    CHARFORMAT2W format {};
    format.cbSize = sizeof(format);
    format.dwMask = mask;
    format.dwEffects = effects;
    if (fontSize.has_value()) {
        HDC dc = GetDC(textEditWindow_);
        const int dpiY = dc != nullptr ? std::max(1, GetDeviceCaps(dc, LOGPIXELSY)) : 96;
        if (dc != nullptr) {
            ReleaseDC(textEditWindow_, dc);
        }
        const int pixelHeight = std::max(1, static_cast<int>(std::lround(*fontSize * pageView.scale)));
        format.yHeight = MulDiv(pixelHeight, 72 * 20, dpiY);
    }
    if (fontFamily.has_value()) {
        lstrcpynW(format.szFaceName, textgeom::PageTextFontFace(*fontFamily), LF_FACESIZE);
    }

    CHARRANGE selection {};
    SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    textEditSyncSuppressed_ = true;
    SendMessageW(textEditWindow_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    if (selection.cpMax > selection.cpMin) {
        textgeom::CapturePageTextFormatting(textEditWindow_, page.texts[*editingTextIndex_], pageView.scale);
    }
    textEditSyncSuppressed_ = false;
}

void App::SetEditingTextFontSize(float fontSize) {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    const float clampedSize = std::max(1.0f, fontSize);
    if (page.texts[*editingTextIndex_].content.empty()) {
        page.texts[*editingTextIndex_].fontSize = clampedSize;
    }
    ApplyEditingTextCharacterFormat(CFM_SIZE, 0, clampedSize);
    textFormatFontMenuOpen_ = false;
    RefreshTextEditControl();
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::SetEditingTextFontFamily(TextFontFamily family) {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    if (page.texts[*editingTextIndex_].content.empty()) {
        page.texts[*editingTextIndex_].fontFamily = family;
    }
    ApplyEditingTextCharacterFormat(CFM_FACE, 0, std::nullopt, family);
    textFormatFontMenuOpen_ = false;
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::ToggleEditingTextBold() {
    if (textEditWindow_ == nullptr || !editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    CHARFORMAT2W current {};
    current.cbSize = sizeof(current);
    SendMessageW(textEditWindow_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&current));
    const bool enable = (current.dwMask & CFM_BOLD) == 0U || (current.dwEffects & CFE_BOLD) == 0U;
    if (page.texts[*editingTextIndex_].content.empty()) {
        page.texts[*editingTextIndex_].bold = enable;
    }
    ApplyEditingTextCharacterFormat(CFM_BOLD, enable ? CFE_BOLD : 0U);
    textFormatFontMenuOpen_ = false;
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::ToggleEditingTextItalic() {
    if (textEditWindow_ == nullptr || !editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    CHARFORMAT2W current {};
    current.cbSize = sizeof(current);
    SendMessageW(textEditWindow_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&current));
    const bool enable = (current.dwMask & CFM_ITALIC) == 0U || (current.dwEffects & CFE_ITALIC) == 0U;
    if (page.texts[*editingTextIndex_].content.empty()) {
        page.texts[*editingTextIndex_].italic = enable;
    }
    ApplyEditingTextCharacterFormat(CFM_ITALIC, enable ? CFE_ITALIC : 0U);
    textFormatFontMenuOpen_ = false;
    RefreshTextEditControl();
    if (textEditWindow_ != nullptr) {
        SetFocus(textEditWindow_);
    }
    if (textFormatPopupOpen_ && window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::ToggleEditingTextUnderline() {
    if (textEditWindow_ == nullptr || !editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return;
    }

    Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (*editingTextIndex_ >= page.texts.size()) {
        return;
    }

    CHARFORMAT2W current {};
    current.cbSize = sizeof(current);
    SendMessageW(textEditWindow_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&current));
    const bool enable = (current.dwMask & CFM_UNDERLINE) == 0U || (current.dwEffects & CFE_UNDERLINE) == 0U;
    if (page.texts[*editingTextIndex_].content.empty()) {
        page.texts[*editingTextIndex_].underline = enable;
    }
    ApplyEditingTextCharacterFormat(CFM_UNDERLINE, enable ? CFE_UNDERLINE : 0U);
    textFormatFontMenuOpen_ = false;
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
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        CancelTextFormatPopup();
        return false;
    }

    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
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
    if (textEditWindow_ != nullptr && textEditWindowIsLatex_) {
        // The shared control is in plain-text latex mode: rich text needs a
        // fresh window.
        EndTextEditControl();
    }

    const DWORD desiredAlignmentStyle = TextEditAlignmentStyle(alignment);
    auto createWindow = [&](const std::wstring& text, LONG selectionStart, LONG selectionEnd) -> bool {
        if (textgeom::EnsureRichEditModule() == nullptr) {
            return false;
        }
        textEditSyncSuppressed_ = true;
        textEditWindow_ = CreateWindowExW(
            0,
            L"RICHEDIT50W",
            text.c_str(),
            WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL | desiredAlignmentStyle,
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
            textEditSyncSuppressed_ = false;
            return false;
        }

        textEditWindowNeedsLayout_ = true;
        textEditBackgroundColor_ = EditingTextBackgroundColor();
        if (textEditSolidBackgroundBrush_ != nullptr) {
            DeleteObject(textEditSolidBackgroundBrush_);
        }
        textEditSolidBackgroundBrush_ = CreateSolidBrush(textEditBackgroundColor_);
        SendMessageW(textEditWindow_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(textEditBackgroundColor_));
        SetWindowLongPtrW(textEditWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        textEditOriginalProc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(textEditWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::TextEditProc)));
        const LRESULT eventMask = SendMessageW(textEditWindow_, EM_GETEVENTMASK, 0, 0);
        SendMessageW(textEditWindow_, EM_SETEVENTMASK, 0, eventMask | ENM_CHANGE);
        SendMessageW(
            textEditWindow_,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            0);
        CHARRANGE selection { selectionStart, selectionEnd };
        SendMessageW(textEditWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        if (textEditFont_ != nullptr) {
            SendMessageW(textEditWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
        }
        InvalidateRect(textEditWindow_, nullptr, TRUE);
        UpdateWindow(textEditWindow_);
        textEditSyncSuppressed_ = false;
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
    CHARRANGE selection {};
    SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));

    if (!text.empty()
        && editingTextPageIndex_.has_value()
        && editingTextIndex_.has_value()
        && *editingTextPageIndex_ < session_->document.PageCount()) {
        Page& page = session_->document.PageAt(*editingTextPageIndex_);
        if (*editingTextIndex_ < page.texts.size()) {
            page.texts[*editingTextIndex_].content = Utf16FromWide(text);
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
            if (pageView.valid) {
                textgeom::CapturePageTextFormatting(textEditWindow_, page.texts[*editingTextIndex_], pageView.scale);
            }
        }
    }
    if (textEditOriginalProc_ != nullptr) {
        SetWindowLongPtrW(textEditWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(textEditOriginalProc_));
    }
    DestroyWindow(textEditWindow_);
    textEditWindow_ = nullptr;
    textEditOriginalProc_ = nullptr;
    return createWindow(text, selection.cpMin, selection.cpMax);
}

void App::EndTextEditControl() {
    const RECT previousBounds = textEditBounds_;
    HideLatexSuggestions();
    if (textEditWindow_ != nullptr) {
        if (textEditOriginalProc_ != nullptr) {
            SetWindowLongPtrW(textEditWindow_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(textEditOriginalProc_));
        }
        DestroyWindow(textEditWindow_);
        textEditWindow_ = nullptr;
    }
    textEditOriginalProc_ = nullptr;
    textEditWindowIsLatex_ = false;
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
    if (textEditSolidBackgroundBrush_ != nullptr) {
        DeleteObject(textEditSolidBackgroundBrush_);
        textEditSolidBackgroundBrush_ = nullptr;
    }
    textEditBackgroundColor_ = RGB(255, 255, 255);
    textEditBounds_ = RECT {};
    if (window_ != nullptr && HasRect(previousBounds)) {
        const RECT frameRect = LiveTextEditFrameRect(previousBounds, WindowDpi(window_));
        InvalidateRect(window_, &frameRect, FALSE);
    }
}

COLORREF App::EditingTextBackgroundColor() const {
    if (!editingTextPageIndex_.has_value() || *editingTextPageIndex_ >= session_->document.PageCount()) {
        return RGB(255, 255, 255);
    }
    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (page.sourcePdfSourceIndex != -1 || page.sourcePdfPageIndex != -1) {
        return RGB(255, 255, 255);
    }
    if (page.raster.pixels.size() >= 3U) {
        // Raster pixels are stored as BGRA; the page background is the first pixel.
        return RGB(page.raster.pixels[2], page.raster.pixels[1], page.raster.pixels[0]);
    }
    return RGB(255, 255, 255);
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
    textEditBackgroundRect_ = RECT {};
}

void App::RefreshTextEditBackgroundBrush(const RECT& editRect) {
    if (textEditBackgroundBrush_ != nullptr
        && editRect.left >= textEditBackgroundRect_.left
        && editRect.top >= textEditBackgroundRect_.top
        && editRect.right <= textEditBackgroundRect_.right
        && editRect.bottom <= textEditBackgroundRect_.bottom) {
        return;
    }
    ReleaseTextEditBackgroundBrush();
    if (window_ == nullptr
        || !editingTextPageIndex_.has_value()
        || *editingTextPageIndex_ >= session_->document.PageCount()
        || editRect.right <= editRect.left
        || editRect.bottom <= editRect.top) {
        return;
    }

    HDC windowDc = GetDC(window_);
    if (windowDc == nullptr) {
        return;
    }

    HDC backgroundDc = CreateCompatibleDC(windowDc);
    if (backgroundDc == nullptr) {
        ReleaseDC(window_, windowDc);
        return;
    }

    const int editWidth = editRect.right - editRect.left;
    const int editHeight = editRect.bottom - editRect.top;
    const UINT captureDpi = WindowDpi(window_);
    RECT captureRect = editRect;
    InflateRect(&captureRect, std::max(ScaleForDpi(96, captureDpi), editWidth / 2), std::max(ScaleForDpi(48, captureDpi), editHeight / 2));
    const int width = captureRect.right - captureRect.left;
    const int height = captureRect.bottom - captureRect.top;
    void* bits = nullptr;
    HBITMAP bitmap = CreateTopDown32BitDibSection(backgroundDc, width, height, &bits);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(backgroundDc);
        ReleaseDC(window_, windowDc);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(backgroundDc, bitmap);
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const RECT canvasRect = CanvasRect(clientRect);
    HBRUSH canvasBrush = CreateSolidBrush(RGB(22, 26, 33));
    FillRect(backgroundDc, &canvasRect, canvasBrush);
    DeleteObject(canvasBrush);

    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    SetViewportOrgEx(backgroundDc, -captureRect.left, -captureRect.top, nullptr);
    if (pageView.valid) {
        PaintPage(backgroundDc, pageView);
    }
    SetViewportOrgEx(backgroundDc, 0, 0, nullptr);

    HBRUSH brush = CreatePatternBrush(bitmap);
    SelectObject(backgroundDc, previousBitmap);
    DeleteDC(backgroundDc);
    ReleaseDC(window_, windowDc);

    if (brush == nullptr) {
        DeleteObject(bitmap);
        return;
    }

    textEditBackgroundBitmap_ = bitmap;
    textEditBackgroundBrush_ = brush;
    textEditBackgroundRect_ = captureRect;
}

void App::RefreshTextEditControl() {
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value() || window_ == nullptr) {
        return;
    }
    if (*editingTextPageIndex_ >= session_->document.PageCount()) {
        if (textEditWindow_ != nullptr) {
            ShowWindow(textEditWindow_, SW_HIDE);
        }
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *editingTextPageIndex_);
    const Page& page = session_->document.PageAt(*editingTextPageIndex_);
    if (!pageView.valid || *editingTextIndex_ >= page.texts.size()) {
        if (textEditWindow_ != nullptr) {
            ShowWindow(textEditWindow_, SW_HIDE);
        }
        return;
    }

    const UINT dpi = WindowDpi(window_);
    const PageText& text = page.texts[*editingTextIndex_];
    HWND previousTextEditWindow = textEditWindow_;
    if (!EnsureTextEditWindow(text.alignment)) {
        return;
    }
    const bool editWindowRecreated = textEditWindow_ != previousTextEditWindow;
    PageText layoutText = CurrentEditingTextLayout().value_or(text);
    const COLORREF backgroundColor = EditingTextBackgroundColor();
    if (backgroundColor != textEditBackgroundColor_) {
        textEditBackgroundColor_ = backgroundColor;
        if (textEditSolidBackgroundBrush_ != nullptr) {
            DeleteObject(textEditSolidBackgroundBrush_);
        }
        textEditSolidBackgroundBrush_ = CreateSolidBrush(backgroundColor);
        SendMessageW(textEditWindow_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(backgroundColor));
    }
    const float editScale = pageView.scale;
    const textgeom::TextPixelMetrics metrics = textgeom::MeasurePageTextPixels(layoutText, page.width, editScale);
    const int fontHeight = textgeom::PageTextFontPixelHeight(layoutText, editScale);
    const int minimumAutoWidth = std::max(ScaleForDpi(80, dpi), fontHeight * 3);
    const int editMargin = std::max(1, ScaleForDpi(6, dpi));
    const int contentWidth = (text.width > 0.0f) ? metrics.width : std::max(metrics.width, minimumAutoWidth);
    const int width = std::max(contentWidth + (editMargin * 2), ScaleForDpi(24, dpi));
    const int explicitHeight = std::max(0, static_cast<int>(std::ceil(text.height * editScale)));
    const int contentHeight = std::max(std::max(metrics.height, explicitHeight), fontHeight);
    const int height = contentHeight + (editMargin * 2);
    const int left = pageView.rect.left + static_cast<int>(std::lround(text.x * pageView.scale)) - editMargin;
    const int top = pageView.rect.top + static_cast<int>(std::lround(text.y * pageView.scale)) - editMargin;
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
    if (fontChanged || textEditWindowNeedsLayout_) {
        textEditSyncSuppressed_ = true;
        if (!fontChanged && !editWindowRecreated) {
            textgeom::CapturePageTextFormatting(textEditWindow_, layoutText, editScale);
        }
        textgeom::ApplyPageTextFormatting(textEditWindow_, layoutText, editScale);
        textEditSyncSuppressed_ = false;
    }

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
        SendMessageW(
            textEditWindow_,
            EM_SETMARGINS,
            EC_LEFTMARGIN | EC_RIGHTMARGIN,
            0);
        UINT setWindowFlags = SWP_NOACTIVATE | SWP_NOCOPYBITS;
        if (wasVisible) {
            setWindowFlags |= SWP_SHOWWINDOW;
        }
        SetWindowPos(textEditWindow_, HWND_TOP, left, top, width, height, setWindowFlags);
        RECT formatRect { editMargin, editMargin, editMargin + contentWidth, editMargin + contentHeight };
        SendMessageW(textEditWindow_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&formatRect));
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

void App::CommitTextEdit() {
    if (textEditWindow_ == nullptr || !editingTextPageIndex_.has_value() || !editingTextIndex_.has_value()) {
        return;
    }

    const std::size_t pageIndex = *editingTextPageIndex_;
    const std::size_t textIndex = *editingTextIndex_;
    const std::optional<PageText> beforeText = editingTextBefore_;
    const bool createdText = editingTextCreatesText_;
    std::optional<PageText> committedText;

    if (pageIndex < session_->document.PageCount() && textIndex < session_->document.PageAt(pageIndex).texts.size()) {
        const int textLength = GetWindowTextLengthW(textEditWindow_);
        std::u16string content;
        if (textLength > 0) {
            std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
            GetWindowTextW(textEditWindow_, buffer.data(), textLength + 1);
            content = Utf16FromWide(std::wstring_view(buffer.data(), static_cast<std::size_t>(textLength)));
        }
        Page& page = session_->document.PageAt(pageIndex);
        page.texts[textIndex].content = std::move(content);
        if (page.texts[textIndex].content.empty()) {
            page.texts.erase(page.texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
        } else {
            RECT clientRect {};
            GetClientRect(window_, &clientRect);
            const PageView pageView = CalculatePageView(clientRect, pageIndex);
            if (pageView.valid) {
                textgeom::CapturePageTextFormatting(textEditWindow_, page.texts[textIndex], pageView.scale);
            }
            committedText = page.texts[textIndex];
        }
    }

    const RECT editFrameRect = (window_ != nullptr && HasRect(textEditBounds_))
        ? LiveTextEditFrameRect(textEditBounds_, WindowDpi(window_))
        : RECT {};
    if (pageIndex < session_->document.PageCount()) {
        InvalidatePageSurfaceCache(pageIndex);
    }
    EndTextEditControl();
    editingTextPageIndex_.reset();
    editingTextIndex_.reset();
    editingTextBefore_.reset();
    editingTextCreatesText_ = false;
    if (window_ != nullptr && HasRect(editFrameRect)) {
        RedrawWindow(window_, &editFrameRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    ClearSelection();
    if (pageIndex < session_->document.PageCount() && textIndex < session_->document.PageAt(pageIndex).texts.size()) {
        selectedTextIndices_[pageIndex].push_back(textIndex);
    }
    if (pageIndex < session_->document.PageCount()) {
        bool changed = false;
        if (createdText) {
            if (committedText.has_value()) {
                PushTextHistoryAction(HistoryActionType::AddText, pageIndex, textIndex, *committedText);
                changed = true;
            }
        } else if (beforeText.has_value()) {
            if (!committedText.has_value()) {
                PushTextHistoryAction(HistoryActionType::RemoveText, pageIndex, textIndex, *beforeText);
                changed = true;
            } else if (!PageTextEqual(*beforeText, *committedText)) {
                PushTextHistoryAction(HistoryActionType::UpdateText, pageIndex, textIndex, *beforeText, *committedText);
                changed = true;
            }
        }
        if (changed) {
            NotifyDocumentMutated();
        }
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginTextEdit(std::size_t pageIndex, std::size_t textIndex, std::optional<PageText> beforeText, bool createsText) {
    CommitTextEdit();
    CommitLatexEdit();
    if (pageIndex >= session_->document.PageCount() || textIndex >= session_->document.PageAt(pageIndex).texts.size()) {
        return;
    }

    ClearSelection();
    selectedTextIndices_[pageIndex].push_back(textIndex);
    editingTextPageIndex_ = pageIndex;
    editingTextIndex_ = textIndex;
    editingTextBefore_ = std::move(beforeText);
    editingTextCreatesText_ = createsText;

    const PageText& text = session_->document.PageAt(pageIndex).texts[textIndex];
    if (!EnsureTextEditWindow(text.alignment)) {
        editingTextPageIndex_.reset();
        editingTextIndex_.reset();
        if (createsText && textIndex < session_->document.PageAt(pageIndex).texts.size()) {
            session_->document.PageAt(pageIndex).texts.erase(session_->document.PageAt(pageIndex).texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
        }
        editingTextBefore_.reset();
        editingTextCreatesText_ = false;
        return;
    }
    textEditSyncSuppressed_ = true;
    SetWindowTextW(textEditWindow_, WideFromUtf16(text.content).c_str());
    textEditSyncSuppressed_ = false;
    InvalidatePageSurfaceCache(pageIndex);
    RefreshTextEditControl();
    ShowWindow(textEditWindow_, SW_SHOW);
    SetFocus(textEditWindow_);
    SendMessageW(textEditWindow_, EM_SETSEL, 0, -1);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::CancelTextEdit() {
    if (!editingTextPageIndex_.has_value() && editingLatexPageIndex_.has_value()) {
        // Shared control is hosting a latex session: cancel that instead.
        CancelLatexEdit();
        return;
    }
    if (!editingTextPageIndex_.has_value() || !editingTextIndex_.has_value()) {
        EndTextEditControl();
        return;
    }

    const std::size_t pageIndex = *editingTextPageIndex_;
    const std::size_t textIndex = editingTextIndex_.value_or(0);
    if (pageIndex < session_->document.PageCount()) {
        auto& texts = session_->document.PageAt(pageIndex).texts;
        if (editingTextCreatesText_) {
            if (textIndex < texts.size()) {
                texts.erase(texts.begin() + static_cast<std::ptrdiff_t>(textIndex));
            }
        } else if (editingTextBefore_.has_value() && textIndex < texts.size()) {
            texts[textIndex] = *editingTextBefore_;
        }
        InvalidatePageSurfaceCache(pageIndex);
    }

    const RECT editFrameRect = (window_ != nullptr && HasRect(textEditBounds_))
        ? LiveTextEditFrameRect(textEditBounds_, WindowDpi(window_))
        : RECT {};
    EndTextEditControl();
    editingTextPageIndex_.reset();
    editingTextIndex_.reset();
    editingTextBefore_.reset();
    editingTextCreatesText_ = false;
    if (window_ != nullptr && HasRect(editFrameRect)) {
        RedrawWindow(window_, &editFrameRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    ClearSelection();
    if (pageIndex < session_->document.PageCount() && textIndex < session_->document.PageAt(pageIndex).texts.size()) {
        selectedTextIndices_[pageIndex].push_back(textIndex);
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginTextToolInteraction(POINT cursor) {
    const auto hitText = HitTestText(cursor);
    if (hitText.has_value()) {
        SetViewPageIndex(hitText->first);
        MarkPdfSidebarCompositionDirty();
        BeginTextEdit(hitText->first, hitText->second, session_->document.PageAt(hitText->first).texts[hitText->second]);
        return;
    }

    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        ClearSelection();
        InvalidateCanvas();
        return;
    }

    Page& page = session_->document.PageAt(hitPage->pageIndex);
    PageText text {};
    const PageText defaultText {};
    const float newTextSizeScale = DefaultNewTextSizeScale();
    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, true);
    text.id = AllocateTextId();
    text.fontSize = std::max(1.0f, defaultText.fontSize * newTextSizeScale);
    text.x = pagePoint.x;
    text.y = pagePoint.y;
    text.width = std::max(1.0f, textgeom::DefaultPageTextBoxWidth(page, defaultText) * newTextSizeScale);
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
    SetViewPageIndex(hitPage->pageIndex);
    MarkPdfSidebarCompositionDirty();
    BeginTextEdit(hitPage->pageIndex, page.texts.size() - 1U, std::nullopt, true);
}

std::uint64_t App::AllocateTextId() const {
    std::uint64_t nextId = 1;
    for (std::size_t pageIndex = 0; pageIndex < session_->document.PageCount(); ++pageIndex) {
        for (const PageText& text : session_->document.PageAt(pageIndex).texts) {
            nextId = std::max(nextId, text.id + 1U);
        }
    }
    return nextId;
}

}  // namespace cnote
