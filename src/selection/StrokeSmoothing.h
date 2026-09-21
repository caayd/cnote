#pragma once

#include <vector>

#include "Document.h"

namespace cnote {

// Pure stroke geometry shared by the renderers and the PDF exporters. Kept in
// a portable TU so the model-side services do not pull in the Qt canvas.
[[nodiscard]] std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength);
[[nodiscard]] float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing);

}  // namespace cnote
