#include "selection/ScratchOut.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cnote {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegrees = kPi / 180.0f;

// The stroke is resampled to this many evenly spaced points, so passes are
// measured by length rather than by how fast the pointer moved.
constexpr std::size_t kSampleCount = 256;
// Shorter strokes (page units, i.e. points) are ticks, never scratch-outs.
constexpr float kMinStrokeLength = 30.0f;
// A reversal counts once the pointer has come back this fraction of the
// stroke's extent along its dominant axis.
constexpr float kReversalHysteresis = 0.3f;
// Consecutive passes, end to end, must point nearly opposite ways: a "w" or a
// "v v v" turns by about 127 degrees, a scratch by 145 or more.
constexpr float kMinPassTurn = 135.0f * kDegrees;
// The turn at a reversal, measured over this fraction of the shorter pass
// either side of it: a sharp turnaround, not an arch or a loop.
constexpr float kTurnWindow = 0.3f;
constexpr float kMinLocalTurn = 115.0f * kDegrees;
// Share of an object that must lie under the scratch, and the grid an area is
// sampled with.
constexpr float kMinCoverage = 0.5f;
constexpr int kAreaGrid = 8;

Vec2 Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
Vec2 Sub(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
Vec2 Scale(Vec2 a, float s) { return { a.x * s, a.y * s }; }
float Dot(Vec2 a, Vec2 b) { return (a.x * b.x) + (a.y * b.y); }
float Cross(Vec2 a, Vec2 b) { return (a.x * b.y) - (a.y * b.x); }
float Length(Vec2 a) { return std::sqrt(Dot(a, a)); }
float Distance(Vec2 a, Vec2 b) { return Length(Sub(a, b)); }
Vec2 Lerp(Vec2 a, Vec2 b, float t) { return Add(a, Scale(Sub(b, a), t)); }
// Unsigned angle between two directions, 0..pi.
float AngleBetween(Vec2 a, Vec2 b) {
    return std::atan2(std::abs(Cross(a, b)), Dot(a, b));
}

// The axis the stroke mostly moves along, either way: the principal direction
// of its step vectors.
Vec2 DominantAxis(const std::vector<Vec2>& samples) {
    float xx = 0.0f, xy = 0.0f, yy = 0.0f;
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const Vec2 step = Sub(samples[index], samples[index - 1]);
        xx += step.x * step.x;
        xy += step.x * step.y;
        yy += step.y * step.y;
    }
    const float angle = 0.5f * std::atan2(2.0f * xy, xx - yy);
    return { std::cos(angle), std::sin(angle) };
}

// Indices where each pass starts and ends along the projections `t`: the
// first pass starts at the extreme it leaves (an opening hook is dropped),
// the last ends at its extreme (so does a closing hook).
std::vector<std::size_t> PassEnds(const std::vector<float>& t, float hysteresis) {
    std::size_t low = 0, high = 0;
    std::size_t index = 1;
    int direction = 0;
    for (; index < t.size() && direction == 0; ++index) {
        low = t[index] < t[low] ? index : low;
        high = t[index] > t[high] ? index : high;
        if (t[index] - t[low] >= hysteresis) {
            direction = 1;
        } else if (t[high] - t[index] >= hysteresis) {
            direction = -1;
        }
    }
    if (direction == 0) {
        return {};
    }
    std::vector<std::size_t> ends { direction > 0 ? low : high };
    std::size_t extreme = index - 1U;
    for (; index < t.size(); ++index) {
        if ((t[index] - t[extreme]) * static_cast<float>(direction) > 0.0f) {
            extreme = index;
        } else if ((t[extreme] - t[index]) * static_cast<float>(direction) >= hysteresis) {
            ends.push_back(extreme);
            direction = -direction;
            extreme = index;
        }
    }
    ends.push_back(extreme);
    return ends;
}

// Andrew's monotone chain; counter-clockwise, no repeated first point.
// `points` is sorted in place; both buffers are reused by incremental callers.
void ConvexHullInto(std::vector<Vec2>& points, std::vector<Vec2>& hull) {
    std::sort(points.begin(), points.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    hull.resize(points.size() * 2U);
    std::size_t size = 0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        while (size >= 2U && Cross(Sub(hull[size - 1U], hull[size - 2U]), Sub(points[index], hull[size - 2U])) <= 0.0f) {
            --size;
        }
        hull[size++] = points[index];
    }
    const std::size_t lower = size + 1U;
    for (std::size_t index = points.size() - 1U; index > 0; --index) {
        const Vec2 point = points[index - 1U];
        while (size >= lower && Cross(Sub(hull[size - 1U], hull[size - 2U]), Sub(point, hull[size - 2U])) <= 0.0f) {
            --size;
        }
        hull[size++] = point;
    }
    hull.resize(size > 1U ? size - 1U : size);
}

std::vector<Vec2> ConvexHull(std::vector<Vec2> points) {
    std::vector<Vec2> hull;
    ConvexHullInto(points, hull);
    return hull;
}

// Whether adding `point` leaves the (already built) hull's vertices alone: it
// is inside or on the boundary.  A point strictly inside cannot become a
// vertex, and a point on an edge is dropped as collinear without changing the
// remaining vertices, so the existing hull stays exactly correct.
bool InsideOrOnHull(const std::vector<Vec2>& hull, Vec2 point) {
    if (hull.size() < 3U) {
        return false;
    }
    for (std::size_t index = 0; index < hull.size(); ++index) {
        const Vec2 a = hull[index];
        const Vec2 b = hull[(index + 1U) % hull.size()];
        if (Cross(Sub(b, a), Sub(point, a)) < 0.0f) {
            return false;
        }
    }
    return true;
}

float DistanceSquared(Vec2 a, Vec2 b) {
    const Vec2 delta = Sub(a, b);
    return Dot(delta, delta);
}

float DistanceSquaredToSegment(Vec2 point, Vec2 a, Vec2 b) {
    const Vec2 ab = Sub(b, a);
    const float lengthSquared = Dot(ab, ab);
    const float t = lengthSquared > 0.0f ? std::clamp(Dot(Sub(point, a), ab) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return DistanceSquared(point, Add(a, Scale(ab, t)));
}

struct HullBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

HullBounds ComputeHullBounds(const std::vector<Vec2>& hull) {
    HullBounds bounds { hull.front().x, hull.front().y, hull.front().x, hull.front().y };
    for (const Vec2 point : hull) {
        bounds.left = std::min(bounds.left, point.x);
        bounds.top = std::min(bounds.top, point.y);
        bounds.right = std::max(bounds.right, point.x);
        bounds.bottom = std::max(bounds.bottom, point.y);
    }
    return bounds;
}

bool HullContains(const std::vector<Vec2>& hull, const HullBounds& bounds, Vec2 point, float margin) {
    // The hull lies inside its box: a point outside the box grown by the
    // margin is neither inside nor near it.  A negative margin only accepts
    // interior points, so there zero reach is the safe box.
    const float reach = std::max(0.0f, margin);
    if (point.x < bounds.left - reach || point.x > bounds.right + reach
        || point.y < bounds.top - reach || point.y > bounds.bottom + reach) {
        return false;
    }
    const float marginSquared = margin * margin;
    bool inside = hull.size() >= 3U;
    for (std::size_t index = 0; index < hull.size(); ++index) {
        const Vec2 a = hull[index];
        const Vec2 b = hull[(index + 1U) % hull.size()];
        inside = inside && Cross(Sub(b, a), Sub(point, a)) >= 0.0f;
        // One edge within the margin settles the answer; later edges cannot
        // take it back.
        if (margin >= 0.0f && DistanceSquaredToSegment(point, a, b) <= marginSquared) {
            return true;
        }
    }
    return inside;
}

// The evenly spaced samples along the path, located through the prefix arc
// lengths instead of walking the raw points.  Sample k sits at arc length
// k * length / (kSampleCount - 1), which only grows as the stroke does, so
// each sample's raw segment cursor moves forward: an append costs the new
// point plus the cursors it crossed, never a replay of the prefix.
void ResamplePrefix(const std::vector<Vec2>& points, const std::vector<float>& arcLengths,
    float length, std::vector<std::size_t>& sampleSegments, std::vector<Vec2>& samples) {
    const float step = length / static_cast<float>(kSampleCount - 1U);
    if (sampleSegments.size() < kSampleCount) {
        sampleSegments.assign(kSampleCount, 1U);
    }
    samples.clear();
    samples.reserve(kSampleCount);
    samples.push_back(points.front());
    for (std::size_t index = 1; index + 1U < kSampleCount; ++index) {
        const float target = step * static_cast<float>(index);
        std::size_t segment = sampleSegments[index];
        // `segment` is the first raw point past `target`; both only grow.
        while (segment < points.size() && arcLengths[segment] <= target) {
            ++segment;
        }
        sampleSegments[index] = segment;
        if (segment >= points.size()) {
            samples.push_back(points.back());
            continue;
        }
        const float segmentLength = arcLengths[segment] - arcLengths[segment - 1U];
        const float t = segmentLength > 0.0f
            ? std::clamp((target - arcLengths[segment - 1U]) / segmentLength, 0.0f, 1.0f)
            : 0.0f;
        samples.push_back(Lerp(points[segment - 1U], points[segment], t));
    }
    while (samples.size() < kSampleCount) {
        samples.push_back(points.back());
    }
}

// The pass test that both the one-shot and incremental recognizers share, so
// only the resampling differs between them.  `projections` is scratch.
bool IsScratchOut(const std::vector<Vec2>& samples, std::vector<float>& projections) {
    const Vec2 axis = DominantAxis(samples);
    projections.resize(samples.size());
    float lowest = Dot(samples.front(), axis), highest = lowest;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        projections[index] = Dot(samples[index], axis);
        lowest = std::min(lowest, projections[index]);
        highest = std::max(highest, projections[index]);
    }
    const std::vector<std::size_t> ends = PassEnds(projections, kReversalHysteresis * (highest - lowest));
    // n passes have n + 1 ends and n - 1 reversals.
    if (ends.size() < static_cast<std::size_t>(kScratchOutMinReversals) + 2U) {
        return false;
    }
    for (std::size_t pass = 1; pass + 1U < ends.size(); ++pass) {
        const std::size_t before = ends[pass - 1U], turn = ends[pass], after = ends[pass + 1U];
        if (AngleBetween(Sub(samples[turn], samples[before]), Sub(samples[after], samples[turn])) < kMinPassTurn) {
            return false;
        }
        const auto window = std::max<std::size_t>(1U,
            static_cast<std::size_t>(kTurnWindow * static_cast<float>(std::min(turn - before, after - turn))));
        if (AngleBetween(Sub(samples[turn], samples[turn - window]), Sub(samples[turn + window], samples[turn]))
            < kMinLocalTurn) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool ScratchOut::CoversPath(const std::vector<Vec2>& path, float margin) const {
    if (hull.empty() || path.empty()) {
        return false;
    }
    const HullBounds hullBounds = ComputeHullBounds(hull);
    if (path.size() == 1U) {
        return HullContains(hull, hullBounds, path.front(), margin);
    }
    // The threshold needs the whole length up front, so the coverage pass can
    // stop as soon as later pieces cannot change the result.  Summed in the
    // same order as before, so the half-covered boundary is unchanged.
    float total = 0.0f;
    for (std::size_t index = 1; index < path.size(); ++index) {
        total += Distance(path[index - 1], path[index]);
    }
    if (!(total > 0.0f)) {
        return HullContains(hull, hullBounds, path.front(), margin);
    }
    const float needed = kMinCoverage * total;
    const float reach = std::max(0.0f, margin);
    // Long segments (a straight line's two points) are split so each piece
    // is weighed where it lies.
    const float piece = std::max(1.0f, margin);
    float covered = 0.0f;
    for (std::size_t index = 1; index < path.size(); ++index) {
        const Vec2 a = path[index - 1];
        const Vec2 b = path[index];
        const float length = Distance(a, b);
        // A segment whose box cannot reach the hull's box cannot contain a
        // covered sample; skip its sampling entirely.
        if (std::max(a.x, b.x) + reach < hullBounds.left
            || std::min(a.x, b.x) - reach > hullBounds.right
            || std::max(a.y, b.y) + reach < hullBounds.top
            || std::min(a.y, b.y) - reach > hullBounds.bottom) {
            continue;
        }
        const int pieces = std::max(1, static_cast<int>(std::ceil(length / piece)));
        for (int step = 0; step < pieces; ++step) {
            const float t = (static_cast<float>(step) + 0.5f) / static_cast<float>(pieces);
            if (HullContains(hull, hullBounds, Lerp(a, b, t), margin)) {
                covered += length / static_cast<float>(pieces);
            }
        }
        // Coverage only grows, so crossing the threshold is final.
        if (covered >= needed) {
            return true;
        }
    }
    return covered >= needed;
}

bool ScratchOut::CoversArea(const std::array<Vec2, 4>& corners, float margin) const {
    if (hull.empty()) {
        return false;
    }
    const HullBounds hullBounds = ComputeHullBounds(hull);
    float left = corners.front().x, top = corners.front().y;
    float right = left, bottom = top;
    for (const Vec2 corner : corners) {
        left = std::min(left, corner.x);
        top = std::min(top, corner.y);
        right = std::max(right, corner.x);
        bottom = std::max(bottom, corner.y);
    }
    const float reach = std::max(0.0f, margin);
    // The sampled points all lie inside the corners' box; when that box cannot
    // reach the hull's, nothing is covered.
    if (left - reach > hullBounds.right || right + reach < hullBounds.left
        || top - reach > hullBounds.bottom || bottom + reach < hullBounds.top) {
        return false;
    }

    constexpr int kSamples = kAreaGrid * kAreaGrid;
    const float needed = kMinCoverage * static_cast<float>(kSamples);
    int covered = 0;
    int remaining = kSamples;
    for (int row = 0; row < kAreaGrid; ++row) {
        const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(kAreaGrid);
        const Vec2 rowLeft = Lerp(corners[0], corners[3], v);
        const Vec2 rowRight = Lerp(corners[1], corners[2], v);
        for (int column = 0; column < kAreaGrid; ++column) {
            --remaining;
            const float u = (static_cast<float>(column) + 0.5f) / static_cast<float>(kAreaGrid);
            if (HullContains(hull, hullBounds, Lerp(rowLeft, rowRight, u), margin)) {
                ++covered;
                // Covered samples only grow from here.
                if (static_cast<float>(covered) >= needed) {
                    return true;
                }
            }
            // Even covering every remaining sample cannot reach the threshold.
            if (static_cast<float>(covered + remaining) < needed) {
                return false;
            }
        }
    }
    return static_cast<float>(covered) >= needed;
}

std::optional<ScratchOut> RecognizeScratchOut(const std::vector<Vec2>& points) {
    if (points.size() < 2U) {
        return std::nullopt;
    }
    std::vector<float> arcLengths(points.size(), 0.0f);
    for (std::size_t index = 1; index < points.size(); ++index) {
        arcLengths[index] = arcLengths[index - 1U] + Distance(points[index - 1U], points[index]);
    }
    const float length = arcLengths.back();
    if (length < kMinStrokeLength) {
        return std::nullopt;
    }
    // The same resampler as the incremental tracker, so the reference and the
    // live path can never drift apart; only the caching differs.
    std::vector<std::size_t> sampleSegments;
    std::vector<Vec2> samples;
    ResamplePrefix(points, arcLengths, length, sampleSegments, samples);
    std::vector<float> projections;
    if (!IsScratchOut(samples, projections)) {
        return std::nullopt;
    }
    return ScratchOut { ConvexHull(points) };
}

void ScratchOutTracker::Reset() {
    points_.clear();
    arcLengths_.clear();
    samples_.clear();
    sampleSegments_.clear();
    projections_.clear();
    result_.hull.clear();
    hullPointCount_ = 0;
    hullWork_.clear();
}

void ScratchOutTracker::Append(Vec2 point) {
    float length = 0.0f;
    if (!points_.empty()) {
        length = arcLengths_.back() + Distance(points_.back(), point);
    }
    points_.push_back(point);
    arcLengths_.push_back(length);
}

std::optional<ScratchOut> ScratchOutTracker::Recognize() {
    if (points_.size() < 2U) {
        return std::nullopt;
    }
    // The incremental length is the cheap gate: ticks never pay for
    // resampling or hull work.
    const float length = arcLengths_.back();
    if (length < kMinStrokeLength) {
        return std::nullopt;
    }
    ResamplePrefix(points_, arcLengths_, length, sampleSegments_, samples_);
    if (!IsScratchOut(samples_, projections_)) {
        return std::nullopt;
    }
    bool reuseHull = result_.hull.size() >= 3U && hullPointCount_ <= points_.size();
    for (std::size_t index = hullPointCount_; reuseHull && index < points_.size(); ++index) {
        reuseHull = InsideOrOnHull(result_.hull, points_[index]);
    }
    if (!reuseHull) {
        hullWork_.assign(points_.begin(), points_.end());
        ConvexHullInto(hullWork_, result_.hull);
    }
    hullPointCount_ = points_.size();
    return result_;
}

}  // namespace cnote
