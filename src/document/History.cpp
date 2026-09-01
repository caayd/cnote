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

std::optional<std::size_t> FindTextIndexById(const Page& page, std::uint64_t textId) {
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        if (page.texts[index].id == textId) {
            return index;
        }
    }
    return std::nullopt;
}
std::optional<std::size_t> FindLatexIndexById(const Page& page, std::uint64_t latexId) {
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (page.latexes[index].id == latexId) {
            return index;
        }
    }
    return std::nullopt;
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
    case HistoryActionType::AddText:
    case HistoryActionType::RemoveText:
    case HistoryActionType::UpdateText: {
        if (action.pageIndex >= document_.PageCount()) {
            return;
        }
        Page& page = document_.PageAt(action.pageIndex);
        if (action.type == HistoryActionType::AddText) {
            if (const auto textIndex = FindTextIndexById(page, action.text.id); textIndex.has_value()) {
                page.texts.erase(page.texts.begin() + static_cast<std::ptrdiff_t>(*textIndex));
            }
        } else if (action.type == HistoryActionType::RemoveText) {
            const std::size_t insertIndex = std::min(action.index, page.texts.size());
            page.texts.insert(page.texts.begin() + static_cast<std::ptrdiff_t>(insertIndex), action.text);
        } else if (const auto textIndex = FindTextIndexById(page, action.replacementText.id); textIndex.has_value()) {
            page.texts[*textIndex] = action.text;
        }
        redoActions_.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ redoActions_.back().pageIndex }, redoActions_.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddLatex:
    case HistoryActionType::RemoveLatex:
    case HistoryActionType::UpdateLatex: {
        if (action.pageIndex >= document_.PageCount()) {
            return;
        }
        Page& page = document_.PageAt(action.pageIndex);
        if (action.type == HistoryActionType::AddLatex) {
            if (const auto latexIndex = FindLatexIndexById(page, action.latex.id); latexIndex.has_value()) {
                page.latexes.erase(page.latexes.begin() + static_cast<std::ptrdiff_t>(*latexIndex));
            }
        } else if (action.type == HistoryActionType::RemoveLatex) {
            const std::size_t insertIndex = std::min(action.index, page.latexes.size());
            page.latexes.insert(page.latexes.begin() + static_cast<std::ptrdiff_t>(insertIndex), action.latex);
        } else if (const auto latexIndex = FindLatexIndexById(page, action.replacementLatex.id); latexIndex.has_value()) {
            page.latexes[*latexIndex] = action.latex;
        }
        redoActions_.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ redoActions_.back().pageIndex }, redoActions_.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
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
    case HistoryActionType::AddText:
    case HistoryActionType::RemoveText:
    case HistoryActionType::UpdateText: {
        if (action.pageIndex >= document_.PageCount()) {
            return;
        }
        Page& page = document_.PageAt(action.pageIndex);
        if (action.type == HistoryActionType::AddText) {
            const std::size_t insertIndex = std::min(action.index, page.texts.size());
            page.texts.insert(page.texts.begin() + static_cast<std::ptrdiff_t>(insertIndex), action.text);
        } else if (action.type == HistoryActionType::RemoveText) {
            if (const auto textIndex = FindTextIndexById(page, action.text.id); textIndex.has_value()) {
                page.texts.erase(page.texts.begin() + static_cast<std::ptrdiff_t>(*textIndex));
            }
        } else if (const auto textIndex = FindTextIndexById(page, action.text.id); textIndex.has_value()) {
            page.texts[*textIndex] = action.replacementText;
        }
        undoActions_.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ undoActions_.back().pageIndex }, undoActions_.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddLatex:
    case HistoryActionType::RemoveLatex:
    case HistoryActionType::UpdateLatex: {
        if (action.pageIndex >= document_.PageCount()) {
            return;
        }
        Page& page = document_.PageAt(action.pageIndex);
        if (action.type == HistoryActionType::AddLatex) {
            const std::size_t insertIndex = std::min(action.index, page.latexes.size());
            page.latexes.insert(page.latexes.begin() + static_cast<std::ptrdiff_t>(insertIndex), action.latex);
        } else if (action.type == HistoryActionType::RemoveLatex) {
            if (const auto latexIndex = FindLatexIndexById(page, action.latex.id); latexIndex.has_value()) {
                page.latexes.erase(page.latexes.begin() + static_cast<std::ptrdiff_t>(*latexIndex));
            }
        } else if (const auto latexIndex = FindLatexIndexById(page, action.latex.id); latexIndex.has_value()) {
            page.latexes[*latexIndex] = action.replacementLatex;
        }
        undoActions_.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ undoActions_.back().pageIndex }, undoActions_.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
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
    CommitTextEdit();
    CommitLatexEdit();
    if (!documentDirty_) {
        return true;
    }
    if (HasSavedCnotePath()) {
        return SaveCnoteToPath(document_.FilePath(), true);
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
    undoActions_.push_back(std::move(action));
    redoActions_.clear();
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
    undoActions_.push_back(std::move(action));
    redoActions_.clear();
}

}  // namespace cnote
