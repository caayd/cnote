#include "App.h"

#include "document/HistoryReducer.h"

#include <algorithm>

#include <commctrl.h>

namespace cnote {

using cnote::PageEqual;
using cnote::PageImageEqual;
using cnote::PageTextEqual;
using cnote::StrokeEqual;
using cnote::StrokeStyleEqual;
using cnote::Vec2Equal;

namespace {

bool IsPageHistoryAction(HistoryActionType type) {
    return type == HistoryActionType::AddPages || type == HistoryActionType::RemovePages;
}

// Pages a sequential removal at a fixed index can actually delete: bounded by
// the request, the pages from `index` to the end, and the one-page minimum.
std::size_t PageRemovalCount(const Document& document, std::size_t index, std::size_t requested) {
    if (document.PageCount() == 0U || index >= document.PageCount()) {
        return 0U;
    }
    return std::min({ requested, document.PageCount() - index, document.PageCount() - 1U });
}

int ShowUnsavedCloseDialog(HWND owner, HINSTANCE instance) {
    using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    static const auto taskDialogIndirect = []() -> TaskDialogIndirectFn {
        HMODULE commonControls = GetModuleHandleW(L"comctl32.dll");
        if (commonControls == nullptr) {
            commonControls = LoadLibraryW(L"comctl32.dll");
        }
        if (commonControls == nullptr) {
            return nullptr;
        }
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
        return reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(commonControls, "TaskDialogIndirect"));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }();

    const TASKDIALOG_BUTTON buttons[] {
        { IDYES, L"Save" },
        { IDNO, L"Don't Save" },
        { IDCANCEL, L"Cancel" },
    };

    TASKDIALOGCONFIG config {};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.hInstance = instance;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"Unsaved Cnote";
    config.pszMainInstruction = L"Save this cnote?";
    config.pszContent = L"This cnote has unsaved edits. Save it before closing?";
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDYES;

    int result = IDCANCEL;
    if (taskDialogIndirect != nullptr && SUCCEEDED(taskDialogIndirect(&config, &result, nullptr, nullptr))) {
        return result;
    }

    return MessageBoxW(
        owner,
        L"This cnote has unsaved changes. Save it before closing?",
        L"Unsaved Cnote",
        MB_YESNOCANCEL | MB_ICONWARNING);
}

}  // namespace

void App::ClearHistory() {
    session_->undoActions.clear();
    session_->redoActions.clear();
}

void App::Undo() {
    FinishOtherViewInteractions();
    CommitTextEdit();
    CommitLatexEdit();
    if (isDrawing_ || isPanning_ || !CanUndo()) {
        return;
    }

    HistoryAction action = session_->undoActions.back();
    const bool pageAction = IsPageHistoryAction(action.type);
    const bool removingPages = action.type == HistoryActionType::AddPages;
    const std::size_t changedPages = !pageAction ? 0U
        : (removingPages ? PageRemovalCount(session_->document, action.index, action.pages.size())
                         : action.pages.size());
    const auto anchors = pageAction ? CaptureViewPageAnchors() : std::vector<std::pair<App*, int>> {};
    if (!ApplyHistoryAction(session_->document, action, true)) {
        return;
    }

    session_->undoActions.pop_back();
    session_->redoActions.push_back(std::move(action));
    ApplyHistorySideEffects(session_->redoActions.back(), true, removingPages, changedPages, anchors);
}

void App::Redo() {
    FinishOtherViewInteractions();
    CommitTextEdit();
    CommitLatexEdit();
    if (isDrawing_ || isPanning_ || !CanRedo()) {
        return;
    }

    HistoryAction action = session_->redoActions.back();
    const bool pageAction = IsPageHistoryAction(action.type);
    const bool removingPages = action.type == HistoryActionType::RemovePages;
    const std::size_t changedPages = !pageAction ? 0U
        : (removingPages ? PageRemovalCount(session_->document, action.index, action.pages.size())
                         : action.pages.size());
    const auto anchors = pageAction ? CaptureViewPageAnchors() : std::vector<std::pair<App*, int>> {};
    if (!ApplyHistoryAction(session_->document, action, false)) {
        return;
    }

    session_->redoActions.pop_back();
    session_->undoActions.push_back(std::move(action));
    ApplyHistorySideEffects(session_->undoActions.back(), false, removingPages, changedPages, anchors);
}

void App::ApplyHistorySideEffects(
    const HistoryAction& action,
    bool undo,
    bool removingPages,
    std::size_t changedPages,
    const std::vector<std::pair<App*, int>>& anchors) {
    switch (action.type) {
    case HistoryActionType::AddStroke:
    case HistoryActionType::RemoveStroke: {
        const bool removed = undo == (action.type == HistoryActionType::AddStroke);
        SetViewPageIndex(action.pageIndex);
        MarkPdfSidebarCompositionDirty();
        bool cacheUpdated = false;
        if (removed) {
            AdjustSelectionForRemoval(action.pageIndex, action.index);
            cacheUpdated = TryRefreshPageSurfaceCacheForStroke(action.pageIndex, action.stroke);
        } else {
            AdjustSelectionForInsertion(action.pageIndex, action.index);
            const auto& strokes = session_->document.PageAt(action.pageIndex).strokes;
            if (action.index + 1U == strokes.size()) {
                cacheUpdated = TryAppendStrokeToPageSurfaceCache(action.pageIndex, action.stroke);
            }
        }
        if (!cacheUpdated) {
            InvalidatePageSurfaceCache(action.pageIndex);
        }
        NotifyDocumentMutated();
        InvalidateStrokeRegion(action.stroke, action.pageIndex, false);
        return;
    }
    case HistoryActionType::AddText:
    case HistoryActionType::RemoveText:
    case HistoryActionType::UpdateText: {
        RestoreAfterContentHistoryChange({ action.pageIndex }, action.pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddLatex:
    case HistoryActionType::RemoveLatex:
    case HistoryActionType::UpdateLatex: {
        RestoreAfterContentHistoryChange({ action.pageIndex }, action.pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddPages:
    case HistoryActionType::RemovePages: {
        if (removingPages) {
            RemapViewsForPageRemovals(action.index, changedPages);
        } else {
            RemapViewsForPageInsertions(action.index, changedPages);
        }
        RestoreViewPageAnchors(anchors);
        RestoreAfterPageHistoryChange(undo ? action.previousPageIndex : action.pageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::ReplacePages: {
        RestoreAfterContentHistoryChange(action.pageIndices, action.focusPageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    }
}

void App::RemapViewsForPageInsertions(std::size_t index, std::size_t count) {
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t inserted = index + offset;
        for (App* view : SessionViews()) {
            if (view->currentPageIndex_ >= inserted) {
                ++view->currentPageIndex_;
            }
        }
    }
}

void App::RemapViewsForPageRemovals(std::size_t index, std::size_t count) {
    for (std::size_t offset = 0; offset < count; ++offset) {
        for (App* view : SessionViews()) {
            if (view->currentPageIndex_ > index) {
                --view->currentPageIndex_;
            }
            view->currentPageIndex_ = view->CurrentPageIndex();
        }
    }
}

bool App::CanUndo() const {
    return !session_->undoActions.empty();
}

bool App::CanRedo() const {
    return !session_->redoActions.empty();
}

bool App::CanGoToPreviousPage() const {
    return CurrentPageIndex() > 0U;
}

bool App::CanGoToNextPage() const {
    return CurrentPageIndex() + 1U < session_->document.PageCount();
}

void App::NotifyDocumentMutated() {
    session_->dirty = true;
    ++session_->revision;
    BroadcastSessionChange(ProjectChange::Kind::Committed);
}

void App::MarkDocumentSaved() {
    session_->dirty = false;
    BroadcastSessionChange(ProjectChange::Kind::Saved);
}

void App::UpdateAutoSaveTimer() {
    if (window_ == nullptr) {
        return;
    }

    KillTimer(window_, kAutoSaveTimerId);
    if (AutoSaveOwner() != this || !session_->dirty || !HasSavedCnotePath() || session_->autoSaveIntervalSeconds == 0U) {
        return;
    }

    SetTimer(window_, kAutoSaveTimerId, session_->autoSaveIntervalSeconds * 1000U, nullptr);
}

bool App::HasSavedCnotePath() const {
    return HasExtension(session_->document.FilePath(), kCnoteFileExtension);
}

bool App::CanAutoSaveCurrentDocument() const {
    return AutoSaveOwner() == this && session_->dirty && HasSavedCnotePath();
}

bool App::IsDocumentInteractionActive() const {
    return isDrawing_
        || isErasing_
        || textEditWindow_ != nullptr
        || pendingTextEditClick_
        || isMovingSelection_
        || isTransformingImage_
        || isTransformingText_
        || isTransformingStroke_;
}

bool App::ConfirmCloseWithUnsavedChanges() {
    FinishDocumentInteraction();
    if (SessionViews().size() > 1U) return true;
    if (!session_->dirty) {
        return true;
    }
    if (HasSavedCnotePath()) {
        return SaveCnoteToPath(session_->document.FilePath(), true);
    }

    const int result = ShowUnsavedCloseDialog(window_, instance_);

    if (result == IDYES) {
        return SaveCurrentCnote();
    }
    return result == IDNO;
}

void App::RestoreAfterPageHistoryChange(std::size_t focusPageIndex) {
    ClearSelection();
    pageSurfaceCaches_.assign(session_->document.PageCount(), {});
    sidebarThumbnailCaches_.assign(session_->document.PageCount(), {});
    SetViewPageIndex(std::min(focusPageIndex, session_->document.PageCount() - 1U));
    MarkPdfSidebarCompositionDirty();
    EnsureSidebarCurrentPageVisible();
    FocusPage(std::min(focusPageIndex, session_->document.PageCount() - 1U));
}

void App::RestoreAfterContentHistoryChange(const std::vector<std::size_t>& pageIndices, std::size_t focusPageIndex) {
    ClearSelection();
    for (std::size_t pageIndex : pageIndices) {
        if (pageIndex >= session_->document.PageCount()) {
            continue;
        }
        InvalidatePageSurfaceCache(pageIndex);
        InvalidateSidebarThumbnailCache(pageIndex);
    }

    SetViewPageIndex(std::min(focusPageIndex, session_->document.PageCount() - 1U));
    MarkPdfSidebarCompositionDirty();
    EnsureSidebarCurrentPageVisible();
}

void App::PushPageHistoryAction(
    HistoryActionType type,
    std::size_t index,
    std::vector<Page> pages,
    std::size_t focusPageIndex,
    std::size_t previousPageIndex) {
    HistoryAction action {};
    action.type = type;
    action.pages = std::move(pages);
    action.index = index;
    action.pageIndex = focusPageIndex;
    action.focusPageIndex = focusPageIndex;
    action.previousPageIndex = previousPageIndex;
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
}

void App::PushReplacePagesHistoryAction(std::vector<std::size_t> pageIndices, std::vector<Page> beforePages, std::size_t focusPageIndex) {
    if (pageIndices.size() != beforePages.size() || pageIndices.empty()) {
        return;
    }

    std::vector<Page> afterPages;
    afterPages.reserve(pageIndices.size());
    bool changed = false;
    for (std::size_t index = 0; index < pageIndices.size(); ++index) {
        const std::size_t pageIndex = pageIndices[index];
        if (pageIndex >= session_->document.PageCount()) {
            return;
        }
        afterPages.push_back(session_->document.PageAt(pageIndex));
        changed = changed || !PageEqual(beforePages[index], afterPages.back());
    }

    if (!changed) {
        return;
    }

    HistoryAction action {};
    action.type = HistoryActionType::ReplacePages;
    action.pages = std::move(beforePages);
    action.replacementPages = std::move(afterPages);
    action.pageIndices = std::move(pageIndices);
    action.focusPageIndex = focusPageIndex;
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
}

void App::PushTextHistoryAction(
    HistoryActionType type,
    std::size_t pageIndex,
    std::size_t index,
    PageText text,
    PageText replacementText) {
    if (type != HistoryActionType::AddText
        && type != HistoryActionType::RemoveText
        && type != HistoryActionType::UpdateText) {
        return;
    }

    HistoryAction action {};
    action.type = type;
    action.pageIndex = pageIndex;
    action.focusPageIndex = pageIndex;
    action.index = index;
    action.text = std::move(text);
    action.replacementText = std::move(replacementText);
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
}

void App::PushLatexHistoryAction(
    HistoryActionType type,
    std::size_t pageIndex,
    std::size_t index,
    PageLatex latex,
    PageLatex replacementLatex) {
    if (type != HistoryActionType::AddLatex
        && type != HistoryActionType::RemoveLatex
        && type != HistoryActionType::UpdateLatex) {
        return;
    }

    HistoryAction action {};
    action.type = type;
    action.pageIndex = pageIndex;
    action.focusPageIndex = pageIndex;
    action.index = index;
    action.latex = std::move(latex);
    action.replacementLatex = std::move(replacementLatex);
    session_->undoActions.push_back(std::move(action));
    session_->redoActions.clear();
}

}  // namespace cnote
