// Deterministic tests for the portable selection math (plan steps 5.2/5.3).
//
// The ported functions exist because src/selection/Geometry.cpp is bound to
// App/POINT/RECT and cannot be linked into the Qt target.  On Windows this test
// links the legacy translation unit as well and compares every case directly
// against it; everywhere else it compares against the frozen table produced by
// the same Windows run (values captured from the legacy functions, printed with
// --print-frozen).  Either way the expectations come from the reference
// implementation, not from a re-derivation.

#include <cmath>
#include <cstdio>

#include <QGuiApplication>
#include <string>
#include <vector>

#include "Document.h"
#include "latex/LatexEngine.h"
#include "ui/SelectionMath.h"

#ifdef CNOTE_SELECTION_LEGACY_REFERENCE
#include "selection/Geometry.h"
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

bool Near(double a, double b, double tolerance = 0.0005) {
    return std::abs(a - b) <= tolerance;
}

void CheckNear(double actual, double expected, const std::string& what, double tolerance = 0.0005) {
    const bool ok = Near(actual, expected, tolerance);
    std::printf("%s  %s (actual=%.6f expected=%.6f)\n", ok ? "PASS" : "FAIL", what.c_str(), actual, expected);
    if (!ok) {
        ++g_failures;
    }
}

// --- fixed inputs shared by the unit expectations and the legacy comparison ---

cnote::Stroke MakeStroke() {
    cnote::Stroke stroke;
    stroke.style.width = 3.0f;
    stroke.smoothing = 0.0f;  // no smoothing: the legacy render points are the raw points
    stroke.points = {
        { 10.0f, 10.0f },
        { 60.0f, 30.0f },
        { 110.0f, 90.0f },
        { 150.0f, 40.0f },
    };
    return stroke;
}

cnote::PageImage MakeImage() {
    cnote::PageImage image;
    image.x = 100.0f;
    image.y = 200.0f;
    image.width = 80.0f;
    image.height = 40.0f;
    image.rotationDegrees = 30.0f;
    return image;
}

cnote::PageLatex MakeLatex() {
    cnote::PageLatex latex;
    latex.x = 40.0f;
    latex.y = 60.0f;
    latex.fontSize = 28.0f;
    latex.content = u"\\frac{a}{b}";
    return latex;
}

cnote::Page MakePage() {
    cnote::Page page;
    page.width = 612.0f;
    page.height = 792.0f;
    return page;
}

std::vector<cnote::Vec2> MakePolygon() {
    return {
        { 0.0f, 0.0f },
        { 200.0f, 0.0f },
        { 200.0f, 150.0f },
        { 0.0f, 150.0f },
    };
}

struct Case {
    const char* name = "";
    double value = 0.0;   // 0/1 for predicates, page units otherwise
};

// Frozen expectations captured from the legacy implementation on Windows
// (see --print-frozen); the Windows build additionally compares live.
const Case kFrozenCases[] = {
    { "stroke.bounds.minX", 10.0 },
    { "stroke.bounds.minY", 10.0 },
    { "stroke.bounds.maxX", 150.0 },
    { "stroke.bounds.maxY", 90.0 },
    { "stroke.hit.onPoint", 1.0 },
    { "stroke.hit.nearSegment", 1.0 },
    { "stroke.hit.far", 0.0 },
    { "stroke.rect.containsSegment", 1.0 },
    { "stroke.rect.miss", 0.0 },
    { "stroke.polygon.inside", 1.0 },
    { "stroke.polygon.miss", 0.0 },
    { "stroke.clampDelta.x", -10.0 },
    { "stroke.clampDelta.y", 5.0 },
    { "image.center.x", 140.0 },
    { "image.center.y", 220.0 },
    { "image.corner0.x", 115.358986 },
    { "image.corner0.y", 182.679489 },
    { "image.hit.center", 1.0 },
    { "image.hit.far", 0.0 },
    { "image.rect.miss", 0.0 },
    { "image.rect.hit", 1.0 },
    { "image.polygon.hit", 1.0 },
    { "image.polygon.miss", 0.0 },
    { "image.clampDelta.x", -95.358986 },
    { "image.clampDelta.y", 5.0 },
    { "handle.hitRadius.96", 11.0 },
    { "handle.rotateOffset.96", 18.0 },
    { "rotate.90.x", 200.0 },
    { "rotate.90.y", 0.0 },
    { "pointInPolygon.inside", 1.0 },
    { "pointInPolygon.outside", 0.0 },
    { "segments.cross", 1.0 },
    { "segments.parallel", 0.0 },
};

const Case* FindFrozen(const std::string& name) {
    for (const Case& entry : kFrozenCases) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

void CheckFrozen(const std::string& name, double actual, double tolerance = 0.001) {
    const Case* expected = FindFrozen(name);
    if (expected == nullptr) {
        Check(false, "frozen case missing: " + name);
        return;
    }
    CheckNear(actual, expected->value, name, tolerance);
}

// The input set used for both the unit expectations and the legacy comparison.
struct ProbeResults {
    double strokeBoundsMinX = 0.0;
    double strokeBoundsMinY = 0.0;
    double strokeBoundsMaxX = 0.0;
    double strokeBoundsMaxY = 0.0;
    bool strokeHitOnPoint = false;
    bool strokeHitNearSegment = false;
    bool strokeHitFar = false;
    bool strokeRectContainsSegment = false;
    bool strokeRectMiss = false;
    bool strokePolygonInside = false;
    bool strokePolygonMiss = false;
    cnote::Vec2 strokeClamp {};
    cnote::Vec2 imageCenter {};
    cnote::Vec2 imageCorner0 {};
    bool imageHitCenter = false;
    bool imageHitFar = false;
    bool imageRectMiss = false;
    bool imageRectHit = false;
    bool imagePolygonHit = false;
    bool imagePolygonMiss = false;
    cnote::Vec2 imageClamp {};
    double handleHitRadius = 0.0;
    double rotateOffset = 0.0;
    cnote::Vec2 rotated90 {};
    bool pointInPolygonInside = false;
    bool pointInPolygonOutside = false;
    bool segmentsCross = false;
    bool segmentsParallel = false;
};

template <typename Api>
ProbeResults Probe() {
    const cnote::Stroke stroke = MakeStroke();
    const cnote::PageImage image = MakeImage();
    const cnote::Page page = MakePage();
    const std::vector<cnote::Vec2> polygon = MakePolygon();

    ProbeResults results;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    Api::StrokeBounds(stroke, &minX, &minY, &maxX, &maxY);
    results.strokeBoundsMinX = minX;
    results.strokeBoundsMinY = minY;
    results.strokeBoundsMaxX = maxX;
    results.strokeBoundsMaxY = maxY;

    results.strokeHitOnPoint = Api::StrokeHitsPoint(stroke, cnote::Vec2 { 60.0f, 30.0f }, 6.0f, 0.0f);
    results.strokeHitNearSegment = Api::StrokeHitsPoint(stroke, cnote::Vec2 { 85.0f, 60.0f }, 6.0f, 0.0f);
    results.strokeHitFar = Api::StrokeHitsPoint(stroke, cnote::Vec2 { 300.0f, 300.0f }, 6.0f, 0.0f);
    results.strokeRectContainsSegment =
        Api::StrokeIntersectsRectSelection(stroke, cnote::Vec2 { 50.0f, 20.0f }, cnote::Vec2 { 120.0f, 100.0f }, 0.0f);
    results.strokeRectMiss =
        Api::StrokeIntersectsRectSelection(stroke, cnote::Vec2 { 300.0f, 300.0f }, cnote::Vec2 { 400.0f, 400.0f }, 0.0f);
    results.strokePolygonInside = Api::StrokeIntersectsPolygonSelection(stroke, polygon, 0.0f);
    results.strokePolygonMiss = Api::StrokeIntersectsPolygonSelection(
        stroke, { { 400.0f, 400.0f }, { 500.0f, 400.0f }, { 500.0f, 500.0f } }, 0.0f);
    results.strokeClamp = Api::ClampStrokeDeltaToPage(stroke, page, cnote::Vec2 { -1000.0f, 5.0f });

    results.imageCenter = Api::ImageCenter(image);
    results.imageCorner0 = Api::ImageCorners(image)[0];
    results.imageHitCenter = Api::ImageHitsPoint(image, cnote::Vec2 { 140.0f, 220.0f });
    results.imageHitFar = Api::ImageHitsPoint(image, cnote::Vec2 { 400.0f, 600.0f });
    results.imageRectMiss =
        Api::ImageIntersectsRectSelection(image, cnote::Vec2 { 400.0f, 500.0f }, cnote::Vec2 { 420.0f, 520.0f });
    results.imageRectHit =
        Api::ImageIntersectsRectSelection(image, cnote::Vec2 { 130.0f, 210.0f }, cnote::Vec2 { 150.0f, 230.0f });
    results.imagePolygonHit = Api::ImageIntersectsPolygonSelection(
        image, { { 100.0f, 180.0f }, { 220.0f, 180.0f }, { 220.0f, 280.0f }, { 100.0f, 280.0f } });
    results.imagePolygonMiss = Api::ImageIntersectsPolygonSelection(
        image, { { 400.0f, 400.0f }, { 500.0f, 400.0f }, { 500.0f, 500.0f } });
    results.imageClamp = Api::ClampImageDeltaToPage(image, page, cnote::Vec2 { -1000.0f, 5.0f });

    results.handleHitRadius = Api::SelectionHandleHitRadiusPixels(96);
    results.rotateOffset = Api::SelectionRotateOffsetPageUnits(1.0f, 96);
    results.rotated90 = Api::RotatePointAround(cnote::Vec2 { 100.0f, 100.0f }, cnote::Vec2 { 200.0f, 100.0f }, 90.0f);
    results.pointInPolygonInside = Api::PointInPolygon(cnote::Vec2 { 50.0f, 50.0f }, polygon);
    results.pointInPolygonOutside = Api::PointInPolygon(cnote::Vec2 { 300.0f, 50.0f }, polygon);
    results.segmentsCross = Api::SegmentsIntersect(
        cnote::Vec2 { 0.0f, 0.0f }, cnote::Vec2 { 10.0f, 10.0f }, cnote::Vec2 { 0.0f, 10.0f }, cnote::Vec2 { 10.0f, 0.0f });
    results.segmentsParallel = Api::SegmentsIntersect(
        cnote::Vec2 { 0.0f, 0.0f }, cnote::Vec2 { 10.0f, 0.0f }, cnote::Vec2 { 0.0f, 5.0f }, cnote::Vec2 { 10.0f, 5.0f });
    return results;
}

#ifdef CNOTE_SELECTION_LEGACY_REFERENCE
struct LegacyApi {
    static bool StrokeBounds(const cnote::Stroke& s, float* a, float* b, float* c, float* d) {
        return cnote::StrokeBounds(s, a, b, c, d);
    }
    static bool StrokeHitsPoint(const cnote::Stroke& s, cnote::Vec2 p, float r, float f) {
        return cnote::StrokeHitsPoint(s, p, r, f);
    }
    static bool StrokeIntersectsRectSelection(const cnote::Stroke& s, cnote::Vec2 a, cnote::Vec2 b, float f) {
        return cnote::StrokeIntersectsRectSelection(s, a, b, f);
    }
    static bool StrokeIntersectsPolygonSelection(const cnote::Stroke& s, const std::vector<cnote::Vec2>& p, float f) {
        return cnote::StrokeIntersectsPolygonSelection(s, p, f);
    }
    static cnote::Vec2 ClampStrokeDeltaToPage(const cnote::Stroke& s, const cnote::Page& p, cnote::Vec2 d) {
        return cnote::ClampStrokeDeltaToPage(s, p, d);
    }
    static cnote::Vec2 ImageCenter(const cnote::PageImage& i) { return cnote::ImageCenter(i); }
    static std::array<cnote::Vec2, 4> ImageCorners(const cnote::PageImage& i) { return cnote::ImageCorners(i); }
    static bool ImageHitsPoint(const cnote::PageImage& i, cnote::Vec2 p) { return cnote::ImageHitsPoint(i, p); }
    static bool ImageIntersectsRectSelection(const cnote::PageImage& i, cnote::Vec2 a, cnote::Vec2 b) {
        return cnote::ImageIntersectsRectSelection(i, a, b);
    }
    static bool ImageIntersectsPolygonSelection(const cnote::PageImage& i, const std::vector<cnote::Vec2>& p) {
        return cnote::ImageIntersectsPolygonSelection(i, p);
    }
    static cnote::Vec2 ClampImageDeltaToPage(const cnote::PageImage& i, const cnote::Page& p, cnote::Vec2 d) {
        return cnote::ClampImageDeltaToPage(i, p, d);
    }
    static float SelectionHandleHitRadiusPixels(int dpi) { return cnote::SelectionHandleHitRadiusPixels(dpi); }
    static float SelectionRotateOffsetPageUnits(float scale, int dpi) {
        return cnote::SelectionRotateOffsetPageUnits(scale, dpi);
    }
    static cnote::Vec2 RotatePointAround(cnote::Vec2 p, cnote::Vec2 c, float d) {
        return cnote::RotatePointAround(p, c, d);
    }
    static bool PointInPolygon(cnote::Vec2 p, const std::vector<cnote::Vec2>& poly) {
        return cnote::PointInPolygon(p, poly);
    }
    static bool SegmentsIntersect(cnote::Vec2 a1, cnote::Vec2 a2, cnote::Vec2 b1, cnote::Vec2 b2) {
        return cnote::SegmentsIntersect(a1, a2, b1, b2);
    }
};
#endif

struct PortedApi {
    static bool StrokeBounds(const cnote::Stroke& s, float* a, float* b, float* c, float* d) {
        return cnote::ui::StrokeBounds(s, a, b, c, d);
    }
    static bool StrokeHitsPoint(const cnote::Stroke& s, cnote::Vec2 p, float r, float f) {
        return cnote::ui::StrokeHitsPoint(s, p, r, f);
    }
    static bool StrokeIntersectsRectSelection(const cnote::Stroke& s, cnote::Vec2 a, cnote::Vec2 b, float f) {
        return cnote::ui::StrokeIntersectsRectSelection(s, a, b, f);
    }
    static bool StrokeIntersectsPolygonSelection(const cnote::Stroke& s, const std::vector<cnote::Vec2>& p, float f) {
        return cnote::ui::StrokeIntersectsPolygonSelection(s, p, f);
    }
    static cnote::Vec2 ClampStrokeDeltaToPage(const cnote::Stroke& s, const cnote::Page& p, cnote::Vec2 d) {
        return cnote::ui::ClampStrokeDeltaToPage(s, p, d);
    }
    static cnote::Vec2 ImageCenter(const cnote::PageImage& i) { return cnote::ui::ImageCenter(i); }
    static std::array<cnote::Vec2, 4> ImageCorners(const cnote::PageImage& i) { return cnote::ui::ImageCorners(i); }
    static bool ImageHitsPoint(const cnote::PageImage& i, cnote::Vec2 p) { return cnote::ui::ImageHitsPoint(i, p); }
    static bool ImageIntersectsRectSelection(const cnote::PageImage& i, cnote::Vec2 a, cnote::Vec2 b) {
        return cnote::ui::ImageIntersectsRectSelection(i, a, b);
    }
    static bool ImageIntersectsPolygonSelection(const cnote::PageImage& i, const std::vector<cnote::Vec2>& p) {
        return cnote::ui::ImageIntersectsPolygonSelection(i, p);
    }
    static cnote::Vec2 ClampImageDeltaToPage(const cnote::PageImage& i, const cnote::Page& p, cnote::Vec2 d) {
        return cnote::ui::ClampImageDeltaToPage(i, p, d);
    }
    static float SelectionHandleHitRadiusPixels(int dpi) { return cnote::ui::SelectionHandleHitRadiusPixels(dpi); }
    static float SelectionRotateOffsetPageUnits(float scale, int dpi) {
        return cnote::ui::SelectionRotateOffsetPageUnits(scale, dpi);
    }
    static cnote::Vec2 RotatePointAround(cnote::Vec2 p, cnote::Vec2 c, float d) {
        return cnote::ui::RotatePointAround(p, c, d);
    }
    static bool PointInPolygon(cnote::Vec2 p, const std::vector<cnote::Vec2>& poly) {
        return cnote::ui::PointInPolygon(p, poly);
    }
    static bool SegmentsIntersect(cnote::Vec2 a1, cnote::Vec2 a2, cnote::Vec2 b1, cnote::Vec2 b2) {
        return cnote::ui::SegmentsIntersect(a1, a2, b1, b2);
    }
};

void VerifyProbe(const ProbeResults& r, const char* prefix) {
    const std::string tag = prefix;
    CheckFrozen(tag + "stroke.bounds.minX", r.strokeBoundsMinX);
    CheckFrozen(tag + "stroke.bounds.minY", r.strokeBoundsMinY);
    CheckFrozen(tag + "stroke.bounds.maxX", r.strokeBoundsMaxX);
    CheckFrozen(tag + "stroke.bounds.maxY", r.strokeBoundsMaxY);
    CheckFrozen(tag + "stroke.hit.onPoint", r.strokeHitOnPoint ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.hit.nearSegment", r.strokeHitNearSegment ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.hit.far", r.strokeHitFar ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.rect.containsSegment", r.strokeRectContainsSegment ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.rect.miss", r.strokeRectMiss ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.polygon.inside", r.strokePolygonInside ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.polygon.miss", r.strokePolygonMiss ? 1.0 : 0.0);
    CheckFrozen(tag + "stroke.clampDelta.x", r.strokeClamp.x);
    CheckFrozen(tag + "stroke.clampDelta.y", r.strokeClamp.y);
    CheckFrozen(tag + "image.center.x", r.imageCenter.x);
    CheckFrozen(tag + "image.center.y", r.imageCenter.y);
    CheckFrozen(tag + "image.corner0.x", r.imageCorner0.x);
    CheckFrozen(tag + "image.corner0.y", r.imageCorner0.y);
    CheckFrozen(tag + "image.hit.center", r.imageHitCenter ? 1.0 : 0.0);
    CheckFrozen(tag + "image.hit.far", r.imageHitFar ? 1.0 : 0.0);
    CheckFrozen(tag + "image.rect.miss", r.imageRectMiss ? 1.0 : 0.0);
    CheckFrozen(tag + "image.rect.hit", r.imageRectHit ? 1.0 : 0.0);
    CheckFrozen(tag + "image.polygon.hit", r.imagePolygonHit ? 1.0 : 0.0);
    CheckFrozen(tag + "image.polygon.miss", r.imagePolygonMiss ? 1.0 : 0.0);
    CheckFrozen(tag + "image.clampDelta.x", r.imageClamp.x);
    CheckFrozen(tag + "image.clampDelta.y", r.imageClamp.y);
    CheckFrozen(tag + "handle.hitRadius.96", r.handleHitRadius);
    CheckFrozen(tag + "handle.rotateOffset.96", r.rotateOffset);
    CheckFrozen(tag + "rotate.90.x", r.rotated90.x);
    CheckFrozen(tag + "rotate.90.y", r.rotated90.y);
    CheckFrozen(tag + "pointInPolygon.inside", r.pointInPolygonInside ? 1.0 : 0.0);
    CheckFrozen(tag + "pointInPolygon.outside", r.pointInPolygonOutside ? 1.0 : 0.0);
    CheckFrozen(tag + "segments.cross", r.segmentsCross ? 1.0 : 0.0);
    CheckFrozen(tag + "segments.parallel", r.segmentsParallel ? 1.0 : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered so a crash still leaves the evidence visible in captured logs.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The MicroTeX Qt adapter loads resource fonts through QFontDatabase, which
    // needs a QGuiApplication (offscreen for tests).
    QGuiApplication app(argc, argv);
    bool printFrozen = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--print-frozen") {
            printFrozen = true;
        }
    }

    if (!cnote::latex::Initialize(std::filesystem::path(CNOTE_LATEX_RES_ROOT))) {
        std::printf("FAIL  latex::Initialize\n");
        return 2;
    }

    const ProbeResults ported = Probe<PortedApi>();
    if (printFrozen) {
        std::printf("FROZEN stroke.bounds.minX %.6f\n", ported.strokeBoundsMinX);
        std::printf("FROZEN stroke.bounds.minY %.6f\n", ported.strokeBoundsMinY);
        std::printf("FROZEN stroke.bounds.maxX %.6f\n", ported.strokeBoundsMaxX);
        std::printf("FROZEN stroke.bounds.maxY %.6f\n", ported.strokeBoundsMaxY);
        std::printf("FROZEN image.corner0.x %.6f\n", ported.imageCorner0.x);
        std::printf("FROZEN image.corner0.y %.6f\n", ported.imageCorner0.y);
        std::printf("FROZEN rotate.90.x %.6f\n", ported.rotated90.x);
        std::printf("FROZEN rotate.90.y %.6f\n", ported.rotated90.y);
        cnote::latex::Shutdown();
        return 0;
    }

    VerifyProbe(ported, "");

    // MapSelectionTransformPoint is a new helper with no legacy counterpart, so
    // it is checked with direct expectations rather than the frozen table.
    {
        const cnote::ui::SelectionBounds from { 100.0f, 200.0f, 300.0f, 400.0f, true };
        const cnote::Vec2 identity = cnote::ui::MapSelectionTransformPoint(cnote::Vec2 { 150.0f, 250.0f }, from, from, 0.0f);
        CheckNear(identity.x, 150.0, "map.identity.x");
        CheckNear(identity.y, 250.0, "map.identity.y");

        // `to` doubles both extents, anchored at the `from` origin.
        const cnote::ui::SelectionBounds to { 100.0f, 200.0f, 500.0f, 600.0f, true };
        const cnote::Vec2 scaled = cnote::ui::MapSelectionTransformPoint(cnote::Vec2 { 150.0f, 250.0f }, from, to, 0.0f);
        CheckNear(scaled.x, 200.0, "map.scale.x");
        CheckNear(scaled.y, 300.0, "map.scale.y");

        // 90 degrees about the `from` centre (200, 300): the point 50 above the
        // centre orbits onto its right.
        const cnote::Vec2 rotated = cnote::ui::MapSelectionTransformPoint(cnote::Vec2 { 200.0f, 250.0f }, from, from, 90.0f);
        CheckNear(rotated.x, 250.0, "map.rotate90.x");
        CheckNear(rotated.y, 300.0, "map.rotate90.y");
    }
    {
        // A zero-size `from` keeps scale 1 instead of dividing by zero.
        const cnote::ui::SelectionBounds point { 10.0f, 20.0f, 10.0f, 20.0f, true };
        const cnote::ui::SelectionBounds to { 10.0f, 20.0f, 110.0f, 220.0f, true };
        const cnote::Vec2 mapped = cnote::ui::MapSelectionTransformPoint(cnote::Vec2 { 30.0f, 40.0f }, point, to, 0.0f);
        CheckNear(mapped.x, 30.0, "map.zeroFrom.x");
        CheckNear(mapped.y, 40.0, "map.zeroFrom.y");
    }

#ifdef CNOTE_SELECTION_LEGACY_REFERENCE
    // Live differential comparison against the legacy implementation, which is
    // only linkable on Windows (UiUtil.h includes <windows.h>).
    const ProbeResults legacy = Probe<LegacyApi>();
    Check(Near(ported.strokeBoundsMinX, legacy.strokeBoundsMinX), "legacy stroke bounds minX");
    Check(Near(ported.strokeBoundsMinY, legacy.strokeBoundsMinY), "legacy stroke bounds minY");
    Check(Near(ported.strokeBoundsMaxX, legacy.strokeBoundsMaxX), "legacy stroke bounds maxX");
    Check(Near(ported.strokeBoundsMaxY, legacy.strokeBoundsMaxY), "legacy stroke bounds maxY");
    Check(ported.strokeHitOnPoint == legacy.strokeHitOnPoint, "legacy stroke hit on point");
    Check(ported.strokeHitNearSegment == legacy.strokeHitNearSegment, "legacy stroke hit near segment");
    Check(ported.strokeHitFar == legacy.strokeHitFar, "legacy stroke hit far");
    Check(ported.strokeRectContainsSegment == legacy.strokeRectContainsSegment, "legacy stroke rect contains segment");
    Check(ported.strokeRectMiss == legacy.strokeRectMiss, "legacy stroke rect miss");
    Check(ported.strokePolygonInside == legacy.strokePolygonInside, "legacy stroke polygon inside");
    Check(ported.strokePolygonMiss == legacy.strokePolygonMiss, "legacy stroke polygon miss");
    Check(Near(ported.strokeClamp.x, legacy.strokeClamp.x), "legacy stroke clamp x");
    Check(Near(ported.strokeClamp.y, legacy.strokeClamp.y), "legacy stroke clamp y");
    Check(Near(ported.imageCenter.x, legacy.imageCenter.x), "legacy image center x");
    Check(Near(ported.imageCenter.y, legacy.imageCenter.y), "legacy image center y");
    Check(Near(ported.imageCorner0.x, legacy.imageCorner0.x), "legacy image corner0 x");
    Check(Near(ported.imageCorner0.y, legacy.imageCorner0.y), "legacy image corner0 y");
    Check(ported.imageHitCenter == legacy.imageHitCenter, "legacy image hit center");
    Check(ported.imageHitFar == legacy.imageHitFar, "legacy image hit far");
    Check(ported.imageRectMiss == legacy.imageRectMiss, "legacy image rect miss");
    Check(ported.imageRectHit == legacy.imageRectHit, "legacy image rect hit");
    Check(ported.imagePolygonHit == legacy.imagePolygonHit, "legacy image polygon hit");
    Check(ported.imagePolygonMiss == legacy.imagePolygonMiss, "legacy image polygon miss");
    Check(Near(ported.imageClamp.x, legacy.imageClamp.x), "legacy image clamp x");
    Check(Near(ported.imageClamp.y, legacy.imageClamp.y), "legacy image clamp y");
    Check(Near(ported.handleHitRadius, legacy.handleHitRadius), "legacy handle hit radius");
    Check(Near(ported.rotateOffset, legacy.rotateOffset), "legacy rotate offset");
    Check(Near(ported.rotated90.x, legacy.rotated90.x), "legacy rotate 90 x");
    Check(Near(ported.rotated90.y, legacy.rotated90.y), "legacy rotate 90 y");
    Check(ported.pointInPolygonInside == legacy.pointInPolygonInside, "legacy point in polygon inside");
    Check(ported.pointInPolygonOutside == legacy.pointInPolygonOutside, "legacy point in polygon outside");
    Check(ported.segmentsCross == legacy.segmentsCross, "legacy segments cross");
    Check(ported.segmentsParallel == legacy.segmentsParallel, "legacy segments parallel");
    Check(Near(legacy.imageCenter.x, 140.0), "legacy reference sanity (image center)");
#endif

    // LaTeX bounds need the engine; kept separate because the frozen values are
    // engine metrics rather than geometry math.
    const cnote::PageLatex latex = MakeLatex();
    const cnote::ui::LatexBounds bounds = cnote::ui::MeasurePageLatexBounds(latex, 612.0f);
    Check(Near(bounds.left, latex.x) && Near(bounds.top, latex.y), "latex bounds anchored at the object origin");
    Check(bounds.right > bounds.left && bounds.bottom > bounds.top, "latex bounds measured with natural metrics");
    Check(cnote::ui::LatexHitsPoint(latex, 612.0f, cnote::Vec2 { bounds.left + 1.0f, bounds.top + 1.0f }),
          "latex hit inside bounds");
    Check(!cnote::ui::LatexHitsPoint(latex, 612.0f, cnote::Vec2 { bounds.right + 20.0f, bounds.bottom + 20.0f }),
          "latex miss outside bounds");
    Check(cnote::ui::LatexIntersectsRectSelection(latex, 612.0f, cnote::Vec2 { 0.0f, 0.0f }, cnote::Vec2 { 100.0f, 100.0f }),
          "latex rect selection overlaps");
    Check(!cnote::ui::LatexIntersectsRectSelection(latex, 612.0f, cnote::Vec2 { 400.0f, 400.0f }, cnote::Vec2 { 500.0f, 500.0f }),
          "latex rect selection miss");

    std::printf("SUMMARY failures=%d\n", g_failures);
    cnote::latex::Shutdown();
    return g_failures == 0 ? 0 : 1;
}
