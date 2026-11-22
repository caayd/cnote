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
    case RecognizedShapeKind::Brace: return "brace";
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

// A hand-drawn-style curly brace from `start` to `end`, its tip `depth` off
// the chord's middle on the left of the start-to-end direction: each arm
// leaves its end steeply and settles `arm` off the chord, and a pointed bump
// in the middle reaches out to the tip.  Not the recognizer's own outline.
std::vector<Vec2> Brace(Vec2 start, Vec2 end, float depth, float arm, float curl = 6.0f) {
    const float length = Distance(start, end);
    const Vec2 along { (end.x - start.x) / length, (end.y - start.y) / length };
    const Vec2 normal { -along.y, along.x };
    std::vector<Vec2> points;
    for (float u = 0.0f; u <= length; u += 0.25f) {
        const float rise = 1.0f - std::exp(-std::min(u, length - u) / curl);
        const float bump = std::max(0.0f, 1.0f - (std::abs(u - (0.5f * length)) / 25.0f));
        const float v = (arm * rise) + ((depth - arm) * bump * bump);
        points.push_back({ start.x + (along.x * u) + (normal.x * v), start.y + (along.y * u) + (normal.y * v) });
    }
    points.push_back(end);
    return points;
}

// A chain of cubic Bezier segments: on-curve points at 0, 3, 6, ... with two
// control points between each pair, walked at about 1 unit per point.
std::vector<Vec2> Cubics(const std::vector<Vec2>& points) {
    std::vector<Vec2> out { points.front() };
    for (std::size_t index = 0; index + 3U < points.size(); index += 3U) {
        const Vec2 p0 = points[index], p1 = points[index + 1U], p2 = points[index + 2U], p3 = points[index + 3U];
        const int steps = std::max(1, static_cast<int>(Distance(p0, p1) + Distance(p1, p2) + Distance(p2, p3)));
        for (int step = 1; step <= steps; ++step) {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const float s = 1.0f - t;
            const float a = s * s * s, b = 3.0f * s * s * t, c = 3.0f * s * t * t, d = t * t * t;
            out.push_back({ (a * p0.x) + (b * p1.x) + (c * p2.x) + (d * p3.x), (a * p0.y) + (b * p1.y) + (c * p2.y) + (d * p3.y) });
        }
    }
    return out;
}

// A "}" drawn as a ">" between two lumps: the centreline of a user's stroke,
// every 8 px of a screenshot.  The lumps bow out nearly as far as the tip,
// which sits above the middle between two inner corners.  `map` places each
// traced point.
template <typename Map>
std::vector<Vec2> LumpyBrace(Map map) {
    std::vector<Vec2> traced {
        { 165.0f, 83.0f }, { 173.0f, 83.0f }, { 181.0f, 83.0f }, { 189.0f, 83.0f }, { 197.0f, 84.0f },
        { 205.0f, 86.0f }, { 213.0f, 89.0f }, { 219.0f, 92.0f }, { 224.0f, 95.0f }, { 229.0f, 100.0f },
        { 233.0f, 104.0f }, { 236.0f, 109.0f }, { 239.0f, 114.0f }, { 242.0f, 122.0f }, { 244.0f, 130.0f },
        { 244.0f, 138.0f }, { 244.0f, 146.0f }, { 243.0f, 154.0f }, { 241.0f, 162.0f }, { 238.0f, 169.0f },
        { 235.0f, 177.0f }, { 232.0f, 185.0f }, { 229.0f, 192.0f }, { 226.0f, 199.0f }, { 223.0f, 206.0f },
        { 220.0f, 213.0f }, { 217.0f, 220.0f }, { 213.0f, 227.0f }, { 210.0f, 234.0f }, { 207.0f, 241.0f },
        { 203.0f, 248.0f }, { 200.0f, 255.0f }, { 197.0f, 262.0f }, { 194.0f, 267.0f }, { 192.0f, 273.0f },
        { 190.0f, 281.0f }, { 198.0f, 281.0f }, { 206.0f, 279.0f }, { 214.0f, 278.0f }, { 222.0f, 277.0f },
        { 230.0f, 277.0f }, { 238.0f, 277.0f }, { 246.0f, 279.0f }, { 252.0f, 282.0f }, { 255.0f, 289.0f },
        { 253.0f, 296.0f }, { 248.0f, 302.0f }, { 243.0f, 307.0f }, { 238.0f, 313.0f }, { 232.0f, 319.0f },
        { 226.0f, 324.0f }, { 222.0f, 328.0f }, { 218.0f, 332.0f }, { 214.0f, 336.0f }, { 209.0f, 342.0f },
        { 204.0f, 348.0f }, { 205.0f, 355.0f }, { 211.0f, 357.0f }, { 216.0f, 361.0f }, { 220.0f, 365.0f },
        { 224.0f, 371.0f }, { 228.0f, 376.0f }, { 232.0f, 382.0f }, { 235.0f, 387.0f }, { 237.0f, 393.0f },
        { 240.0f, 398.0f }, { 242.0f, 404.0f }, { 245.0f, 411.0f }, { 247.0f, 419.0f }, { 248.0f, 427.0f },
        { 249.0f, 434.0f }, { 249.0f, 442.0f }, { 250.0f, 450.0f }, { 250.0f, 458.0f }, { 249.0f, 466.0f },
        { 248.0f, 474.0f }, { 247.0f, 482.0f }, { 246.0f, 490.0f }, { 244.0f, 498.0f }, { 241.0f, 506.0f },
        { 239.0f, 514.0f }, { 236.0f, 522.0f }, { 233.0f, 530.0f }, { 230.0f, 537.0f }, { 227.0f, 544.0f },
        { 224.0f, 551.0f }, { 221.0f, 558.0f }, { 217.0f, 564.0f }, { 213.0f, 570.0f }, { 209.0f, 576.0f },
        { 205.0f, 582.0f }, { 201.0f, 588.0f }, { 196.0f, 593.0f }, { 192.0f, 597.0f }, { 187.0f, 602.0f },
        { 181.0f, 606.0f }, { 174.0f, 611.0f },
    };
    for (Vec2& point : traced) {
        point = map(point);
    }
    return Polyline(traced);
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

    // --- braces -------------------------------------------------------------
    // A "{" drawn top to bottom (tip on the left), a "}" drawn bottom to top,
    // a brace under something and one at an angle.
    const std::vector<Vec2> leftBrace = Brace({ 300.0f, 100.0f }, { 300.0f, 320.0f }, 45.0f, 20.0f);
    CheckEverySeed("a { is a brace", RecognizedShapeKind::Brace, leftBrace, 1.0f);
    CheckEverySeed("a } drawn upward is a brace", RecognizedShapeKind::Brace,
        Brace({ 300.0f, 320.0f }, { 300.0f, 100.0f }, 40.0f, 18.0f), 1.0f);
    CheckEverySeed("an underbrace is a brace", RecognizedShapeKind::Brace,
        Brace({ 100.0f, 300.0f }, { 400.0f, 300.0f }, 40.0f, 18.0f), 1.0f);
    CheckEverySeed("a tilted brace is a brace", RecognizedShapeKind::Brace,
        Brace({ 100.0f, 100.0f }, { 280.0f, 220.0f }, 40.0f, 18.0f), 1.0f);
    CheckEverySeed("a small brace is a brace", RecognizedShapeKind::Brace,
        Brace({ 300.0f, 100.0f }, { 300.0f, 200.0f }, 22.0f, 10.0f, 4.0f), 1.0f);
    CheckEverySeed("a lazily curled, shaky brace is a brace", RecognizedShapeKind::Brace,
        Brace({ 300.0f, 100.0f }, { 300.0f, 320.0f }, 45.0f, 20.0f, 12.0f), 2.0f);
    {
        // Slightly off vertical: the snapped brace stands level between the
        // drawn ends, its tip on the left at the drawn depth.
        const std::optional<RecognizedShape> brace =
            RecognizeShape(HandDrawn(Brace({ 300.0f, 100.0f }, { 310.0f, 320.0f }, 45.0f, 20.0f), 1.0f, 4U));
        const Bounds b = Is(brace, RecognizedShapeKind::Brace) ? BoundsOf(brace->points) : Bounds {};
        Check(Is(brace, RecognizedShapeKind::Brace) && std::abs(brace->points.front().x - brace->points.back().x) < 0.01f
                && Distance(brace->points.front(), { 305.0f, 100.0f }) < 5.0f
                && Distance(brace->points.back(), { 305.0f, 320.0f }) < 5.0f
                && std::abs((brace->points.front().x - b.minX) - 45.0f) < 5.0f && b.maxX - brace->points.front().x < 0.01f,
            "the brace snaps level, keeps its ends and tip depth and opens toward its ends");
    }
    // The ">" between two lumps style.
    CheckEverySeed("a } drawn as a > between two lumps is a brace", RecognizedShapeKind::Brace,
        LumpyBrace([](Vec2 p) { return p; }), 1.5f);
    CheckEverySeed("a lumpy { (mirrored) is a brace", RecognizedShapeKind::Brace,
        LumpyBrace([](Vec2 p) { return Vec2 { 400.0f - p.x, p.y }; }), 1.5f);
    CheckEverySeed("a small lumpy brace on its side is a brace", RecognizedShapeKind::Brace,
        LumpyBrace([](Vec2 p) { return Vec2 { 100.0f + (0.4f * p.y), 100.0f + (0.4f * p.x) }; }), 1.0f);
    // Variants of that stroke: lumps bulging past the tip (the tip is then not
    // the farthest point from the chord), and the tip drawn well above the
    // middle (the top half squashed).
    const auto inZ = [](Vec2 p) { return p.y > 272.0f && p.y < 360.0f; };
    CheckEverySeed("a lumpy brace whose lumps reach past the tip is a brace", RecognizedShapeKind::Brace,
        LumpyBrace([&](Vec2 p) { return inZ(p) ? p : Vec2 { 170.0f + ((p.x - 170.0f) * 1.3f), p.y }; }), 1.5f);
    CheckEverySeed("a lumpy brace with its tip well above the middle is a brace", RecognizedShapeKind::Brace,
        LumpyBrace([](Vec2 p) { return Vec2 { p.x, p.y < 290.0f ? 290.0f - ((290.0f - p.y) * 0.75f) : p.y }; }), 1.5f);
    {
        const std::optional<RecognizedShape> brace = RecognizeShape(HandDrawn(LumpyBrace([](Vec2 p) { return p; }), 1.5f, 2U));
        const Bounds b = Is(brace, RecognizedShapeKind::Brace) ? BoundsOf(brace->points) : Bounds {};
        Check(Is(brace, RecognizedShapeKind::Brace) && b.minX - brace->points.front().x > -0.01f
                && std::abs(b.maxX - 255.0f) < 8.0f && std::abs(brace->points.front().x - brace->points.back().x) < 0.01f,
            "the lumpy brace snaps to a level } reaching the drawn tip");
    }
    {
        // A "<" also turns sharply at its middle, but leaves its ends along
        // the chord.
        int braces = 0;
        for (int seed = 1; seed <= kSeeds; ++seed) {
            const std::vector<Vec2> chevron = Polyline({ { 300.0f, 100.0f }, { 255.0f, 210.0f }, { 300.0f, 320.0f } });
            braces += Is(RecognizeShape(HandDrawn(chevron, 1.0f, static_cast<std::uint32_t>(seed))), RecognizedShapeKind::Brace) ? 1 : 0;
        }
        Check(braces == 0, "a < is not a brace: " + std::to_string(braces) + "/" + std::to_string(kSeeds));
    }
    {
        // A "3" has the lumps but its middle corner points in, not out.
        int braces = 0;
        for (int seed = 1; seed <= kSeeds; ++seed) {
            const std::vector<Vec2> three = Cubics({
                { 165.0f, 83.0f }, { 210.0f, 78.0f }, { 248.0f, 100.0f }, { 245.0f, 145.0f },
                { 240.0f, 200.0f }, { 215.0f, 280.0f }, { 195.0f, 320.0f },
                { 240.0f, 370.0f }, { 255.0f, 440.0f }, { 243.0f, 500.0f },
                { 232.0f, 555.0f }, { 200.0f, 595.0f }, { 175.0f, 610.0f },
            });
            braces += Is(RecognizeShape(HandDrawn(three, 1.5f, static_cast<std::uint32_t>(seed))), RecognizedShapeKind::Brace) ? 1 : 0;
        }
        Check(braces == 0, "a 3 is not a brace: " + std::to_string(braces) + "/" + std::to_string(kSeeds));
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
