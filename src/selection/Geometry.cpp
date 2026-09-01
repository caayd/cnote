#include "selection/Geometry.h"

#include <algorithm>
#include <cmath>

#include "DpiUtil.h"
#include "latex/LatexEngine.h"
#include "text/TextGeometry.h"
#include "ui/UiUtil.h"

namespace {

RECT MakeLatexRect(float left, float top, float right, float bottom) {
    return {
        static_cast<LONG>(std::lround(left)),
        static_cast<LONG>(std::lround(top)),
        static_cast<LONG>(std::lround(right)),
        static_cast<LONG>(std::lround(bottom)),
    };
}

}

namespace cnote {

constexpr int kSelectionHandleHalfDip = 4;
constexpr float kSelectionHandleHitRadiusDip = 11.0f;
constexpr float kSelectionRotateOffsetDip = 18.0f;
constexpr float kPi = 3.14159265358979323846f;

using cnote::ui::HasRect;

std::vector<Vec2> BuildSmoothedPoints(const std::vector<Vec2>& points, float strength) {
    if (points.size() <= 2U || strength <= 0.001f) {
        return points;
    }

    std::vector<Vec2> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    const float sideWeight = 0.25f * std::clamp(strength, 0.0f, 1.0f);
    const float centerWeight = 1.0f - (sideWeight * 2.0f);

    for (std::size_t index = 1; index + 1 < points.size(); ++index) {
        const Vec2 previous = points[index - 1];
        const Vec2 current = points[index];
        const Vec2 next = points[index + 1];
        const Vec2 filtered {
            (previous.x * sideWeight) + (current.x * centerWeight) + (next.x * sideWeight),
            (previous.y * sideWeight) + (current.y * centerWeight) + (next.y * sideWeight),
        };

        if (DistanceSquared(filtered, smoothed.back()) >= 0.05f) {
            smoothed.push_back(filtered);
        }
    }

    if (DistanceSquared(points.back(), smoothed.back()) >= 0.01f) {
        smoothed.push_back(points.back());
    }

    return smoothed;
}

float StrokeRenderSmoothing(const Stroke& stroke, float fallbackSmoothing) {
    const float smoothing = std::isfinite(stroke.smoothing) ? stroke.smoothing : fallbackSmoothing;
    return std::clamp(smoothing, 0.0f, 1.0f);
}

std::vector<Vec2> StrokeRenderPoints(const Stroke& stroke, float fallbackSmoothing) {
    return BuildSmoothedPoints(stroke.points, StrokeRenderSmoothing(stroke, fallbackSmoothing));
}

bool PointInRectF(Vec2 point, float left, float top, float right, float bottom) {
    return point.x >= left && point.x <= right && point.y >= top && point.y <= bottom;
}

float DistancePointToSegmentSquared(Vec2 point, Vec2 a, Vec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSquared = (dx * dx) + (dy * dy);
    if (lengthSquared <= 0.0001f) {
        return DistanceSquared(point, a);
    }

    float t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vec2 projection {
        a.x + (dx * t),
        a.y + (dy * t),
    };
    return DistanceSquared(point, projection);
}

float CrossProduct(Vec2 a, Vec2 b, Vec2 c) {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

bool OnSegment(Vec2 a, Vec2 b, Vec2 point) {
    return point.x >= std::min(a.x, b.x) - 0.001f
        && point.x <= std::max(a.x, b.x) + 0.001f
        && point.y >= std::min(a.y, b.y) - 0.001f
        && point.y <= std::max(a.y, b.y) + 0.001f;
}

bool SegmentsIntersect(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2) {
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

bool SegmentIntersectsRectF(Vec2 a, Vec2 b, float left, float top, float right, float bottom) {
    if (PointInRectF(a, left, top, right, bottom) || PointInRectF(b, left, top, right, bottom)) {
        return true;
    }

    return SegmentsIntersect(a, b, { left, top }, { right, top })
        || SegmentsIntersect(a, b, { right, top }, { right, bottom })
        || SegmentsIntersect(a, b, { right, bottom }, { left, bottom })
        || SegmentsIntersect(a, b, { left, bottom }, { left, top });
}

bool PointInPolygon(Vec2 point, const std::vector<Vec2>& polygon) {
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

Vec2 ToImageLocalPoint(const PageImage& image, Vec2 point) {
    const Vec2 center = ImageCenter(image);
    return RotatePointAround(point, center, -image.rotationDegrees);
}

float DistanceSquared(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

Vec2 PointToVec2(POINT point) {
    return {
        static_cast<float>(point.x),
        static_cast<float>(point.y),
    };
}

Vec2 RemapPagePointBetweenViews(const App::PageView& fromView, const App::PageView& toView, Vec2 point) {
    const float screenX = static_cast<float>(fromView.rect.left) + (point.x * fromView.scale);
    const float screenY = static_cast<float>(fromView.rect.top) + (point.y * fromView.scale);
    return {
        (screenX - static_cast<float>(toView.rect.left)) / std::max(0.1f, toView.scale),
        (screenY - static_cast<float>(toView.rect.top)) / std::max(0.1f, toView.scale),
    };
}

bool StrokeBounds(const Stroke& stroke, float* minX, float* minY, float* maxX, float* maxY) {
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

void TranslateStrokePoints(Stroke& stroke, Vec2 delta) {
    for (Vec2& point : stroke.points) {
        point.x += delta.x;
        point.y += delta.y;
    }
}

Vec2 ClampStrokeDeltaToPage(const Stroke& stroke, const Page& page, Vec2 delta) {
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

Vec2 ClampImageDeltaToPage(const PageImage& image, const Page& page, Vec2 delta) {
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

Vec2 ClampLatexDeltaToPage(const PageLatex& latex, const Page& page, Vec2 delta) {
    const PageLatexBounds bounds = MeasurePageLatexBounds(latex, page.width);
    return {
        std::clamp(delta.x, -bounds.left, page.width - bounds.right),
        std::clamp(delta.y, -bounds.top, page.height - bounds.bottom),
    };
}

PageLatexBounds MeasurePageLatexBounds(const PageLatex& latex, float pageWidth) {
    static_cast<void>(pageWidth);
    const cnote::latex::FormulaMetrics metrics = cnote::latex::Measure(latex);
    return {
        latex.x,
        latex.y,
        latex.x + metrics.width,
        latex.y + metrics.height,
    };
}

std::optional<RECT> PageLatexBoundsRect(const PageLatex& latex, float pageWidth, float scale, int extraPadding) {
    if (scale <= 0.0f) {
        return std::nullopt;
    }

    const PageLatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    const float padding = static_cast<float>(extraPadding);
    return MakeLatexRect(
        (bounds.left * scale) - padding,
        (bounds.top * scale) - padding,
        (bounds.right * scale) + padding + 1.0f,
        (bounds.bottom * scale) + padding + 1.0f);
}

bool LatexHitsPoint(const PageLatex& latex, float pageWidth, Vec2 point) {
    const PageLatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    return PointInRectF(point, bounds.left, bounds.top, bounds.right, bounds.bottom);
}

bool LatexIntersectsRectSelection(const PageLatex& latex, float pageWidth, Vec2 a, Vec2 b) {
    const PageLatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    const float left = std::min(a.x, b.x);
    const float right = std::max(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float bottom = std::max(a.y, b.y);
    return !(bounds.right < left || bounds.left > right || bounds.bottom < top || bounds.top > bottom);
}

bool LatexIntersectsPolygonSelection(const PageLatex& latex, float pageWidth, const std::vector<Vec2>& polygon) {
    if (polygon.size() < 3U) {
        return false;
    }

    const PageLatexBounds bounds = MeasurePageLatexBounds(latex, pageWidth);
    const std::array<Vec2, 4> corners {{
        { bounds.left, bounds.top },
        { bounds.right, bounds.top },
        { bounds.right, bounds.bottom },
        { bounds.left, bounds.bottom },
    }};

    for (const Vec2& corner : corners) {
        if (PointInPolygon(corner, polygon)) {
            return true;
        }
    }

    for (const Vec2& point : polygon) {
        if (LatexHitsPoint(latex, pageWidth, point)) {
            return true;
        }
    }

    for (std::size_t edgeIndex = 0; edgeIndex < polygon.size(); ++edgeIndex) {
        const Vec2 polyA = polygon[edgeIndex];
        const Vec2 polyB = polygon[(edgeIndex + 1U) % polygon.size()];
        for (std::size_t rectEdgeIndex = 0; rectEdgeIndex < corners.size(); ++rectEdgeIndex) {
            if (SegmentsIntersect(polyA, polyB, corners[rectEdgeIndex], corners[(rectEdgeIndex + 1U) % corners.size()])) {
                return true;
            }
        }
    }

    return false;
}

float SelectionHandleHitRadiusPixels(UINT dpi) {
    const int halfSize = std::max(1, ScaleForDpi(kSelectionHandleHalfDip, dpi));
    return std::max(
        static_cast<float>(halfSize + ScaleForDpi(2, dpi)),
        ScaleForDpi(kSelectionHandleHitRadiusDip, dpi));
}

float SelectionRotateOffsetPageUnits(float pageScale, UINT dpi) {
    return ScaleForDpi(kSelectionRotateOffsetDip, dpi) / std::max(0.1f, pageScale);
}

bool RectContainsPointWithPadding(RECT rect, POINT point, int padding) {
    if (!HasRect(rect)) {
        return false;
    }

    padding = std::max(0, padding);
    InflateRect(&rect, padding, padding);
    return PtInRect(&rect, point) != 0;
}

bool PolygonContainsPoint(const std::vector<Vec2>& polygon, POINT point) {
    if (polygon.size() < 3U) {
        return false;
    }

    return PointInPolygon(PointToVec2(point), polygon);
}

bool StrokeHitsPoint(const Stroke& stroke, Vec2 point, float radius, float fallbackSmoothing) {
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

bool StrokeIntersectsRectSelection(const Stroke& stroke, Vec2 a, Vec2 b, float fallbackSmoothing) {
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

bool StrokeIntersectsPolygonSelection(const Stroke& stroke, const std::vector<Vec2>& polygon, float fallbackSmoothing) {
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

Vec2 RotatePointAround(Vec2 point, Vec2 center, float degrees) {
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

Vec2 ImageCenter(const PageImage& image) {
    return {
        image.x + (image.width * 0.5f),
        image.y + (image.height * 0.5f),
    };
}

std::array<Vec2, 4> ImageCorners(const PageImage& image) {
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

Vec2 ScreenPointFromPagePoint(Vec2 point, RECT pageRect, float pageScale) {
    return {
        static_cast<float>(pageRect.left) + (point.x * pageScale),
        static_cast<float>(pageRect.top) + (point.y * pageScale),
    };
}

bool ImageHitsPoint(const PageImage& image, Vec2 point) {
    const Vec2 local = ToImageLocalPoint(image, point);
    return PointInRectF(local, image.x, image.y, image.x + image.width, image.y + image.height);
}

bool ImageIntersectsRectSelection(const PageImage& image, Vec2 a, Vec2 b) {
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

bool ImageIntersectsPolygonSelection(const PageImage& image, const std::vector<Vec2>& polygon) {
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

}  // namespace cnote
