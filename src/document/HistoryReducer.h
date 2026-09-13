#pragma once

#include "ProjectSession.h"

namespace cnote {

// Applies the document-mutation half of one undo/redo step in place. `undo`
// selects the inverse of the recorded action (undo reverses, redo reapplies).
//
// Returns true only when the document actually changed. A false return means
// the action could not be applied (missing page, stale object id or an
// out-of-range index); the document is left untouched and the caller must leave
// both history stacks and the session revision unchanged.
[[nodiscard]] bool ApplyHistoryAction(Document& document, const HistoryAction& action, bool undo);

}  // namespace cnote
