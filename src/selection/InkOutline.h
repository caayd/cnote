#pragma once

#include <cstdint>
#include <vector>

#include "Document.h"
#include "selection/StrokeSmoothing.h"

namespace cnote {

// Pressure-width ink as filled geometry, shared by the canvas, the page
// renderer and the PDF exporter.  A flattened centerline with a radius per
// vertex becomes a set of convex pieces whose union is the swept ink: a
// tapered quad (the external tangents of two discs) per segment, a round cap
// at each end and an arc fan wherever the outline turns outward between two
// segments.  The pieces only share edges on the ink's outer boundary end to
// end, never overlap along it, so an antialiased nonzero (winding) fill shows
// neither seams nor thickened edges.  All pieces wind the same way, so their
// interior overlaps (inside of turns) add up instead of cutting holes.
// Coordinates are in whatever units the caller maps the centerline to.

// Ink diameter relative to the stroke's nominal width at `pressure`
// (perfect-freehand's thinning 0.6, the value Excalidraw ships): 1 at half
// pressure, 0.4 with none, 1.6 at full.
[[nodiscard]] float InkWidthFactor(float pressure);

// Flat polygon list: polygon i is vertices[polygonEnds[i-1] .. polygonEnds[i]).
struct InkOutline {
    std::vector<Vec2> vertices;
    std::vector<std::uint32_t> polygonEnds;
};

// A flattened centerline with the ink radius at each vertex.
struct InkCenterline {
    std::vector<Vec2> points;
    std::vector<float> radii;

    // Appends a vertex; one that repeats the last position keeps the larger
    // radius instead of adding a zero-length segment.
    void Add(Vec2 point, float radius);
};

// `tolerance` is the largest distance a flattened arc or curve may stray
// from the exact shape, in the centerline's units.

// Appends the quadratic Bezier start -> (control) -> end, radii interpolated
// the same way, flattened into vertices (start included).
void AppendInkQuadratic(InkCenterline& centerline, Vec2 start, float radiusStart, Vec2 control,
    float radiusControl, Vec2 end, float radiusEnd, float tolerance);

// Appends the ink swept along `centerline`, capped at both ends (a disc for a
// single vertex).
void AppendInkOutline(InkOutline& outline, const InkCenterline& centerline, float tolerance);

// The centerline of a rendered `path` (pressures required), scaled by
// `scale`, following the same quadratic midpoint chain the constant-width pen
// draws.  `diameter(pressure)` maps a pressure to an ink diameter in output
// units.
template <typename DiameterFn>
InkCenterline BuildInkCenterline(const InkPath& path, float scale, DiameterFn diameter, float tolerance) {
    InkCenterline centerline;
    const std::vector<Vec2>& points = path.points;
    if (points.empty() || path.pressures.size() != points.size()) {
        return centerline;
    }
    const auto at = [&points, scale](std::size_t index) {
        return Vec2 { points[index].x * scale, points[index].y * scale };
    };
    const auto radius = [&path, &diameter](std::size_t index) {
        return 0.5f * diameter(path.pressures[index]);
    };
    centerline.Add(at(0), radius(0));
    Vec2 start = at(0);
    float startRadius = radius(0);
    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 control = at(index);
        const Vec2 next = at(index + 1);
        const Vec2 end { (control.x + next.x) * 0.5f, (control.y + next.y) * 0.5f };
        const float endRadius = (radius(index) + radius(index + 1)) * 0.5f;
        AppendInkQuadratic(centerline, start, startRadius, control, radius(index), end, endRadius, tolerance);
        start = end;
        startRadius = endRadius;
    }
    if (points.size() > 1U) {
        centerline.Add(at(points.size() - 1U), radius(points.size() - 1U));
    }
    return centerline;
}

}  // namespace cnote
