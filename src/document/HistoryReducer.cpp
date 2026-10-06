#include "document/HistoryReducer.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace cnote {
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

// Number of pages a sequential removal at a fixed index can actually delete:
// bounded by the request, by the pages from `index` to the end, and by the
// one-page minimum Document::RemovePage enforces.
std::size_t PageRemovalCount(const Document& document, std::size_t index, std::size_t requested) {
    if (document.PageCount() == 0U || index >= document.PageCount()) {
        return 0U;
    }
    return std::min({ requested, document.PageCount() - index, document.PageCount() - 1U });
}

}  // namespace

bool ApplyHistoryAction(Document& document, const HistoryAction& action, bool undo) {
    switch (action.type) {
    // Object-mutating cases bump the page's content epoch so renderer caches
    // (page surfaces, sidebar thumbnails) invalidate on undo/redo.
    case HistoryActionType::AddStroke:
    case HistoryActionType::RemoveStroke: {
        if (action.pageIndex >= document.PageCount()) {
            return false;
        }
        auto& strokes = document.PageAt(action.pageIndex).strokes;
        // Undo AddStroke / redo RemoveStroke remove the committed stroke; the
        // opposite direction reinserts it at its recorded index.
        document.TouchPage(action.pageIndex);
        const bool remove = undo == (action.type == HistoryActionType::AddStroke);
        if (remove) {
            if (action.index >= strokes.size()) {
                return false;
            }
            strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(action.index));
        } else {
            if (action.index > strokes.size()) {
                return false;
            }
            strokes.insert(strokes.begin() + static_cast<std::ptrdiff_t>(action.index), action.stroke);
        }
        return true;
    }
    case HistoryActionType::AddText:
    case HistoryActionType::RemoveText:
    case HistoryActionType::UpdateText: {
        if (action.pageIndex >= document.PageCount()) {
            return false;
        }
        Page& page = document.PageAt(action.pageIndex);
        document.TouchPage(action.pageIndex);
        if (action.type == HistoryActionType::UpdateText) {
            // Undo restores the previous text, redo reapplies the replacement;
            // the object is located by the id that was current in that state.
            const std::uint64_t fromId = undo ? action.replacementText.id : action.text.id;
            const auto textIndex = FindTextIndexById(page, fromId);
            if (!textIndex.has_value()) {
                return false;
            }
            page.texts[*textIndex] = undo ? action.text : action.replacementText;
            return true;
        }
        const bool insert = undo == (action.type == HistoryActionType::RemoveText);
        if (insert) {
            if (action.index > page.texts.size()) {
                return false;
            }
            page.texts.insert(page.texts.begin() + static_cast<std::ptrdiff_t>(action.index), action.text);
            return true;
        }
        const auto textIndex = FindTextIndexById(page, action.text.id);
        if (!textIndex.has_value()) {
            return false;
        }
        page.texts.erase(page.texts.begin() + static_cast<std::ptrdiff_t>(*textIndex));
        return true;
    }
    case HistoryActionType::AddLatex:
    case HistoryActionType::RemoveLatex:
    case HistoryActionType::UpdateLatex: {
        if (action.pageIndex >= document.PageCount()) {
            return false;
        }
        Page& page = document.PageAt(action.pageIndex);
        document.TouchPage(action.pageIndex);
        if (action.type == HistoryActionType::UpdateLatex) {
            const std::uint64_t fromId = undo ? action.replacementLatex.id : action.latex.id;
            const auto latexIndex = FindLatexIndexById(page, fromId);
            if (!latexIndex.has_value()) {
                return false;
            }
            page.latexes[*latexIndex] = undo ? action.latex : action.replacementLatex;
            return true;
        }
        const bool insert = undo == (action.type == HistoryActionType::RemoveLatex);
        if (insert) {
            if (action.index > page.latexes.size()) {
                return false;
            }
            page.latexes.insert(page.latexes.begin() + static_cast<std::ptrdiff_t>(action.index), action.latex);
            return true;
        }
        const auto latexIndex = FindLatexIndexById(page, action.latex.id);
        if (!latexIndex.has_value()) {
            return false;
        }
        page.latexes.erase(page.latexes.begin() + static_cast<std::ptrdiff_t>(*latexIndex));
        return true;
    }
    case HistoryActionType::AddPages:
    case HistoryActionType::RemovePages: {
        // Undo AddPages / redo RemovePages delete the recorded pages; the
        // opposite direction reinserts the snapshot at its recorded index.
        const bool remove = undo == (action.type == HistoryActionType::AddPages);
        if (remove) {
            const std::size_t count = PageRemovalCount(document, action.index, action.pages.size());
            if (count == 0U) {
                return false;
            }
            for (std::size_t offset = 0; offset < count; ++offset) {
                document.RemovePage(action.index);
            }
            return true;
        }
        if (action.pages.empty() || action.index > document.PageCount()) {
            return false;
        }
        std::size_t insertIndex = action.index;
        for (const Page& page : action.pages) {
            document.InsertPage(insertIndex++, page);
        }
        return true;
    }
    case HistoryActionType::ReplacePages: {
        const std::vector<Page>& targetPages = undo ? action.pages : action.replacementPages;
        if (action.pageIndices.empty() || action.pageIndices.size() != targetPages.size()) {
            return false;
        }
        for (const std::size_t pageIndex : action.pageIndices) {
            if (pageIndex >= document.PageCount()) {
                return false;
            }
        }
        for (const std::size_t pageIndex : action.pageIndices) {
            document.TouchPage(pageIndex);
        }
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            document.PageAt(action.pageIndices[index]) = targetPages[index];
        }
        return true;
    }
    }
    return false;
}

}  // namespace cnote
