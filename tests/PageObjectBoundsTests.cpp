// Deterministic offscreen regression test for the erase/scratch-out object
// bounds cache (PERF-006).
//
// The eraser queries its targets once per pointer sample.  Before the cache
// every sample measured every text/LaTeX block and smoothed every stroke on
// the page, so the work scaled with the page rather than the sweep.  The cache
// is keyed on Document::PageEpoch, which every committed mutation bumps (and
// which insert/remove/reorder replace), so a cached entry can never outlive
// its contents.
//
// This test drives the real QtDocumentController + CanvasWidget offscreen and
// asserts:
//   (a) one page measurement serves many erase/scratch samples,
//   (b) the same targets are found before and after a move/undo, at the moved
//       and original positions,
//   (c) text and LaTeX edits invalidate the cached bounds, and undo restores
//       them,
//   (d) a page insert cannot serve the old page's bounds for the new index.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QPointF>
#include <QString>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "qt/QtDocumentController.h"
#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/PageDefaultsQt.h"
#include "ui/PageObjectBounds.h"

namespace {

namespace ui = cnote::ui;
using cnote::Document;
using cnote::Page;
using cnote::PageImage;
using cnote::PageLatex;
using cnote::PageText;
using cnote::ScratchOut;
using cnote::Stroke;
using cnote::Vec2;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        ++g_failures;
    }
}

bool Near(float a, float b) {
    return std::abs(a - b) <= 0.01f;
}

bool SameBounds(const ui::ObjectBounds& a, const ui::ObjectBounds& b) {
    if (a.valid != b.valid) {
        return false;
    }
    return !a.valid
        || (Near(a.left, b.left) && Near(a.top, b.top) && Near(a.right, b.right) && Near(a.bottom, b.bottom));
}

bool SameBounds(const std::vector<ui::ObjectBounds>& a, const std::vector<ui::ObjectBounds>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (!SameBounds(a[index], b[index])) {
            return false;
        }
    }
    return true;
}

bool SamePageBounds(const ui::PageObjectBounds& a, const ui::PageObjectBounds& b) {
    return SameBounds(a.images, b.images) && SameBounds(a.texts, b.texts)
        && SameBounds(a.latexes, b.latexes) && SameBounds(a.strokes, b.strokes);
}

// The cache must answer exactly what a cold cache measuring the current page
// answers; this is the stale-bounds probe for every mutation below.
void CheckFresh(const ui::PageObjectBoundsCache& cache, const Document& document,
    std::size_t pageIndex, std::uint64_t transientGeneration, const std::string& what) {
    ui::PageObjectBoundsCache fresh;
    Check(SamePageBounds(cache.ForPage(document, pageIndex, transientGeneration),
            fresh.ForPage(document, pageIndex, transientGeneration)),
        what);
}

bool HasTarget(const std::vector<ui::ObjectRef>& targets, ui::ObjectKind kind, std::size_t objectIndex) {
    for (const ui::ObjectRef& target : targets) {
        if (target.kind == kind && target.objectIndex == objectIndex) {
            return true;
        }
    }
    return false;
}

std::size_t KindCount(const std::vector<ui::ObjectRef>& targets) {
    bool seen[4] = {};
    for (const ui::ObjectRef& target : targets) {
        seen[static_cast<std::size_t>(target.kind)] = true;
    }
    return static_cast<std::size_t>(std::count(std::begin(seen), std::end(seen), true));
}

ScratchOut Hull(float left, float top, float right, float bottom) {
    ScratchOut scratch;
    scratch.hull = {
        { left, top }, { right, top }, { right, bottom }, { left, bottom },
    };
    return scratch;
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
        std::fprintf(stderr, "page object bounds: QApplication did not initialize within 30 s\n");
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

    cnote::AppManager manager;
    cnote::qt::QtDocumentController controller(manager);
    controller.NewBlankDocument();

    Document& document = controller.Session()->document;
    Page& page = document.PageAt(0);
    PageImage image;
    image.x = 10.0f;
    image.y = 200.0f;
    image.width = 40.0f;
    image.height = 30.0f;
    page.images.push_back(image);
    PageText text;
    text.id = 11U;
    text.x = 10.0f;
    text.y = 10.0f;
    text.fontSize = 24.0f;
    text.content = u"hello";
    page.texts.push_back(text);
    PageLatex latex;
    latex.id = 12U;
    latex.x = 300.0f;
    latex.y = 10.0f;
    latex.fontSize = 24.0f;
    latex.content = u"x";
    page.latexes.push_back(latex);
    Stroke stroke;
    stroke.style.width = 3.0f;
    stroke.points = { { 100.0f, 100.0f }, { 140.0f, 120.0f } };
    page.strokes.push_back(stroke);
    document.TouchPage(0);

    ui::PageObjectBoundsCache cache;
    const ui::PageObjectBounds initial = cache.ForPage(document, 0, controller.TransientGeneration());
    Check(initial.images.size() == 1U && initial.texts.size() == 1U
            && initial.latexes.size() == 1U && initial.strokes.size() == 1U,
        "bounds cover every object kind");
    Check(cache.RecomputeCount() == 1U, "first query measures the page once");
    Check(initial.strokes[0].valid && initial.strokes[0].left <= 100.0f
            && initial.strokes[0].right >= 140.0f && initial.strokes[0].top <= 100.0f
            && initial.strokes[0].bottom >= 120.0f,
        "stroke bounds enclose the raw points");
    Check(initial.texts[0].valid && initial.texts[0].right > initial.texts[0].left,
        "text bounds are measured");
    Check(initial.latexes[0].valid && initial.latexes[0].right > initial.latexes[0].left,
        "LaTeX bounds are measured");
    Check(initial.images[0].valid && Near(initial.images[0].left, 10.0f)
            && Near(initial.images[0].right, 50.0f),
        "image bounds cover the rotated box");

    for (int sample = 0; sample < 100; ++sample) {
        static_cast<void>(cache.ForPage(document, 0, controller.TransientGeneration()));
    }
    Check(cache.RecomputeCount() == 1U, "repeated queries hit the cached page");

    ui::CanvasWidget canvas(controller);
    canvas.resize(1200, 1200);
    canvas.SetEraseBrushSize(3.0f);
    Check(canvas.CalculatePageView(0).valid, "canvas page view is valid");

    // (a) Many erase samples, one page measurement.
    const Vec2 oldEraseStart { 90.0f, 120.0f };
    const Vec2 oldEraseEnd { 150.0f, 120.0f };
    std::vector<ui::ObjectRef> targets = canvas.EraseTargetsAt(0, oldEraseStart, oldEraseEnd);
    Check(HasTarget(targets, ui::ObjectKind::Stroke, 0U), "erase hits the seeded stroke");
    targets = canvas.EraseTargetsAt(0, { 0.0f, 20.0f }, { 100.0f, 20.0f });
    Check(HasTarget(targets, ui::ObjectKind::Text, 0U), "erase hits the seeded text");
    targets = canvas.EraseTargetsAt(0, { 280.0f, 20.0f }, { 350.0f, 20.0f });
    Check(HasTarget(targets, ui::ObjectKind::Latex, 0U), "erase hits the seeded LaTeX");
    targets = canvas.EraseTargetsAt(0, { 0.0f, 215.0f }, { 60.0f, 215.0f });
    Check(HasTarget(targets, ui::ObjectKind::Image, 0U), "erase hits the seeded image");
    Check(canvas.ObjectBoundsRecomputeCount() == 1U, "first erase sample measures the page once");
    for (int sample = 0; sample < 64; ++sample) {
        const float y = 300.0f + static_cast<float>(sample);
        static_cast<void>(canvas.EraseTargetsAt(0, { 40.0f, y }, { 160.0f, y }));
    }
    Check(canvas.ObjectBoundsRecomputeCount() == 1U, "64 erase samples reuse the page bounds");

    // Scratch-out over every seeded object, then a scratch away from them.
    controller.BeginStroke(0, { 0.0f, 0.0f }, cnote::StrokeSampling {}, std::nullopt);
    controller.ExtendStroke({ 1.0f, 0.0f }, std::nullopt);
    targets = canvas.ScratchOutTargets(0, Hull(0.0f, 0.0f, 400.0f, 300.0f));
    Check(KindCount(targets) == 4U, "scratch over the page targets every kind");
    Check(canvas.ObjectBoundsRecomputeCount() == 1U, "scratch samples reuse the same page bounds");
    controller.CancelStroke();

    // (b) Move everything far down the page, commit, then erase/scratch at the
    // new and old positions.  The commit bumps the page epoch.
    ui::PageSelection selection;
    selection.strokes = { 0U };
    selection.images = { 0U };
    selection.texts = { 0U };
    selection.latexes = { 0U };
    controller.SelectObjects(0, selection);
    controller.BeginPageGesture(0);
    controller.MoveSelectionBy(0, { 0.0f, 500.0f });
    controller.CommitPageGesture();

    const ui::PageObjectBounds moved = cache.ForPage(document, 0, controller.TransientGeneration());
    Check(cache.RecomputeCount() == 2U, "move commit invalidates the cached page");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "moved bounds match a cold measurement");
    Check(Near(moved.strokes[0].top, initial.strokes[0].top + 500.0f), "stroke bounds move with the object");
    Check(Near(moved.images[0].top, initial.images[0].top + 500.0f), "image bounds move with the object");
    Check(Near(moved.texts[0].top, initial.texts[0].top + 500.0f), "text bounds move with the object");
    Check(Near(moved.latexes[0].top, initial.latexes[0].top + 500.0f), "LaTeX bounds move with the object");

    targets = canvas.EraseTargetsAt(0, oldEraseStart, oldEraseEnd);
    Check(targets.empty(), "erase no longer hits the moved stroke at its old position");
    Check(canvas.ObjectBoundsRecomputeCount() == 2U, "post-move erase measured the page once");
    targets = canvas.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
    Check(HasTarget(targets, ui::ObjectKind::Stroke, 0U), "erase hits the moved stroke");

    controller.BeginStroke(0, { 0.0f, 500.0f }, cnote::StrokeSampling {}, std::nullopt);
    controller.ExtendStroke({ 1.0f, 500.0f }, std::nullopt);
    targets = canvas.ScratchOutTargets(0, Hull(0.0f, 0.0f, 400.0f, 300.0f));
    Check(targets.empty(), "scratch over the old region no longer covers the moved objects");
    targets = canvas.ScratchOutTargets(0, Hull(0.0f, 500.0f, 400.0f, 800.0f));
    Check(KindCount(targets) == 4U, "scratch over the new region covers every kind");
    controller.CancelStroke();

    // Undo the move: everything returns, and the old cache entry must not be
    // reused for the restored contents.
    Check(controller.Undo(), "undo restores the move");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "undo restores the original cached bounds");
    Check(cache.RecomputeCount() == 3U, "undo invalidates the cached page");
    const ui::PageObjectBounds restored = cache.ForPage(document, 0, controller.TransientGeneration());
    Check(SameBounds(restored.strokes, initial.strokes) && SameBounds(restored.images, initial.images)
            && SameBounds(restored.texts, initial.texts) && SameBounds(restored.latexes, initial.latexes),
        "undone bounds equal the original measurement");

    targets = canvas.EraseTargetsAt(0, oldEraseStart, oldEraseEnd);
    Check(HasTarget(targets, ui::ObjectKind::Stroke, 0U), "erase hits the restored stroke");
    targets = canvas.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
    Check(targets.empty(), "erase misses the undone stroke at the moved position");

    // A live pointer gesture mutates the page in place before its commit bumps
    // the epoch.  Warm the cache, then verify an erase inside the gesture sees
    // the moved bounds and that cancelling does not leave those bounds behind.
    controller.SelectObjects(0, selection);
    targets = canvas.EraseTargetsAt(0, oldEraseStart, oldEraseEnd);
    Check(HasTarget(targets, ui::ObjectKind::Stroke, 0U), "erase warms the current page bounds");
    const std::size_t gestureBefore = canvas.ObjectBoundsRecomputeCount();
    canvas.BeginMoveSelection(0);
    canvas.UpdateMoveSelection(QPointF(0.0, 500.0 * canvas.CalculatePageView(0).scale));
    targets = canvas.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
    Check(HasTarget(targets, ui::ObjectKind::Stroke, 0U), "erase during a move sees the moved bounds");
    Check(canvas.ObjectBoundsRecomputeCount() == gestureBefore + 1U,
        "a gesture sample invalidates the cached bounds");
    canvas.CancelMoveSelection();
    targets = canvas.EraseTargetsAt(0, oldEraseStart, oldEraseEnd);
    Check(HasTarget(targets, ui::ObjectKind::Stroke, 0U), "cancel restores the bounds the eraser uses");
    targets = canvas.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
    Check(targets.empty(), "cancel does not leave the gesture's bounds cached");

    // (c) Text edit invalidates and undo restores.
    Check(controller.BeginTextEdit(0, 0), "text edit begins");
    if (const PageText* editing = controller.EditingTextObject(); editing != nullptr) {
        PageText edited = *editing;
        edited.content = u"a much longer line of text than before, for bounds";
        controller.CommitTextEdit(std::move(edited));
    }
    const ui::PageObjectBounds afterText = cache.ForPage(document, 0, controller.TransientGeneration());
    Check(cache.RecomputeCount() == 4U, "text edit invalidates the cached page");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "edited text bounds match a cold measurement");
    Check(afterText.texts[0].right > initial.texts[0].right + 1.0f, "longer text has wider bounds");
    Check(controller.Undo(), "undo restores the text edit");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "undone text bounds match a cold measurement");
    Check(SameBounds(cache.ForPage(document, 0, controller.TransientGeneration()).texts, initial.texts), "undone text bounds equal the original");

    // (d) LaTeX edit invalidates and undo restores.
    Check(controller.BeginLatexEdit(0, 0), "LaTeX edit begins");
    controller.CommitLatexEdit(QStringLiteral("x+y+z+w"));
    const ui::PageObjectBounds afterLatex = cache.ForPage(document, 0, controller.TransientGeneration());
    Check(cache.RecomputeCount() == 6U, "LaTeX edit invalidates the cached page");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "edited LaTeX bounds match a cold measurement");
    Check(std::abs(afterLatex.latexes[0].right - afterLatex.latexes[0].left)
            > std::abs(initial.latexes[0].right - initial.latexes[0].left) + 0.5f,
        "longer formula has wider bounds");
    Check(controller.Undo(), "undo restores the LaTeX edit");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "undone LaTeX bounds match a cold measurement");
    Check(SameBounds(cache.ForPage(document, 0, controller.TransientGeneration()).latexes, initial.latexes), "undone LaTeX bounds equal the original");

    // (e) Inserting a page shifts the old page to index 1; the slot's fresh
    // epoch must force a recompute rather than serve index 0's old bounds.
    ui::NewPageDraft draft {};
    Check(controller.InsertPages(0, 1, draft) == 0U, "page inserted before the document");
    Check(document.PageCount() == 2U, "document has two pages");
    const ui::PageObjectBounds inserted = cache.ForPage(document, 0, controller.TransientGeneration());
    Check(inserted.images.empty() && inserted.texts.empty() && inserted.latexes.empty()
            && inserted.strokes.empty(),
        "new page has no cached objects under the old index");
    CheckFresh(cache, document, 0, controller.TransientGeneration(), "new page bounds match a cold measurement");
    CheckFresh(cache, document, 1, controller.TransientGeneration(), "shifted page bounds match a cold measurement");
    Check(SameBounds(cache.ForPage(document, 1, controller.TransientGeneration()).strokes, initial.strokes),
        "shifted page serves the old page's own bounds");

    {
        // A gesture aborted from inside the controller (Undo of another page's
        // commit) restores the page in place without a page-epoch bump.  The
        // shared transient generation must invalidate this canvas anyway.
        cnote::qt::QtDocumentController abortController(manager);
        abortController.NewBlankDocument();
        Document& abortDocument = abortController.Session()->document;
        Page& abortPage = abortDocument.PageAt(0);
        Stroke abortStroke;
        abortStroke.style.width = 3.0f;
        abortStroke.points = { { 100.0f, 100.0f }, { 140.0f, 120.0f } };
        abortPage.strokes.push_back(abortStroke);
        abortDocument.TouchPage(0);

        ui::NewPageDraft abortDraft {};
        Check(abortController.InsertPages(1, 1, abortDraft) == 1U, "abort test appends a page");
        abortController.BeginStroke(1, { 100.0f, 100.0f }, cnote::StrokeSampling {}, std::nullopt);
        abortController.ExtendStroke({ 200.0f, 120.0f }, std::nullopt);
        abortController.EndStroke();
        Check(abortDocument.PageAt(1).strokes.size() == 1U, "abort test commits on the other page");

        ui::CanvasWidget abortCanvas(abortController);
        abortCanvas.resize(1200, 1200);
        abortCanvas.SetEraseBrushSize(3.0f);
        const Vec2 abortStart { 90.0f, 120.0f };
        const Vec2 abortEnd { 150.0f, 120.0f };
        std::vector<ui::ObjectRef> abortTargets = abortCanvas.EraseTargetsAt(0, abortStart, abortEnd);
        Check(HasTarget(abortTargets, ui::ObjectKind::Stroke, 0U), "abort test erase hits before the gesture");

        ui::PageSelection abortSelection;
        abortSelection.strokes = { 0U };
        abortController.SelectObjects(0, abortSelection);
        abortCanvas.BeginMoveSelection(0);
        abortCanvas.UpdateMoveSelection(QPointF(0.0, 500.0 * abortCanvas.CalculatePageView(0).scale));
        abortTargets = abortCanvas.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
        Check(HasTarget(abortTargets, ui::ObjectKind::Stroke, 0U), "abort test erase hits the in-gesture position");

        Check(abortController.Undo(), "undo commits on the other page and aborts the gesture");
        abortTargets = abortCanvas.EraseTargetsAt(0, abortStart, abortEnd);
        Check(HasTarget(abortTargets, ui::ObjectKind::Stroke, 0U),
            "erase after an abort hits the restored position");
        abortTargets = abortCanvas.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
        Check(abortTargets.empty(), "an aborted gesture's bounds are not cached");
    }

    {
        // Two views share one session: a gesture in one view mutates the page
        // in place, so the other view's cache must invalidate through the
        // session-wide generation, not through anything local to its canvas.
        cnote::qt::QtDocumentController viewA(manager);
        viewA.NewBlankDocument();
        Document& sharedDocument = viewA.Session()->document;
        Stroke sharedStroke;
        sharedStroke.style.width = 3.0f;
        sharedStroke.points = { { 100.0f, 100.0f }, { 140.0f, 120.0f } };
        sharedDocument.PageAt(0).strokes.push_back(sharedStroke);
        sharedDocument.TouchPage(0);

        cnote::qt::QtDocumentController viewB(manager);
        viewB.AttachSession(viewA.Session());
        ui::CanvasWidget canvasA(viewA);
        ui::CanvasWidget canvasB(viewB);
        canvasA.resize(1200, 1200);
        canvasB.resize(1200, 1200);

        std::vector<ui::ObjectRef> sharedTargets =
            canvasB.EraseTargetsAt(0, { 90.0f, 120.0f }, { 150.0f, 120.0f });
        Check(HasTarget(sharedTargets, ui::ObjectKind::Stroke, 0U), "second view erase hits before the gesture");

        ui::PageSelection sharedSelection;
        sharedSelection.strokes = { 0U };
        viewA.SelectObjects(0, sharedSelection);
        canvasA.BeginMoveSelection(0);
        canvasA.UpdateMoveSelection(QPointF(0.0, 500.0 * canvasA.CalculatePageView(0).scale));
        sharedTargets = canvasB.EraseTargetsAt(0, { 90.0f, 620.0f }, { 150.0f, 620.0f });
        Check(HasTarget(sharedTargets, ui::ObjectKind::Stroke, 0U),
            "another view sees the in-gesture bounds");
        sharedTargets = canvasB.EraseTargetsAt(0, { 90.0f, 120.0f }, { 150.0f, 120.0f });
        Check(sharedTargets.empty(), "another view does not use the pre-gesture bounds");

        canvasA.CancelMoveSelection();
        sharedTargets = canvasB.EraseTargetsAt(0, { 90.0f, 120.0f }, { 150.0f, 120.0f });
        Check(HasTarget(sharedTargets, ui::ObjectKind::Stroke, 0U),
            "another view sees the restored bounds after cancel");
    }

    if (g_failures == 0) {
        std::printf("PASS: page object bounds cache\n");
        return 0;
    }
    std::printf("FAILED: %d checks\n", g_failures);
    return 1;
}
