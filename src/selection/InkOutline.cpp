#include "selection/InkOutline.h"

#include <algorithm>
#include <cmath>

namespace cnote {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kThinning = 0.6f;
constexpr int kMaxArcSegments = 64;
constexpr int kMaxCurveSegments = 32;

float Cross(Vec2 a, Vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

float Dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

float PositiveAngle(float angle) {
    const float wrapped = std::fmod(angle, kTwoPi);
    return wrapped < 0.0f ? wrapped + kTwoPi : wrapped;
}

// Closes the polygon begun at `begin`, reversed if needed so every piece has
// positive (shoelace) winding: a nonzero fill then unites overlapping pieces
// instead of cutting their overlap out.
void ClosePolygon(InkOutline& outline, std::size_t begin) {
    float area = 0.0f;
    const std::size_t end = outline.vertices.size();
    for (std::size_t i = begin; i < end; ++i) {
        const Vec2 p = outline.vertices[i];
        const Vec2 q = outline.vertices[i + 1U < end ? i + 1U : begin];
        area += Cross(p, q);
    }
    if (area < 0.0f) {
        std::reverse(outline.vertices.begin() + static_cast<std::ptrdiff_t>(begin), outline.vertices.end());
    }
    outline.polygonEnds.push_back(static_cast<std::uint32_t>(end));
}

// Angle step whose chord stays within `tolerance` of a circle of `radius`.
float ArcStep(float radius, float tolerance) {
    const float ratio = std::clamp(1.0f - (std::max(tolerance, 1e-4f) / radius), -1.0f, 1.0f);
    return std::max(2.0f * std::acos(ratio), 1e-3f);
}

void AppendDisc(InkOutline& outline, Vec2 center, float radius, float tolerance) {
    if (!(radius > 0.0f)) {
        return;
    }
    const int segments = std::clamp(static_cast<int>(std::ceil(kTwoPi / ArcStep(radius, tolerance))), 8, kMaxArcSegments);
    const std::size_t begin = outline.vertices.size();
    for (int i = 0; i < segments; ++i) {
        const float angle = (kTwoPi * static_cast<float>(i)) / static_cast<float>(segments);
        outline.vertices.push_back(Vec2 { center.x + (radius * std::cos(angle)), center.y + (radius * std::sin(angle)) });
    }
    ClosePolygon(outline, begin);
}

// The pie slice of the disc at `center` from angle `from`, sweeping `sweep`
// radians (signed).
void AppendFan(InkOutline& outline, Vec2 center, float radius, float from, float sweep, float tolerance) {
    if (!(radius > 0.0f) || std::abs(sweep) < 1e-6f) {
        return;
    }
    const int segments = std::clamp(
        static_cast<int>(std::ceil(std::abs(sweep) / ArcStep(radius, tolerance))), 1, kMaxArcSegments);
    const std::size_t begin = outline.vertices.size();
    outline.vertices.push_back(center);
    for (int i = 0; i <= segments; ++i) {
        const float angle = from + (sweep * static_cast<float>(i) / static_cast<float>(segments));
        outline.vertices.push_back(Vec2 { center.x + (radius * std::cos(angle)), center.y + (radius * std::sin(angle)) });
    }
    ClosePolygon(outline, begin);
}

// A round cap: the arc from normal `from` to normal `to` that passes through
// direction `through`.
void AppendCap(InkOutline& outline, Vec2 center, float radius, Vec2 from, Vec2 to, Vec2 through, float tolerance) {
    const float start = std::atan2(from.y, from.x);
    const float positive = PositiveAngle(std::atan2(to.y, to.x) - start);
    const bool passes = PositiveAngle(std::atan2(through.y, through.x) - start) <= positive;
    AppendFan(outline, center, radius, start, passes ? positive : positive - kTwoPi, tolerance);
}

// The triangle from a vertex to one segment's tangent points there.  When the
// radius tapers at a different rate on each side of the vertex, the two
// segments' end edges sit at different offsets along the centerline and leave
// a sliver between them (and between a cap and its segment); these triangles
// fill it.  Their outer edge is the segment's own end edge, so nothing new
// lands on the ink's boundary.
void AppendHub(InkOutline& outline, Vec2 center, float radius, Vec2 w1, Vec2 w2) {
    const Vec2 a { center.x + (radius * w1.x), center.y + (radius * w1.y) };
    const Vec2 b { center.x + (radius * w2.x), center.y + (radius * w2.y) };
    if (std::abs(Cross(Vec2 { a.x - center.x, a.y - center.y }, Vec2 { b.x - center.x, b.y - center.y })) < 1e-6f) {
        return;
    }
    const std::size_t begin = outline.vertices.size();
    outline.vertices.push_back(center);
    outline.vertices.push_back(a);
    outline.vertices.push_back(b);
    ClosePolygon(outline, begin);
}

struct Segment {
    Vec2 u {};
    Vec2 w1 {};   // outward normal of the tangent on the +90 degree side
    Vec2 w2 {};   // outward normal of the tangent on the -90 degree side
    bool tangent = false;  // false: one end's disc contains the other
};

}  // namespace

float InkWidthFactor(float pressure) {
    const float clamped = std::isfinite(pressure) ? std::clamp(pressure, 0.0f, 1.0f) : 0.5f;
    return 1.0f - (kThinning * (1.0f - (2.0f * clamped)));
}

void InkCenterline::Add(Vec2 point, float radius) {
    if (!points.empty()) {
        const float dx = point.x - points.back().x;
        const float dy = point.y - points.back().y;
        if ((dx * dx) + (dy * dy) < 1e-8f) {
            radii.back() = std::max(radii.back(), radius);
            return;
        }
    }
    points.push_back(point);
    radii.push_back(radius);
}

void AppendInkQuadratic(InkCenterline& centerline, Vec2 start, float radiusStart, Vec2 control,
    float radiusControl, Vec2 end, float radiusEnd, float tolerance) {
    // A quadratic's chord error with n uniform steps is |S - 2C + E| / (4 n^2).
    const float bendX = start.x - (2.0f * control.x) + end.x;
    const float bendY = start.y - (2.0f * control.y) + end.y;
    const float bend = std::sqrt((bendX * bendX) + (bendY * bendY));
    const int segments = std::clamp(
        static_cast<int>(std::ceil(std::sqrt(bend / (4.0f * std::max(tolerance, 1e-4f))))), 1, kMaxCurveSegments);
    centerline.Add(start, radiusStart);
    for (int i = 1; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float a = (1.0f - t) * (1.0f - t);
        const float b = 2.0f * t * (1.0f - t);
        const float c = t * t;
        centerline.Add(
            Vec2 { (a * start.x) + (b * control.x) + (c * end.x), (a * start.y) + (b * control.y) + (c * end.y) },
            (a * radiusStart) + (b * radiusControl) + (c * radiusEnd));
    }
}

void AppendInkOutline(InkOutline& outline, const InkCenterline& centerline, float tolerance) {
    const std::vector<Vec2>& points = centerline.points;
    const std::vector<float>& radii = centerline.radii;
    const std::size_t count = points.size();
    if (count == 0U || radii.size() != count) {
        return;
    }
    if (count == 1U) {
        AppendDisc(outline, points.front(), radii.front(), tolerance);
        return;
    }

    std::vector<Segment> segments(count - 1U);
    for (std::size_t i = 0; i + 1U < count; ++i) {
        const Vec2 a = points[i];
        const Vec2 b = points[i + 1U];
        const float ra = radii[i];
        const float rb = radii[i + 1U];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float distance = std::sqrt((dx * dx) + (dy * dy));
        Segment& segment = segments[i];
        segment.u = Vec2 { dx / distance, dy / distance };
        segment.tangent = distance > std::abs(ra - rb) + 1e-6f;
        if (!segment.tangent) {
            // One disc holds the other: the larger disc is the whole piece.
            AppendDisc(outline, ra >= rb ? a : b, std::max(ra, rb), tolerance);
            continue;
        }
        // The outward normal w of an external tangent satisfies
        // w . (b - a) = ra - rb, so w = u*s +/- n*c.
        const Vec2 n { -segment.u.y, segment.u.x };
        const float s = (ra - rb) / distance;
        const float c = std::sqrt(std::max(0.0f, 1.0f - (s * s)));
        segment.w1 = Vec2 { (segment.u.x * s) + (n.x * c), (segment.u.y * s) + (n.y * c) };
        segment.w2 = Vec2 { (segment.u.x * s) - (n.x * c), (segment.u.y * s) - (n.y * c) };
        const std::size_t begin = outline.vertices.size();
        outline.vertices.push_back(Vec2 { a.x + (ra * segment.w1.x), a.y + (ra * segment.w1.y) });
        outline.vertices.push_back(Vec2 { b.x + (rb * segment.w1.x), b.y + (rb * segment.w1.y) });
        outline.vertices.push_back(Vec2 { b.x + (rb * segment.w2.x), b.y + (rb * segment.w2.y) });
        outline.vertices.push_back(Vec2 { a.x + (ra * segment.w2.x), a.y + (ra * segment.w2.y) });
        ClosePolygon(outline, begin);
    }

    // Caps: the arc between the end tangents around the outside of the end.
    const Segment& first = segments.front();
    if (first.tangent) {
        AppendCap(outline, points.front(), radii.front(), first.w1, first.w2, Vec2 { -first.u.x, -first.u.y }, tolerance);
        AppendHub(outline, points.front(), radii.front(), first.w1, first.w2);
    } else {
        AppendDisc(outline, points.front(), radii.front(), tolerance);
    }
    const Segment& last = segments.back();
    if (last.tangent) {
        AppendCap(outline, points.back(), radii.back(), last.w2, last.w1, last.u, tolerance);
        AppendHub(outline, points.back(), radii.back(), last.w1, last.w2);
    } else {
        AppendDisc(outline, points.back(), radii.back(), tolerance);
    }

    // Joins: hub triangles close the sliver between the two segments' end
    // edges, and where the outline turns outward the pie slice between their
    // tangents closes the gap.  Inner sides overlap inside the ink.
    for (std::size_t i = 1; i + 1U < count; ++i) {
        const Segment& before = segments[i - 1U];
        const Segment& after = segments[i];
        if (!before.tangent || !after.tangent || Dot(before.u, after.u) < -0.999f) {
            // A contained disc or a full reversal: the vertex's whole disc.
            AppendDisc(outline, points[i], radii[i], tolerance);
            continue;
        }
        AppendHub(outline, points[i], radii[i], before.w1, before.w2);
        AppendHub(outline, points[i], radii[i], after.w1, after.w2);
        if (Cross(before.w1, after.w1) < 0.0f) {
            const float from = std::atan2(before.w1.y, before.w1.x);
            AppendFan(outline, points[i], radii[i], from,
                -PositiveAngle(from - std::atan2(after.w1.y, after.w1.x)), tolerance);
        }
        if (Cross(before.w2, after.w2) > 0.0f) {
            const float from = std::atan2(before.w2.y, before.w2.x);
            AppendFan(outline, points[i], radii[i], from,
                PositiveAngle(std::atan2(after.w2.y, after.w2.x) - from), tolerance);
        }
    }
}

}  // namespace cnote
