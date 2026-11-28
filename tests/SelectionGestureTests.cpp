// Deterministic offscreen regression test for the selection-gesture geometry
// work (PERF-018, PERF-019, PERF-020).
//
// PERF-018: damage, handle hover and the overlay paint used to measure every
// selected stroke/text/LaTeX block independently — a move or transform sample
// recomputed the outlines for the old frame, the new frame and the paint, and
// every pointer move recomputed them again for the handle hit test.  The
// canvas now shares one SelectionGeometryCache entry across all consumers.
// PERF-019: UpdateTransform hoists the affine coefficients and sin/cos per
// sample and measures the captured text/LaTeX source boxes once per gesture.
// PERF-020: a lasso sample appends one vertex instead of copying the path and
// scans only its incremental bounds; the release rejects objects whose cached
// bounds miss the polygon before smoothing or segment tests.
//
// The test drives the real QtDocumentController + CanvasWidget offscreen and
// asserts:
//   (a) one selection measurement serves many hovers/paints, and every
//       mutation path (selection change, move commit, undo) invalidates it;
//   (b) a move/transform sample measures the selection at most once and the
//       following hover/paint measures nothing;
//   (c) the transform's absolute semantics hold with the per-gesture source
//       bounds cache (a sample sequence equals applying the last sample once),
//       for every object kind and for the box-resize and Shift-scale paths;
//   (d) lasso releases test only objects whose cached bounds meet the
//       polygon, with identical selection results to a brute-force scan for
//       convex, concave and self-intersecting lassos;
//   (e) overlay pixels are identical warm and cold and do not change under
//       hover.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QImage>
#include <QPointF>
#include <QString>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "qt/QtDocumentController.h"
#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/SelectionGeometry.h"
#include "ui/SelectionMath.h"

namespace {

namespace ui = cnote::ui;
namespace text = cnote::text;
using cnote::Document;
using cnote::Page;
using cnote::PageImage;
using cnote::PageLatex;
using cnote::PageText;
using cnote::Stroke;
using cnote::Vec2;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

bool SameVec(Vec2 a, Vec2 b) {
    return a.x == b.x && a.y == b.y;
}

Stroke MakeStroke(float x, float y, int points = 20) {
    Stroke stroke;
    stroke.style.width = 2.0f;
    stroke.sampling.spacing = 1.0f;
    stroke.points.reserve(static_cast<std::size_t>(points));
    for (int index = 0; index < points; ++index) {
        stroke.points.push_back(Vec2 {
            x + static_cast<float>(index) * 1.5f,
            y + std::sin(static_cast<float>(index) * 0.4f) * 6.0f,
        });
    }
    return stroke;
}

PageText MakeText(std::uint64_t id, float x, float y, float rotation, const char16_t* content) {
    PageText text;
    text.id = id;
    text.x = x;
    text.y = y;
    text.fontSize = 18.0f;
    text.content = content;
    text.rotationDegrees = rotation;
    return text;
}

PageLatex MakeLatex(std::uint64_t id, float x, float y, float rotation) {
    PageLatex latex;
    latex.id = id;
    latex.x = x;
    latex.y = y;
    latex.fontSize = 18.0f;
    latex.content = u"x^2";
    latex.rotationDegrees = rotation;
    return latex;
}

PageImage MakeImage(float x, float y, float rotation) {
    PageImage image;
    image.x = x;
    image.y = y;
    image.width = 50.0f;
    image.height = 30.0f;
    image.rotationDegrees = rotation;
    return image;
}

// One page with every object kind, one rotated text/latex/image each.
void SeedObjects(Page& page) {
    page.strokes.push_back(MakeStroke(40.0f, 60.0f));
    page.strokes.push_back(MakeStroke(80.0f, 90.0f));
    page.images.push_back(MakeImage(160.0f, 60.0f, 0.0f));
    page.images.push_back(MakeImage(160.0f, 130.0f, 25.0f));
    page.texts.push_back(MakeText(1U, 240.0f, 60.0f, 0.0f, u"hello world"));
    page.texts.push_back(MakeText(2U, 240.0f, 140.0f, 30.0f, u"rotated text"));
    page.latexes.push_back(MakeLatex(3U, 360.0f, 60.0f, 0.0f));
    page.latexes.push_back(MakeLatex(4U, 360.0f, 140.0f, 20.0f));
}

ui::PageSelection AllSelection(const Page& page) {
    ui::PageSelection selection;
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        selection.strokes.push_back(index);
    }
    for (std::size_t index = 0; index < page.images.size(); ++index) {
        selection.images.push_back(index);
    }
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        selection.texts.push_back(index);
    }
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        selection.latexes.push_back(index);
    }
    return selection;
}

std::unique_ptr<cnote::qt::QtDocumentController> NewController(cnote::AppManager& manager) {
    auto controller = std::make_unique<cnote::qt::QtDocumentController>(manager);
    controller->NewBlankDocument();
    return controller;
}

QPointF Logical(const ui::CanvasWidget& canvas, Vec2 pagePoint) {
    return canvas.PageToScreen(0, pagePoint);
}

// The controller's selection after a lasso, computed exactly as the canvas
// does but without the cached-bounds pre-rejection.
ui::PageSelection BruteForceLasso(const Page& page, const std::vector<Vec2>& polygon, float smoothing) {
    ui::PageSelection selection;
    if (polygon.size() < 3U) {
        return selection;
    }
    for (std::size_t index = 0; index < page.strokes.size(); ++index) {
        if (ui::StrokeIntersectsPolygonSelection(page.strokes[index], polygon, smoothing)) {
            selection.strokes.push_back(index);
        }
    }
    for (std::size_t index = 0; index < page.images.size(); ++index) {
        if (ui::ImageIntersectsPolygonSelection(page.images[index], polygon)) {
            selection.images.push_back(index);
        }
    }
    for (std::size_t index = 0; index < page.texts.size(); ++index) {
        const text::QtTextBounds bounds = text::MeasurePageTextBoundsQt(page.texts[index], page.width);
        if (ui::RotatedRectIntersectsPolygon(bounds.left, bounds.top, bounds.right, bounds.bottom,
                page.texts[index].rotationDegrees, polygon)) {
            selection.texts.push_back(index);
        }
    }
    for (std::size_t index = 0; index < page.latexes.size(); ++index) {
        if (ui::LatexIntersectsPolygonSelection(page.latexes[index], page.width, polygon)) {
            selection.latexes.push_back(index);
        }
    }
    return selection;
}

bool SameSelection(const ui::PageSelection& a, const ui::PageSelection& b) {
    return a.strokes == b.strokes && a.images == b.images && a.texts == b.texts
        && a.latexes == b.latexes;
}

bool SamePage(const Page& a, const Page& b) {
    if (a.strokes.size() != b.strokes.size() || a.images.size() != b.images.size()
        || a.texts.size() != b.texts.size() || a.latexes.size() != b.latexes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.strokes.size(); ++index) {
        const Stroke& left = a.strokes[index];
        const Stroke& right = b.strokes[index];
        if (left.points.size() != right.points.size() || left.style.width != right.style.width
            || left.sampling.spacing != right.sampling.spacing) {
            return false;
        }
        for (std::size_t point = 0; point < left.points.size(); ++point) {
            if (!SameVec(left.points[point], right.points[point])) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < a.images.size(); ++index) {
        const PageImage& left = a.images[index];
        const PageImage& right = b.images[index];
        if (left.x != right.x || left.y != right.y || left.width != right.width
            || left.height != right.height || left.rotationDegrees != right.rotationDegrees) {
            return false;
        }
    }
    for (std::size_t index = 0; index < a.texts.size(); ++index) {
        const PageText& left = a.texts[index];
        const PageText& right = b.texts[index];
        if (left.id != right.id || left.x != right.x || left.y != right.y || left.width != right.width
            || left.fontSize != right.fontSize || left.rotationDegrees != right.rotationDegrees
            || left.content != right.content) {
            return false;
        }
        if (left.styleSpans.size() != right.styleSpans.size()) {
            return false;
        }
        for (std::size_t span = 0; span < left.styleSpans.size(); ++span) {
            if (left.styleSpans[span].style.fontSize != right.styleSpans[span].style.fontSize) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < a.latexes.size(); ++index) {
        const PageLatex& left = a.latexes[index];
        const PageLatex& right = b.latexes[index];
        if (left.id != right.id || left.x != right.x || left.y != right.y
            || left.fontSize != right.fontSize || left.rotationDegrees != right.rotationDegrees
            || left.content != right.content) {
            return false;
        }
    }
    return true;
}

// A static selection: one measurement, then no work for hover or paint, and
// every mutation path invalidates exactly once.
void TestStaticSelection(cnote::AppManager& manager) {
    auto controller = NewController(manager);
    Document& document = controller->Session()->document;
    Page& page = document.PageAt(0);
    SeedObjects(page);
    document.TouchPage(0);

    ui::CanvasWidget canvas(*controller);
    canvas.resize(1000, 1000);
    controller->SelectObjects(0, AllSelection(page));

    const QPointF probe = Logical(canvas, Vec2 { 150.0f, 150.0f });
    const std::size_t start = canvas.SelectionGeometryRecomputeCount();
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    Check(canvas.SelectionGeometryRecomputeCount() == start + 1U,
        "the first handle probe measures the selection once");

    for (int hover = 0; hover < 50; ++hover) {
        static_cast<void>(canvas.HitTestSelectionHandle(Logical(canvas, Vec2 {
            30.0f + static_cast<float>(hover), 30.0f + static_cast<float>(hover) })));
        static_cast<void>(canvas.CursorInteractsWithSelection(Logical(canvas, Vec2 {
            300.0f - static_cast<float>(hover), 200.0f })));
    }
    Check(canvas.SelectionGeometryRecomputeCount() == start + 1U,
        "50 hover probes over a static selection measure nothing");

    const QImage painted = canvas.grab().toImage();
    Check(canvas.SelectionGeometryRecomputeCount() == start + 1U,
        "the overlay paint reuses the measured outlines");
    Check(painted == canvas.grab().toImage(), "overlay pixels are stable across paints");

    // A selection change is a structural invalidation: the new subset is
    // measured once, then stable again.
    ui::PageSelection subset;
    subset.strokes = { 0U };
    controller->SelectObjects(0, subset);
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    Check(canvas.SelectionGeometryRecomputeCount() == start + 2U,
        "a selection change measures the new selection once");
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    static_cast<void>(canvas.grab());
    Check(canvas.SelectionGeometryRecomputeCount() == start + 2U,
        "the changed selection is stable for hover and paint");

    // A paint while no selection exists must not measure anything.
    controller->ClearSelection();
    static_cast<void>(canvas.grab());
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    Check(canvas.SelectionGeometryRecomputeCount() == start + 2U,
        "an empty selection measures nothing");
}

// Move samples: one measurement per sample, none for the following hover and
// paint, and the cached frame matches the moved objects after commit.
void TestMoveSamples(cnote::AppManager& manager) {
    auto controller = NewController(manager);
    Document& document = controller->Session()->document;
    Page& page = document.PageAt(0);
    SeedObjects(page);
    document.TouchPage(0);

    ui::CanvasWidget canvas(*controller);
    canvas.resize(1000, 1000);
    controller->SelectObjects(0, AllSelection(page));
    const QPointF probe = Logical(canvas, Vec2 { 150.0f, 150.0f });
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    ui::SelectionGeometryCache startCache;
    const ui::SelectionOutlines startOutlines = startCache.ForPage(
        document, 0, controller->SelectionFor(0), controller->TransientGeneration());

    const std::size_t beforeGesture = canvas.SelectionGeometryRecomputeCount();
    canvas.BeginMoveSelection(0);
    Check(canvas.SelectionGeometryRecomputeCount() == beforeGesture,
        "starting a move reuses the measured selection frame");

    int measuredSamples = 0;
    for (int sample = 0; sample < 12; ++sample) {
        const std::size_t before = canvas.SelectionGeometryRecomputeCount();
        canvas.UpdateMoveSelection(QPointF(3.0, 2.0));
        const std::size_t after = canvas.SelectionGeometryRecomputeCount();
        Check(after - before <= 1U, "a move sample measures the selection at most once");
        measuredSamples += after - before == 1U ? 1 : 0;

        // The paint and the cursor probe that follow the sample reuse the
        // entry measured for the damage rect.
        const std::size_t beforeConsumers = canvas.SelectionGeometryRecomputeCount();
        static_cast<void>(canvas.HitTestSelectionHandle(probe));
        static_cast<void>(canvas.CursorInteractsWithSelection(probe));
        static_cast<void>(canvas.grab());
        Check(canvas.SelectionGeometryRecomputeCount() == beforeConsumers,
            "hover and paint after a move sample measure nothing");
    }
    Check(measuredSamples == 12, "every moving sample measured the selection exactly once");

    canvas.CommitMoveSelection();
    // The commit bumps the page epoch; the next query measures the moved
    // selection once and it must match a cold measurement.
    ui::SelectionGeometryCache reference;
    const ui::SelectionOutlines& cold = reference.ForPage(
        document, 0, controller->SelectionFor(0), controller->TransientGeneration());
    Check(cold.bounds.has_value(), "the committed move leaves a selectable frame");
    Check(reference.RecomputeCount() == 1U, "a cold query measures the moved selection once");
    const std::size_t afterCommit = canvas.SelectionGeometryRecomputeCount();
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    Check(canvas.SelectionGeometryRecomputeCount() == afterCommit + 1U,
        "the move commit invalidates the cached frame");

    // Cancel restores the captured objects and must not leave the moved frame
    // in either view.
    const ui::SelectionOutlines original = [&] {
        ui::SelectionGeometryCache fresh;
        return fresh.ForPage(document, 0, controller->SelectionFor(0), controller->TransientGeneration());
    }();
    canvas.BeginMoveSelection(0);
    for (int sample = 0; sample < 4; ++sample) {
        canvas.UpdateMoveSelection(QPointF(5.0, 5.0));
    }
    canvas.CancelMoveSelection();
    ui::SelectionGeometryCache cancelled;
    const ui::SelectionOutlines& restored = cancelled.ForPage(
        document, 0, controller->SelectionFor(0), controller->TransientGeneration());
    bool sameOutlines = original.outlines.size() == restored.outlines.size();
    for (std::size_t index = 0; sameOutlines && index < original.outlines.size(); ++index) {
        for (std::size_t corner = 0; corner < 4U; ++corner) {
            sameOutlines = sameOutlines && SameVec(original.outlines[index][corner], restored.outlines[index][corner]);
        }
    }
    Check(sameOutlines, "cancelling a move restores the original selection frame exactly");

    // Undo bumps the page epoch; the restored objects must be measured again,
    // not served from the post-commit entry.
    Check(controller->Undo(), "undo restores the committed move");
    const std::size_t beforeUndoQuery = canvas.SelectionGeometryRecomputeCount();
    static_cast<void>(canvas.HitTestSelectionHandle(probe));
    Check(canvas.SelectionGeometryRecomputeCount() == beforeUndoQuery + 1U,
        "undo invalidates the cached selection frame");
    ui::SelectionGeometryCache undoneCache;
    const ui::SelectionOutlines& undone = undoneCache.ForPage(
        document, 0, controller->SelectionFor(0), controller->TransientGeneration());
    bool sameUndone = startOutlines.outlines.size() == undone.outlines.size();
    for (std::size_t index = 0; sameUndone && index < startOutlines.outlines.size(); ++index) {
        for (std::size_t corner = 0; corner < 4U; ++corner) {
            sameUndone = sameUndone
                && SameVec(startOutlines.outlines[index][corner], undone.outlines[index][corner]);
        }
    }
    Check(sameUndone, "undo restores the pre-move selection frame exactly");
}

// The prepared transform must produce exactly the per-point result the old
// MapSelectionTransformPoint formula produced: same operation order, no FMA
// reassociation.  The reference is written out independently here so a future
// edit to the hoisted coefficients is caught numerically.
Vec2 ReferenceTransformPoint(Vec2 point, const ui::SelectionBounds& from, const ui::SelectionBounds& to, float rotateDegrees) {
    const float fromWidth = from.right - from.left;
    const float fromHeight = from.bottom - from.top;
    const float scaleX = fromWidth > 0.0001f ? (to.right - to.left) / fromWidth : 1.0f;
    const float scaleY = fromHeight > 0.0001f ? (to.bottom - to.top) / fromHeight : 1.0f;
    const Vec2 mapped {
        to.left + ((point.x - from.left) * scaleX),
        to.top + ((point.y - from.top) * scaleY),
    };
    if (rotateDegrees == 0.0f) {
        return mapped;
    }
    const float radians = rotateDegrees * (ui::kPi / 180.0f);
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    const Vec2 center {
        (from.left + from.right) * 0.5f,
        (from.top + from.bottom) * 0.5f,
    };
    const float translatedX = mapped.x - center.x;
    const float translatedY = mapped.y - center.y;
    return {
        center.x + (translatedX * cosine) - (translatedY * sine),
        center.y + (translatedX * sine) + (translatedY * cosine),
    };
}

void TestPreparedTransformParity() {
    const ui::SelectionBounds from { -40.0f, 20.0f, 160.0f, 220.0f, true };
    const ui::SelectionBounds degenerate { 50.0f, 50.0f, 50.0f, 120.0f, true };
    struct Case {
        ui::SelectionBounds from;
        ui::SelectionBounds to;
        float rotateDegrees;
    };
    const std::vector<Case> cases = {
        { from, from, 0.0f },
        { from, from, 0.0f },
        { from, ui::SelectionBounds { 10.0f, 5.0f, 310.0f, 405.0f, true }, 0.0f },
        { from, ui::SelectionBounds { -90.0f, -40.0f, 110.0f, 160.0f, true }, 0.0f },
        { from, from, 25.0f },
        { from, from, -90.0f },
        { from, ui::SelectionBounds { 30.0f, 40.0f, 260.0f, 180.0f, true }, 133.0f },
        { degenerate, ui::SelectionBounds { 0.0f, 0.0f, 100.0f, 300.0f, true }, 0.0f },
    };
    int mismatches = 0;
    for (const Case& testCase : cases) {
        const ui::PreparedSelectionTransform prepared =
            ui::PrepareSelectionTransform(testCase.from, testCase.to, testCase.rotateDegrees);
        for (int x = -120; x <= 320; x += 17) {
            for (int y = -60; y <= 340; y += 23) {
                const Vec2 point { static_cast<float>(x), static_cast<float>(y) };
                const Vec2 expected = ReferenceTransformPoint(point, testCase.from, testCase.to, testCase.rotateDegrees);
                const Vec2 actual = prepared.Map(point);
                mismatches += SameVec(expected, actual) ? 0 : 1;
            }
        }
        Check(SameVec(prepared.Map({ 0.0f, 0.0f }), ui::MapSelectionTransformPoint(
                  { 0.0f, 0.0f }, testCase.from, testCase.to, testCase.rotateDegrees)),
            "the prepared map matches the shared per-point entry point");
    }
    Check(mismatches == 0, "prepared transform output is bit-identical to the old per-point formula");
}

ui::SelectionTransform MakeTransform(const ui::SelectionBounds& from, float left, float top,
    float right, float bottom, float rotateDegrees, bool scaleTextFont) {
    ui::SelectionTransform transform;
    transform.from = from;
    transform.to = from;
    transform.to.left = left;
    transform.to.top = top;
    transform.to.right = right;
    transform.to.bottom = bottom;
    transform.rotateDegrees = rotateDegrees;
    transform.scaleTextFont = scaleTextFont;
    return transform;
}

// Canvas transform samples: one measurement per sample, none for the hover and
// paint that follow it.
void TestTransformSampleCost(cnote::AppManager& manager) {
    auto controller = NewController(manager);
    Document& document = controller->Session()->document;
    Page& page = document.PageAt(0);
    SeedObjects(page);
    document.TouchPage(0);

    ui::CanvasWidget canvas(*controller);
    canvas.resize(1000, 1000);
    controller->SelectObjects(0, AllSelection(page));
    const QPointF probe = Logical(canvas, Vec2 { 150.0f, 150.0f });
    static_cast<void>(canvas.HitTestSelectionHandle(probe));

    ui::SelectionGeometryCache cache;
    const ui::SelectionOutlines& outlines =
        cache.ForPage(document, 0, controller->SelectionFor(0), controller->TransientGeneration());
    Check(outlines.bounds.has_value(), "the transform fixture has a selection frame");
    const ui::SelectionBounds from = *outlines.bounds;
    const QPointF press = Logical(canvas, Vec2 { from.right, from.bottom });
    Check(canvas.BeginTransform(ui::TransformRequest { ui::TransformTarget::Stroke, 0U, 0U,
        ui::TransformHandle::BottomRight, press }),
        "the transform gesture starts from the selection handle");

    int transformSamples = 0;
    for (int sample = 1; sample <= 8; ++sample) {
        const std::size_t before = canvas.SelectionGeometryRecomputeCount();
        canvas.UpdateTransform(Logical(canvas, Vec2 {
            from.right + static_cast<float>(sample) * 4.0f,
            from.bottom + static_cast<float>(sample) * 3.0f }), Qt::NoModifier);
        const std::size_t after = canvas.SelectionGeometryRecomputeCount();
        Check(after - before <= 1U, "a transform sample measures the selection at most once");
        transformSamples += after - before == 1U ? 1 : 0;

        const std::size_t beforeConsumers = canvas.SelectionGeometryRecomputeCount();
        static_cast<void>(canvas.HitTestSelectionHandle(probe));
        static_cast<void>(canvas.grab());
        Check(canvas.SelectionGeometryRecomputeCount() == beforeConsumers,
            "hover and paint after a transform sample measure nothing");
    }
    Check(transformSamples == 8, "every transform sample measured the selection exactly once");
    canvas.CommitTransform();
}

// Transform output is absolute: a sequence of controller samples must equal
// applying its last transform once, for every object kind and for both the
// box-resize and the Shift-scale/rotate paths.  The per-gesture source-bounds
// cache must not change the result.
void TestTransformParity(cnote::AppManager& managerA, cnote::AppManager& managerB) {
    auto controllerA = NewController(managerA);
    auto controllerB = NewController(managerB);
    Page& pageA = controllerA->Session()->document.PageAt(0);
    Page& pageB = controllerB->Session()->document.PageAt(0);
    SeedObjects(pageA);
    SeedObjects(pageB);
    controllerA->Session()->document.TouchPage(0);
    controllerB->Session()->document.TouchPage(0);

    controllerA->SelectObjects(0, AllSelection(pageA));
    controllerB->SelectObjects(0, AllSelection(pageB));

    ui::SelectionGeometryCache cache;
    const ui::SelectionOutlines& outlines = cache.ForPage(
        controllerA->Session()->document, 0, controllerA->SelectionFor(0), controllerA->TransientGeneration());
    Check(outlines.bounds.has_value(), "the transform fixture has a selection frame");
    const ui::SelectionBounds from = *outlines.bounds;

    // Box resize (text reflows), rotation, then Shift-resize (font scales).
    const ui::SelectionTransform resize = MakeTransform(from,
        from.left, from.top, from.right + 60.0f, from.bottom, 0.0f, false);
    const ui::SelectionTransform rotate = MakeTransform(from,
        from.left, from.top, from.right, from.bottom, 25.0f, false);
    const ui::SelectionTransform scale = MakeTransform(from,
        from.left, from.top, from.right + 40.0f, from.bottom + 30.0f, 0.0f, true);
    const std::vector<ui::SelectionTransform> sequence = { resize, rotate, scale };

    controllerA->BeginPageGesture(0);
    for (int sample = 0; sample < 6; ++sample) {
        controllerA->TransformSelection(0, sequence[static_cast<std::size_t>(sample) % sequence.size()]);
    }
    controllerA->CommitPageGesture();

    controllerB->BeginPageGesture(0);
    controllerB->TransformSelection(0, scale);
    controllerB->CommitPageGesture();

    Page& finalA = controllerA->Session()->document.PageAt(0);
    Page& finalB = controllerB->Session()->document.PageAt(0);
    Check(SamePage(finalA, finalB),
        "a transform sample sequence equals applying its last transform once (all object kinds)");
}

// Lasso: the release's exact tests are limited to objects whose cached bounds
// meet the polygon, and the selection result is unchanged for convex, concave
// and self-intersecting lassos.
void TestLasso(cnote::AppManager& manager) {
    auto controller = NewController(manager);
    Document& document = controller->Session()->document;
    Page& page = document.PageAt(0);

    // Two near objects (one stroke, one text) and 160 far ones.
    page.strokes.push_back(MakeStroke(60.0f, 60.0f, 12));
    page.texts.push_back(MakeText(1U, 100.0f, 80.0f, 0.0f, u"near"));
    for (int index = 0; index < 120; ++index) {
        const float x = 360.0f + static_cast<float>(index % 10) * 18.0f;
        const float y = 120.0f + static_cast<float>(index / 10) * 22.0f;
        page.strokes.push_back(MakeStroke(x, y, 8));
    }
    for (int index = 0; index < 20; ++index) {
        page.texts.push_back(MakeText(static_cast<std::uint64_t>(10 + index),
            380.0f, 420.0f + static_cast<float>(index) * 20.0f, 0.0f, u"far"));
    }
    for (int index = 0; index < 20; ++index) {
        page.latexes.push_back(MakeLatex(static_cast<std::uint64_t>(40 + index),
            380.0f + static_cast<float>(index) * 10.0f, 30.0f, 0.0f));
    }
    document.TouchPage(0);

    ui::CanvasWidget canvas(*controller);
    canvas.resize(1000, 1000);
    const ui::CanvasPageView view = canvas.CalculatePageView(0);
    Check(view.valid && view.scale > 0.0f, "the lasso fixture maps to the widget");

    const auto runLasso = [&](const std::vector<Vec2>& pagePolygon, bool expectNearOnly) {
        const auto toLogical = [&](Vec2 point) { return canvas.PageToScreen(0, point); };
        const QPointF anchor = toLogical(pagePolygon.front());
        canvas.UpdateSelectionOverlay(anchor, anchor, true, { anchor });
        std::vector<Vec2> mapped;
        mapped.push_back(canvas.ScreenToPage(view, anchor, false));
        for (std::size_t index = 1; index < pagePolygon.size(); ++index) {
            const QPointF logical = toLogical(pagePolygon[index]);
            canvas.AppendSelectionLassoPoint(logical);
            mapped.push_back(canvas.ScreenToPage(view, logical, false));
        }
        const QPointF current = toLogical(pagePolygon.back());
        const std::size_t testsBefore = canvas.LassoObjectTestCount();
        canvas.CommitSelection(anchor, current, true, false, false, false);
        const std::size_t tests = canvas.LassoObjectTestCount() - testsBefore;
        const ui::PageSelection expected = BruteForceLasso(page, mapped, controller->InputSettings().strokeSmoothing);
        Check(SameSelection(controller->SelectionFor(0), expected),
            "the lasso selects exactly the brute-force result");
        if (expectNearOnly) {
            Check(tests == 2U, "the lasso release tests only the two nearby objects (tested "
                + std::to_string(tests) + ")");
        }
        return tests;
    };

    // Convex lasso around the two near objects.
    runLasso({ { 30.0f, 30.0f }, { 200.0f, 30.0f }, { 200.0f, 130.0f }, { 30.0f, 130.0f } }, true);

    // Concave U: the near stroke sits inside one arm; the hollow must not
    // select the far objects even though they are inside the bounding box.
    runLasso({ { 30.0f, 30.0f }, { 210.0f, 30.0f }, { 210.0f, 140.0f }, { 150.0f, 140.0f },
                 { 150.0f, 80.0f }, { 60.0f, 80.0f }, { 60.0f, 140.0f }, { 30.0f, 140.0f } }, false);

    // Self-intersecting bowtie across the page; parity with the brute-force
    // scan is the invariant, whichever objects it encloses.
    runLasso({ { 20.0f, 20.0f }, { 560.0f, 500.0f }, { 560.0f, 20.0f }, { 20.0f, 500.0f } }, false);

    // A lasso that touches nothing: zero exact tests.
    const auto toLogical = [&](Vec2 point) { return canvas.PageToScreen(0, point); };
    const QPointF anchor = toLogical({ 300.0f, 700.0f });
    canvas.UpdateSelectionOverlay(anchor, anchor, true, { anchor });
    for (const Vec2 point : { Vec2 { 330.0f, 700.0f }, Vec2 { 330.0f, 730.0f }, Vec2 { 300.0f, 730.0f } }) {
        canvas.AppendSelectionLassoPoint(toLogical(point));
    }
    const std::size_t testsBefore = canvas.LassoObjectTestCount();
    canvas.CommitSelection(anchor, toLogical({ 300.0f, 730.0f }), true, false, false, false);
    Check(canvas.LassoObjectTestCount() == testsBefore,
        "a lasso disjoint from every object performs no exact tests");
    Check(controller->SelectionFor(0).Empty(), "the disjoint lasso selects nothing");
    Check(canvas.ObjectBoundsRecomputeCount() > 0U, "the release used the cached page bounds");
}

// Warm and cold overlay pixels are identical for the same committed selection.
void TestOverlayParity(cnote::AppManager& manager) {
    auto controller = NewController(manager);
    Document& document = controller->Session()->document;
    Page& page = document.PageAt(0);
    SeedObjects(page);
    document.TouchPage(0);

    ui::CanvasWidget warm(*controller);
    warm.resize(1000, 1000);
    controller->SelectObjects(0, AllSelection(page));
    // Warm the shared geometry cache and the surface cache.
    static_cast<void>(warm.grab());

    ui::CanvasWidget cold(*controller);
    cold.resize(1000, 1000);
    const QImage warmImage = warm.grab().toImage();
    const QImage coldImage = cold.grab().toImage();
    Check(warmImage == coldImage, "warm-cache overlay pixels equal a cold measurement");

    for (int hover = 0; hover < 20; ++hover) {
        static_cast<void>(warm.HitTestSelectionHandle(Logical(warm, Vec2 {
            120.0f + static_cast<float>(hover), 120.0f })));
    }
    Check(warm.grab().toImage() == warmImage, "hovering does not alter overlay pixels");
}

// Dense-selection cost: one measurement per sample rather than the old
// damage-before + damage-after + paint + hover measurements.
void MeasureDenseSelection(cnote::AppManager& manager) {
    auto controller = NewController(manager);
    Document& document = controller->Session()->document;
    Page& page = document.PageAt(0);
    for (int index = 0; index < 300; ++index) {
        page.strokes.push_back(MakeStroke(
            20.0f + static_cast<float>(index % 20) * 27.0f,
            30.0f + static_cast<float>(index / 20) * 50.0f, 40));
    }
    for (int index = 0; index < 30; ++index) {
        page.texts.push_back(MakeText(static_cast<std::uint64_t>(100 + index),
            20.0f + static_cast<float>(index % 6) * 90.0f,
            120.0f + static_cast<float>(index / 6) * 110.0f, 0.0f, u"dense text sample"));
    }
    for (int index = 0; index < 6; ++index) {
        page.latexes.push_back(MakeLatex(static_cast<std::uint64_t>(200 + index),
            60.0f + static_cast<float>(index) * 80.0f, 700.0f, 0.0f));
    }
    for (int index = 0; index < 12; ++index) {
        page.images.push_back(MakeImage(40.0f + static_cast<float>(index) * 42.0f, 770.0f, 0.0f));
    }
    document.TouchPage(0);

    ui::CanvasWidget canvas(*controller);
    canvas.resize(1000, 1000);
    const ui::PageSelection selection = AllSelection(page);
    controller->SelectObjects(0, selection);

    constexpr int kSamples = 25;
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto micros = [](std::chrono::steady_clock::duration duration) {
        return std::chrono::duration<double, std::micro>(duration).count();
    };

    // One cold selection measurement on this fixture.
    double coldTotal = 0.0;
    for (int index = 0; index < 5; ++index) {
        ui::SelectionGeometryCache cold;
        const auto start = now();
        static_cast<void>(cold.ForPage(document, 0, controller->SelectionFor(0),
            controller->TransientGeneration()));
        coldTotal += micros(now() - start);
    }
    const double coldMicros = coldTotal / 5.0;

    // The same measurement again with the font/layout engine warm: the first
    // query above includes the one-time text-layout setup of the fixture.
    double steadyTotal = 0.0;
    for (int index = 0; index < 10; ++index) {
        ui::SelectionGeometryCache steady;
        const auto start = now();
        static_cast<void>(steady.ForPage(document, 0, controller->SelectionFor(0),
            controller->TransientGeneration()));
        steadyTotal += micros(now() - start);
    }
    const double steadyMicros = steadyTotal / 10.0;

    // The old sample cost: damage-before, damage-after, paint and handle hover
    // each measured the selection independently.
    const auto oldStart = now();
    for (int sample = 0; sample < kSamples; ++sample) {
        for (int consumer = 0; consumer < 4; ++consumer) {
            ui::SelectionGeometryCache cold;
            static_cast<void>(cold.ForPage(document, 0, controller->SelectionFor(0),
                controller->TransientGeneration()));
        }
    }
    const double oldPerSample = micros(now() - oldStart) / static_cast<double>(kSamples);

    canvas.BeginMoveSelection(0);
    const std::size_t before = canvas.SelectionGeometryRecomputeCount();
    const auto newStart = now();
    for (int sample = 0; sample < kSamples; ++sample) {
        canvas.UpdateMoveSelection(QPointF(1.0, 0.0));
    }
    const double newPerSample = micros(now() - newStart) / static_cast<double>(kSamples);
    const std::size_t measurements = canvas.SelectionGeometryRecomputeCount() - before;
    canvas.CommitMoveSelection();

    std::printf("MEASURE  dense fixture: %zu strokes (40 points), %zu texts, %zu latex, %zu images\n",
        page.strokes.size(), page.texts.size(), page.latexes.size(), page.images.size());
    std::printf("MEASURE  one cold selection measurement: %.1f us (steady state %.1f us)\n",
        coldMicros, steadyMicros);
    std::printf("MEASURE  old-style per sample (4 independent measurements): %.1f us\n", oldPerSample);
    std::printf("MEASURE  current per move sample (geometry + translation): %.1f us, "
                "measurements for %d samples: %zu\n",
        newPerSample, kSamples, measurements);

    Check(measurements == static_cast<std::size_t>(kSamples),
        "the dense move samples each measured the selection exactly once");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    std::atomic<bool> appReady { false };
    std::thread watchdog([&appReady] {
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (appReady.load()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "selection gesture: QApplication did not initialize within 30 s\n");
        std::fflush(stderr);
        std::_Exit(7);
    });
    watchdog.detach();

    QApplication app(argc, argv);
    appReady.store(true);

    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL  LaTeX engine initialization\n");
        return 1;
    }

    {
        cnote::AppManager manager;
        TestStaticSelection(manager);
    }
    {
        cnote::AppManager manager;
        TestMoveSamples(manager);
    }
    {
        TestPreparedTransformParity();
    }
    {
        cnote::AppManager manager;
        TestTransformSampleCost(manager);
    }
    {
        cnote::AppManager managerA;
        cnote::AppManager managerB;
        TestTransformParity(managerA, managerB);
    }
    {
        cnote::AppManager manager;
        TestLasso(manager);
    }
    {
        cnote::AppManager manager;
        TestOverlayParity(manager);
    }
    {
        cnote::AppManager manager;
        MeasureDenseSelection(manager);
    }

    if (g_failures == 0) {
        std::printf("OK\n");
    } else {
        std::printf("FAILURES: %d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
