// Scratch-out recognition (cnote::RecognizeScratchOut) on synthetic hand-drawn
// strokes over many deterministic seeds: zigzag scratches in any direction
// are recognized, handwriting and shapes are not, and the scratch covers only
// what lies mostly under it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "selection/ScratchOut.h"

namespace {

using cnote::RecognizeScratchOut;
using cnote::ScratchOut;
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

// The ideal path walked at ~1 unit per sample with a slow two-axis wobble of
// `amplitude` and +-0.3 jitter.
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

// One corner-cutting pass: each corner is replaced by the points `cut` of the
// way along its two edges, rounding the turnarounds of a zigzag.
std::vector<Vec2> RoundCorners(const std::vector<Vec2>& vertices, float cut) {
    std::vector<Vec2> rounded { vertices.front() };
    for (std::size_t index = 1; index + 1U < vertices.size(); ++index) {
        const Vec2 a = vertices[index - 1], b = vertices[index], c = vertices[index + 1];
        rounded.push_back({ b.x + ((a.x - b.x) * cut), b.y + ((a.y - b.y) * cut) });
        rounded.push_back({ b.x + ((c.x - b.x) * cut), b.y + ((c.y - b.y) * cut) });
    }
    rounded.push_back(vertices.back());
    return rounded;
}

// `passes` straight passes between x = left and x = right, drifting down from
// `top` to `bottom`: a scratch over a line of writing.
std::vector<Vec2> HorizontalZigzag(float left, float right, float top, float bottom, int passes) {
    std::vector<Vec2> vertices;
    for (int index = 0; index <= passes; ++index) {
        const float y = top + ((bottom - top) * static_cast<float>(index) / static_cast<float>(passes));
        vertices.push_back({ index % 2 == 0 ? left : right, y });
    }
    return vertices;
}

// The same with passes up and down, drifting right.
std::vector<Vec2> VerticalZigzag(float left, float right, float top, float bottom, int passes) {
    std::vector<Vec2> vertices;
    for (int index = 0; index <= passes; ++index) {
        const float x = left + ((right - left) * static_cast<float>(index) / static_cast<float>(passes));
        vertices.push_back({ x, index % 2 == 0 ? bottom : top });
    }
    return vertices;
}

std::vector<Vec2> Rotated(std::vector<Vec2> points, Vec2 center, float angle) {
    for (Vec2& p : points) {
        const float x = p.x - center.x, y = p.y - center.y;
        p = { center.x + (x * std::cos(angle)) - (y * std::sin(angle)), center.y + (x * std::sin(angle)) + (y * std::cos(angle)) };
    }
    return points;
}

std::vector<Vec2> ArcPoints(Vec2 center, float radius, float start, float sweep) {
    std::vector<Vec2> points;
    const int steps = std::max(2, static_cast<int>(std::abs(sweep) * radius));
    for (int step = 0; step <= steps; ++step) {
        const float t = start + (sweep * static_cast<float>(step) / static_cast<float>(steps));
        points.push_back({ center.x + (radius * std::cos(t)), center.y + (radius * std::sin(t)) });
    }
    return points;
}

void Append(std::vector<Vec2>& path, const std::vector<Vec2>& more) {
    path.insert(path.end(), more.begin(), more.end());
}

// Cursive "mmm" (y down): stems of height `height`, humps of radius `radius`
// over the top, each stem but the first retraced back up, letters joined by
// an upstroke from the last stem's foot.
std::vector<Vec2> CursiveM(Vec2 origin, float height, float radius, int letters) {
    std::vector<Vec2> path;
    float x = origin.x;
    const float foot = origin.y + height;
    const float shoulder = origin.y + radius;
    for (int letter = 0; letter < letters; ++letter) {
        Append(path, Polyline({ { x, foot }, { x, shoulder } }));
        for (int hump = 0; hump < 2; ++hump) {
            Append(path, ArcPoints({ x + radius, shoulder }, radius, kPi, kPi));
            x += 2.0f * radius;
            Append(path, Polyline({ { x, shoulder }, { x, foot } }));
            if (hump == 0) {
                Append(path, Polyline({ { x, foot }, { x, shoulder } }));
            }
        }
        Append(path, Polyline({ { x, foot }, { x + (2.0f * radius), shoulder + (0.5f * height) } }));
        x += 2.0f * radius;
        Append(path, Polyline({ path.back(), { x, foot } }));
    }
    return path;
}

// Looped cursive "eeee"/"llll": a prolate cycloid.
std::vector<Vec2> Loops(Vec2 origin, int loops) {
    std::vector<Vec2> path;
    const int steps = loops * 64;
    for (int step = 0; step <= steps; ++step) {
        const float t = 2.0f * kPi * static_cast<float>(step) / 64.0f;
        path.push_back({ origin.x + (5.0f * t) - (12.0f * std::sin(t)), origin.y + (15.0f * std::cos(t)) });
    }
    return path;
}

bool SameScratch(const std::optional<ScratchOut>& a, const std::optional<ScratchOut>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a.has_value()) {
        return true;
    }
    if (a->hull.size() != b->hull.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a->hull.size(); ++index) {
        if (a->hull[index].x != b->hull[index].x || a->hull[index].y != b->hull[index].y) {
            return false;
        }
    }
    return true;
}

// The canvas recognizes the growing stroke through ScratchOutTracker on every
// pointer sample; each prefix must decide and measure exactly like the
// one-shot reference it replaced.
void CheckIncrementalParity(const std::string& what, const std::vector<Vec2>& points) {
    cnote::ScratchOutTracker tracker;
    int mismatches = 0;
    for (std::size_t count = 1; count <= points.size(); ++count) {
        tracker.Append(points[count - 1U]);
        const std::vector<Vec2> prefix(points.begin(), points.begin() + static_cast<long>(count));
        if (!SameScratch(RecognizeScratchOut(prefix), tracker.Recognize())) {
            ++mismatches;
            if (mismatches == 1) {
                Check(false, what + ": prefix " + std::to_string(count) + " differs from the one-shot recognizer");
            }
        }
    }
    if (mismatches == 0) {
        Check(true, what + ": every prefix matches the one-shot recognizer");
    } else {
        Check(false, what + ": " + std::to_string(mismatches) + " of " + std::to_string(points.size()) + " prefixes differ");
    }
}

// Every seed must give `expected`; reports the first mismatch.
void CheckEverySeed(const std::string& what, bool expected, const std::vector<Vec2>& ideal, float amplitude) {
    int hits = 0;
    int firstMiss = 0;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        const bool recognized = RecognizeScratchOut(HandDrawn(ideal, amplitude, static_cast<std::uint32_t>(seed))).has_value();
        if (recognized == expected) {
            ++hits;
        } else if (firstMiss == 0) {
            firstMiss = seed;
        }
    }
    Check(hits == kSeeds, what + ": " + std::to_string(hits) + "/" + std::to_string(kSeeds)
        + (firstMiss != 0 ? " (first miss: seed " + std::to_string(firstMiss) + ")" : ""));
}

}  // namespace

int main() {
    // --- scratch-outs -------------------------------------------------------
    const std::vector<Vec2> overWord = Polyline(HorizontalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 6));
    CheckEverySeed("a back-and-forth scratch over a word", true, overWord, 1.5f);
    CheckEverySeed("a scratch with rounded turnarounds", true,
        Polyline(RoundCorners(HorizontalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 6), 0.08f)), 1.5f);
    CheckEverySeed("an up-and-down scratch drifting along a word", true,
        Polyline(VerticalZigzag(100.0f, 190.0f, 100.0f, 130.0f, 10)), 1.0f);
    CheckEverySeed("a short scratch over one letter (four passes)", true,
        Polyline(HorizontalZigzag(100.0f, 125.0f, 100.0f, 112.0f, 4)), 0.5f);
    CheckEverySeed("a scratch drawn at an angle", true,
        Rotated(overWord, { 160.0f, 115.0f }, 35.0f * kPi / 180.0f), 1.5f);
    {
        // The pen lands with a hook against the first pass and lifts with a
        // flick: neither is a pass.
        std::vector<Vec2> hooked = Polyline({ { 108.0f, 96.0f }, { 100.0f, 100.0f } });
        Append(hooked, overWord);
        Append(hooked, Polyline({ overWord.back(), { 95.0f, 140.0f } }));
        CheckEverySeed("a scratch with a landing hook and a lifting flick", true, hooked, 1.5f);
    }

    // --- not scratch-outs ---------------------------------------------------
    CheckEverySeed("three passes (two reversals) are not enough", false,
        Polyline(HorizontalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 3)), 1.5f);
    CheckEverySeed("a printed \"vvvv\" zigzag is writing", false,
        Polyline(VerticalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 8)), 1.0f);
    CheckEverySeed("cursive \"mmm\" is writing", false, CursiveM({ 100.0f, 100.0f }, 30.0f, 6.0f, 3), 0.5f);
    CheckEverySeed("cursive \"mmm\" with wide humps is writing", false, CursiveM({ 100.0f, 100.0f }, 24.0f, 10.0f, 3), 0.5f);
    CheckEverySeed("looped cursive is writing", false, Loops({ 100.0f, 100.0f }, 5), 0.5f);
    CheckEverySeed("a straight line", false, Polyline({ { 100.0f, 100.0f }, { 300.0f, 140.0f } }), 2.0f);
    CheckEverySeed("a circle", false, ArcPoints({ 200.0f, 200.0f }, 50.0f, 0.0f, 2.0f * kPi), 2.0f);
    Check(!RecognizeScratchOut(Polyline(HorizontalZigzag(100.0f, 106.0f, 100.0f, 104.0f, 4))).has_value(),
        "a tiny scribble is too short to be a scratch-out");
    Check(!RecognizeScratchOut({}).has_value() && !RecognizeScratchOut({ { 1.0f, 1.0f } }).has_value(),
        "an empty or one-point stroke is not a scratch-out");

    // --- coverage -----------------------------------------------------------
    const std::optional<ScratchOut> scratch = RecognizeScratchOut(overWord);
    Check(scratch.has_value(), "the coverage scratch is recognized");
    if (scratch.has_value()) {
        const std::vector<Vec2> word = Polyline({ { 120.0f, 125.0f }, { 135.0f, 105.0f }, { 150.0f, 125.0f },
            { 165.0f, 105.0f }, { 180.0f, 125.0f }, { 200.0f, 110.0f } });
        Check(scratch->CoversPath(word, 1.5f), "a word under the scratch is covered");
        Check(!scratch->CoversPath({ { 0.0f, 115.0f }, { 400.0f, 115.0f } }, 1.5f),
            "a long line the scratch only crosses is not covered");
        Check(scratch->CoversPath({ { 90.0f, 115.0f }, { 200.0f, 115.0f } }, 1.5f),
            "a two-point line mostly under the scratch is covered");
        Check(!scratch->CoversPath(Polyline({ { 120.0f, 160.0f }, { 200.0f, 175.0f } }), 1.5f),
            "a stroke beside the scratch is not covered");
        Check(scratch->CoversPath({ { 160.0f, 115.0f } }, 1.5f) && !scratch->CoversPath({ { 160.0f, 140.0f } }, 1.5f),
            "a dot is covered only under the scratch");
        Check(scratch->CoversPath({ { 223.0f, 115.0f } }, 4.0f) && !scratch->CoversPath({ { 223.0f, 115.0f } }, 1.5f),
            "the margin widens the scratch");
        Check(scratch->CoversArea({ { { 130.0f, 105.0f }, { 190.0f, 105.0f }, { 190.0f, 125.0f }, { 130.0f, 125.0f } } }, 1.5f),
            "a text box under the scratch is covered");
        Check(!scratch->CoversArea({ { { 0.0f, 0.0f }, { 400.0f, 0.0f }, { 400.0f, 400.0f }, { 0.0f, 400.0f } } }, 1.5f),
            "an image the scratch is drawn on is not covered");
        const std::vector<Vec2> box = Rotated({ { 135.0f, 108.0f }, { 185.0f, 108.0f }, { 185.0f, 122.0f }, { 135.0f, 122.0f } },
            { 160.0f, 115.0f }, 20.0f * kPi / 180.0f);
        Check(scratch->CoversArea({ { box[0], box[1], box[2], box[3] } }, 1.5f), "a rotated text box under the scratch is covered");
        const std::vector<Vec2> below = Rotated({ { 135.0f, 120.0f }, { 185.0f, 120.0f }, { 185.0f, 160.0f }, { 135.0f, 160.0f } },
            { 160.0f, 140.0f }, 20.0f * kPi / 180.0f);
        Check(!scratch->CoversArea({ { below[0], below[1], below[2], below[3] } }, 1.5f),
            "a rotated box mostly below the scratch is not covered");
    }

    // --- incremental parity -------------------------------------------------
    // The live canvas recognizes every prefix through ScratchOutTracker; it
    // must match the one-shot reference on scratches, handwriting and shapes
    // alike (including the first sample where nothing is recognized).
    CheckIncrementalParity("scratch over a word", overWord);
    CheckIncrementalParity("rounded scratch",
        Polyline(RoundCorners(HorizontalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 6), 0.08f)));
    CheckIncrementalParity("vertical scratch", Polyline(VerticalZigzag(100.0f, 190.0f, 100.0f, 130.0f, 10)));
    CheckIncrementalParity("short scratch", Polyline(HorizontalZigzag(100.0f, 125.0f, 100.0f, 112.0f, 4)));
    CheckIncrementalParity("angled scratch", Rotated(overWord, { 160.0f, 115.0f }, 35.0f * kPi / 180.0f));
    CheckIncrementalParity("three passes", Polyline(HorizontalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 3)));
    CheckIncrementalParity("\"vvvv\"", Polyline(VerticalZigzag(100.0f, 220.0f, 100.0f, 130.0f, 8)));
    CheckIncrementalParity("cursive \"mmm\"", Polyline(CursiveM({ 100.0f, 100.0f }, 30.0f, 6.0f, 3)));
    CheckIncrementalParity("loops", Polyline(Loops({ 100.0f, 100.0f }, 5)));
    CheckIncrementalParity("straight line", Polyline({ { 100.0f, 100.0f }, { 300.0f, 140.0f } }));
    CheckIncrementalParity("circle", Polyline(ArcPoints({ 200.0f, 200.0f }, 50.0f, 0.0f, 2.0f * kPi)));
    for (int seed = 1; seed <= 4; ++seed) {
        CheckIncrementalParity("hand-drawn scratch, seed " + std::to_string(seed),
            HandDrawn(overWord, 1.5f, static_cast<std::uint32_t>(seed)));
        CheckIncrementalParity("hand-drawn \"mmm\", seed " + std::to_string(seed),
            HandDrawn(Polyline(CursiveM({ 100.0f, 100.0f }, 30.0f, 6.0f, 3)), 0.5f, static_cast<std::uint32_t>(seed)));
    }

    std::printf("%s\n", g_failures == 0 ? "All scratch-out tests passed." : "Scratch-out tests FAILED.");
    return g_failures == 0 ? 0 : 1;
}
