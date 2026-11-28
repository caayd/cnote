#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Document.h"
#include "ui/CanvasController.h"
#include "ui/SelectionMath.h"

namespace cnote::ui {

// One selected object's page-space outline (four corners, in order) and the
// axis-aligned union the selection frame is built from.  The canvas needs both
// for damage, handle hit tests and the overlay paint, so they are measured
// once per page state instead of once per consumer.
struct SelectionOutlines {
    std::vector<std::array<Vec2, 4>> outlines {};
    std::optional<SelectionBounds> bounds {};
};

// Per-document cache of the selected objects' outlines, keyed by the page
// epoch, the session-wide transient generation and the selected index sets.
//
// Damage, hover and paint used to measure every selected stroke, text and
// LaTeX block independently: a move or transform sample recomputed the
// outlines for the old frame, the new frame and the paint, and every pointer
// move recomputed them again for the handle hit test.  The keys cover every
// mutation the geometry depends on:
//   * a committed edit bumps Document::PageEpoch;
//   * an in-place gesture edit (move/transform/clone/cancel) bumps
//     ProjectSession::transientGeneration before it commits;
//   * selecting or deselecting changes the index sets.
// Entries are per page and O(selected objects); they are replaced in place, so
// the cache cannot grow with the number of samples.
class SelectionGeometryCache {
public:
    // Outlines for `selection` on `pageIndex`, measured when the page epoch,
    // the transient generation or the selected objects changed.  An empty
    // selection is answered without touching the cache.
    [[nodiscard]] const SelectionOutlines& ForPage(const Document& document, std::size_t pageIndex,
        const PageSelection& selection, std::uint64_t transientGeneration) const;

    // Test/diagnostic counter: how many times a page's selection was measured.
    [[nodiscard]] std::size_t RecomputeCount() const noexcept { return recomputeCount_; }

private:
    struct Entry {
        std::uint64_t epoch = 0;
        std::uint64_t transientGeneration = 0;
        // The index sets the outlines were built from.  Compared field by
        // field: the four object kinds are exactly what the outlines cover,
        // and a PDF-text selection has no object outlines either way.
        PageSelection selection {};
        SelectionOutlines geometry {};
    };

    mutable std::unordered_map<std::size_t, Entry> entries_;
    mutable std::size_t recomputeCount_ = 0;
};

}  // namespace cnote::ui
