#pragma once

#include <array>
#include <optional>
#include <vector>

#include "Document.h"

namespace cnote {

// Scratch-out: a freehand stroke scribbled back and forth over ink erases the
// ink instead of adding to it.
//
// The gesture is a zigzag: at least kScratchOutMinReversals sharp reversals
// along one axis, every pass nearly antiparallel to the one before it.  The
// stroke is resampled evenly and projected on its dominant direction of
// motion; a reversal counts once the pointer has come back a good part of the
// stroke's extent, so jitter and hooks at the ends are not passes.  Rounded
// turns (the arches of a cursive "m" or "n", loops) and slanted ones (a "w")
// do not count, so handwriting does not read as a scratch-out.
inline constexpr int kScratchOutMinReversals = 3;

struct ScratchOut {
    // The convex hull of the stroke, counter-clockwise in the input's units.
    std::vector<Vec2> hull;

    // Whether the scratch covers most of an object: at least half of the
    // polyline's length (a stroke's path), or of the quad's area (an image's
    // or a text's rotated bounds, corners in order), lies within `margin` of
    // the hull.  A single point is covered when it lies within the margin.
    [[nodiscard]] bool CoversPath(const std::vector<Vec2>& path, float margin) const;
    [[nodiscard]] bool CoversArea(const std::array<Vec2, 4>& corners, float margin) const;
};

[[nodiscard]] std::optional<ScratchOut> RecognizeScratchOut(const std::vector<Vec2>& points);

}  // namespace cnote
