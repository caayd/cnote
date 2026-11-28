#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Document.h"

namespace cnote::ui {

// Conservative page-space axis-aligned bounds of one object, used to reject
// objects that cannot be near a swept region before an exact hit test measures
// text/LaTeX or smooths a stroke.  A superset of the object's real geometry, so
// a miss here can never hide a hit; `valid` is false for object kinds that have
// no geometry at all (an empty stroke).
struct ObjectBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool valid = false;
};

// One page's object bounds, parallel to the page's object vectors.
struct PageObjectBounds {
    std::vector<ObjectBounds> images;
    std::vector<ObjectBounds> texts;
    std::vector<ObjectBounds> latexes;
    std::vector<ObjectBounds> strokes;
};

// Per-document cache of PageObjectBounds keyed by Document::PageEpoch and the
// session-wide transient generation.  The eraser queries it once per pointer
// sample; without it every sample measured every text and LaTeX block and
// rescanned every stroke's raw points.  Every committed mutation bumps the page
// epoch, so a cached entry cannot outlive the contents it describes; inserting,
// removing or reordering pages replaces an index's epoch (or page) and forces a
// recompute.  In-place gestures (move/transform/clone/cancel) do not bump the
// epoch until they commit, so the controller bumps the shared transient
// generation on every one of those mutations; that invalidates every view's
// entry without call-site discipline.  Entries are bounded by the pages the
// document has been viewed at, and the bounds themselves are O(the page's
// objects), not O(points).
class PageObjectBoundsCache {
public:
    // Bounds for `pageIndex`, recomputed when its page epoch or the session's
    // transient generation changed.  Returns an empty set for an out-of-range
    // index.
    [[nodiscard]] const PageObjectBounds& ForPage(const Document& document, std::size_t pageIndex,
        std::uint64_t transientGeneration) const;

    // Test/diagnostic counter: how many times a page's bounds were measured.
    [[nodiscard]] std::size_t RecomputeCount() const noexcept { return recomputeCount_; }

private:
    struct Entry {
        std::uint64_t epoch = 0;
        std::uint64_t transientGeneration = 0;
        PageObjectBounds bounds {};
    };

    mutable std::unordered_map<std::size_t, Entry> entries_;
    mutable std::size_t recomputeCount_ = 0;
};

}  // namespace cnote::ui
