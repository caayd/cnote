#pragma once

#include <vector>

#include "Document.h"

namespace cnote {

// Pure stroke geometry shared by the renderers and the PDF exporters. Kept in
// a portable TU so services do not pull in the Win32 selection layer.
[[nodiscard]] std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength);
[[nodiscard]] float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing);

}  // namespace cnote
