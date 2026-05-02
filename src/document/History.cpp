#include "App.h"

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
    undoActions_.clear();
    redoActions_.clear();
}

void App::Undo() {
    if (isDrawing_ || isPanning_ || !CanUndo()) {
        return;
    }

    HistoryAction action = undoActions_.back();
    undoActions_.pop_back();
    bool cacheUpdated = false;

    switch (action.type) {
    case HistoryActionType::AddStroke:
    case HistoryActionType::RemoveStroke: {
        auto& strokes = document_.PageAt(action.pageIndex).strokes;
        document_.SetCurrentPageIndex(action.pageIndex);
        MarkPdfSidebarCompositionDirty();
        if (action.type == HistoryActionType::AddStroke) {
            if (action.index < strokes.size()) {
                strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
                AdjustSelectionForRemoval(action.pageIndex, action.index);
                cacheUpdated = TryRefreshPageSurfaceCacheForStroke(action.pageIndex, action.stroke);
            }
        } else if (action.index <= strokes.size()) {
            AdjustSelectionForInsertion(action.pageIndex, action.index);
            strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
            if (action.index + 1U == strokes.size()) {
                cacheUpdated = TryAppendStrokeToPageSurfaceCache(action.pageIndex, action.stroke);
            }
        }
        redoActions_.push_back(std::move(action));
        if (!cacheUpdated) {
            InvalidatePageSurfaceCache(redoActions_.back().pageIndex);
        }
        NotifyDocumentMutated();
        InvalidateStrokeRegion(redoActions_.back().stroke, redoActions_.back().pageIndex, false);
        return;
    }
    case HistoryActionType::AddPages: {
        const std::size_t removeCount = std::min(action.pages.size(), document_.PageCount() > action.index ? document_.PageCount() - action.index : 0U);
        for (std::size_t offset = 0; offset < removeCount; ++offset) {
            document_.RemovePage(action.index);
        }
        redoActions_.push_back(std::move(action));
        RestoreAfterPageHistoryChange(redoActions_.back().previousPageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::RemovePages: {
        std::size_t insertIndex = action.index;
        for (const Page& page : action.pages) {
            document_.InsertPage(insertIndex++, page);
        }
        redoActions_.push_back(std::move(action));
        RestoreAfterPageHistoryChange(redoActions_.back().previousPageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::ReplacePages: {
        if (action.pageIndices.size() != action.pages.size()) {
            return;
        }
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            const std::size_t pageIndex = action.pageIndices[index];
            if (pageIndex < document_.PageCount()) {
                document_.PageAt(pageIndex) = action.pages[index];
            }
        }
        redoActions_.push_back(std::move(action));
        RestoreAfterContentHistoryChange(redoActions_.back().pageIndices, redoActions_.back().focusPageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    default:
        return;
    }
}

void App::Redo() {
    if (isDrawing_ || isPanning_ || !CanRedo()) {
        return;
    }

    HistoryAction action = redoActions_.back();
    redoActions_.pop_back();
    bool cacheUpdated = false;

    switch (action.type) {
    case HistoryActionType::AddStroke:
    case HistoryActionType::RemoveStroke: {
        auto& strokes = document_.PageAt(action.pageIndex).strokes;
        document_.SetCurrentPageIndex(action.pageIndex);
        MarkPdfSidebarCompositionDirty();
        if (action.type == HistoryActionType::AddStroke) {
            if (action.index <= strokes.size()) {
                AdjustSelectionForInsertion(action.pageIndex, action.index);
                strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
                if (action.index + 1U == strokes.size()) {
                    cacheUpdated = TryAppendStrokeToPageSurfaceCache(action.pageIndex, action.stroke);
                }
            }
        } else if (action.index < strokes.size()) {
            strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
            AdjustSelectionForRemoval(action.pageIndex, action.index);
            cacheUpdated = TryRefreshPageSurfaceCacheForStroke(action.pageIndex, action.stroke);
        }
        undoActions_.push_back(std::move(action));
        if (!cacheUpdated) {
            InvalidatePageSurfaceCache(undoActions_.back().pageIndex);
        }
        NotifyDocumentMutated();
        InvalidateStrokeRegion(undoActions_.back().stroke, undoActions_.back().pageIndex, false);
        return;
    }
    case HistoryActionType::AddPages: {
        std::size_t insertIndex = action.index;
        for (const Page& page : action.pages) {
            document_.InsertPage(insertIndex++, page);
        }
        undoActions_.push_back(std::move(action));
        RestoreAfterPageHistoryChange(undoActions_.back().pageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::RemovePages: {
        const std::size_t removeCount = std::min(action.pages.size(), document_.PageCount() > action.index ? document_.PageCount() - action.index : 0U);
        for (std::size_t offset = 0; offset < removeCount; ++offset) {
            document_.RemovePage(action.index);
        }
        undoActions_.push_back(std::move(action));
        RestoreAfterPageHistoryChange(undoActions_.back().pageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::ReplacePages: {
        if (action.pageIndices.size() != action.replacementPages.size()) {
            return;
        }
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            const std::size_t pageIndex = action.pageIndices[index];
            if (pageIndex < document_.PageCount()) {
                document_.PageAt(pageIndex) = action.replacementPages[index];
            }
        }
        undoActions_.push_back(std::move(action));
        RestoreAfterContentHistoryChange(undoActions_.back().pageIndices, undoActions_.back().focusPageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    default:
        return;
    }
}

bool App::CanUndo() const {
    return !undoActions_.empty();
}

bool App::CanRedo() const {
    return !redoActions_.empty();
}

bool App::CanGoToPreviousPage() const {
    return document_.CurrentPageIndex() > 0U;
}

bool App::CanGoToNextPage() const {
    return document_.CurrentPageIndex() + 1U < document_.PageCount();
}

void App::NotifyDocumentMutated() {
    const bool wasDirty = documentDirty_;
    documentDirty_ = true;
    UpdateAutoSaveTimer();
    if (!wasDirty) {
        UpdateWindowTitle();
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void App::MarkDocumentSaved() {
    documentDirty_ = false;
    UpdateAutoSaveTimer();
    UpdateWindowTitle();
}

void App::UpdateAutoSaveTimer() {
    if (window_ == nullptr) {
        return;
    }

    KillTimer(window_, kAutoSaveTimerId);
    if (!documentDirty_ || !HasSavedCnotePath() || settings_.autoSaveIntervalSeconds == 0U) {
        return;
    }

    SetTimer(window_, kAutoSaveTimerId, settings_.autoSaveIntervalSeconds * 1000U, nullptr);
}

bool App::HasSavedCnotePath() const {
    return document_.FilePath().ends_with(kCnoteFileExtension);
}

bool App::CanAutoSaveCurrentDocument() const {
    return documentDirty_ && HasSavedCnotePath();
}

bool App::IsDocumentInteractionActive() const {
    return isDrawing_ || isErasing_ || pendingTextEditClick_ || isMovingSelection_ || isTransformingImage_ || isTransformingText_ || isTransformingStroke_;
}

bool App::ConfirmCloseWithUnsavedChanges() {
    CommitTextEdit();
    if (!documentDirty_ || HasSavedCnotePath()) {
        return true;
    }

    const int result = ShowUnsavedCloseDialog(window_, instance_);

    if (result == IDYES) {
        return SaveCurrentCnote();
    }
    return result == IDNO;
}

void App::RestoreAfterPageHistoryChange(std::size_t focusPageIndex) {
    ClearSelection();
    pageSurfaceCaches_.assign(document_.PageCount(), {});
    sidebarThumbnailCaches_.assign(document_.PageCount(), {});
    document_.SetCurrentPageIndex(std::min(focusPageIndex, document_.PageCount() - 1U));
    MarkPdfSidebarCompositionDirty();
    EnsureSidebarCurrentPageVisible();
    FocusPage(std::min(focusPageIndex, document_.PageCount() - 1U));
}

void App::RestoreAfterContentHistoryChange(const std::vector<std::size_t>& pageIndices, std::size_t focusPageIndex) {
    ClearSelection();
    for (std::size_t pageIndex : pageIndices) {
        if (pageIndex >= document_.PageCount()) {
            continue;
        }
        InvalidatePageSurfaceCache(pageIndex);
        InvalidateSidebarThumbnailCache(pageIndex);
    }

    document_.SetCurrentPageIndex(std::min(focusPageIndex, document_.PageCount() - 1U));
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
    undoActions_.push_back(std::move(action));
    redoActions_.clear();
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
        if (pageIndex >= document_.PageCount()) {
            return;
        }
        afterPages.push_back(document_.PageAt(pageIndex));
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
    undoActions_.push_back(std::move(action));
    redoActions_.clear();
}

}  // namespace cnote
