// Deterministic regression test for the stroke render-path cache (PERF-023).
//
// Every hit test, render and PDF export stabilizes and smooths the same stroke
// from its raw points.  The cache is content-addressed: raw points and
// pressures, the effective sampling, the effective smoothing and whether the
// pressure channel is wanted.  That makes it correct without an epoch or
// call-site invalidation, including in-place gesture edits (which rewrite the
// points), undo/redo, clipboard copies and multiple views.  This test drives
// the real consumer entry points (SelectionMath hit tests and the page
// renderer), asserts exact parity with the uncached BuildStrokeRenderPath, and
// counts recomputations so "unchanged content rebuilds nothing" cannot regress.
//
// The dense-page fixture at the bottom is also the measurement fixture: the
// printed cold (cache cleared before every query, i.e. the old per-call work)
// and warm timings describe exactly the page this test builds.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include "Document.h"
#include "render/PageRenderer.h"
#include "selection/StrokeSmoothing.h"
#include "ui/SelectionMath.h"

namespace {

using cnote::InkPath;
using cnote::Page;
using cnote::Stroke;
using cnote::StrokeSampling;
using cnote::Vec2;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

bool SameBits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool SamePoints(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (!SameBits(a[index].x, b[index].x) || !SameBits(a[index].y, b[index].y)) {
            return false;
        }
    }
    return true;
}

bool SameInkPath(const InkPath& a, const InkPath& b) {
    if (a.points.size() != b.points.size() || a.pressures.size() != b.pressures.size()) {
        return false;
    }
    if (!SamePoints(a.points, b.points)) {
        return false;
    }
    for (std::size_t index = 0; index < a.pressures.size(); ++index) {
        if (!SameBits(a.pressures[index], b.pressures[index])) {
            return false;
        }
    }
    return true;
}

std::vector<Stroke> ParityStrokes() {
    std::vector<Stroke> strokes;
    const auto addPoints = [](Stroke& stroke, std::initializer_list<Vec2> points) {
        stroke.points.assign(points.begin(), points.end());
    };

    {
        Stroke stroke;  // empty
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;
        addPoints(stroke, { { 10.0f, 10.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;
        addPoints(stroke, { { 10.0f, 10.0f }, { 90.0f, 40.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // no smoothing
        stroke.smoothing = 0.0f;
        addPoints(stroke, { { 10.0f, 10.0f }, { 60.0f, 30.0f }, { 110.0f, 90.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // default smoothing
        stroke.smoothing = 0.55f;
        addPoints(stroke, { { 10.0f, 10.0f }, { 60.0f, 30.0f }, { 110.0f, 90.0f }, { 150.0f, 40.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // maximum smoothing
        stroke.smoothing = 5.0f;
        addPoints(stroke, { { 0.0f, 0.0f }, { 30.0f, 80.0f }, { 70.0f, -20.0f }, { 140.0f, 10.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // the fallback decides the strength
        stroke.smoothing = std::numeric_limits<float>::quiet_NaN();
        addPoints(stroke, { { 0.0f, 0.0f }, { 30.0f, 80.0f }, { 70.0f, -20.0f }, { 140.0f, 10.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // reference pull/spacing (App.cpp:1958-1979)
        stroke.sampling = StrokeSampling { 0.3f, 1.75f };
        addPoints(stroke, { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 4.0f, 0.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // spacing-only resampling
        stroke.sampling = StrokeSampling { 0.0f, 1.0f };
        addPoints(stroke, { { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 4.0f, 0.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // out-of-range sampling clamps in Reset
        stroke.sampling = StrokeSampling { 3.0f, -4.0f };
        addPoints(stroke, { { 0.0f, 0.0f }, { 5.0f, 0.0f }, { 9.0f, 5.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // non-finite sampling falls back to the identity
        stroke.sampling = StrokeSampling {
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN(),
        };
        addPoints(stroke, { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 4.0f, 0.0f } });
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // pressure travels through both stages
        addPoints(stroke, { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 4.0f, 0.0f }, { 8.0f, 2.0f } });
        stroke.pressures = { 0.2f, 0.6f, 1.0f, 0.4f };
        stroke.sampling = StrokeSampling { 0.3f, 1.75f };
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // dotted strokes never render a pressure outline
        stroke.style.dotted = true;
        addPoints(stroke, { { 0.0f, 0.0f }, { 20.0f, 0.0f }, { 40.0f, 5.0f } });
        stroke.pressures = { 0.2f, 0.6f, 1.0f };
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // mismatched pressures are ignored
        addPoints(stroke, { { 0.0f, 0.0f }, { 20.0f, 0.0f }, { 40.0f, 5.0f } });
        stroke.pressures = { 0.2f };
        strokes.push_back(stroke);
    }
    {
        Stroke stroke;  // a long pull hits the 64-step resample cap
        stroke.sampling = StrokeSampling { 0.0f, 0.05f };
        addPoints(stroke, { { 0.0f, 0.0f }, { 400.0f, 0.0f } });
        strokes.push_back(stroke);
    }
    return strokes;
}

void TestParity() {
    cnote::StrokePathCache cache;
    const float fallback = 0.55f;
    std::size_t caseIndex = 0;
    for (const Stroke& stroke : ParityStrokes()) {
        const std::string label = "case " + std::to_string(caseIndex++);
        const InkPath expected = cnote::BuildStrokeRenderPath(stroke, fallback);
        cache.Clear();
        const std::size_t recomputesBefore = cache.RecomputeCount();
        const std::size_t hitsBefore = cache.HitCount();

        const std::shared_ptr<const InkPath> full = cache.Path(stroke, fallback, true);
        Check(SameInkPath(*full, expected), label + ": cached full path equals BuildStrokeRenderPath");
        Check(cache.RecomputeCount() == recomputesBefore + 1U, label + ": first query computes once");

        const std::shared_ptr<const InkPath> again = cache.Path(stroke, fallback, true);
        Check(SameInkPath(*again, expected), label + ": repeated query returns the same path");
        Check(cache.RecomputeCount() == recomputesBefore + 1U,
            label + ": repeated query rebuilds nothing");
        Check(cache.HitCount() == hitsBefore + 1U, label + ": repeated query is a hit");

        const std::shared_ptr<const InkPath> geometry = cache.Path(stroke, fallback, false);
        Check(geometry->pressures.empty(), label + ": geometry-only path carries no pressures");
        Check(SamePoints(geometry->points, expected.points),
            label + ": geometry-only points equal the full path");
        cache.Clear();
    }
}

void TestMutationAndCopies() {
    const float fallback = 0.55f;
    Stroke stroke;
    stroke.smoothing = 0.6f;
    stroke.sampling = StrokeSampling { 0.25f, 1.5f };
    stroke.points = { { 0.0f, 0.0f }, { 20.0f, 10.0f }, { 60.0f, 40.0f }, { 90.0f, 10.0f } };
    stroke.pressures = { 0.2f, 0.4f, 0.7f, 1.0f };

    cnote::StrokePathCache cache;
    const std::shared_ptr<const InkPath> original = cache.Path(stroke, fallback, true);
    const std::size_t afterOriginal = cache.RecomputeCount();

    // A copy with identical content (clipboard, history snapshot, second view)
    // shares the entry: no rebuild, no dependence on object identity.
    Stroke copy = stroke;
    const std::shared_ptr<const InkPath> copied = cache.Path(copy, fallback, true);
    Check(SameInkPath(*copied, *original), "a content copy returns the same path");
    Check(cache.RecomputeCount() == afterOriginal, "a content copy rebuilds nothing");

    // An in-place gesture translation (MoveSelectionBy rewrites the points
    // without a page-epoch bump) must miss and match the uncached translation.
    cnote::ui::TranslateStrokePoints(stroke, Vec2 { 37.0f, -12.0f });
    const std::shared_ptr<const InkPath> moved = cache.Path(stroke, fallback, true);
    Check(SameInkPath(*moved, cnote::BuildStrokeRenderPath(stroke, fallback)),
        "translated-in-place stroke builds the translated path");
    Check(cache.RecomputeCount() == afterOriginal + 1U, "translated-in-place stroke rebuilds");

    // Rewriting an interior point keeps the first, last and count but must
    // still miss, which a sample-only key would get wrong.
    const std::size_t afterMove = cache.RecomputeCount();
    stroke.points[1].y += 25.0f;
    const std::shared_ptr<const InkPath> edited = cache.Path(stroke, fallback, true);
    Check(SameInkPath(*edited, cnote::BuildStrokeRenderPath(stroke, fallback)),
        "interior point edit builds the edited path");
    Check(cache.RecomputeCount() == afterMove + 1U, "interior point edit rebuilds");

    // Smoothing and sampling are part of the key.
    stroke.smoothing = 0.1f;
    static_cast<void>(cache.Path(stroke, fallback, true));
    Check(cache.RecomputeCount() == afterMove + 2U, "smoothing change rebuilds");
    stroke.sampling.spacing = 3.0f;
    static_cast<void>(cache.Path(stroke, fallback, true));
    Check(cache.RecomputeCount() == afterMove + 3U, "sampling change rebuilds");

    // A finite stroke.smoothing ignores the fallback, so both callers share.
    const std::size_t beforeFallback = cache.RecomputeCount();
    const std::shared_ptr<const InkPath> otherFallback = cache.Path(stroke, 0.9f, true);
    Check(SameInkPath(*otherFallback, cnote::BuildStrokeRenderPath(stroke, 0.9f)),
        "different fallback with finite smoothing matches the shared path");
    Check(cache.RecomputeCount() == beforeFallback,
        "different fallback with finite smoothing reuses the entry");

    // A non-finite stroke.smoothing takes the fallback, so the fallback is part
    // of the identity.
    stroke.smoothing = std::numeric_limits<float>::quiet_NaN();
    static_cast<void>(cache.Path(stroke, 0.2f, true));
    static_cast<void>(cache.Path(stroke, 0.9f, true));
    Check(cache.RecomputeCount() == beforeFallback + 2U,
        "different fallbacks on a NaN smoothing rebuild");

    // Undo restores a copied snapshot, not a translated vector, so the earlier
    // content comes back exactly.  Its entry is still valid (content-addressed)
    // even though the page epoch/undo stack changed around it.
    {
        cnote::StrokePathCache undoCache;
        Stroke base;
        base.points = { { 0.0f, 0.0f }, { 15.0f, 5.0f }, { 30.0f, 0.0f }, { 45.0f, 10.0f } };
        base.pressures = { 0.1f, 0.4f, 0.7f, 1.0f };
        const Stroke saved = base;
        const std::shared_ptr<const InkPath> before = undoCache.Path(base, fallback, true);
        cnote::ui::TranslateStrokePoints(base, Vec2 { 80.0f, 60.0f });
        const std::size_t afterMoveRecomputes = undoCache.RecomputeCount();
        static_cast<void>(undoCache.Path(base, fallback, true));
        Check(undoCache.RecomputeCount() == afterMoveRecomputes + 1U, "gesture edit after warming rebuilds");
        base = saved;  // undo/redo restores the copied object
        const std::shared_ptr<const InkPath> restored = undoCache.Path(base, fallback, true);
        Check(SameInkPath(*restored, *before), "undo restores the original path");
        Check(undoCache.RecomputeCount() == afterMoveRecomputes + 1U,
            "undo of a warmed stroke rebuilds nothing");
    }
}

void TestBoundedCache() {
    cnote::StrokePathCache cache(3U, 2048U);
    std::vector<Stroke> strokes;
    for (int index = 0; index < 12; ++index) {
        Stroke stroke;
        stroke.points = {
            { static_cast<float>(index), 0.0f },
            { static_cast<float>(index) + 10.0f, 10.0f },
            { static_cast<float>(index) + 20.0f, -5.0f },
            { static_cast<float>(index) + 30.0f, 15.0f },
        };
        strokes.push_back(stroke);
    }
    for (const Stroke& stroke : strokes) {
        static_cast<void>(cache.Path(stroke, 0.55f, true));
    }
    Check(cache.EntryCount() <= 3U, "entry bound holds");
    Check(cache.CachedBytes() <= 2048U, "byte bound holds");
    Check(cache.PeakCachedBytes() <= 2048U, "peak bytes stay under the byte bound");
    Check(cache.RecomputeCount() == strokes.size(), "distinct contents each compute once");
    Check(cache.EntryCount() > 0U, "bounded cache retains the most recent entries");

    // A single centerline larger than the byte budget is returned, not stored.
    cnote::StrokePathCache tiny(4U, 64U);
    Stroke longStroke;
    longStroke.sampling = StrokeSampling { 0.0f, 0.05f };
    longStroke.points.reserve(200);
    for (int index = 0; index < 200; ++index) {
        longStroke.points.push_back(Vec2 { static_cast<float>(index) * 2.0f, 0.0f });
    }
    static_cast<void>(tiny.Path(longStroke, 0.55f, true));
    Check(tiny.EntryCount() == 0U, "oversized path is not retained");
    Check(tiny.CachedBytes() == 0U, "oversized path adds no retained bytes");
    Check(tiny.PeakCachedBytes() == 0U, "oversized path never exceeds the byte bound");

    // Least-recently-used eviction: touching an old entry keeps it resident.
    cnote::StrokePathCache lru(2U, 1U << 20U);
    static_cast<void>(lru.Path(strokes[0], 0.55f, true));
    static_cast<void>(lru.Path(strokes[1], 0.55f, true));
    static_cast<void>(lru.Path(strokes[0], 0.55f, true));  // hit: 0 is now newest
    static_cast<void>(lru.Path(strokes[2], 0.55f, true));  // evicts 1
    const std::size_t recomputes = lru.RecomputeCount();
    static_cast<void>(lru.Path(strokes[0], 0.55f, true));
    Check(lru.RecomputeCount() == recomputes, "recently used entry survives eviction");
    static_cast<void>(lru.Path(strokes[1], 0.55f, true));
    Check(lru.RecomputeCount() == recomputes + 1U, "evicted entry recomputes");
}

Page MakeDensePage(std::size_t strokeCount, std::size_t pointCount) {
    Page page;
    page.width = 612.0f;
    page.height = 792.0f;
    page.strokes.reserve(strokeCount);
    constexpr float kPi = 3.14159265358979323846f;
    for (std::size_t index = 0; index < strokeCount; ++index) {
        Stroke stroke;
        stroke.style.width = 2.0f + static_cast<float>(index % 3U);
        stroke.smoothing = 0.55f;
        stroke.sampling = StrokeSampling { 0.35f, 1.2f };
        stroke.style.dotted = index % 11U == 5U;
        const float column = static_cast<float>(index % 16U);
        const float row = static_cast<float>(index / 16U);
        const float left = 22.0f + (column * 36.0f);
        const float top = 26.0f + (row * 40.0f);
        stroke.points.reserve(pointCount);
        for (std::size_t point = 0; point < pointCount; ++point) {
            const float t = pointCount > 1U
                ? static_cast<float>(point) / static_cast<float>(pointCount - 1U)
                : 0.0f;
            const float phase = static_cast<float>(index) * 0.37f;
            stroke.points.push_back(Vec2 {
                left + (t * 30.0f),
                top + (t * 22.0f) + (std::sin((t * 2.0f * kPi) + phase) * 6.0f),
            });
            if (index % 4U == 1U) {
                stroke.pressures.push_back(0.2f + (0.8f * t));
            }
        }
        page.strokes.push_back(std::move(stroke));
    }
    return page;
}

QImage PaintDenseStrokes(const Page& page) {
    QImage image(static_cast<int>(page.width), static_cast<int>(page.height), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    cnote::render::PageRenderParams params;
    params.scale = 1.0f;
    params.strokeSmoothing = 0.55f;
    cnote::render::ExcludedIndices only;
    only.strokes.reserve(page.strokes.size());
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        only.strokes.push_back(index);
    }
    cnote::render::PaintPageSubset(painter, page, QPointF(0.0, 0.0), params, only);
    painter.end();
    return image;
}

bool SameImage(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.format() != b.format()) {
        return false;
    }
    for (int row = 0; row < a.height(); ++row) {
        if (std::memcmp(a.constScanLine(row), b.constScanLine(row),
                static_cast<std::size_t>(a.bytesPerLine())) != 0) {
            return false;
        }
    }
    return true;
}

void TestConsumersRebuildNothing() {
    constexpr float kFallback = 0.55f;
    Stroke stroke;
    stroke.smoothing = 0.55f;
    stroke.sampling = StrokeSampling { 0.3f, 1.5f };
    stroke.points = { { 10.0f, 10.0f }, { 40.0f, 25.0f }, { 80.0f, 70.0f }, { 120.0f, 40.0f } };

    cnote::StrokePathCache& shared = cnote::SharedStrokePathCache();
    shared.Clear();
    const std::size_t hitRecomputes = shared.RecomputeCount();
    Check(cnote::ui::StrokeHitsPoint(stroke, Vec2 { 0.0f, 0.0f }, 200.0f, kFallback),
        "hit test finds the stroke");
    Check(cnote::ui::StrokeHitsPoint(stroke, Vec2 { 0.0f, 0.0f }, 200.0f, kFallback),
        "repeated hit test finds the stroke");
    Check(shared.RecomputeCount() == hitRecomputes + 1U,
        "two hit tests of unchanged content rebuild once");

    // The renderer is the other hot consumer: a second paint of the same page
    // must reuse every centerline, and both paints must be byte-identical.
    Page page = MakeDensePage(48U, 24U);
    shared.Clear();
    const std::size_t renderRecomputes = shared.RecomputeCount();
    const QImage cold = PaintDenseStrokes(page);
    Check(shared.RecomputeCount() == renderRecomputes + page.strokes.size(),
        "first paint builds one centerline per stroke");
    const QImage warm = PaintDenseStrokes(page);
    Check(shared.RecomputeCount() == renderRecomputes + page.strokes.size(),
        "second paint rebuilds nothing");
    Check(SameImage(cold, warm), "cached and uncached paints are byte-identical");
}

std::uint64_t NowMicroseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double Median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2U];
}

void ReportDensePageTimings(std::size_t strokeCount, std::size_t pointCount) {
    constexpr float kFallback = 0.55f;
    const Page page = MakeDensePage(strokeCount, pointCount);
    cnote::StrokePathCache& shared = cnote::SharedStrokePathCache();
    const Vec2 missPoint { 4.0f, 782.0f };  // below every fixture stroke

    const auto hoverSweep = [&page, missPoint, kFallback]() {
        bool hit = false;
        for (const Stroke& stroke : page.strokes) {
            hit = cnote::ui::StrokeHitsPoint(stroke, missPoint, 0.5f, kFallback) || hit;
        }
        return hit;
    };
    // Warm once so both loops measure steady state and no first-allocation
    // noise; the cold loop then clears before every sweep.
    static_cast<void>(hoverSweep());
    std::vector<double> hoverCold;
    std::vector<double> hoverWarm;
    constexpr int kHoverRounds = 15;
    for (int round = 0; round < kHoverRounds; ++round) {
        shared.Clear();
        const std::uint64_t start = NowMicroseconds();
        static_cast<void>(hoverSweep());
        hoverCold.push_back(static_cast<double>(NowMicroseconds() - start));
        const std::uint64_t warmStart = NowMicroseconds();
        static_cast<void>(hoverSweep());
        hoverWarm.push_back(static_cast<double>(NowMicroseconds() - warmStart));
    }

    const auto paint = [&page]() {
        const QImage image = PaintDenseStrokes(page);
        return static_cast<std::uint64_t>(image.sizeInBytes());
    };
    static_cast<void>(paint());
    std::vector<double> paintCold;
    std::vector<double> paintWarm;
    constexpr int kPaintRounds = 7;
    for (int round = 0; round < kPaintRounds; ++round) {
        shared.Clear();
        const std::uint64_t start = NowMicroseconds();
        static_cast<void>(paint());
        paintCold.push_back(static_cast<double>(NowMicroseconds() - start));
        const std::uint64_t warmStart = NowMicroseconds();
        static_cast<void>(paint());
        paintWarm.push_back(static_cast<double>(NowMicroseconds() - warmStart));
    }

    // The warm paint must hit every stroke; this is the same assertion as the
    // counter test but on the measurement fixture.
    shared.Clear();
    static_cast<void>(PaintDenseStrokes(page));
    const std::size_t builds = shared.RecomputeCount();
    static_cast<void>(PaintDenseStrokes(page));
    Check(shared.RecomputeCount() == builds, "measurement fixture: warm paint rebuilds nothing");

    // Isolate the renderer's centerline phase from QPainter rasterization: the
    // old per-paint work was BuildStrokeRenderPath for every stroke, the new
    // work is the same number of cache hits.
    std::vector<double> pathCold;
    std::vector<double> pathWarm;
    constexpr int kPathRounds = 15;
    for (int round = 0; round < kPathRounds; ++round) {
        const std::uint64_t start = NowMicroseconds();
        for (const Stroke& stroke : page.strokes) {
            static_cast<void>(cnote::BuildStrokeRenderPath(stroke, kFallback));
        }
        pathCold.push_back(static_cast<double>(NowMicroseconds() - start));
        const std::uint64_t warmStart = NowMicroseconds();
        for (const Stroke& stroke : page.strokes) {
            static_cast<void>(cnote::CachedStrokeRenderPath(stroke, kFallback, true));
        }
        pathWarm.push_back(static_cast<double>(NowMicroseconds() - warmStart));
    }

    const double coldHover = Median(hoverCold);
    const double warmHover = Median(hoverWarm);
    const double coldPaint = Median(paintCold) / 1000.0;
    const double warmPaint = Median(paintWarm) / 1000.0;
    const double coldBuild = Median(pathCold) / 1000.0;
    const double warmBuild = Median(pathWarm) / 1000.0;
    std::printf("FIXTURE strokes=%zu points=%zu (dense page %.0fx%.0f)\n",
        strokeCount, pointCount, page.width, page.height);
    std::printf("TIMING hover cold %.2f ms/sweep (%.2f us/stroke) warm %.2f ms/sweep (%.2f us/stroke)\n",
        coldHover / 1000.0, coldHover / static_cast<double>(strokeCount),
        warmHover / 1000.0, warmHover / static_cast<double>(strokeCount));
    std::printf("TIMING render-paths cold %.2f ms/sweep (%.2f us/stroke) warm %.2f ms/sweep (%.2f us/stroke)\n",
        coldBuild, (coldBuild * 1000.0) / static_cast<double>(strokeCount),
        warmBuild, (warmBuild * 1000.0) / static_cast<double>(strokeCount));
    std::printf("TIMING paint end-to-end cold %.2f ms/paint warm %.2f ms/paint\n", coldPaint, warmPaint);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);

    TestParity();
    TestMutationAndCopies();
    TestBoundedCache();
    TestConsumersRebuildNothing();
    ReportDensePageTimings(120U, 64U);

    std::printf("SUMMARY failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
