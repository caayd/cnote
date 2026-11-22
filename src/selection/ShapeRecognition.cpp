#include "selection/ShapeRecognition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace cnote {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegrees = kPi / 180.0f;

// Strokes are resampled to evenly spaced points before fitting, so every fit
// weighs the stroke by length rather than by how fast the pointer moved.
constexpr std::size_t kSampleCount = 128;
// Shorter strokes (page units, i.e. points) are ticks and dots, never shapes.
constexpr float kMinStrokeLength = 12.0f;
// A stroke whose ends (after trimming an overshoot) are closer than this
// fraction of its length is closed.
constexpr float kClosedGapRatio = 0.15f;
// Line: the largest deviation from the fitted line, relative to its length,
// and how much of the stroke's length the line must span (no scribbles).
constexpr float kLineMaxDeviation = 0.06f;
constexpr float kLineMinSpan = 0.85f;
// Mean and largest distance from the stroke to a fitted outline, relative to
// the stroke's bounding-box diagonal.
constexpr float kOutlineMeanError = 0.03f;
constexpr float kOutlineMaxError = 0.10f;
// Douglas-Peucker tolerance for corner finding, relative to the diagonal.
constexpr float kCornerTolerance = 0.05f;
// A vertex that turns the outline by less than this is not a corner, and on
// a closed outline two corners closer than this fraction of the perimeter are
// one rounded corner.
constexpr float kMinCornerTurn = 30.0f * kDegrees;
constexpr float kMinSideFraction = 0.08f;
// The sides next to a refined corner must meet at least this steeply for
// their intersection to replace the drawn vertex.
constexpr float kMinIntersectionSine = 0.25f;
// Snapping to the ideal shape.
constexpr float kCircleAxisRatio = 0.85f;
constexpr float kAxisSnapAngle = 8.0f * kDegrees;
constexpr float kSquareSideRatio = 0.9f;
// Constrained (Shift) triangles: equilateral when every angle is this close
// to 60 degrees.
constexpr float kEquilateralTolerance = 15.0f * kDegrees;
// Arrow head: barb angle against the shaft and barb length relative to it.
constexpr float kMinBarbAngle = 10.0f * kDegrees;
constexpr float kMaxBarbAngle = 80.0f * kDegrees;
constexpr float kMinBarbRatio = 0.05f;
constexpr float kMaxBarbRatio = 0.6f;
// Curly brace, measured against the chord between the stroke's ends.  The
// tip is a sharp outward point (it turns the stroke by at least
// kMinBraceTipTurn and lies farther from the chord than the samples either
// side) within kBraceTipCenterTolerance of the chord's middle, its depth off
// the chord this fraction of the chord.  No other sharp outward point is
// allowed (a zigzag), each half must bow at least kMinBraceBow of its length
// off the straight segment from its end to the tip (a "<"), and the stroke
// may cross the chord by at most kMaxBraceUnderside of the depth.
constexpr float kBraceTipCenterTolerance = 0.2f;
constexpr float kMinBraceDepth = 0.08f;
constexpr float kMaxBraceDepth = 0.5f;
constexpr float kMinBraceTipTurn = 50.0f * kDegrees;
constexpr float kMinBraceBow = 0.08f;
constexpr float kMaxBraceUnderside = 0.15f;
// Samples either side of a point its turn is measured over.
constexpr std::size_t kBraceTangentSpan = 4;
// Output density: straight edges are subdivided at this spacing (page units),
// ellipses drawn with this many segments.
constexpr float kOutlineSpacing = 1.5f;
constexpr int kEllipseSegments = 72;

Vec2 Add(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
Vec2 Sub(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
Vec2 Scale(Vec2 a, float s) { return { a.x * s, a.y * s }; }
float Dot(Vec2 a, Vec2 b) { return (a.x * b.x) + (a.y * b.y); }
float Cross(Vec2 a, Vec2 b) { return (a.x * b.y) - (a.y * b.x); }
float Length(Vec2 a) { return std::sqrt(Dot(a, a)); }
float Distance(Vec2 a, Vec2 b) { return Length(Sub(a, b)); }
Vec2 Lerp(Vec2 a, Vec2 b, float t) { return Add(a, Scale(Sub(b, a), t)); }
Vec2 Rotate(Vec2 a, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return { (a.x * c) - (a.y * s), (a.x * s) + (a.y * c) };
}
Vec2 Normalized(Vec2 a) {
    const float length = Length(a);
    return length > 0.0f ? Scale(a, 1.0f / length) : Vec2 { 1.0f, 0.0f };
}
// Unsigned angle between two directions, 0..pi.
float AngleBetween(Vec2 a, Vec2 b) {
    return std::atan2(std::abs(Cross(a, b)), Dot(a, b));
}

float PathLength(std::span<const Vec2> points) {
    float length = 0.0f;
    for (std::size_t index = 1; index < points.size(); ++index) {
        length += Distance(points[index - 1], points[index]);
    }
    return length;
}

float Diagonal(std::span<const Vec2> points) {
    float minX = points.front().x, maxX = minX;
    float minY = points.front().y, maxY = minY;
    for (const Vec2 point : points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    return Length({ maxX - minX, maxY - minY });
}

// `count` points evenly spaced along the path, first and last kept.
std::vector<Vec2> Resample(std::span<const Vec2> points, std::size_t count) {
    const float step = PathLength(points) / static_cast<float>(count - 1U);
    std::vector<Vec2> samples;
    samples.reserve(count);
    samples.push_back(points.front());
    float walked = 0.0f;  // distance since the last sample
    Vec2 previous = points.front();
    for (std::size_t index = 1; index < points.size() && samples.size() + 1U < count; ++index) {
        const Vec2 current = points[index];
        float segment = Distance(previous, current);
        while (walked + segment >= step && samples.size() + 1U < count) {
            const Vec2 sample = Lerp(previous, current, (step - walked) / segment);
            samples.push_back(sample);
            segment -= step - walked;
            previous = sample;
            walked = 0.0f;
        }
        walked += segment;
        previous = current;
    }
    while (samples.size() < count) {
        samples.push_back(points.back());
    }
    return samples;
}

float DistanceToSegment(Vec2 point, Vec2 a, Vec2 b) {
    const Vec2 edge = Sub(b, a);
    const float lengthSquared = Dot(edge, edge);
    const float t = lengthSquared > 0.0f ? std::clamp(Dot(Sub(point, a), edge) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return Distance(point, Add(a, Scale(edge, t)));
}

struct FitError {
    float mean = 0.0f;
    float max = 0.0f;
};

// Distance from every sample to the polyline through `outline`.
FitError PolylineError(std::span<const Vec2> samples, std::span<const Vec2> outline) {
    FitError error;
    for (const Vec2 sample : samples) {
        float nearest = Distance(sample, outline.front());
        for (std::size_t index = 1; index < outline.size(); ++index) {
            nearest = std::min(nearest, DistanceToSegment(sample, outline[index - 1], outline[index]));
        }
        error.mean += nearest;
        error.max = std::max(error.max, nearest);
    }
    error.mean /= static_cast<float>(samples.size());
    return error;
}

bool Acceptable(FitError error, float scale) {
    return error.mean <= kOutlineMeanError * scale && error.max <= kOutlineMaxError * scale;
}

// Total-least-squares line: the centroid and the direction of most variance.
struct LineFit {
    Vec2 center {};
    Vec2 direction { 1.0f, 0.0f };
};

LineFit FitLine(std::span<const Vec2> points) {
    LineFit fit;
    for (const Vec2 point : points) {
        fit.center = Add(fit.center, point);
    }
    fit.center = Scale(fit.center, 1.0f / static_cast<float>(points.size()));
    float sxx = 0.0f, syy = 0.0f, sxy = 0.0f;
    for (const Vec2 point : points) {
        const Vec2 d = Sub(point, fit.center);
        sxx += d.x * d.x;
        syy += d.y * d.y;
        sxy += d.x * d.y;
    }
    const float angle = 0.5f * std::atan2(2.0f * sxy, sxx - syy);
    fit.direction = { std::cos(angle), std::sin(angle) };
    return fit;
}

// Appends the edge to `to` subdivided at kOutlineSpacing; `out` ends at its start.
void AppendEdge(std::vector<Vec2>& out, Vec2 from, Vec2 to) {
    const int steps = std::max(1, static_cast<int>(std::ceil(Distance(from, to) / kOutlineSpacing)));
    for (int step = 1; step <= steps; ++step) {
        out.push_back(Lerp(from, to, static_cast<float>(step) / static_cast<float>(steps)));
    }
}

std::vector<Vec2> ClosedOutline(std::span<const Vec2> corners) {
    std::vector<Vec2> outline { corners.front() };
    for (std::size_t index = 0; index < corners.size(); ++index) {
        AppendEdge(outline, corners[index], corners[(index + 1U) % corners.size()]);
    }
    outline.back() = corners.front();
    return outline;
}

// Douglas-Peucker: appends, in order, the indices strictly between `first`
// and `last` that the simplified polyline keeps.  Indices wrap modulo the
// sample count so a closed outline can simplify its last span back to 0.
void Simplify(std::span<const Vec2> samples, std::size_t first, std::size_t last, float tolerance,
    std::vector<std::size_t>& kept) {
    if (last <= first + 1U) {
        return;
    }
    const std::size_t count = samples.size();
    const Vec2 a = samples[first % count];
    const Vec2 b = samples[last % count];
    float farthest = 0.0f;
    std::size_t split = first;
    for (std::size_t index = first + 1U; index < last; ++index) {
        const float distance = DistanceToSegment(samples[index % count], a, b);
        if (distance > farthest) {
            farthest = distance;
            split = index;
        }
    }
    if (farthest <= tolerance) {
        return;
    }
    Simplify(samples, first, split, tolerance, kept);
    kept.push_back(split);
    Simplify(samples, split, last, tolerance, kept);
}

// Removes vertices that are not corners: those turning the outline by less
// than kMinCornerTurn and, on a closed outline, the weaker of two corners on a
// side too short to be one (a rounded corner the simplification split).  Open
// outlines keep their end vertices.
void DropWeakCorners(std::span<const Vec2> samples, std::vector<std::size_t>& corners, bool closed) {
    const std::size_t count = samples.size();
    const auto turnAt = [&](std::size_t position) {
        const std::size_t size = corners.size();
        const Vec2 previous = samples[corners[(position + size - 1U) % size]];
        const Vec2 current = samples[corners[position]];
        const Vec2 next = samples[corners[(position + 1U) % size]];
        return AngleBetween(Sub(current, previous), Sub(next, current));
    };
    while (corners.size() >= 3U) {
        const std::size_t begin = closed ? 0U : 1U;
        const std::size_t end = closed ? corners.size() : corners.size() - 1U;
        if (begin >= end) {
            return;
        }
        std::size_t weakest = begin;
        float weakestTurn = turnAt(begin);
        for (std::size_t position = begin + 1U; position < end; ++position) {
            const float turn = turnAt(position);
            if (turn < weakestTurn) {
                weakestTurn = turn;
                weakest = position;
            }
        }
        if (weakestTurn < kMinCornerTurn) {
            corners.erase(corners.begin() + static_cast<std::ptrdiff_t>(weakest));
            continue;
        }
        if (!closed) {
            return;
        }
        std::size_t shortest = 0;
        std::size_t shortestSpan = count;
        for (std::size_t position = 0; position < corners.size(); ++position) {
            const std::size_t next = corners[(position + 1U) % corners.size()];
            const std::size_t span = (next + count - corners[position]) % count;
            if (span < shortestSpan) {
                shortestSpan = span;
                shortest = position;
            }
        }
        if (static_cast<float>(shortestSpan) >= kMinSideFraction * static_cast<float>(count)) {
            return;
        }
        const std::size_t other = (shortest + 1U) % corners.size();
        const std::size_t drop = turnAt(shortest) < turnAt(other) ? shortest : other;
        corners.erase(corners.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

// The rotation that brings `angle` onto the nearest `offset + k * period`
// when it is within kAxisSnapAngle of it, else 0.
float AxisSnapDelta(float angle, float period, float offset) {
    const float target = offset + (std::round((angle - offset) / period) * period);
    return std::abs(target - angle) <= kAxisSnapAngle ? target - angle : 0.0f;
}

// ---------------------------------------------------------------- open -----

std::optional<RecognizedShape> FitStraightLine(std::span<const Vec2> samples, float length) {
    const LineFit fit = FitLine(samples);
    float low = 0.0f, high = 0.0f, deviation = 0.0f;
    for (const Vec2 sample : samples) {
        const Vec2 d = Sub(sample, fit.center);
        const float along = Dot(d, fit.direction);
        low = std::min(low, along);
        high = std::max(high, along);
        deviation = std::max(deviation, std::abs(Cross(fit.direction, d)));
    }
    if (deviation > kLineMaxDeviation * length || high - low < kLineMinSpan * length) {
        return std::nullopt;
    }
    Vec2 start = Add(fit.center, Scale(fit.direction, low));
    Vec2 end = Add(fit.center, Scale(fit.direction, high));
    if (Distance(start, samples.front()) > Distance(end, samples.front())) {
        std::swap(start, end);
    }
    RecognizedShape shape { RecognizedShapeKind::Line, { start } };
    AppendEdge(shape.points, start, end);
    return shape;
}

// One-stroke arrow: tail, tip, first barb, back to the tip, second barb.
std::optional<RecognizedShape> FitArrow(std::span<const Vec2> samples, float diagonal) {
    std::vector<std::size_t> corners { 0U };
    Simplify(samples, 0U, samples.size() - 1U, kCornerTolerance * diagonal, corners);
    corners.push_back(samples.size() - 1U);
    DropWeakCorners(samples, corners, false);
    if (corners.size() != 5U) {
        return std::nullopt;
    }
    std::array<Vec2, 5> v {};
    for (std::size_t index = 0; index < v.size(); ++index) {
        v[index] = samples[corners[index]];
    }
    const Vec2 tail = v[0];
    const Vec2 tip = v[1];
    const float shaft = Distance(tail, tip);
    const Vec2 back = Normalized(Sub(tail, tip));
    const Vec2 barb1 = Sub(v[2], tip);
    const Vec2 barb2 = Sub(v[4], v[3]);
    const float length1 = Length(barb1);
    const float length2 = Length(barb2);
    const float angle1 = AngleBetween(back, barb1);
    const float angle2 = AngleBetween(back, barb2);
    const float side1 = Cross(back, barb1);
    const float side2 = Cross(back, barb2);
    const auto barbFits = [shaft](float length, float angle) {
        return length >= kMinBarbRatio * shaft && length <= kMaxBarbRatio * shaft
            && angle >= kMinBarbAngle && angle <= kMaxBarbAngle;
    };
    if (!barbFits(length1, angle1) || !barbFits(length2, angle2) || (side1 > 0.0f) == (side2 > 0.0f)
        || Distance(v[3], tip) > 0.5f * (length1 + length2) * 0.5f) {
        return std::nullopt;
    }
    if (!Acceptable(PolylineError(samples, v), diagonal)) {
        return std::nullopt;
    }

    // A symmetric head: both barbs at their mean angle and length, the first
    // on the side the user drew it.
    const float angle = 0.5f * (angle1 + angle2);
    const float length = 0.5f * (length1 + length2);
    const float turn = side1 > 0.0f ? angle : -angle;
    const Vec2 first = Add(tip, Scale(Rotate(back, turn), length));
    const Vec2 second = Add(tip, Scale(Rotate(back, -turn), length));
    RecognizedShape shape { RecognizedShapeKind::Arrow, { tail } };
    AppendEdge(shape.points, tail, tip);
    AppendEdge(shape.points, tip, first);
    AppendEdge(shape.points, first, tip);
    AppendEdge(shape.points, tip, second);
    return shape;
}

// Appends the arc of `radius` about `center` from angle `from` to `to`
// (either direction), subdivided at kOutlineSpacing; `out` ends at its start.
void AppendArc(std::vector<Vec2>& out, Vec2 center, float radius, float from, float to) {
    const int steps = std::max(1, static_cast<int>(std::ceil(radius * std::abs(to - from) / kOutlineSpacing)));
    for (int step = 1; step <= steps; ++step) {
        const float angle = from + ((to - from) * static_cast<float>(step) / static_cast<float>(steps));
        out.push_back(Add(center, Scale(Vec2 { std::cos(angle), std::sin(angle) }, radius)));
    }
}

// The brace from `start` to `end` with its tip `depth` off the chord's middle
// and its arms `arm` off the chord, on the side of `normal` (unit, square to
// the chord).  Each half curls out of its end with radius `arm`, runs
// parallel to the chord and curls into the tip with radius `depth - arm`.
std::vector<Vec2> BraceOutline(Vec2 start, Vec2 end, Vec2 normal, float depth, float arm) {
    const float length = Distance(start, end);
    const Vec2 along = Normalized(Sub(end, start));
    const float half = 0.5f * length;
    const float tipRadius = depth - arm;
    // Arcs are built in the chord frame (u along the chord, v toward the tip)
    // and mapped out point by point.
    std::vector<Vec2> local { { 0.0f, 0.0f } };
    AppendArc(local, { arm, 0.0f }, arm, kPi, 0.5f * kPi);
    AppendEdge(local, local.back(), { half - tipRadius, arm });
    AppendArc(local, { half - tipRadius, depth }, tipRadius, -0.5f * kPi, 0.0f);
    AppendArc(local, { half + tipRadius, depth }, tipRadius, kPi, 1.5f * kPi);
    AppendEdge(local, local.back(), { length - arm, arm });
    AppendArc(local, { length - arm, 0.0f }, arm, 0.5f * kPi, 0.0f);
    std::vector<Vec2> points;
    points.reserve(local.size());
    for (const Vec2 p : local) {
        points.push_back(Add(start, Add(Scale(along, p.x), Scale(normal, p.y))));
    }
    points.back() = end;
    return points;
}

// A curly brace in any orientation, opening either way, recognized by its
// structure rather than an outline fit: the drawn arms may run straight ("]"
// with a ">" in the middle) or bow out as far as the tip (a ">" between two
// lumps, with inner corners either side of the tip).  Either snaps to the
// same brace: the tip's depth and the arms' mean offset from the chord,
// symmetric about the chord's middle, level when the chord is near-level.
std::optional<RecognizedShape> FitBrace(std::span<const Vec2> samples) {
    const std::size_t count = samples.size();
    const Vec2 start = samples.front();
    const Vec2 end = samples.back();
    const float length = Distance(start, end);
    if (length < kMinStrokeLength || count <= 4U * kBraceTangentSpan) {
        return std::nullopt;
    }
    const Vec2 along = Scale(Sub(end, start), 1.0f / length);
    // Offsets from the chord, positive on the side the stroke mostly lies.
    std::vector<float> offset(count);
    float area = 0.0f;
    for (std::size_t index = 0; index < count; ++index) {
        offset[index] = Cross(along, Sub(samples[index], start));
        area += offset[index];
    }
    const float side = area >= 0.0f ? 1.0f : -1.0f;
    for (float& value : offset) {
        value *= side;
    }
    const auto turnAt = [&](std::size_t index) {
        return AngleBetween(Sub(samples[index], samples[index - kBraceTangentSpan]),
            Sub(samples[index + kBraceTangentSpan], samples[index]));
    };
    const auto sharpOutward = [&](std::size_t index) {
        return offset[index] >= offset[index - kBraceTangentSpan] && offset[index] >= offset[index + kBraceTangentSpan]
            && turnAt(index) >= kMinBraceTipTurn;
    };

    // The tip: the sharpest outward point near the chord's middle.
    std::optional<std::size_t> tip;
    for (std::size_t index = kBraceTangentSpan; index + kBraceTangentSpan < count; ++index) {
        const float t = Dot(along, Sub(samples[index], start)) / length;
        if (std::abs(t - 0.5f) <= kBraceTipCenterTolerance && sharpOutward(index)
            && (!tip.has_value() || turnAt(index) > turnAt(*tip))) {
            tip = index;
        }
    }
    if (!tip.has_value()) {
        return std::nullopt;
    }
    const float depth = offset[*tip];
    if (depth < kMinBraceDepth * length || depth > kMaxBraceDepth * length
        || *std::min_element(offset.begin(), offset.end()) < -kMaxBraceUnderside * depth) {
        return std::nullopt;
    }
    for (std::size_t index = kBraceTangentSpan; index + kBraceTangentSpan < count; ++index) {
        const std::size_t apart = index > *tip ? index - *tip : *tip - index;
        if (apart > 2U * kBraceTangentSpan && sharpOutward(index)) {
            return std::nullopt;
        }
    }
    const auto bowed = [&](std::size_t first, std::size_t last) {
        const Vec2 a = samples[first];
        const Vec2 b = samples[last];
        float farthest = 0.0f;
        for (std::size_t index = first; index <= last; ++index) {
            farthest = std::max(farthest, DistanceToSegment(samples[index], a, b));
        }
        return farthest >= kMinBraceBow * Distance(a, b);
    };
    if (!bowed(0U, *tip) || !bowed(*tip, count - 1U)) {
        return std::nullopt;
    }

    // The arms: the mean offset of the samples a sixth to a third of the way
    // in from either end, kept clear of the curls and the tip.
    float armSum = 0.0f;
    int armCount = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const float t = Dot(along, Sub(samples[index], start)) / length;
        if ((t >= 1.0f / 6.0f && t <= 1.0f / 3.0f) || (t >= 2.0f / 3.0f && t <= 5.0f / 6.0f)) {
            armSum += offset[index];
            ++armCount;
        }
    }
    if (armCount == 0) {
        return std::nullopt;
    }
    const float arm = std::clamp(armSum / static_cast<float>(armCount), 0.25f * depth, 0.75f * depth);
    const Vec2 normal = Scale(Vec2 { -along.y, along.x }, side);

    const Vec2 middle = Lerp(start, end, 0.5f);
    const float level = AxisSnapDelta(std::atan2(along.y, along.x), 0.5f * kPi, 0.0f);
    return RecognizedShape { RecognizedShapeKind::Brace,
        BraceOutline(Add(middle, Rotate(Sub(start, middle), level)), Add(middle, Rotate(Sub(end, middle), level)),
            Rotate(normal, level), depth, arm) };
}

// -------------------------------------------------------------- closed -----

struct Candidate {
    RecognizedShape shape;
    float meanError = 0.0f;
};

// Least-squares ellipse: centroid and principal axes from the samples, then
// the semi-axes solving min sum (A x^2 + B y^2 - 1)^2 in the axis frame.  A
// near-round fit, or any fit when `constrained`, becomes a circle of the
// mean semi-axis.
std::optional<Candidate> FitEllipse(std::span<const Vec2> samples, float diagonal, bool constrained) {
    const LineFit axes = FitLine(samples);
    Vec2 major = axes.direction;
    const Vec2 center = axes.center;
    const auto frame = [&](Vec2 point, Vec2 axis) {
        const Vec2 d = Sub(point, center);
        return Vec2 { Dot(d, axis), Cross(axis, d) };
    };
    float sx4 = 0.0f, sy4 = 0.0f, sx2y2 = 0.0f, sx2 = 0.0f, sy2 = 0.0f;
    for (const Vec2 sample : samples) {
        const Vec2 p = frame(sample, major);
        const float x2 = p.x * p.x;
        const float y2 = p.y * p.y;
        sx4 += x2 * x2;
        sy4 += y2 * y2;
        sx2y2 += x2 * y2;
        sx2 += x2;
        sy2 += y2;
    }
    const float determinant = (sx4 * sy4) - (sx2y2 * sx2y2);
    if (std::abs(determinant) <= 1e-12f * sx4 * sy4) {
        return std::nullopt;
    }
    const float coefficientA = ((sx2 * sy4) - (sy2 * sx2y2)) / determinant;
    const float coefficientB = ((sy2 * sx4) - (sx2 * sx2y2)) / determinant;
    if (coefficientA <= 0.0f || coefficientB <= 0.0f) {
        return std::nullopt;
    }
    float radiusA = 1.0f / std::sqrt(coefficientA);
    float radiusB = 1.0f / std::sqrt(coefficientB);

    FitError error;
    for (const Vec2 sample : samples) {
        const Vec2 p = frame(sample, major);
        const float rho = std::sqrt((p.x * p.x * coefficientA) + (p.y * p.y * coefficientB));
        const float distance = rho > 0.0f ? Length(p) * std::abs(1.0f - (1.0f / rho)) : std::min(radiusA, radiusB);
        error.mean += distance;
        error.max = std::max(error.max, distance);
    }
    error.mean /= static_cast<float>(samples.size());
    if (!Acceptable(error, diagonal)) {
        return std::nullopt;
    }

    RecognizedShapeKind kind = RecognizedShapeKind::Ellipse;
    if (constrained || std::min(radiusA, radiusB) >= kCircleAxisRatio * std::max(radiusA, radiusB)) {
        kind = RecognizedShapeKind::Circle;
        radiusA = radiusB = 0.5f * (radiusA + radiusB);
    }
    // Near-axis-aligned ellipses snap to the axes.
    major = Rotate(major, AxisSnapDelta(std::atan2(major.y, major.x), 0.5f * kPi, 0.0f));
    const Vec2 minor { -major.y, major.x };

    Candidate candidate { { kind, {} }, error.mean };
    candidate.shape.points.reserve(kEllipseSegments + 1U);
    for (int segment = 0; segment <= kEllipseSegments; ++segment) {
        const float t = (static_cast<float>(segment) / static_cast<float>(kEllipseSegments)) * 2.0f * kPi;
        candidate.shape.points.push_back(
            Add(center, Add(Scale(major, radiusA * std::cos(t)), Scale(minor, radiusB * std::sin(t)))));
    }
    candidate.shape.points.back() = candidate.shape.points.front();
    return candidate;
}

// Four corners become an exact rectangle: the sides' mean orientation and the
// corners' mean extents.
std::vector<Vec2> SnapRectangle(std::span<const Vec2> corners) {
    // Side directions modulo 90 degrees, averaged on the 4x angle circle.
    Vec2 sum {};
    Vec2 center {};
    for (std::size_t index = 0; index < 4U; ++index) {
        const Vec2 side = Sub(corners[(index + 1U) % 4U], corners[index]);
        const float angle = 4.0f * std::atan2(side.y, side.x);
        sum = Add(sum, Scale(Vec2 { std::cos(angle), std::sin(angle) }, Length(side)));
        center = Add(center, corners[index]);
    }
    center = Scale(center, 0.25f);
    float orientation = 0.25f * std::atan2(sum.y, sum.x);
    orientation += AxisSnapDelta(orientation, 0.5f * kPi, 0.0f);
    const Vec2 u { std::cos(orientation), std::sin(orientation) };
    const Vec2 v { -u.y, u.x };
    std::array<float, 4> alongU {};
    std::array<float, 4> alongV {};
    for (std::size_t index = 0; index < 4U; ++index) {
        alongU[index] = Dot(Sub(corners[index], center), u);
        alongV[index] = Dot(Sub(corners[index], center), v);
    }
    std::sort(alongU.begin(), alongU.end());
    std::sort(alongV.begin(), alongV.end());
    float halfWidth = 0.25f * ((alongU[2] + alongU[3]) - (alongU[0] + alongU[1]));
    float halfHeight = 0.25f * ((alongV[2] + alongV[3]) - (alongV[0] + alongV[1]));
    if (std::min(halfWidth, halfHeight) >= kSquareSideRatio * std::max(halfWidth, halfHeight)) {
        halfWidth = halfHeight = 0.5f * (halfWidth + halfHeight);
    }
    const Vec2 du = Scale(u, halfWidth);
    const Vec2 dv = Scale(v, halfHeight);
    return std::vector<Vec2> {
        Sub(Sub(center, du), dv),
        Sub(Add(center, du), dv),
        Add(Add(center, du), dv),
        Add(Sub(center, du), dv),
    };
}

// A constrained triangle: equilateral (centroid and mean circumradius) when
// every angle is within kEquilateralTolerance of 60 degrees, else a right
// triangle squared at its widest corner with the legs' drawn lengths.  Either
// snaps level when a side (equilateral) or a leg (right) is near an axis.
std::vector<Vec2> SnapTriangle(std::span<const Vec2> corners) {
    std::array<float, 3> angles {};
    for (std::size_t index = 0; index < 3U; ++index) {
        const Vec2 corner = corners[index];
        angles[index] = AngleBetween(Sub(corners[(index + 2U) % 3U], corner), Sub(corners[(index + 1U) % 3U], corner));
    }
    std::vector<Vec2> snapped(corners.begin(), corners.end());

    const bool equilateral = std::all_of(angles.begin(), angles.end(),
        [](float angle) { return std::abs(angle - (kPi / 3.0f)) <= kEquilateralTolerance; });
    if (equilateral) {
        const Vec2 center = Scale(Add(Add(corners[0], corners[1]), corners[2]), 1.0f / 3.0f);
        float radius = 0.0f;
        Vec2 sum {};
        for (const Vec2 corner : corners) {
            const Vec2 d = Sub(corner, center);
            radius += Length(d) / 3.0f;
            const float angle = 3.0f * std::atan2(d.y, d.x);
            sum = Add(sum, Vec2 { std::cos(angle), std::sin(angle) });
        }
        // Vertex directions modulo 120 degrees; a side is level when they sit
        // at 30 degrees modulo 60.
        float base = std::atan2(sum.y, sum.x) / 3.0f;
        base += AxisSnapDelta(base, kPi / 3.0f, kPi / 6.0f);
        const float step = 2.0f * kPi / 3.0f;
        for (std::size_t index = 0; index < 3U; ++index) {
            const Vec2 d = Sub(corners[index], center);
            const float angle = base + (std::round((std::atan2(d.y, d.x) - base) / step) * step);
            snapped[index] = Add(center, Scale(Vec2 { std::cos(angle), std::sin(angle) }, radius));
        }
        return snapped;
    }

    const std::size_t right = static_cast<std::size_t>(std::max_element(angles.begin(), angles.end()) - angles.begin());
    const std::size_t first = (right + 1U) % 3U;
    const std::size_t second = (right + 2U) % 3U;
    const Vec2 apex = corners[right];
    const Vec2 legA = Sub(corners[first], apex);
    const Vec2 legB = Sub(corners[second], apex);
    // Both legs turn symmetrically about the corner's bisector to 45 degrees
    // either side of it.
    const Vec2 bisector = Normalized(Add(Normalized(legA), Normalized(legB)));
    const float turn = Cross(bisector, legA) >= 0.0f ? 0.25f * kPi : -0.25f * kPi;
    Vec2 directionA = Rotate(bisector, turn);
    const float level = AxisSnapDelta(std::atan2(directionA.y, directionA.x), 0.5f * kPi, 0.0f);
    directionA = Rotate(directionA, level);
    const Vec2 directionB = Rotate(bisector, level - turn);
    snapped[first] = Add(apex, Scale(directionA, Length(legA)));
    snapped[second] = Add(apex, Scale(directionB, Length(legB)));
    return snapped;
}

std::optional<Candidate> FitPolygon(std::span<const Vec2> samples, float diagonal, bool constrained) {
    // Simplify the cyclic outline from sample 0 to the sample farthest from
    // it and back (index `count` is sample 0 again).
    const std::size_t count = samples.size();
    std::size_t farthest = 0;
    for (std::size_t index = 1; index < count; ++index) {
        if (Distance(samples[index], samples[0]) > Distance(samples[farthest], samples[0])) {
            farthest = index;
        }
    }
    if (farthest == 0U) {
        return std::nullopt;
    }
    const float tolerance = kCornerTolerance * diagonal;
    std::vector<std::size_t> corners { 0U };
    Simplify(samples, 0U, farthest, tolerance, corners);
    corners.push_back(farthest);
    Simplify(samples, farthest, count, tolerance, corners);
    DropWeakCorners(samples, corners, true);
    if (corners.size() < 3U || corners.size() > 4U) {
        return std::nullopt;
    }

    // Each corner moves to the intersection of the lines fitted to its two
    // sides (their middle 60%), so a rounded drawn corner snaps sharp.
    const std::size_t sides = corners.size();
    std::vector<LineFit> lines;
    lines.reserve(sides);
    std::vector<Vec2> span;
    for (std::size_t side = 0; side < sides; ++side) {
        const std::size_t first = corners[side];
        const std::size_t length = (corners[(side + 1U) % sides] + count - first) % count;
        const std::size_t trim = length / 5U;
        span.clear();
        for (std::size_t offset = trim; offset <= length - trim; ++offset) {
            span.push_back(samples[(first + offset) % count]);
        }
        lines.push_back(FitLine(span));
    }
    std::vector<Vec2> vertices;
    vertices.reserve(sides);
    for (std::size_t side = 0; side < sides; ++side) {
        const LineFit& incoming = lines[(side + sides - 1U) % sides];
        const LineFit& outgoing = lines[side];
        const float sine = Cross(incoming.direction, outgoing.direction);
        if (std::abs(sine) < kMinIntersectionSine) {
            vertices.push_back(samples[corners[side]]);
            continue;
        }
        const float t = Cross(Sub(outgoing.center, incoming.center), outgoing.direction) / sine;
        vertices.push_back(Add(incoming.center, Scale(incoming.direction, t)));
    }

    std::vector<Vec2> fitted = vertices;
    fitted.push_back(vertices.front());
    const FitError error = PolylineError(samples, fitted);
    if (!Acceptable(error, diagonal)) {
        return std::nullopt;
    }

    RecognizedShapeKind kind = RecognizedShapeKind::Rectangle;
    if (sides == 3U) {
        kind = RecognizedShapeKind::Triangle;
        if (constrained) {
            vertices = SnapTriangle(vertices);
        }
    } else {
        vertices = SnapRectangle(vertices);
    }
    return Candidate { { kind, ClosedOutline(vertices) }, error.mean };
}

// The closed part of a stroke: an overshoot past the start, or a lead-in
// before the point the stroke ends at, is cut at the point nearest the
// other end.
std::vector<Vec2> TrimClosure(std::vector<Vec2> samples) {
    std::size_t end = samples.size() - 1U;
    for (std::size_t index = (2U * samples.size()) / 3U; index < samples.size(); ++index) {
        if (Distance(samples[index], samples.front()) < Distance(samples[end], samples.front())) {
            end = index;
        }
    }
    samples.resize(end + 1U);
    std::size_t begin = 0;
    for (std::size_t index = 1; index <= samples.size() / 3U; ++index) {
        if (Distance(samples[index], samples.back()) < Distance(samples[begin], samples.back())) {
            begin = index;
        }
    }
    samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(begin));
    return samples;
}

}  // namespace

std::optional<RecognizedShape> RecognizeShape(const std::vector<Vec2>& points, bool constrained) {
    if (points.size() < 2U || PathLength(points) < kMinStrokeLength) {
        return std::nullopt;
    }
    const std::vector<Vec2> samples = Resample(points, kSampleCount);
    const float length = PathLength(samples);

    const std::vector<Vec2> closedPart = TrimClosure(samples);
    const float closedLength = PathLength(closedPart);
    if (closedPart.size() >= 8U && closedLength >= kMinStrokeLength
        && Distance(closedPart.front(), closedPart.back()) <= kClosedGapRatio * closedLength) {
        const std::vector<Vec2> outline = Resample(closedPart, kSampleCount);
        const float diagonal = Diagonal(outline);
        std::optional<Candidate> ellipse = FitEllipse(outline, diagonal, constrained);
        std::optional<Candidate> polygon = FitPolygon(outline, diagonal, constrained);
        if (ellipse.has_value() && (!polygon.has_value() || ellipse->meanError <= polygon->meanError)) {
            return std::move(ellipse->shape);
        }
        if (polygon.has_value()) {
            return std::move(polygon->shape);
        }
        return std::nullopt;
    }

    // A shallow brace is also nearly straight, so it is tested first; its
    // sharp, bowed middle rules out a wobbly line.
    if (std::optional<RecognizedShape> brace = FitBrace(samples)) {
        return brace;
    }
    if (std::optional<RecognizedShape> line = FitStraightLine(samples, length)) {
        return line;
    }
    return FitArrow(samples, Diagonal(samples));
}

}  // namespace cnote
