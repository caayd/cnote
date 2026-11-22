#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "Document.h"

namespace cnote {

// Geometric shape recognition for one freehand stroke (hold-to-snap).
//
// Every candidate shape is fitted to the stroke and the best fit wins when its
// error, normalized by the stroke's size, is under that shape's threshold.
// There is no classifier: the fit itself yields the exact geometry the snapped
// stroke is drawn with, and a stroke nothing fits well is left alone.
//
// Open strokes are tested as a line or a one-stroke arrow (shaft, one barb,
// back to the tip, the other barb).  Closed strokes are tested as an ellipse
// and as a polygon of 3 or 4 corners; the lower error wins.  An outline with
// more corners is only ever an ellipse, so a lumpy oval never snaps to a
// many-sided polygon.
enum class RecognizedShapeKind : std::uint8_t {
    Line,
    Arrow,
    Ellipse,
    Circle,
    Triangle,
    Rectangle,  // any four corners, squared into a rectangle or square, rotated or not
};

struct RecognizedShape {
    RecognizedShapeKind kind = RecognizedShapeKind::Line;
    // The shape's outline in the input's units, ready to replace the stroke's
    // points.  Straight edges are subdivided densely, so the render-time
    // smoothing (BuildSmoothedPath) keeps corners sharp; closed shapes end on
    // their first point.
    std::vector<Vec2> points;
};

// `constrained` (Shift held) perfects the shape: an ellipse becomes a circle,
// and a triangle equilateral when its angles are all near 60 degrees, else a
// right triangle squared at its widest corner.  Four corners always square up.
[[nodiscard]] std::optional<RecognizedShape> RecognizeShape(const std::vector<Vec2>& points, bool constrained = false);

}  // namespace cnote
