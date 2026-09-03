#include "App.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>
#include <richedit.h>

#include "latex/LatexCompletion.h"
#include "latex/LatexEngine.h"
#include "selection/Geometry.h"
#include "text/TextGeometry.h"
#include "ui/UiUtil.h"
#include "DpiUtil.h"

namespace cnote {

namespace textgeom = cnote::text;
namespace ui = cnote::ui;

namespace {

using ui::HasRect;
using ui::MakeRect;
using ui::UnionRectangles;

constexpr int kLatexEditControlId = 4001;
constexpr int kLatexSuggestionControlId = 4002;
constexpr int kLatexSuggestionMaximumVisible = 8;

// Source editing is deliberately smaller than the rendered equation.
constexpr float kLatexSourceFontScale = 0.55f;

// Mirror of the helper in TextEditing.cpp (file-static there): the edit frame
// invalidation region is the control rect inflated by the frame padding.
RECT LiveTextEditFrameRect(const RECT& editRect, UINT dpi) {
    RECT frameRect = editRect;
    InflateRect(&frameRect, ScaleForDpi(4, dpi), ScaleForDpi(4, dpi));
    return frameRect;
}

HFONT CreateLatexEditFont(int pixelHeight) {
    return CreateFontW(
        -std::max(1, pixelHeight),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN,
        L"Consolas");
}
int LatexEditLineHeight(HWND editWindow, HFONT font, int fallbackHeight, UINT dpi) {
    int lineHeight = fallbackHeight;
    if (editWindow != nullptr && font != nullptr) {
        HDC dc = GetDC(editWindow);
        if (dc != nullptr) {
            const HGDIOBJ previousFont = SelectObject(dc, font);
            TEXTMETRICW textMetrics {};
            if (GetTextMetricsW(dc, &textMetrics) != 0) {
                lineHeight = std::max(lineHeight, static_cast<int>(textMetrics.tmHeight));
            }
            if (previousFont != nullptr) {
                SelectObject(dc, previousFont);
            }
            ReleaseDC(editWindow, dc);
        }
    }
    // RichEdit positions the baseline within the formatting rectangle. Keep
    // one scaled pixel below the measured line so descenders are not clipped.
    return lineHeight + std::max(1, ScaleForDpi(1, dpi));
}

std::wstring ReadEditText(HWND editWindow) {
    if (editWindow == nullptr) {
        return {};
    }
    const int textLength = GetWindowTextLengthW(editWindow);
    if (textLength <= 0) {
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
    GetWindowTextW(editWindow, buffer.data(), textLength + 1);
    return std::wstring(buffer.data(), static_cast<std::size_t>(textLength));
}

POINT LatexEditCaretPosition(HWND editWindow, LONG characterIndex) {
    const LRESULT packedPosition = SendMessageW(editWindow, EM_POSFROMCHAR, characterIndex, 0);
    return {
        static_cast<LONG>(static_cast<SHORT>(LOWORD(packedPosition))),
        static_cast<LONG>(static_cast<SHORT>(HIWORD(packedPosition))),
    };
}
}  // namespace
bool App::IsEditingLatex(std::size_t pageIndex, std::size_t latexIndex) const {
    return textEditWindow_ != nullptr
        && editingLatexPageIndex_.has_value()
        && editingLatexIndex_.has_value()
        && *editingLatexPageIndex_ == pageIndex
        && *editingLatexIndex_ == latexIndex;
}

std::uint64_t App::AllocateLatexId() const {
    std::uint64_t nextId = 1;
    for (std::size_t pageIndex = 0; pageIndex < document_.PageCount(); ++pageIndex) {
        for (const PageLatex& latex : document_.PageAt(pageIndex).latexes) {
            nextId = std::max(nextId, latex.id + 1U);
        }
    }
    return nextId;
}

COLORREF App::EditingLatexBackgroundColor() const {
    if (!editingLatexPageIndex_.has_value() || *editingLatexPageIndex_ >= document_.PageCount()) {
        return RGB(255, 255, 255);
    }
    const Page& page = document_.PageAt(*editingLatexPageIndex_);
    if (page.sourcePdfSourceIndex != -1 || page.sourcePdfPageIndex != -1) {
        return RGB(255, 255, 255);
    }
    if (page.raster.pixels.size() >= 3U) {
        // Raster pixels are stored as BGRA; the page background is the first pixel.
        return RGB(page.raster.pixels[2], page.raster.pixels[1], page.raster.pixels[0]);
    }
    return RGB(255, 255, 255);
}

bool App::EnsureLatexEditWindow() {
    if (window_ == nullptr) {
        return false;
    }
    if (textEditWindow_ != nullptr && textEditWindowIsLatex_) {
        return true;
    }
    if (textEditWindow_ != nullptr) {
        // Wrong-mode window (created for rich text): drop it and recreate.
        EndTextEditControl();
    }
    if (textgeom::EnsureRichEditModule() == nullptr) {
        return false;
    }

    textEditSyncSuppressed_ = true;
    textEditWindow_ = CreateWindowExW(
        0,
        L"RICHEDIT50W",
        L"",
        WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLatexEditControlId)),
        instance_,
        nullptr);
    if (textEditWindow_ == nullptr) {
        textEditOriginalProc_ = nullptr;
        textEditSyncSuppressed_ = false;
        return false;
    }
    textEditWindowIsLatex_ = true;
    textEditWindowNeedsLayout_ = true;

    // Latex source is plain text: no rich formatting, no autocorrect.
    SendMessageW(textEditWindow_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);

    textEditBackgroundColor_ = EditingLatexBackgroundColor();
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
    SendMessageW(textEditWindow_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    textEditSyncSuppressed_ = false;
    return true;
}

bool App::HandleLatexAutoPair(wchar_t character) {
    if (textEditWindow_ == nullptr || !editingLatexIndex_.has_value()) {
        return false;
    }

    wchar_t closingCharacter = L'\0';
    switch (character) {
    case L'{':
        closingCharacter = L'}';
        break;
    case L'(':
        closingCharacter = L')';
        break;
    case L'[':
        closingCharacter = L']';
        break;
    case L'}':
    case L')':
    case L']': {
        const std::wstring text = ReadEditText(textEditWindow_);
        CHARRANGE selection {};
        SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        if (selection.cpMin == selection.cpMax
            && selection.cpMin >= 0
            && static_cast<std::size_t>(selection.cpMin) < text.size()
            && text[static_cast<std::size_t>(selection.cpMin)] == character) {
            CHARRANGE nextPosition { selection.cpMin + 1, selection.cpMin + 1 };
            SendMessageW(textEditWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&nextPosition));
            UpdateLatexSuggestions();
            return true;
        }
        return false;
    }
    default:
        return false;
    }

    const std::wstring text = ReadEditText(textEditWindow_);
    CHARRANGE selection {};
    SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMin < 0 || selection.cpMax < selection.cpMin) {
        return false;
    }
    const std::size_t selectedStart = static_cast<std::size_t>(selection.cpMin);
    const std::size_t selectedEnd = static_cast<std::size_t>(selection.cpMax);
    if (selectedEnd > text.size()) {
        return false;
    }

    std::wstring replacement;
    replacement.reserve((selectedEnd - selectedStart) + 2U);
    replacement.push_back(character);
    replacement.append(text, selectedStart, selectedEnd - selectedStart);
    replacement.push_back(closingCharacter);

    const bool wasSyncSuppressed = textEditSyncSuppressed_;
    textEditSyncSuppressed_ = true;
    SendMessageW(
        textEditWindow_,
        EM_REPLACESEL,
        TRUE,
        reinterpret_cast<LPARAM>(replacement.c_str()));
    textEditSyncSuppressed_ = wasSyncSuppressed;

    const LONG cursorPosition = selection.cpMin + 1
        + static_cast<LONG>(selectedEnd - selectedStart);
    CHARRANGE cursor { cursorPosition, cursorPosition };
    SendMessageW(textEditWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cursor));
    SyncLatexEditModel();
    RefreshLatexEditControl();
    UpdateLatexSuggestions();
    RedrawWindow(textEditWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    return true;
}

void App::HideLatexSuggestions() {
    if (latexSuggestionWindow_ != nullptr) {
        DestroyWindow(latexSuggestionWindow_);
        latexSuggestionWindow_ = nullptr;
    }
    latexSuggestionEntries_.clear();
    latexSuggestionSelection_ = 0;
}

void App::UpdateLatexSuggestions() {
    if (latexSuggestionUpdating_
        || !editingLatexIndex_.has_value()
        || textEditWindow_ == nullptr
        || window_ == nullptr) {
        return;
    }

    const std::wstring text = ReadEditText(textEditWindow_);
    CHARRANGE selection {};
    SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMin != selection.cpMax) {
        HideLatexSuggestions();
        return;
    }

    const auto context = latex::FindCompletionContext(
        text,
        static_cast<std::size_t>(std::max<LONG>(0, selection.cpMin)));
    if (!context.has_value()) {
        HideLatexSuggestions();
        return;
    }
    const std::vector<latex::CompletionItem> suggestions = latex::SuggestionsFor(context->prefix);
    if (suggestions.empty()) {
        HideLatexSuggestions();
        return;
    }

    latexSuggestionUpdating_ = true;
    const std::wstring selectedCommand = (latexSuggestionWindow_ != nullptr
            && latexSuggestionSelection_ < latexSuggestionEntries_.size())
        ? latexSuggestionEntries_[latexSuggestionSelection_].command
        : std::wstring {};
    latexSuggestionEntries_ = suggestions;
    latexSuggestionSelection_ = 0;
    if (!selectedCommand.empty()) {
        for (std::size_t index = 0; index < latexSuggestionEntries_.size(); ++index) {
            if (latexSuggestionEntries_[index].command == selectedCommand) {
                latexSuggestionSelection_ = index;
                break;
            }
        }
    }

    if (latexSuggestionWindow_ == nullptr) {
        latexSuggestionWindow_ = CreateWindowExW(
            0,
            L"LISTBOX",
            nullptr,
            WS_CHILD | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLatexSuggestionControlId)),
            instance_,
            nullptr);
    }
    if (latexSuggestionWindow_ == nullptr) {
        latexSuggestionUpdating_ = false;
        return;
    }

    SendMessageW(latexSuggestionWindow_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(latexSuggestionWindow_, LB_RESETCONTENT, 0, 0);
    for (const latex::CompletionItem& item : latexSuggestionEntries_) {
        SendMessageW(
            latexSuggestionWindow_,
            LB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(item.command.c_str()));
    }
    const UINT dpi = WindowDpi(window_);
    const int itemHeight = std::max(ScaleForDpi(22, dpi), textEditFontPixelHeight_ + ScaleForDpi(6, dpi));
    SendMessageW(latexSuggestionWindow_, LB_SETITEMHEIGHT, 0, itemHeight);
    SendMessageW(
        latexSuggestionWindow_,
        LB_SETCURSEL,
        static_cast<WPARAM>(latexSuggestionSelection_),
        0);
    SendMessageW(latexSuggestionWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
    SendMessageW(latexSuggestionWindow_, WM_SETREDRAW, TRUE, 0);

    const POINT caret = LatexEditCaretPosition(textEditWindow_, selection.cpMin);
    POINT parentCaret = caret;
    MapWindowPoints(textEditWindow_, window_, &parentCaret, 1);
    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const int width = ScaleForDpi(220, dpi);
    const int visibleCount = std::min(
        kLatexSuggestionMaximumVisible,
        static_cast<int>(latexSuggestionEntries_.size()));
    const int height = itemHeight * std::max(1, visibleCount) + ScaleForDpi(2, dpi);
    int x = parentCaret.x;
    int y = parentCaret.y + std::max(itemHeight, textEditFontPixelHeight_ + ScaleForDpi(4, dpi));
    if (x + width > clientRect.right) {
        x = std::max(0, static_cast<int>(clientRect.right) - width);
    }
    if (y + height > clientRect.bottom) {
        y = parentCaret.y - height;
    }
    x = std::max(0, x);
    y = std::clamp(y, 0, std::max(0, static_cast<int>(clientRect.bottom) - height));
    SetWindowPos(
        latexSuggestionWindow_,
        HWND_TOP,
        x,
        y,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    latexSuggestionUpdating_ = false;
}

void App::MoveLatexSuggestionSelection(int delta) {
    if (latexSuggestionWindow_ == nullptr || latexSuggestionEntries_.empty()) {
        return;
    }
    const int count = static_cast<int>(latexSuggestionEntries_.size());
    int next = static_cast<int>(latexSuggestionSelection_) + delta;
    next = std::clamp(next, 0, count - 1);
    latexSuggestionSelection_ = static_cast<std::size_t>(next);
    SendMessageW(
        latexSuggestionWindow_,
        LB_SETCURSEL,
        static_cast<WPARAM>(latexSuggestionSelection_),
        0);
}

void App::AcceptLatexSuggestion() {
    if (latexSuggestionEntries_.empty()
        || latexSuggestionSelection_ >= latexSuggestionEntries_.size()
        || textEditWindow_ == nullptr) {
        return;
    }
    const std::wstring text = ReadEditText(textEditWindow_);
    CHARRANGE selection {};
    SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    if (selection.cpMin != selection.cpMax || selection.cpMin < 0) {
        return;
    }
    const auto context = latex::FindCompletionContext(
        text,
        static_cast<std::size_t>(selection.cpMin));
    if (!context.has_value()) {
        HideLatexSuggestions();
        return;
    }

    const latex::CompletionItem item = latexSuggestionEntries_[latexSuggestionSelection_];
    const std::wstring insertion = item.insertion.empty() ? item.command : item.insertion;
    const std::size_t cursorOffset = std::min(item.cursorOffset, insertion.size());
    latexSuggestionUpdating_ = true;
    const bool wasSyncSuppressed = textEditSyncSuppressed_;
    textEditSyncSuppressed_ = true;
    CHARRANGE replacement {
        static_cast<LONG>(context->start),
        static_cast<LONG>(context->end),
    };
    SendMessageW(textEditWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&replacement));
    SendMessageW(
        textEditWindow_,
        EM_REPLACESEL,
        TRUE,
        reinterpret_cast<LPARAM>(insertion.c_str()));
    textEditSyncSuppressed_ = wasSyncSuppressed;
    const LONG cursorPosition = static_cast<LONG>(context->start + cursorOffset);
    CHARRANGE cursor { cursorPosition, cursorPosition };
    SendMessageW(textEditWindow_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cursor));
    SyncLatexEditModel();
    RefreshLatexEditControl();
    HideLatexSuggestions();
    latexSuggestionUpdating_ = false;
}

bool App::HandleLatexSuggestionKey(WPARAM key) {
    if (key == VK_TAB || key == VK_RETURN) {
        if (latexSuggestionWindow_ == nullptr || !IsWindowVisible(latexSuggestionWindow_)) {
            const std::wstring text = ReadEditText(textEditWindow_);
            CHARRANGE selection {};
            SendMessageW(textEditWindow_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
            if (selection.cpMin != selection.cpMax || selection.cpMin < 0) {
                return false;
            }
            const auto context = latex::FindCompletionContext(
                text,
                static_cast<std::size_t>(selection.cpMin));
            if (!context.has_value()) {
                return false;
            }
            latexSuggestionEntries_ = latex::SuggestionsFor(context->prefix);
            latexSuggestionSelection_ = 0;
        }
        if (latexSuggestionEntries_.empty()) {
            return false;
        }
        AcceptLatexSuggestion();
        return true;
    }
    if (latexSuggestionWindow_ == nullptr || !IsWindowVisible(latexSuggestionWindow_)) {
        return false;
    }
    switch (key) {
    case VK_ESCAPE:
        HideLatexSuggestions();
        return true;
    case VK_UP:
        MoveLatexSuggestionSelection(-1);
        return true;
    case VK_DOWN:
        MoveLatexSuggestionSelection(1);
        return true;
    case VK_PRIOR:
        MoveLatexSuggestionSelection(-static_cast<int>(kLatexSuggestionMaximumVisible));
        return true;
    case VK_NEXT:
        MoveLatexSuggestionSelection(static_cast<int>(kLatexSuggestionMaximumVisible));
        return true;
    default:
        return false;
    }
}

void App::RefreshLatexEditControl() {
    if (!editingLatexPageIndex_.has_value() || !editingLatexIndex_.has_value() || window_ == nullptr) {
        return;
    }
    if (*editingLatexPageIndex_ >= document_.PageCount()) {
        if (textEditWindow_ != nullptr) {
            ShowWindow(textEditWindow_, SW_HIDE);
        }
        return;
    }

    RECT clientRect {};
    GetClientRect(window_, &clientRect);
    const PageView pageView = CalculatePageView(clientRect, *editingLatexPageIndex_);
    const Page& page = document_.PageAt(*editingLatexPageIndex_);
    if (!pageView.valid || *editingLatexIndex_ >= page.latexes.size()) {
        if (textEditWindow_ != nullptr) {
            ShowWindow(textEditWindow_, SW_HIDE);
        }
        return;
    }

    const UINT dpi = WindowDpi(window_);
    const PageLatex& block = page.latexes[*editingLatexIndex_];
    if (!EnsureLatexEditWindow()) {
        return;
    }

    const COLORREF backgroundColor = EditingLatexBackgroundColor();
    if (backgroundColor != textEditBackgroundColor_) {
        textEditBackgroundColor_ = backgroundColor;
        if (textEditSolidBackgroundBrush_ != nullptr) {
            DeleteObject(textEditSolidBackgroundBrush_);
        }
        textEditSolidBackgroundBrush_ = CreateSolidBrush(backgroundColor);
        SendMessageW(textEditWindow_, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(backgroundColor));
    }

    // Size the source box from the live-parsed equation, with a floor so an
    // empty/unparseable source still gets a usable editor.
    const latex::FormulaMetrics metrics = latex::Measure(latex::Parse(block));
    const float scale = std::max(0.1f, pageView.scale);
    const int equationWidth = static_cast<int>(std::ceil(metrics.width * scale));
    const int equationHeight = static_cast<int>(std::ceil(std::max(metrics.height, block.fontSize * 1.35f) * scale));

    const int fontHeight = std::max(ScaleForDpi(12, dpi), static_cast<int>(std::lround(block.fontSize * kLatexSourceFontScale * scale)));
    const bool fontChanged = textEditFont_ == nullptr || textEditFontPixelHeight_ != fontHeight;
    if (fontChanged) {
        if (textEditFont_ != nullptr) {
            DeleteObject(textEditFont_);
            textEditFont_ = nullptr;
        }
        textEditFont_ = CreateLatexEditFont(fontHeight);
        textEditFontPixelHeight_ = fontHeight;
        if (textEditFont_ != nullptr) {
            SendMessageW(textEditWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(textEditFont_), TRUE);
        }
    }
    const int lineHeight = LatexEditLineHeight(textEditWindow_, textEditFont_, fontHeight, dpi);
    const int editMargin = std::max(1, ScaleForDpi(6, dpi));

    int sourceLines = 1;
    for (const wchar_t ch : block.content) {
        if (ch == L'\n') {
            ++sourceLines;
        }
    }
    const int contentWidth = std::max(std::max(equationWidth, ScaleForDpi(260, dpi)), fontHeight * 12);
    const int contentHeight = std::max(sourceLines * lineHeight, lineHeight);
    const int width = contentWidth + (editMargin * 2);
    const int height = contentHeight + (editMargin * 2);

    // Place the source box above the equation preview; flip below when there
    // is no room, then clamp to the window.
    const int equationLeft = pageView.rect.left + static_cast<int>(std::lround(block.x * pageView.scale));
    const int equationTop = pageView.rect.top + static_cast<int>(std::lround(block.y * pageView.scale));
    const int gap = ScaleForDpi(8, dpi);
    int left = equationLeft - editMargin;
    int top = equationTop - height - gap;
    if (top < pageView.rect.top) {
        top = equationTop + equationHeight + gap;
    }
    left = std::clamp(left, 0, std::max(0, static_cast<int>(clientRect.right) - width));
    top = std::clamp(top, 0, std::max(0, static_cast<int>(clientRect.bottom) - height));
    const RECT editRect = MakeRect(left, top, left + width, top + height);

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
    }
    if (boundsChanged || needsLayout) {
        SendMessageW(textEditWindow_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
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

void App::SyncLatexEditModel() {
    if (textEditWindow_ == nullptr
        || !editingLatexPageIndex_.has_value()
        || !editingLatexIndex_.has_value()
        || *editingLatexPageIndex_ >= document_.PageCount()) {
        return;
    }
    Page& page = document_.PageAt(*editingLatexPageIndex_);
    if (*editingLatexIndex_ >= page.latexes.size()) {
        return;
    }

    const int textLength = GetWindowTextLengthW(textEditWindow_);
    std::wstring content;
    if (textLength > 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
        GetWindowTextW(textEditWindow_, buffer.data(), textLength + 1);
        content.assign(buffer.data(), static_cast<std::size_t>(textLength));
    }
    page.latexes[*editingLatexIndex_].content = std::move(content);
    // Live preview repaints directly (the editing block is excluded from the
    // page surface caches), so a canvas repaint is all that is needed.
    InvalidateCanvas();
}

void App::BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex, std::optional<PageLatex> beforeLatex, bool createsLatex) {
    CommitTextEdit();
    CommitLatexEdit();
    if (pageIndex >= document_.PageCount() || latexIndex >= document_.PageAt(pageIndex).latexes.size()) {
        return;
    }

    ClearSelection();
    selectedLatexIndices_[pageIndex].push_back(latexIndex);
    editingLatexPageIndex_ = pageIndex;
    editingLatexIndex_ = latexIndex;
    editingLatexBefore_ = std::move(beforeLatex);
    editingLatexCreatesLatex_ = createsLatex;

    const PageLatex& block = document_.PageAt(pageIndex).latexes[latexIndex];
    if (!EnsureLatexEditWindow()) {
        editingLatexPageIndex_.reset();
        editingLatexIndex_.reset();
        if (createsLatex && latexIndex < document_.PageAt(pageIndex).latexes.size()) {
            document_.PageAt(pageIndex).latexes.erase(document_.PageAt(pageIndex).latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
        }
        editingLatexBefore_.reset();
        editingLatexCreatesLatex_ = false;
        return;
    }
    textEditSyncSuppressed_ = true;
    SetWindowTextW(textEditWindow_, block.content.c_str());
    textEditSyncSuppressed_ = false;
    InvalidatePageSurfaceCache(pageIndex);
    RefreshLatexEditControl();
    ShowWindow(textEditWindow_, SW_SHOW);
    SetFocus(textEditWindow_);
    SendMessageW(textEditWindow_, EM_SETSEL, 0, -1);
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::CommitLatexEdit() {
    if (textEditWindow_ == nullptr || !editingLatexPageIndex_.has_value() || !editingLatexIndex_.has_value()) {
        return;
    }

    const std::size_t pageIndex = *editingLatexPageIndex_;
    const std::size_t latexIndex = *editingLatexIndex_;
    const std::optional<PageLatex> beforeLatex = editingLatexBefore_;
    const bool createdLatex = editingLatexCreatesLatex_;
    std::optional<PageLatex> committedLatex;

    if (pageIndex < document_.PageCount() && latexIndex < document_.PageAt(pageIndex).latexes.size()) {
        const int textLength = GetWindowTextLengthW(textEditWindow_);
        std::wstring content;
        if (textLength > 0) {
            std::vector<wchar_t> buffer(static_cast<std::size_t>(textLength) + 1U, L'\0');
            GetWindowTextW(textEditWindow_, buffer.data(), textLength + 1);
            content.assign(buffer.data(), static_cast<std::size_t>(textLength));
        }
        Page& page = document_.PageAt(pageIndex);
        page.latexes[latexIndex].content = std::move(content);
        if (page.latexes[latexIndex].content.empty()) {
            page.latexes.erase(page.latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
        } else {
            committedLatex = page.latexes[latexIndex];
        }
    }

    const RECT editFrameRect = (window_ != nullptr && HasRect(textEditBounds_))
        ? LiveTextEditFrameRect(textEditBounds_, WindowDpi(window_))
        : RECT {};
    if (pageIndex < document_.PageCount()) {
        InvalidatePageSurfaceCache(pageIndex);
    }
    EndTextEditControl();
    editingLatexPageIndex_.reset();
    editingLatexIndex_.reset();
    editingLatexBefore_.reset();
    editingLatexCreatesLatex_ = false;
    if (window_ != nullptr && HasRect(editFrameRect)) {
        RedrawWindow(window_, &editFrameRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    ClearSelection();
    if (pageIndex < document_.PageCount() && latexIndex < document_.PageAt(pageIndex).latexes.size()) {
        selectedLatexIndices_[pageIndex].push_back(latexIndex);
    }
    if (pageIndex < document_.PageCount()) {
        bool changed = false;
        if (createdLatex) {
            if (committedLatex.has_value()) {
                PushLatexHistoryAction(HistoryActionType::AddLatex, pageIndex, latexIndex, *committedLatex);
                changed = true;
            }
        } else if (beforeLatex.has_value()) {
            if (!committedLatex.has_value()) {
                PushLatexHistoryAction(HistoryActionType::RemoveLatex, pageIndex, latexIndex, *beforeLatex);
                changed = true;
            } else if (!PageLatexEqual(*beforeLatex, *committedLatex)) {
                PushLatexHistoryAction(HistoryActionType::UpdateLatex, pageIndex, latexIndex, *beforeLatex, *committedLatex);
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

void App::CancelLatexEdit() {
    if (!editingLatexPageIndex_.has_value() || !editingLatexIndex_.has_value()) {
        EndTextEditControl();
        return;
    }

    const std::size_t pageIndex = *editingLatexPageIndex_;
    const std::size_t latexIndex = editingLatexIndex_.value_or(0);
    if (pageIndex < document_.PageCount()) {
        auto& latexes = document_.PageAt(pageIndex).latexes;
        if (editingLatexCreatesLatex_) {
            if (latexIndex < latexes.size()) {
                latexes.erase(latexes.begin() + static_cast<std::ptrdiff_t>(latexIndex));
            }
        } else if (editingLatexBefore_.has_value() && latexIndex < latexes.size()) {
            latexes[latexIndex] = *editingLatexBefore_;
        }
        InvalidatePageSurfaceCache(pageIndex);
    }

    const RECT editFrameRect = (window_ != nullptr && HasRect(textEditBounds_))
        ? LiveTextEditFrameRect(textEditBounds_, WindowDpi(window_))
        : RECT {};
    EndTextEditControl();
    editingLatexPageIndex_.reset();
    editingLatexIndex_.reset();
    editingLatexBefore_.reset();
    editingLatexCreatesLatex_ = false;
    if (window_ != nullptr && HasRect(editFrameRect)) {
        RedrawWindow(window_, &editFrameRect, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    ClearSelection();
    if (pageIndex < document_.PageCount() && latexIndex < document_.PageAt(pageIndex).latexes.size()) {
        selectedLatexIndices_[pageIndex].push_back(latexIndex);
    }
    UpdateCaptureState();
    InvalidateCanvas();
}

void App::BeginLatexToolInteraction(POINT cursor) {
    const auto hitLatex = HitTestLatex(cursor);
    if (hitLatex.has_value()) {
        document_.SetCurrentPageIndex(hitLatex->first);
        MarkPdfSidebarCompositionDirty();
        BeginLatexEdit(hitLatex->first, hitLatex->second, document_.PageAt(hitLatex->first).latexes[hitLatex->second]);
        return;
    }

    const auto hitPage = HitTestPage(cursor);
    if (!hitPage.has_value()) {
        ClearSelection();
        InvalidateCanvas();
        return;
    }

    Page& page = document_.PageAt(hitPage->pageIndex);
    PageLatex block {};
    const PageLatex defaultBlock {};
    constexpr float newLatexSizeScale = 0.5f;
    const Vec2 pagePoint = ScreenToPage(*hitPage, cursor, true);
    block.id = AllocateLatexId();
    block.fontSize = std::max(1.0f, defaultBlock.fontSize * newLatexSizeScale);
    block.x = pagePoint.x;
    block.y = pagePoint.y;
    if (page.width > 0.0f) {
        block.x = std::min(block.x, std::max(0.0f, page.width - block.fontSize));
    }
    if (page.height > 0.0f) {
        const float estimatedInitialHeight = std::max(1.0f, block.fontSize * 1.35f);
        if (block.y + estimatedInitialHeight > page.height) {
            block.y = std::max(0.0f, page.height - estimatedInitialHeight);
        }
    }
    block.color = palette_[colorIndex_];
    page.latexes.push_back(block);
    document_.SetCurrentPageIndex(hitPage->pageIndex);
    MarkPdfSidebarCompositionDirty();
    BeginLatexEdit(hitPage->pageIndex, page.latexes.size() - 1U, std::nullopt, true);
}

}  // namespace cnote
