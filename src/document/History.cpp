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
    session_->undoActions.pop_back();
    bool cacheUpdated = false;

    switch (action.type) {
    case HistoryActionType::AddStroke:
    case HistoryActionType::RemoveStroke: {
        auto& strokes = session_->document.PageAt(action.pageIndex).strokes;
        SetViewPageIndex(action.pageIndex);
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
        session_->redoActions.push_back(std::move(action));
        if (!cacheUpdated) {
            InvalidatePageSurfaceCache(session_->redoActions.back().pageIndex);
        }
        NotifyDocumentMutated();
        InvalidateStrokeRegion(session_->redoActions.back().stroke, session_->redoActions.back().pageIndex, false);
        return;
    }
    case HistoryActionType::AddText:
    case HistoryActionType::RemoveText:
    case HistoryActionType::UpdateText: {
        if (action.pageIndex >= session_->document.PageCount()) {
            return;
        }
        Page& page = session_->document.PageAt(action.pageIndex);
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
        session_->redoActions.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ session_->redoActions.back().pageIndex }, session_->redoActions.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddLatex:
    case HistoryActionType::RemoveLatex:
    case HistoryActionType::UpdateLatex: {
        if (action.pageIndex >= session_->document.PageCount()) {
            return;
        }
        Page& page = session_->document.PageAt(action.pageIndex);
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
        session_->redoActions.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ session_->redoActions.back().pageIndex }, session_->redoActions.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddPages: {
        const std::size_t removeCount = std::min(action.pages.size(), session_->document.PageCount() > action.index ? session_->document.PageCount() - action.index : 0U);
        for (std::size_t offset = 0; offset < removeCount; ++offset) {
            RemoveDocumentPage(action.index);
        }
        session_->redoActions.push_back(std::move(action));
        RestoreAfterPageHistoryChange(session_->redoActions.back().previousPageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::RemovePages: {
        std::size_t insertIndex = action.index;
        for (const Page& page : action.pages) {
            InsertDocumentPage(insertIndex++, page);
        }
        session_->redoActions.push_back(std::move(action));
        RestoreAfterPageHistoryChange(session_->redoActions.back().previousPageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::ReplacePages: {
        if (action.pageIndices.size() != action.pages.size()) {
            return;
        }
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            const std::size_t pageIndex = action.pageIndices[index];
            if (pageIndex < session_->document.PageCount()) {
                session_->document.PageAt(pageIndex) = action.pages[index];
            }
        }
        session_->redoActions.push_back(std::move(action));
        RestoreAfterContentHistoryChange(session_->redoActions.back().pageIndices, session_->redoActions.back().focusPageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    default:
        return;
    }
}

void App::Redo() {
    FinishOtherViewInteractions();
    CommitTextEdit();
    CommitLatexEdit();
    if (isDrawing_ || isPanning_ || !CanRedo()) {
        return;
    }

    HistoryAction action = session_->redoActions.back();
    session_->redoActions.pop_back();
    bool cacheUpdated = false;

    switch (action.type) {
    case HistoryActionType::AddStroke:
    case HistoryActionType::RemoveStroke: {
        auto& strokes = session_->document.PageAt(action.pageIndex).strokes;
        SetViewPageIndex(action.pageIndex);
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
        session_->undoActions.push_back(std::move(action));
        if (!cacheUpdated) {
            InvalidatePageSurfaceCache(session_->undoActions.back().pageIndex);
        }
        NotifyDocumentMutated();
        InvalidateStrokeRegion(session_->undoActions.back().stroke, session_->undoActions.back().pageIndex, false);
        return;
    }
    case HistoryActionType::AddText:
    case HistoryActionType::RemoveText:
    case HistoryActionType::UpdateText: {
        if (action.pageIndex >= session_->document.PageCount()) {
            return;
        }
        Page& page = session_->document.PageAt(action.pageIndex);
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
        session_->undoActions.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ session_->undoActions.back().pageIndex }, session_->undoActions.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddLatex:
    case HistoryActionType::RemoveLatex:
    case HistoryActionType::UpdateLatex: {
        if (action.pageIndex >= session_->document.PageCount()) {
            return;
        }
        Page& page = session_->document.PageAt(action.pageIndex);
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
        session_->undoActions.push_back(std::move(action));
        RestoreAfterContentHistoryChange({ session_->undoActions.back().pageIndex }, session_->undoActions.back().pageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    case HistoryActionType::AddPages: {
        std::size_t insertIndex = action.index;
        for (const Page& page : action.pages) {
            InsertDocumentPage(insertIndex++, page);
        }
        session_->undoActions.push_back(std::move(action));
        RestoreAfterPageHistoryChange(session_->undoActions.back().pageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::RemovePages: {
        const std::size_t removeCount = std::min(action.pages.size(), session_->document.PageCount() > action.index ? session_->document.PageCount() - action.index : 0U);
        for (std::size_t offset = 0; offset < removeCount; ++offset) {
            RemoveDocumentPage(action.index);
        }
        session_->undoActions.push_back(std::move(action));
        RestoreAfterPageHistoryChange(session_->undoActions.back().pageIndex);
        NotifyDocumentMutated();
        return;
    }
    case HistoryActionType::ReplacePages: {
        if (action.pageIndices.size() != action.replacementPages.size()) {
            return;
        }
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            const std::size_t pageIndex = action.pageIndices[index];
            if (pageIndex < session_->document.PageCount()) {
                session_->document.PageAt(pageIndex) = action.replacementPages[index];
            }
        }
        session_->undoActions.push_back(std::move(action));
        RestoreAfterContentHistoryChange(session_->undoActions.back().pageIndices, session_->undoActions.back().focusPageIndex);
        NotifyDocumentMutated();
        InvalidateCanvas();
        return;
    }
    default:
        return;
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
    if (session_->views.size() > 1U) return true;
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
