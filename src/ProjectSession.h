#pragma once

#include "Document.h"

namespace cnote {

enum class HistoryActionType {
    AddStroke,
    RemoveStroke,
    AddText,
    RemoveText,
    UpdateText,
    // `pages` inserted as one block at `index`.
    AddPages,
    // `pages` removed from the sorted indices `pageIndices` (pre-removal).
    RemovePages,
    // Only the affected objects of one page change; `beforeObjects`/`afterObjects`
    // hold them.  Used for object gestures (move, transform, recolor, clone,
    // erase) so their history cost is O(affected objects), not O(page).
    ReplaceObjects,
    ReplacePages,
    // Document::ReorderPages with `pageIndices` as the order; undo applies
    // its inverse.
    ReorderPages,
    AddLatex,
    RemoveLatex,
    UpdateLatex,
};

// The objects a ReplaceObjects action touches, grouped by kind.  Indices are
// the objects' positions in the page's vectors when the action was recorded.
// An index present on one side only means the object was inserted (after) or
// removed (before); an index on both sides is an in-place update.  Objects of
// one kind in one action are all of the same sort (updates, insertions or
// removals) and the index lists are sorted ascending.
struct PageObjectSet {
    std::vector<std::size_t> strokeIndices {};
    std::vector<Stroke> strokes {};
    std::vector<std::size_t> imageIndices {};
    std::vector<PageImage> images {};
    std::vector<std::size_t> textIndices {};
    std::vector<PageText> texts {};
    std::vector<std::size_t> latexIndices {};
    std::vector<PageLatex> latexes {};

    [[nodiscard]] bool Empty() const noexcept {
        return strokeIndices.empty() && imageIndices.empty() && textIndices.empty() && latexIndices.empty();
    }
};

struct HistoryAction {
    HistoryActionType type = HistoryActionType::AddStroke;
    Stroke stroke {};
    PageText text {};
    PageText replacementText {};
    PageLatex latex {};
    PageLatex replacementLatex {};
    std::vector<Page> pages {};
    std::vector<Page> replacementPages {};
    // ReplaceObjects only: the page's affected objects before and after.
    PageObjectSet beforeObjects {};
    PageObjectSet afterObjects {};
    std::vector<std::size_t> pageIndices {};
    std::size_t index = 0;
    std::size_t pageIndex = 0;
};

// Committed content and save-state events are distinct from any future transient
// preview stream. Revisions identify document commits, never viewport changes.
struct ProjectChange {
    enum class Kind { Committed, Saved };
    Kind kind = Kind::Committed;
};

// Accessed exclusively on the application's UI thread. All attached views share
// content, history and save ownership; selections and interaction state live in
// the canvas (CanvasWidget/InteractionRouter) and its QtDocumentController, not
// here. The set of attached views is owned by the window/session manager, not
// here.
struct ProjectSession {
    Document document = Document::CreateBlank();
    std::vector<HistoryAction> undoActions;
    std::vector<HistoryAction> redoActions;
    bool dirty = false;
    std::uint64_t revision = 0;
    unsigned int autoSaveIntervalSeconds = 60;
};

} // namespace cnote
