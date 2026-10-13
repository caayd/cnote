#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "selection/StrokeSmoothing.h"

namespace cnote::ui {

// Portable selection/geometry math for the Qt canvas (plan steps 5.2/5.3).
//
// Direct port of the pure-math half of src/selection/Geometry.cpp, which is
// bound to App/POINT/RECT (UiUtil.h includes <windows.h>) and therefore cannot
// be linked into the Qt target.  Every function cites the legacy function it
// ports and the formulas match it exactly; the Windows-only
// tests/SelectionMathTests.cpp compares against the legacy implementation
// directly.  The math is header-only so the canvas target does not need another
// translation unit in its source list.
//
// Reference constants, all 96-DPI logical values (Geometry.cpp:23-27).
inline constexpr int kSelectionHandleHalfDip = 4;
inline constexpr float kSelectionHandleHitRadiusDip = 11.0f;
inline constexpr float kSelectionRotateOffsetDip = 18.0f;
inline constexpr float kPi = 3.14159265358979323846f;



// Declarations first: the anonymous-namespace helpers below use these.
float DistanceSquared(Vec2 a, Vec2 b);
float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b);
float CrossProduct(Vec2 a, Vec2 b, Vec2 c);
bool OnSegment(Vec2 a, Vec2 b, Vec2 point);
bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2);
bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon);
bool PointInRectF(Vec2 point, float left, float top, float right, float bottom);
std::vector<Vec2> StrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing);
bool StrokeBounds(const Stroke& stroke, float* minX, float* minY, float* maxX, float* maxY);
void TranslateStrokePoints(Stroke& stroke, Vec2 delta);
Vec2 ClampStrokeDeltaToPage(const Stroke& stroke, const Page& page, Vec2 delta);
Vec2 ClampImageDeltaToPage(const PageImage& image, const Page& page, Vec2 delta);
Vec2 ClampLatexDeltaToPage(const PageLatex& latex, const Page& page, Vec2 delta);
float SelectionHandleHitRadiusPixels(int dpi);
float SelectionRotateOffsetPageUnits(float pageScale, int dpi);
bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius, float fallbackSmoothing);
bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b, float fallbackSmoothing);
float DistanceSegmentToSegmentSquared(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2);
bool CircleHitsRect(float left, float top, float right, float bottom, Vec2 center, float radius);
bool SegmentHitsRect(float left, float top, float right, float bottom, Vec2 a, Vec2 b, float radius);
bool ImageHitsSegment(const PageImage& image, Vec2 a, Vec2 b, float radius);
bool StrokeHitsSegment(const Stroke& stroke, Vec2 a, Vec2 b, float radius, float fallbackSmoothing);
bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon, float fallbackSmoothing);
bool LatexHitsPoint(const PageLatex& latex, float pageWidth, Vec2 point);
bool LatexIntersectsRectSelection(const PageLatex& latex, float pageWidth, Vec2 a, Vec2 b);
bool LatexIntersectsPolygonSelection(const PageLatex& latex, float pageWidth, const std::vector<Vec2>& polygon);
Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees);
Vec2 ImageCenter(const PageImage& image);
std::array<Vec2, 4> ImageCorners(const PageImage& image);
bool ImageHitsPoint(const PageImage& image, Vec2 point);
bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b);
bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon);

struct LatexBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

LatexBounds MeasurePageLatexBounds(const PageLatex& latex, float pageWidth);

// Bounds of a selected object set in page units; used for the selection frame,
// handles and the move grip (legacy App::SelectionBounds).
struct SelectionBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool valid = false;
};

// Maps a page-space point through the selection transform.  New helper for the
// Qt transform gesture; it has no legacy counterpart (the reference applies its
// handle transforms per object kind inside App).  Translates `from` to the
// origin, scales by the `to`/`from` extent ratio, translates back to `to`, then
// rotates about the `from` centre.
Vec2 MapSelectionTransformPoint(Vec2 point, const SelectionBounds& from, const SelectionBounds& to, float rotateDegrees);

namespace {

// DpiUtil::ScaleForDpi in logical pixels (the Qt canvas is a 96-DPI reference
// device), kept explicit so the ported formulas stay readable.
int ScaleForDpi(int value, int dpi) {
    return (value * dpi) / 96;
}

inline float ScaleForDpi(float value, int dpi) {
    return value * static_cast<float>(dpi) / 96.0f;
}

inline Vec2 ToImageLocalPoint(const PageImage& image, Vec2 point) {
    const Vec2 center = ImageCenter(image);
    return RotatePointAround(point, center, -image.rotationDegrees);
}

inline bool SegmentIntersectsRectF(Vec2 a, Vec2 b, float left, float top, float right, float bottom);

// Generic rotated-rect probes for the page-rotation feature: texts and LaTeX
// blocks carry a rotationDegrees about their bounds centre exactly like
// images.  `left/top/right/bottom` is the unrotated (object-space) rect.

inline std::array<Vec2, 4> RotatedRectCorners(float left, float top, float right, float bottom, float rotationDegrees) {
    std::array<Vec2, 4> corners {{
        { left, top }, { right, top }, { right, bottom }, { left, bottom },
    }};
    const Vec2 center { (left + right) * 0.5f, (top + bottom) * 0.5f };
    for (Vec2& corner : corners) {
        corner = RotatePointAround(corner, center, rotationDegrees);
    }
    return corners;
}

inline bool PointInRotatedRect(Vec2 point, float left, float top, float right, float bottom, float rotationDegrees) {
    const Vec2 center { (left + right) * 0.5f, (top + bottom) * 0.5f };
    const Vec2 local = RotatePointAround(point, center, -rotationDegrees);
    return local.x >= left && local.x <= right && local.y >= top && local.y <= bottom;
}

// Axis-aligned bounds of the rotated rect (the selection frame encloses it).
inline void IncludeRotatedRect(float left, float top, float right, float bottom, float rotationDegrees,
    float& outLeft, float& outTop, float& outRight, float& outBottom) {
    const std::array<Vec2, 4> corners = RotatedRectCorners(left, top, right, bottom, rotationDegrees);
    outLeft = corners[0].x;
    outTop = corners[0].y;
    outRight = corners[0].x;
    outBottom = corners[0].y;
    for (std::size_t index = 1; index < corners.size(); ++index) {
        outLeft = std::min(outLeft, corners[index].x);
        outTop = std::min(outTop, corners[index].y);
        outRight = std::max(outRight, corners[index].x);
        outBottom = std::max(outBottom, corners[index].y);
    }
}

// Rotated rect vs axis-aligned rect: any rotated corner inside the rect, any
// rect corner inside the rotated rect, or an edge intersection.
inline bool RotatedRectIntersectsRect(float left, float top, float right, float bottom, float rotationDegrees, Vec2 a, Vec2 b) {
    const float selLeft = std::min(a.x, b.x);
    const float selTop = std::min(a.y, b.y);
    const float selRight = std::max(a.x, b.x);
    const float selBottom = std::max(a.y, b.y);
    const std::array<Vec2, 4> corners = RotatedRectCorners(left, top, right, bottom, rotationDegrees);
    for (const Vec2& corner : corners) {
        if (PointInRectF(corner, selLeft, selTop, selRight, selBottom)) {
            return true;
        }
    }
    const std::array<Vec2, 4> selCorners {{
        { selLeft, selTop }, { selRight, selTop }, { selRight, selBottom }, { selLeft, selBottom },
    }};
    for (const Vec2& corner : selCorners) {
        if (PointInRotatedRect(corner, left, top, right, bottom, rotationDegrees)) {
            return true;
        }
    }
    for (std::size_t index = 0; index < 4U; ++index) {
        if (SegmentIntersectsRectF(corners[index], corners[(index + 1U) % 4U], selLeft, selTop, selRight, selBottom)) {
            return true;
        }
    }
    return false;
}

inline bool RotatedRectIntersectsPolygon(float left, float top, float right, float bottom, float rotationDegrees,
    const std::vector<Vec2>& polygon) {
    const std::array<Vec2, 4> corners = RotatedRectCorners(left, top, right, bottom, rotationDegrees);
    for (const Vec2& corner : corners) {
        if (PointInPolygon(corner, polygon)) {
            return true;
        }
    }
    for (const Vec2& point : polygon) {
        if (PointInRotatedRect(point, left, top, right, bottom, rotationDegrees)) {
            return true;
        }
    }
    for (std::size_t index = 0; index < 4U; ++index) {
        const Vec2 edgeA = corners[index];
        const Vec2 edgeB = corners[(index + 1U) % 4U];
        for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
            if (SegmentsIntersect(edgeA, edgeB, polygon[edgeIndex], polygon[(edgeIndex + 1U) % polygon.size()])) {
                return true;
            }
        }
    }
    return false;
}

// Segment (eraser path inflated by `radius`) vs a rotated rect: approximate
// the rotated rect by its polygon edges plus a containment check.
inline bool RotatedRectHitsSegment(float left, float top, float right, float bottom, float rotationDegrees,
    Vec2 a, Vec2 b, float radius) {
    const std::array<Vec2, 4> corners = RotatedRectCorners(left, top, right, bottom, rotationDegrees);
    for (std::size_t index = 0; index < 4U; ++index) {
        if (DistanceSegmentToSegmentSquared(a, b, corners[index], corners[(index + 1U) % 4U]) <= radius * radius) {
            return true;
        }
    }
    return PointInRotatedRect(a, left, top, right, bottom, rotationDegrees)
        || PointInRotatedRect(b, left, top, right, bottom, rotationDegrees);
}

inline bool SegmentIntersectsRectF(Vec2 a, Vec2 b, float left, float top, float right, float bottom) {
    if (PointInRectF(a, left, top, right, bottom) || PointInRectF(b, left, top, right, bottom)) {
        return true;
    }
    return SegmentsIntersect(a, b, { left, top }, { right, top })
        || SegmentsIntersect(a, b, { right, top }, { right, bottom })
        || SegmentsIntersect(a, b, { right, bottom }, { left, bottom })
        || SegmentsIntersect(a, b, { left, bottom }, { left, top });
}

}  // namespace

inline float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

inline float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = (dx * dx) + (dy * dy);
    if (lengthSquared <= 0.0001f) {
        return DistanceSquared(point, a);
    }

    float t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vec2 projection { a.x + (dx * t), a.y + (dy * t) };
    return DistanceSquared(point, projection);
}

inline float CrossProduct(Vec2 a, Vec2 b, Vec2 c) {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

inline bool OnSegment(Vec2 a, Vec2 b, Vec2 point) {
    return point.x >= std::min(a.x, b.x) - 0.001f
        && point.x <= std::max(a.x, b.x) + 0.001f
        && point.y >= std::min(a.y, b.y) - 0.001f
        && point.y <= std::max(a.y, b.y) + 0.001f;
}

inline bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2) {
    const float d1 = CrossProduct(a1, a2, b1);
    const float d2 = CrossProduct(a1, a2, b2);
    const float d3 = CrossProduct(b1, b2, a1);
    const float d4 = CrossProduct(b1, b2, a2);

    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f))
        && ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f))) {
        return true;
    }
    if (std::abs(d1) <= 0.001f && OnSegment(a1, a2, b1)) {
        return true;
    }
    if (std::abs(d2) <= 0.001f && OnSegment(a1, a2, b2)) {
        return true;
    }
    if (std::abs(d3) <= 0.001f && OnSegment(b1, b2, a1)) {
        return true;
    }
    if (std::abs(d4) <= 0.001f && OnSegment(b1, b2, a2)) {
        return true;
    }
    return false;
}

// Segment-vs-segment distance and the swept-circle hit tests the eraser uses
// (App.cpp:2144-2209 DistanceSegmentToSegmentSquared, CircleHitsRect,
// SegmentHitsRect, ImageHitsSegment).  Testing the whole segment between the
// previous and current cursor samples is what keeps a fast brush stroke from
// skipping objects between mouse-move events.
inline float DistanceSegmentToSegmentSquared(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2) {
    if (SegmentsIntersect(a1, a2, b1, b2)) {
        return 0.0f;
    }
    return std::min({
        DistancePointToSegmentSquared(a1, b1, b2),
        DistancePointToSegmentSquared(a2, b1, b2),
        DistancePointToSegmentSquared(b1, a1, a2),
        DistancePointToSegmentSquared(b2, a1, a2),
    });
}

inline bool CircleHitsRect(float left, float top, float right, float bottom, Vec2 center, float radius) {
    const float closestX = std::clamp(center.x, left, right);
    const float closestY = std::clamp(center.y, top, bottom);
    const float dx = center.x - closestX;
    const float dy = center.y - closestY;
    return (dx * dx) + (dy * dy) <= radius * radius;
}

inline bool SegmentHitsRect(float left, float top, float right, float bottom, Vec2 a, Vec2 b, float radius) {
    if (CircleHitsRect(left, top, right, bottom, a, radius)
        || CircleHitsRect(left, top, right, bottom, b, radius)
        || SegmentIntersectsRectF(a, b, left, top, right, bottom)) {
        return true;
    }
    const float radiusSquared = radius * radius;
    return DistancePointToSegmentSquared({ left, top }, a, b) <= radiusSquared
        || DistancePointToSegmentSquared({ right, top }, a, b) <= radiusSquared
        || DistancePointToSegmentSquared({ right, bottom }, a, b) <= radiusSquared
        || DistancePointToSegmentSquared({ left, bottom }, a, b) <= radiusSquared;
}

inline bool ImageHitsSegment(const PageImage& image, Vec2 a, Vec2 b, float radius) {
    return SegmentHitsRect(
        image.x, image.y, image.x + image.width, image.y + image.height,
        ToImageLocalPoint(image, a), ToImageLocalPoint(image, b), radius);
}

// Stroke half of App::EraseAt's segment sweep (App.cpp:5780-5796): the stroke's
// first point against the erase segment, then segment-vs-segment for the rest.
inline bool StrokeHitsSegment(const Stroke& stroke, Vec2 a, Vec2 b, float radius, float fallbackSmoothing) {
    const float radiusSquared = radius * radius;
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty()) {
        return false;
    }
    if (DistancePointToSegmentSquared(points.front(), a, b) <= radiusSquared) {
        return true;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (DistanceSegmentToSegmentSquared(a, b, points[index - 1], points[index]) <= radiusSquared) {
            return true;
        }
    }
    return false;
}

inline bool PointInRectF(Vec2 point, float left, float top, float right, float bottom) {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
}

inline bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1U; i < polygon.size(); j = i++) {
        const Vec2 a = polygon[i];
        const Vec2 b = polygon[j];
        const bool intersects = ((a.y > point.y) != (b.y > point.y))
            && (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 0.00001f)) + a.x);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

inline std::vector<Vec2> StrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing) {
    return BuildSmoothedPoints(stroke.points, StrokeRenderSmoothing(stroke, fallbackSmoothing));
}

inline bool StrokeBounds(const Stroke& stroke, float* minX, float* minY, float* maxX, float* maxY) {
    if (stroke.points.empty() || minX == nullptr || minY == nullptr || maxX == nullptr || maxY == nullptr) {
        return false;
    }

    *minX = stroke.points.front().x;
    *minY = stroke.points.front().y;
    *maxX = stroke.points.front().x;
    *maxY = stroke.points.front().y;
    for (const Vec2& point : stroke.points) {
        *minX = std::min(*minX, point.x);
        *minY = std::min(*minY, point.y);
        *maxX = std::max(*maxX, point.x);
        *maxY = std::max(*maxY, point.y);
    }
    return true;
}

inline void TranslateStrokePoints(Stroke& stroke, Vec2 delta) {
    for (Vec2& point : stroke.points) {
        point.x += delta.x;
        point.y += delta.y;
    }
}

inline Vec2 ClampStrokeDeltaToPage(const Stroke& stroke, const Page& page, Vec2 delta) {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    if (!StrokeBounds(stroke, &minX, &minY, &maxX, &maxY)) {
        return {};
    }
    return {
        std::clamp(delta.x, -minX, page.width - maxX),
        std::clamp(delta.y, -minY, page.height - maxY),
    };
}

inline Vec2 ClampImageDeltaToPage(const PageImage& image, const Page& page, Vec2 delta) {
    float minX = page.width;
    float minY = page.height;
    float maxX = 0.0f;
    float maxY = 0.0f;
    for (const Vec2& corner : ImageCorners(image)) {
        minX = std::min(minX, corner.x);
        minY = std::min(minY, corner.y);
        maxX = std::max(maxX, corner.x);
        maxY = std::max(maxY, corner.y);
    }
    return {
        std::clamp(delta.x, -minX, page.width - maxX),
        std::clamp(delta.y, -minY, page.height - maxY),
    };
}

inline LatexBounds MeasurePageLatexBounds(const PageLatex& latex, float pageWidth) {
    static_cast<void>(pageWidth);
    const latex::FormulaMetrics metrics = latex::Measure(latex::Parse(latex));
    return { latex.x, latex.y, latex.x + metrics.width, latex.y + metrics.height };
}

inline Vec2 ClampLatexDeltaToPage(const PageLatex& latex, const Page& page, Vec2 delta) {
    const LatexBounds bounds = MeasurePageLatexBounds(latex, page.width);
    return {
        std::clamp(delta.x, -bounds.left, page.width - bounds.right),
        std::clamp(delta.y, -bounds.top, page.height - bounds.bottom),
    };
}

inline float SelectionHandleHitRadiusPixels(int dpi) {
    const int halfSize = std::max(1, ScaleForDpi(kSelectionHandleHalfDip, dpi));
    return std::max(
        static_cast<float>(halfSize) + ScaleForDpi(2.0f, dpi),
        ScaleForDpi(kSelectionHandleHitRadiusDip, dpi));
}

inline float SelectionRotateOffsetPageUnits(float pageScale, int dpi) {
    return ScaleForDpi(kSelectionRotateOffsetDip, dpi) / std::max(0.1f, pageScale);
}

inline bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius, float fallbackSmoothing) {
    const float radiusSquared = radius * radius;
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty()) {
        return false;
    }
    if (DistanceSquared(points.front(), point) <= radiusSquared) {
        return true;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (DistancePointToSegmentSquared(point, points[index - 1], points[index]) <= radiusSquared) {
            return true;
        }
    }
    return false;
}

inline bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b, float fallbackSmoothing) {
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty()) {
        return false;
    }

    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x, b.x);
    const float bottom = std::max(a.y, b.y);

    for (const Vec2& point : points) {
        if (PointInRectF(point, left, top, right, bottom)) {
            return true;
        }
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (SegmentIntersectsRectF(points[index - 1], points[index], left, top, right, bottom)) {
            return true;
        }
    }
    return false;
}

inline bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon, float fallbackSmoothing) {
    const std::vector<Vec2> points = StrokeRenderPoints(stroke, fallbackSmoothing);
    if (points.empty() || polygon.size() < 3U) {
        return false;
    }

    for (const Vec2& point : points) {
        if (PointInPolygon(point, polygon)) {
            return true;
        }
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        const Vec2 strokeA = points[index - 1];
        const Vec2 strokeB = points[index];
        for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
            const Vec2 polyA = polygon[edgeIndex];
            const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
            if (SegmentsIntersect(strokeA, strokeB, polyA, polyB)) {
                return true;
            }
        }
    }
    return false;
}

inline bool LatexHitsPoint(const PageLatex& latex, float pageWidth, Vec2 point) {
    const LatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    return PointInRotatedRect(point, bounds.left, bounds.top, bounds.right, bounds.bottom, latex.rotationDegrees);
}

inline bool LatexIntersectsRectSelection(const PageLatex& latex, float pageWidth, Vec2 a, Vec2 b) {
    const LatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    return RotatedRectIntersectsRect(bounds.left, bounds.top, bounds.right, bounds.bottom, latex.rotationDegrees, a, b);
}

inline bool LatexIntersectsPolygonSelection(const PageLatex& latex, float pageWidth, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    const LatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    return RotatedRectIntersectsPolygon(bounds.left, bounds.top, bounds.right, bounds.bottom, latex.rotationDegrees, polygon);
}

inline Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees) {
    const float radians = degrees * (kPi / 180.0f);
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const float translatedX = point.x - center.x;
    const float translatedY = point.y - center.y;
    return {
        center.x + (translatedX * cosine) - (translatedY * sine),
        center.y + (translatedX * sine) + (translatedY * cosine),
    };
}

inline Vec2 MapSelectionTransformPoint(Vec2 point, const SelectionBounds& from, const SelectionBounds& to, float rotateDegrees) {
    const float fromWidth = from.right - from.left;
    const float fromHeight = from.bottom - from.top;
    // A degenerate `from` (a selection that collapsed to a point or a line)
    // keeps the source scale instead of dividing by zero.
    const float scaleX = fromWidth > 0.0001f ? (to.right - to.left) / fromWidth : 1.0f;
    const float scaleY = fromHeight > 0.0001f ? (to.bottom - to.top) / fromHeight : 1.0f;
    const Vec2 mapped {
        to.left + ((point.x - from.left) * scaleX),
        to.top + ((point.y - from.top) * scaleY),
    };
    if (rotateDegrees == 0.0f) {
        return mapped;
    }
    const Vec2 center {
        (from.left + from.right) * 0.5f,
        (from.top + from.bottom) * 0.5f,
    };
    return RotatePointAround(mapped, center, rotateDegrees);
}

inline Vec2 ImageCenter(const PageImage& image) {
    return { image.x + (image.width * 0.5f), image.y + (image.height * 0.5f) };
}

inline std::array<Vec2, 4> ImageCorners(const PageImage& image) {
    const Vec2 center = ImageCenter(image);
    std::array<Vec2, 4> corners {
        Vec2 { image.x, image.y },
        Vec2 { image.x + image.width, image.y },
        Vec2 { image.x + image.width, image.y + image.height },
        Vec2 { image.x, image.y + image.height },
    };
    for (Vec2& corner : corners) {
        corner = RotatePointAround(corner, center, image.rotationDegrees);
    }
    return corners;
}

inline bool ImageHitsPoint(const PageImage& image, Vec2 point) {
    const Vec2 local = ToImageLocalPoint(image, point);
    return PointInRectF(local, image.x, image.y, image.x + image.width, image.y + image.height);
}

inline bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b) {
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x, b.x);
    const float bottom = std::max(a.y, b.y);
    const auto corners = ImageCorners(image);
    for (const Vec2& corner : corners) {
        if (PointInRectF(corner, left, top, right, bottom)) {
            return true;
        }
    }
    if (ImageHitsPoint(image, { left, top }) || ImageHitsPoint(image, { right, top })
        || ImageHitsPoint(image, { right, bottom }) || ImageHitsPoint(image, { left, bottom })) {
        return true;
    }
    for (std::size_t index = 0; index < corners.size(); ++index) {
        if (SegmentIntersectsRectF(corners[index], corners[(index + 1U) % corners.size()], left, top, right, bottom)) {
            return true;
        }
    }
    return false;
}

inline bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    const auto corners = ImageCorners(image);
    for (const Vec2& corner : corners) {
        if (PointInPolygon(corner, polygon)) {
            return true;
        }
    }
    for (const Vec2& point : polygon) {
        if (ImageHitsPoint(image, point)) {
            return true;
        }
    }
    for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
        const Vec2 polyA = polygon[edgeIndex];
        const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
        for (std::size_t imageEdgeIndex = 0; imageEdgeIndex < corners.size(); ++imageEdgeIndex) {
            if (SegmentsIntersect(polyA, polyB, corners[imageEdgeIndex], corners[(imageEdgeIndex + 1U) % corners.size()])) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace cnote::ui
