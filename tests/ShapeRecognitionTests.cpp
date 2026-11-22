// Shape recognition (cnote::RecognizeShape) on synthetic hand-drawn strokes:
// ideal outlines walked at pointer-sample spacing with a slow wobble and
// jitter, overshoots and mid-edge starts, over many deterministic seeds.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "selection/ShapeRecognition.h"

namespace {

using cnote::RecognizedShape;
using cnote::RecognizedShapeKind;
using cnote::RecognizeShape;
using cnote::Vec2;

constexpr float kPi = 3.14159265358979323846f;
constexpr int kSeeds = 24;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

const char* KindName(std::optional<RecognizedShape> shape) {
    if (!shape.has_value()) {
        return "none";
    }
    switch (shape->kind) {
    case RecognizedShapeKind::Line: return "line";
    case RecognizedShapeKind::Arrow: return "arrow";
    case RecognizedShapeKind::Ellipse: return "ellipse";
    case RecognizedShapeKind::Circle: return "circle";
    case RecognizedShapeKind::Triangle: return "triangle";
    case RecognizedShapeKind::Rectangle: return "rectangle";
    }
    return "?";
}

bool Is(const std::optional<RecognizedShape>& shape, RecognizedShapeKind kind) {
    return shape.has_value() && shape->kind == kind;
}

float Distance(Vec2 a, Vec2 b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// Deterministic across standard libraries (no <random> distributions).
class Noise {
public:
    explicit Noise(std::uint32_t seed) : state_(seed * 2654435761U + 1U) {}
    float Next() {  // -1..1
        state_ = (state_ * 1664525U) + 1013904223U;
        return (static_cast<float>(state_ >> 8U) / 8388608.0f) - 1.0f;
    }

private:
    std::uint32_t state_;
};

// The ideal outline walked at ~1 unit per sample with a slow two-axis wobble
// of `amplitude` and +-0.3 jitter, like a steady hand on a tablet.
std::vector<Vec2> HandDrawn(const std::vector<Vec2>& ideal, float amplitude, std::uint32_t seed) {
    Noise noise(seed);
    const float phaseX = noise.Next() * kPi;
    const float phaseY = noise.Next() * kPi;
    const float waveX = 45.0f + (15.0f * noise.Next());
    const float waveY = 55.0f + (15.0f * noise.Next());
    std::vector<Vec2> drawn;
    float walked = 0.0f;
    for (std::size_t index = 0; index < ideal.size(); ++index) {
        if (index > 0) {
            walked += Distance(ideal[index - 1], ideal[index]);
        }
        drawn.push_back({
            ideal[index].x + (amplitude * std::sin((walked / waveX) + phaseX)) + (0.3f * noise.Next()),
            ideal[index].y + (amplitude * std::sin((walked / waveY) + phaseY)) + (0.3f * noise.Next()),
        });
    }
    return drawn;
}

std::vector<Vec2> Polyline(const std::vector<Vec2>& vertices) {
    std::vector<Vec2> points { vertices.front() };
    for (std::size_t index = 1; index < vertices.size(); ++index) {
        const Vec2 a = vertices[index - 1];
        const Vec2 b = vertices[index];
        const int steps = std::max(1, static_cast<int>(Distance(a, b)));
        for (int step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            points.push_back({ a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t) });
        }
    }
    return points;
}

// An elliptical arc from `start` sweeping `sweep` radians.
std::vector<Vec2> Arc(Vec2 center, float a, float b, float rotation, float start, float sweep) {
    std::vector<Vec2> points;
    const int steps = static_cast<int>(std::abs(sweep) * std::max(a, b));
    for (int step = 0; step <= steps; ++step) {
        const float t = start + (sweep * static_cast<float>(step) / static_cast<float>(steps));
        const float x = a * std::cos(t);
        const float y = b * std::sin(t);
        points.push_back({
            center.x + (x * std::cos(rotation)) - (y * std::sin(rotation)),
            center.y + (x * std::sin(rotation)) + (y * std::cos(rotation)),
        });
    }
    return points;
}

struct Bounds {
    float minX, minY, maxX, maxY;
};

Bounds BoundsOf(const std::vector<Vec2>& points) {
    Bounds b { points.front().x, points.front().y, points.front().x, points.front().y };
    for (const Vec2 p : points) {
        b.minX = std::min(b.minX, p.x);
        b.minY = std::min(b.minY, p.y);
        b.maxX = std::max(b.maxX, p.x);
        b.maxY = std::max(b.maxY, p.y);
    }
    return b;
}

bool Closed(const RecognizedShape& shape) {
    return shape.points.size() > 2U && Distance(shape.points.front(), shape.points.back()) < 1e-4f;
}

// Every seed must give `kind`; reports the first mismatch.
void CheckEverySeed(const std::string& what, RecognizedShapeKind kind,
    const std::vector<Vec2>& ideal, float amplitude, bool constrained = false) {
    int hits = 0;
    std::string firstMiss;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        const std::optional<RecognizedShape> shape =
            RecognizeShape(HandDrawn(ideal, amplitude, static_cast<std::uint32_t>(seed)), constrained);
        if (Is(shape, kind)) {
            ++hits;
        } else if (firstMiss.empty()) {
            firstMiss = " (seed " + std::to_string(seed) + " gave " + KindName(shape) + ")";
        }
    }
    Check(hits == kSeeds, what + ": " + std::to_string(hits) + "/" + std::to_string(kSeeds) + firstMiss);
}

}  // namespace

int main() {
    // --- lines --------------------------------------------------------------
    const std::vector<Vec2> lineIdeal = Polyline({ { 100.0f, 100.0f }, { 300.0f, 160.0f } });
    CheckEverySeed("a wobbly straight stroke is a line", RecognizedShapeKind::Line, lineIdeal, 2.0f);
    {
        const std::optional<RecognizedShape> line = RecognizeShape(HandDrawn(lineIdeal, 2.0f, 7U));
        Check(Is(line, RecognizedShapeKind::Line) && Distance(line->points.front(), { 100.0f, 100.0f }) < 5.0f
                && Distance(line->points.back(), { 300.0f, 160.0f }) < 5.0f,
            "the line runs from where the stroke started to where it ended");
    }

    // --- ellipses -----------------------------------------------------------
    const Vec2 center { 250.0f, 250.0f };
    const std::vector<Vec2> circleIdeal = Arc(center, 60.0f, 60.0f, 0.0f, 0.5f, 2.0f * kPi + 0.4f);
    CheckEverySeed("an overshooting circle is a circle", RecognizedShapeKind::Circle, circleIdeal, 2.0f);
    {
        const std::optional<RecognizedShape> circle = RecognizeShape(HandDrawn(circleIdeal, 2.0f, 3U));
        bool onRadius = Is(circle, RecognizedShapeKind::Circle) && Closed(*circle);
        for (const Vec2 p : onRadius ? circle->points : std::vector<Vec2> {}) {
            onRadius = onRadius && std::abs(Distance(p, center) - 60.0f) < 3.0f;
        }
        Check(onRadius, "the snapped circle is closed and keeps the drawn radius");
    }
    // A circle drawn the other way round, starting at a different angle.
    CheckEverySeed("a clockwise circle is a circle", RecognizedShapeKind::Circle,
        Arc(center, 45.0f, 45.0f, 0.0f, 3.0f, -2.0f * kPi - 0.2f), 1.5f);

    const std::vector<Vec2> tiltedIdeal = Arc(center, 110.0f, 55.0f, 0.5f, 1.0f, 2.0f * kPi);
    CheckEverySeed("a tilted ellipse is an ellipse", RecognizedShapeKind::Ellipse, tiltedIdeal, 2.0f);
    {
        const std::optional<RecognizedShape> ellipse = RecognizeShape(HandDrawn(tiltedIdeal, 2.0f, 5U));
        float nearest = 1e9f, farthest = 0.0f;
        for (const Vec2 p : Is(ellipse, RecognizedShapeKind::Ellipse) ? ellipse->points : std::vector<Vec2> {}) {
            nearest = std::min(nearest, Distance(p, center));
            farthest = std::max(farthest, Distance(p, center));
        }
        Check(std::abs(farthest - 110.0f) < 6.0f && std::abs(nearest - 55.0f) < 6.0f,
            "the ellipse keeps the drawn axes (" + std::to_string(farthest) + ", " + std::to_string(nearest) + ")");
    }
    {
        // 4 degrees off horizontal snaps level: the ends of the major axis
        // (segments 0 and 36 of 72) share a y.
        const std::optional<RecognizedShape> ellipse = RecognizeShape(
            HandDrawn(Arc(center, 100.0f, 50.0f, 4.0f * kPi / 180.0f, 0.3f, 2.0f * kPi), 1.0f, 11U));
        Check(Is(ellipse, RecognizedShapeKind::Ellipse) && ellipse->points.size() == 73U
                && std::abs(ellipse->points[0].y - ellipse->points[36].y) < 0.01f,
            "a nearly level ellipse snaps level");
    }
    CheckEverySeed("Shift makes a tilted ellipse a circle", RecognizedShapeKind::Circle, tiltedIdeal, 2.0f, true);
    {
        // A perfect circle of the mean semi-axis, (110 + 55) / 2.
        const std::optional<RecognizedShape> circle = RecognizeShape(HandDrawn(tiltedIdeal, 2.0f, 5U), true);
        bool round = Is(circle, RecognizedShapeKind::Circle) && Closed(*circle);
        for (const Vec2 p : round ? circle->points : std::vector<Vec2> {}) {
            round = round && std::abs(Distance(p, center) - 82.5f) < 4.0f;
        }
        Check(round, "the Shift circle keeps the ellipse's center and mean radius");
    }

    // --- polygons -----------------------------------------------------------
    // Starts mid-way along the top edge, rounds its corners a little and
    // overshoots the start.
    const std::vector<Vec2> rectangleIdeal = Polyline({
        { 200.0f, 100.0f }, { 294.0f, 100.0f }, { 300.0f, 106.0f }, { 300.0f, 214.0f }, { 294.0f, 220.0f },
        { 106.0f, 220.0f }, { 100.0f, 214.0f }, { 100.0f, 106.0f }, { 106.0f, 100.0f }, { 225.0f, 100.0f },
    });
    CheckEverySeed("a rounded, overshooting rectangle is a rectangle", RecognizedShapeKind::Rectangle, rectangleIdeal, 1.5f);
    {
        const std::optional<RecognizedShape> rectangle = RecognizeShape(HandDrawn(rectangleIdeal, 1.5f, 2U));
        bool axisAligned = Is(rectangle, RecognizedShapeKind::Rectangle) && Closed(*rectangle);
        Bounds b {};
        if (axisAligned) {
            b = BoundsOf(rectangle->points);
            for (const Vec2 p : rectangle->points) {
                axisAligned = axisAligned
                    && (std::abs(p.x - b.minX) < 0.01f || std::abs(p.x - b.maxX) < 0.01f
                        || std::abs(p.y - b.minY) < 0.01f || std::abs(p.y - b.maxY) < 0.01f);
            }
        }
        Check(axisAligned && std::abs(b.minX - 100.0f) < 4.0f && std::abs(b.maxX - 300.0f) < 4.0f
                && std::abs(b.minY - 100.0f) < 4.0f && std::abs(b.maxY - 220.0f) < 4.0f,
            "the rectangle is exact, axis-aligned and keeps the drawn extents");
    }

    const std::vector<Vec2> diamondIdeal = Polyline({
        { 250.0f, 150.0f }, { 335.0f, 235.0f }, { 250.0f, 320.0f }, { 165.0f, 235.0f }, { 250.0f, 150.0f },
    });
    CheckEverySeed("a diamond is a (rotated) rectangle", RecognizedShapeKind::Rectangle, diamondIdeal, 1.5f);
    {
        const std::optional<RecognizedShape> square = RecognizeShape(HandDrawn(diamondIdeal, 1.5f, 4U));
        const Bounds b = Is(square, RecognizedShapeKind::Rectangle) ? BoundsOf(square->points) : Bounds {};
        Check(std::abs((b.maxX - b.minX) - (b.maxY - b.minY)) < 0.05f && b.maxX - b.minX > 160.0f,
            "the diamond snaps to an exact square");
    }

    const std::vector<Vec2> triangleIdeal = Polyline({
        { 150.0f, 300.0f }, { 350.0f, 300.0f }, { 250.0f, 130.0f }, { 150.0f, 300.0f },
    });
    CheckEverySeed("a triangle is a triangle", RecognizedShapeKind::Triangle, triangleIdeal, 1.5f);

    // Any four-sided outline is squared: a trapezoid becomes a level rectangle.
    const std::vector<Vec2> skewedIdeal =
        Polyline({ { 150.0f, 300.0f }, { 350.0f, 300.0f }, { 300.0f, 180.0f }, { 200.0f, 180.0f }, { 150.0f, 300.0f } });
    CheckEverySeed("a skewed four-sided outline is a rectangle", RecognizedShapeKind::Rectangle, skewedIdeal, 1.5f);
    {
        const std::optional<RecognizedShape> rectangle = RecognizeShape(HandDrawn(skewedIdeal, 1.5f, 3U));
        bool onBox = Is(rectangle, RecognizedShapeKind::Rectangle) && Closed(*rectangle);
        const Bounds b = onBox ? BoundsOf(rectangle->points) : Bounds {};
        for (const Vec2 p : onBox ? rectangle->points : std::vector<Vec2> {}) {
            onBox = onBox
                && (std::abs(p.x - b.minX) < 0.01f || std::abs(p.x - b.maxX) < 0.01f
                    || std::abs(p.y - b.minY) < 0.01f || std::abs(p.y - b.maxY) < 0.01f);
        }
        Check(onBox, "the skewed outline snaps to right-angle corners without Shift");
    }

    // --- Shift: perfected triangles -----------------------------------------
    {
        // Angles of about 59.5, 59.5 and 61 degrees: equilateral, base level.
        const std::optional<RecognizedShape> triangle = RecognizeShape(HandDrawn(triangleIdeal, 1.5f, 6U), true);
        const Bounds b = Is(triangle, RecognizedShapeKind::Triangle) ? BoundsOf(triangle->points) : Bounds {};
        const float width = b.maxX - b.minX;
        Check(width > 150.0f && std::abs((b.maxY - b.minY) - (width * std::sqrt(3.0f) * 0.5f)) < 0.05f,
            "Shift makes a near-equilateral triangle exactly equilateral with a level side");
    }
    {
        // About 93 degrees at the bottom-left corner: squared there, the
        // near-level leg snapped level, so the bounding box's bottom-left
        // corner is on the outline.
        const std::vector<Vec2> rightIdeal = Polyline({
            { 100.0f, 300.0f }, { 300.0f, 300.0f }, { 108.0f, 140.0f }, { 100.0f, 300.0f },
        });
        CheckEverySeed("Shift keeps a right-ish triangle a triangle", RecognizedShapeKind::Triangle, rightIdeal, 1.5f, true);
        const std::optional<RecognizedShape> triangle = RecognizeShape(HandDrawn(rightIdeal, 1.5f, 8U), true);
        bool cornerOnOutline = false;
        Bounds b {};
        if (Is(triangle, RecognizedShapeKind::Triangle)) {
            b = BoundsOf(triangle->points);
            for (const Vec2 p : triangle->points) {
                cornerOnOutline = cornerOnOutline || Distance(p, { b.minX, b.maxY }) < 0.05f;
            }
        }
        Check(cornerOnOutline && std::abs((b.maxX - b.minX) - 200.0f) < 8.0f && std::abs((b.maxY - b.minY) - 160.0f) < 8.0f,
            "Shift squares the widest corner of a triangle and keeps its legs' lengths");
    }

    // A narrow, shaky oval stays an ellipse: five or more corners are never a
    // polygon (seed 9 used to snap to one).
    CheckEverySeed("a narrow shaky oval is an ellipse", RecognizedShapeKind::Ellipse,
        Arc(center, 120.0f, 36.0f, 0.3f, 1.0f, 2.0f * kPi + 0.2f), 3.5f);
    CheckEverySeed("a shaky circle is still a circle", RecognizedShapeKind::Circle,
        Arc(center, 70.0f, 70.0f, 0.0f, 2.0f, 2.0f * kPi + 0.2f), 3.5f);

    // --- arrows -------------------------------------------------------------
    const std::vector<Vec2> arrowIdeal = Polyline({
        { 100.0f, 400.0f }, { 300.0f, 400.0f }, { 274.0f, 385.0f }, { 300.0f, 400.0f }, { 274.0f, 415.0f },
    });
    CheckEverySeed("a one-stroke arrow is an arrow", RecognizedShapeKind::Arrow, arrowIdeal, 1.0f);
    {
        const std::optional<RecognizedShape> arrow = RecognizeShape(HandDrawn(arrowIdeal, 1.0f, 9U));
        bool reachesTip = false;
        for (const Vec2 p : Is(arrow, RecognizedShapeKind::Arrow) ? arrow->points : std::vector<Vec2> {}) {
            reachesTip = reachesTip || Distance(p, { 300.0f, 400.0f }) < 4.0f;
        }
        Check(reachesTip && Distance(arrow->points.front(), { 100.0f, 400.0f }) < 4.0f,
            "the arrow starts at the tail and reaches the tip");
    }

    // --- not shapes ---------------------------------------------------------
    std::vector<Vec2> zigzag;
    for (int peak = 0; peak <= 8; ++peak) {
        zigzag.push_back({ 100.0f + (15.0f * static_cast<float>(peak)), peak % 2 == 0 ? 300.0f : 270.0f });
    }
    int kept = 0;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        const auto seedValue = static_cast<std::uint32_t>(seed);
        const bool zigzagKept = !RecognizeShape(HandDrawn(Polyline(zigzag), 1.0f, seedValue)).has_value();
        const bool arcKept = !RecognizeShape(HandDrawn(Arc(center, 80.0f, 80.0f, 0.0f, 0.0f, kPi), 1.5f, seedValue)).has_value();
        const bool spiralKept = !RecognizeShape(HandDrawn([&] {
            std::vector<Vec2> spiral;
            for (int step = 0; step <= 900; ++step) {
                const float t = static_cast<float>(step) / 100.0f;
                spiral.push_back({ center.x + (8.0f * t * std::cos(t)), center.y + (8.0f * t * std::sin(t)) });
            }
            return spiral;
        }(), 1.0f, seedValue)).has_value();
        kept += zigzagKept && arcKept && spiralKept ? 1 : 0;
    }
    Check(kept == kSeeds, "zigzags, half circles and spirals are left alone: " + std::to_string(kept) + "/" + std::to_string(kSeeds));
    Check(!RecognizeShape(Polyline({ { 10.0f, 10.0f }, { 18.0f, 10.0f } })).has_value(), "a short tick is left alone");
    Check(!RecognizeShape({ { 10.0f, 10.0f } }).has_value(), "a dot is left alone");

    std::printf("SUMMARY failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
