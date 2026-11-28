#pragma once

#include <array>
#include <cstddef>
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

// One-shot recognition; the reference definition of the gesture.
[[nodiscard]] std::optional<ScratchOut> RecognizeScratchOut(const std::vector<Vec2>& points);

// Incremental recognition for the freehand stroke that is being drawn.
//
// It owns the authoritative raw samples and their prefix arc lengths, both
// updated in constant time per append.  Recognition locates the evenly spaced
// samples by arc length instead of replaying the prefix, so the per-sample
// cost does not grow with the stroke, and the result is identical to
// RecognizeScratchOut for the same prefix.
class ScratchOutTracker {
public:
    void Reset();
    void Append(Vec2 point);

    [[nodiscard]] bool Empty() const { return points_.empty(); }
    [[nodiscard]] const std::vector<Vec2>& Points() const { return points_; }

    [[nodiscard]] std::optional<ScratchOut> Recognize();

private:
    std::vector<Vec2> points_;
    // Arc length from points_.front() to each point; arcLengths_[0] is 0.
    std::vector<float> arcLengths_;
    // Reused by every recognition call, which runs once per pointer sample.
    // `sampleSegments_[k]` is the raw segment holding sample k; sample arc
    // positions only grow with the stroke, so each cursor moves forward.
    std::vector<Vec2> samples_;
    std::vector<std::size_t> sampleSegments_;
    std::vector<float> projections_;
    // The last recognized hull and the raw points it covers; a growing
    // prefix whose new points stay inside it cannot change it.
    ScratchOut result_;
    std::size_t hullPointCount_ = 0;
    std::vector<Vec2> hullWork_;
};

}  // namespace cnote
