#include "document/HistoryReducer.h"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <vector>

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

// Where the pages of an AddPages/RemovePages action sit in the document that
// holds them, ascending: AddPages inserted one block at `index`, RemovePages
// records each removed page's index (`pageIndices`).
std::vector<std::size_t> PagePositions(const HistoryAction& action) {
    if (action.type == HistoryActionType::RemovePages) {
        return action.pageIndices;
    }
    std::vector<std::size_t> positions(action.pages.size());
    std::iota(positions.begin(), positions.end(), action.index);
    return positions;
}

bool SortedUniqueIndices(const std::vector<std::size_t>& indices) {
    return std::is_sorted(indices.begin(), indices.end())
        && std::adjacent_find(indices.begin(), indices.end()) == indices.end();
}

// Moves one kind's vector from the `from` side to the `to` side of an object
// delta. Indices present on one side only are inserted (to) or removed (from);
// indices on both sides are updated in place through `locate`, which finds the
// object that `from` identifies (an index for strokes/images, an id for
// text/LaTeX). `apply == false` only validates; the action's contract requires
// a failure to leave the page untouched, so every kind is validated before any
// kind is mutated.
template <typename T, typename Locate>
bool ApplyObjectKind(std::vector<T>& objects,
    const std::vector<std::size_t>& fromIndices,
    const std::vector<T>& fromObjects,
    const std::vector<std::size_t>& toIndices,
    const std::vector<T>& toObjects,
    Locate&& locate,
    bool apply) {
    if (fromIndices.size() != fromObjects.size() || toIndices.size() != toObjects.size()
        || !SortedUniqueIndices(fromIndices) || !SortedUniqueIndices(toIndices)) {
        return false;
    }

    std::vector<std::size_t> removals;
    std::vector<std::size_t> insertions;
    std::vector<std::size_t> updateFrom;
    std::vector<std::size_t> updateTo;
    std::size_t from = 0;
    std::size_t to = 0;
    while (from < fromIndices.size() || to < toIndices.size()) {
        if (to == toIndices.size()
            || (from < fromIndices.size() && fromIndices[from] < toIndices[to])) {
            removals.push_back(fromIndices[from]);
            ++from;
        } else if (from == fromIndices.size() || toIndices[to] < fromIndices[from]) {
            insertions.push_back(toIndices[to]);
            ++to;
        } else {
            updateFrom.push_back(from);
            updateTo.push_back(to);
            ++from;
            ++to;
        }
    }
    // A gesture either updates existing objects, inserts new ones (`after`
    // only) or removes them (`before` only); the two shapes never mix.
    if (!updateFrom.empty() && (!removals.empty() || !insertions.empty())) {
        return false;
    }
    if (!removals.empty() && !insertions.empty()) {
        return false;
    }

    for (const std::size_t index : removals) {
        if (index >= objects.size()) {
            return false;
        }
    }
    std::size_t size = objects.size();
    for (const std::size_t index : insertions) {
        if (index > size) {
            return false;
        }
        ++size;
    }
    std::vector<std::size_t> updateIndices;
    updateIndices.reserve(updateFrom.size());
    for (std::size_t update = 0; update < updateFrom.size(); ++update) {
        const std::optional<std::size_t> location = locate(
            fromIndices[updateFrom[update]], fromObjects[updateFrom[update]]);
        if (!location.has_value() || *location >= objects.size()) {
            return false;
        }
        updateIndices.push_back(*location);
    }
    if (!apply) {
        return true;
    }

    for (auto index = removals.rbegin(); index != removals.rend(); ++index) {
        objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(*index));
    }
    for (const std::size_t index : insertions) {
        const auto position = std::lower_bound(toIndices.begin(), toIndices.end(), index);
        const std::size_t object = static_cast<std::size_t>(position - toIndices.begin());
        objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index), toObjects[object]);
    }
    for (std::size_t update = 0; update < updateFrom.size(); ++update) {
        objects[updateIndices[update]] = toObjects[updateTo[update]];
    }
    return true;
}

}  // namespace

bool ApplyPageObjectDelta(Page& page, const PageObjectSet& before, const PageObjectSet& after, bool undo, bool apply) {
    const PageObjectSet& from = undo ? after : before;
    const PageObjectSet& to = undo ? before : after;
    const auto byIndex = [](std::size_t index, const auto&) {
        return std::optional<std::size_t>(index);
    };
    const auto textById = [&page](std::size_t, const PageText& object) {
        return FindTextIndexById(page, object.id);
    };
    const auto latexById = [&page](std::size_t, const PageLatex& object) {
        return FindLatexIndexById(page, object.id);
    };

    const bool fits =
        ApplyObjectKind(page.strokes, from.strokeIndices, from.strokes,
            to.strokeIndices, to.strokes, byIndex, false)
        && ApplyObjectKind(page.images, from.imageIndices, from.images,
            to.imageIndices, to.images, byIndex, false)
        && ApplyObjectKind(page.texts, from.textIndices, from.texts,
            to.textIndices, to.texts, textById, false)
        && ApplyObjectKind(page.latexes, from.latexIndices, from.latexes,
            to.latexIndices, to.latexes, latexById, false);
    if (!fits || !apply) {
        return fits;
    }

    ApplyObjectKind(page.strokes, from.strokeIndices, from.strokes,
        to.strokeIndices, to.strokes, byIndex, true);
    ApplyObjectKind(page.images, from.imageIndices, from.images,
        to.imageIndices, to.images, byIndex, true);
    ApplyObjectKind(page.texts, from.textIndices, from.texts,
        to.textIndices, to.texts, textById, true);
    ApplyObjectKind(page.latexes, from.latexIndices, from.latexes,
        to.latexIndices, to.latexes, latexById, true);
    return true;
}

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
        // opposite direction reinserts the snapshots at their recorded indices.
        // Both validate first, so a failed step leaves the document untouched
        // (and always keeps at least one page).
        const std::vector<std::size_t> positions = PagePositions(action);
        if (positions.empty() || positions.size() != action.pages.size()
            || !std::is_sorted(positions.begin(), positions.end())
            || std::adjacent_find(positions.begin(), positions.end()) != positions.end()) {
            return false;
        }
        const bool remove = undo == (action.type == HistoryActionType::AddPages);
        if (remove) {
            if (positions.back() >= document.PageCount() || positions.size() >= document.PageCount()) {
                return false;
            }
            for (auto position = positions.rbegin(); position != positions.rend(); ++position) {
                document.RemovePage(*position);
            }
            return true;
        }
        // Ascending inserts: each index counts the pages inserted before it.
        for (std::size_t offset = 0; offset < positions.size(); ++offset) {
            if (positions[offset] > document.PageCount() + offset) {
                return false;
            }
        }
        for (std::size_t offset = 0; offset < positions.size(); ++offset) {
            document.InsertPage(positions[offset], action.pages[offset]);
        }
        return true;
    }
    case HistoryActionType::ReplaceObjects: {
        if (action.pageIndices.empty()
            || action.pageIndices.size() != action.beforeObjects.size()
            || action.pageIndices.size() != action.afterObjects.size()) {
            return false;
        }
        for (const std::size_t pageIndex : action.pageIndices) {
            if (pageIndex >= document.PageCount()) {
                return false;
            }
        }
        // Validate every page before touching any of them, so a stale delta
        // leaves the whole document unchanged.
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            if (!ApplyPageObjectDelta(document.PageAt(action.pageIndices[index]),
                    action.beforeObjects[index], action.afterObjects[index], undo, false)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            Page& page = document.PageAt(action.pageIndices[index]);
            static_cast<void>(ApplyPageObjectDelta(
                page, action.beforeObjects[index], action.afterObjects[index], undo, true));
            document.TouchPage(action.pageIndices[index]);
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
    case HistoryActionType::ReorderPages: {
        if (!undo) {
            return document.ReorderPages(action.pageIndices);
        }
        // order[new] = old, so the inverse puts each page back at its old index.
        std::vector<std::size_t> inverse(action.pageIndices.size(), 0U);
        for (std::size_t index = 0; index < action.pageIndices.size(); ++index) {
            if (action.pageIndices[index] >= inverse.size()) {
                return false;
            }
            inverse[action.pageIndices[index]] = index;
        }
        return document.ReorderPages(inverse);
    }
    }
    return false;
}

}  // namespace cnote
