#pragma once

#include <array>
#include <optional>
#include <vector>

#include "App.h"

namespace cnote {

float DistanceSquared(Vec2 a, Vec2 b);
float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b);
float CrossProduct(Vec2 a, Vec2 b, Vec2 c);
bool OnSegment(Vec2 a, Vec2 b, Vec2 point);
bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2);
bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon);
std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength);
float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing);
std::vector<Vec2> StrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing);
Vec2 PointToVec2(POINT point);
Vec2 RemapPagePointBetweenViews(const App::PageView& fromView, const App::PageView& toView, Vec2 point);

bool StrokeBounds(const Stroke& stroke, float* minX, float* minY, float* maxX, float* maxY);
void TranslateStrokePoints(Stroke& stroke, Vec2 delta);
Vec2 ClampStrokeDeltaToPage(const Stroke& stroke, const Page& page, Vec2 delta);
Vec2 ClampImageDeltaToPage(const PageImage& image, const Page& page, Vec2 delta);
Vec2 ClampLatexDeltaToPage(const PageLatex& latex, const Page& page, Vec2 delta);

struct PageLatexBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

PageLatexBounds MeasurePageLatexBounds(const PageLatex& latex, float pageWidth);
std::optional<RECT> PageLatexBoundsRect(const PageLatex& latex, float pageWidth, float scale, int extraPadding = 0);
bool LatexHitsPoint(const PageLatex& latex, float pageWidth, Vec2 point);
bool LatexIntersectsRectSelection(const PageLatex& latex, float pageWidth, Vec2 a, Vec2 b);
bool LatexIntersectsPolygonSelection(const PageLatex& latex, float pageWidth, const std::vector<Vec2>& polygon);

float SelectionHandleHitRadiusPixels(UINT dpi);
float SelectionRotateOffsetPageUnits(float pageScale, UINT dpi);

bool RectContainsPointWithPadding(RECT rect, POINT point, int padding);
bool PolygonContainsPoint(const std::vector<Vec2>& polygon, POINT point);

bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius, float fallbackSmoothing);
bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b, float fallbackSmoothing);
bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon, float fallbackSmoothing);

Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees);
Vec2 ImageCenter(const PageImage& image);
std::array<Vec2, 4> ImageCorners(const PageImage& image);
Vec2 ScreenPointFromPagePoint(Vec2 point, RECT pageRect, float pageScale);
bool ImageHitsPoint(const PageImage& image, Vec2 point);
bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b);
bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon);

}  // namespace cnote
