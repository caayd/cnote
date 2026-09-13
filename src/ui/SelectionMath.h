#pragma once

#include <array>
#include <optional>
#include <vector>

#include "Document.h"
#include "selection/StrokeSmoothing.h"

namespace cnote::ui {

// Portable selection/geometry math for the Qt canvas (plan steps 5.2/5.3).
//
// Direct port of the pure-math half of src/selection/Geometry.cpp, which is
// bound to App/HWND/RECT (UiUtil.h includes <windows.h>) and therefore cannot be
// linked into the Qt target.  Function-by-function the numbers match the legacy
// implementation (cited per function below and in the .cpp); the Win32
// RECT/POINT wrappers stay behind in the legacy TU and disappear at cutover
// (plan step 7.1), at which point this file is the single selection math
// implementation.
//
// Reference constants, all 96-DPI logical values (Geometry.cpp:23-27).
inline constexpr int kSelectionHandleHalfDip = 4;
inline constexpr float kSelectionHandleHitRadiusDip = 11.0f;
inline constexpr float kSelectionRotateOffsetDip = 18.0f;
inline constexpr float kPi = 3.14159265358979323846f;

float DistanceSquared(Vec2 a, Vec2 b);
float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b);
float CrossProduct(Vec2 a, Vec2 b, Vec2 c);
bool OnSegment(Vec2 a, Vec2 b, Vec2 point);
bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2);
bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon);
bool PointInRectF(Vec2 point, float left, float top, float right, float bottom);

// Geometry.cpp:33 StrokeRenderPoints.
[[nodiscard]] std::vector<Vec2> StrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing);

bool StrokeBounds(const Stroke& stroke, float* minX, float* minY, float* maxX, float* maxY);
void TranslateStrokePoints(Stroke& stroke, Vec2 delta);
Vec2 ClampStrokeDeltaToPage(const Stroke& stroke, const Page& page, Vec2 delta);
Vec2 ClampImageDeltaToPage(const PageImage& image, const Page& page, Vec2 delta);
Vec2 ClampLatexDeltaToPage(const PageLatex& latex, const Page& page, Vec2 delta);

struct LatexBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

// Geometry.cpp:218 MeasurePageLatexBounds.
[[nodiscard]] LatexBounds MeasurePageLatexBounds(const PageLatex& latex, float pageWidth);

float SelectionHandleHitRadiusPixels(int dpi);
float SelectionRotateOffsetPageUnits(float pageScale, int dpi);

bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius, float fallbackSmoothing);
bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b, float fallbackSmoothing);
bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon, float fallbackSmoothing);

bool LatexHitsPoint(const PageLatex& latex, float pageWidth, Vec2 point);
bool LatexIntersectsRectSelection(const PageLatex& latex, float pageWidth, Vec2 a, Vec2 b);
bool LatexIntersectsPolygonSelection(const PageLatex& latex, float pageWidth, const std::vector<Vec2>& polygon);

Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees);
Vec2 ImageCenter(const PageImage& image);
[[nodiscard]] std::array<Vec2, 4> ImageCorners(const PageImage& image);
bool ImageHitsPoint(const PageImage& image, Vec2 point);
bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b);
bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon);

// Bounds of a selected object set in page units; used for the selection frame,
// handles and the move grip (legacy App::SelectionBounds).
struct SelectionBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool valid = false;
};

}  // namespace cnote::ui
