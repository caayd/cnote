#pragma once

#include "Document.h"

namespace cnote {

enum class HistoryActionType {
    AddStroke,
    RemoveStroke,
    AddText,
    RemoveText,
    UpdateText,
    AddPages,
    RemovePages,
    ReplacePages,
    AddLatex,
    RemoveLatex,
    UpdateLatex,
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
