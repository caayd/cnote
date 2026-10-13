// Deterministic interaction-router tests (migration plan steps 5.1-5.3).
//
// Every assertion below states the reference behavior it pins, with the legacy
// line it was ported from.  The router is Qt Gui plus model types only, so this
// test needs no QGuiApplication, no platform plugin and no window: it drives
// PointerSample sequences into a recording host and asserts the emitted
// operations, ordering and precedence.
//
// Why there is no live legacy comparison here (unlike layout_parity_qt):
// the ported decisions live in App:: methods that require window_/session_
// state and a message pump (App::BeginStroke, ApplyStrokePoint, BeginErase,
// BeginSelection, BeginMoveSelection, BeginPan, ...), and the shared helpers
// are anonymous-namespace functions inside src/App.cpp (App.cpp:1958-2060), so
// they cannot be linked and called from a test.  The reference values and
// sequences are therefore pinned as frozen expectations with their line
// references, and the constants are asserted directly (see
// TestPortedReferenceValues).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ui/InteractionRouter.h"

namespace {

using cnote::Vec2;
namespace ui = cnote::ui;

int g_checks = 0;
int g_failures = 0;
constexpr int kMaxPrintedFailures = 40;

void Check(bool condition, const std::string& message) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        if (g_failures <= kMaxPrintedFailures) {
            std::printf("FAIL: %s\n", message.c_str());
        }
    }
}

std::string Format(double value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
}

std::string Format(Vec2 point) {
    return "(" + Format(point.x) + "," + Format(point.y) + ")";
}

bool Near(double a, double b, double tolerance = 1e-3) {
    return std::abs(a - b) <= tolerance;
}

// Records every host call as a compact string so ordering and precedence can be
// asserted without a mock framework.
class RecordingHost : public ui::InteractionHost {
public:
    std::vector<std::string> ops;
    int invalidateCount = 0;
    bool capture = false;

    // One page covering everything except y > 1000 (outside): the segmentation
    // test installs its own boundary.
    std::function<std::optional<ui::PageHit>(const QPointF&)> hitPage =
        [](const QPointF& logical) -> std::optional<ui::PageHit> {
            if (logical.y() > 1000.0) {
                return std::nullopt;
            }
            return ui::PageHit { 0U, 1.0f };
        };
    std::function<Vec2(const QPointF&, bool)> screenToPage =
        [](const QPointF& logical, bool) { return Vec2 { static_cast<float>(logical.x() / 100.0), static_cast<float>(logical.y() / 100.0) }; };
    bool pointInCanvas = true;
    bool hasSelection = false;
    bool cursorInteractsWithSelection = false;
    std::optional<std::pair<std::size_t, std::size_t>> textHit;
    std::optional<std::pair<std::size_t, std::size_t>> latexHit;
    std::optional<std::size_t> pdfTextPage;
    std::optional<ui::SelectionHit> selectionObject;
    std::optional<ui::TransformRequest> selectionHandle;
    std::vector<ui::ObjectRef> eraseTargets;

    std::optional<ui::PageHit> HitTestPage(const QPointF& logical) const override { return hitPage(logical); }
    Vec2 ScreenToPage(const QPointF& logical, bool clampToPage) const override { return screenToPage(logical, clampToPage); }
    // The router uses this only when HitTestPage missed (pointer between
    // pages); the mock maps through screenToPage so tests observe the same
    // coordinates either path produces.
    Vec2 ScreenToPageOnPage(std::size_t, const QPointF& logical, bool clampToPage) const override {
        return screenToPage(logical, clampToPage);
    }
    bool IsPointInCanvas(const QPointF&) const override { return pointInCanvas; }
    bool HasSelection() const override { return hasSelection; }
    bool CursorInteractsWithSelection(const QPointF&) const override { return cursorInteractsWithSelection; }
    std::optional<std::pair<std::size_t, std::size_t>> HitTestText(const QPointF&) const override { return textHit; }
    std::optional<std::pair<std::size_t, std::size_t>> HitTestLatex(const QPointF&) const override { return latexHit; }
    std::optional<std::size_t> PageWithPdfTextAt(const QPointF&) const override { return pdfTextPage; }
    std::optional<ui::SelectionHit> HitTestSelectionObject(const QPointF&, std::size_t) const override { return selectionObject; }
    std::optional<ui::TransformRequest> HitTestSelectionHandle(const QPointF&) const override { return selectionHandle; }
    std::vector<ui::ObjectRef> EraseTargetsAt(std::size_t, Vec2, Vec2) const override { return eraseTargets; }

    void PanBy(const QPointF& delta) override { ops.push_back("PanBy(" + Format(delta.x()) + "," + Format(delta.y()) + ")"); }
    void ZoomByWheelSteps(int steps, const QPointF& anchor) override {
        ops.push_back("ZoomByWheelSteps(" + std::to_string(steps) + "," + Format(anchor.x()) + "," + Format(anchor.y()) + ")");
    }
    void ScrollBy(const QPointF& delta) override { ops.push_back("ScrollBy(" + Format(delta.x()) + "," + Format(delta.y()) + ")"); }
    void SetViewPage(std::size_t pageIndex) override { ops.push_back("SetViewPage(" + std::to_string(pageIndex) + ")"); }
    void SetCursor(ui::CursorKind cursor) override { ops.push_back("SetCursor(" + std::to_string(static_cast<int>(cursor)) + ")"); }
    void InvalidateCanvas() override { ++invalidateCount; }

    void BeginStroke(std::size_t pageIndex, Vec2 point, bool dotted) override {
        ops.push_back("BeginStroke(page=" + std::to_string(pageIndex) + ",point=" + Format(point) + ",dotted=" + (dotted ? "1" : "0") + ")");
    }
    void SetStrokeDotted(bool dotted) override { ops.push_back("SetStrokeDotted(" + std::string(dotted ? "1" : "0") + ")"); }
    void AppendStrokePoint(Vec2 point, bool dotted) override {
        ops.push_back("AppendStrokePoint(" + Format(point) + ",dotted=" + (dotted ? "1" : "0") + ")");
    }
    void ReplaceStrokePoints(const std::vector<Vec2>& points, bool dotted) override {
        ops.push_back("ReplaceStrokePoints(" + std::to_string(points.size()) + ",dotted=" + (dotted ? "1" : "0") + ")");
    }
    void CommitStroke() override { ops.push_back("CommitStroke"); }
    void DiscardStroke() override { ops.push_back("DiscardStroke"); }

    void ClearSelection() override { ops.push_back("ClearSelection"); }
    void UpdateSelectionOverlay(const QPointF&, const QPointF&, bool lasso, const std::vector<QPointF>& lassoPoints) override {
        ops.push_back("UpdateSelectionOverlay(lasso=" + std::string(lasso ? "1" : "0") + ",points=" + std::to_string(lassoPoints.size()) + ")");
    }
    void CommitSelection(const QPointF& anchor, const QPointF& current, bool lasso, bool boxOverride, bool click) override {
        ops.push_back("CommitSelection(anchor=" + Format(anchor.x()) + "," + Format(anchor.y()) + ",current=" + Format(current.x())
            + "," + Format(current.y()) + ",lasso=" + (lasso ? "1" : "0") + ",boxOverride=" + (boxOverride ? "1" : "0")
            + ",click=" + (click ? "1" : "0") + ")");
    }
    void AddObjectToSelection(ui::ObjectRef object) override {
        ops.push_back("AddObjectToSelection(kind=" + std::to_string(static_cast<int>(object.kind)) + ",index=" + std::to_string(object.objectIndex) + ")");
    }
    void BeginMoveSelection(std::size_t pageIndex) override { ops.push_back("BeginMoveSelection(" + std::to_string(pageIndex) + ")"); }
    void BeginCloneMoveSelection(std::size_t pageIndex) override { ops.push_back("BeginCloneMoveSelection(" + std::to_string(pageIndex) + ")"); }
    void UpdateMoveSelection(const QPointF& delta) override { ops.push_back("UpdateMoveSelection(" + Format(delta.x()) + "," + Format(delta.y()) + ")"); }
    void CommitMoveSelection() override { ops.push_back("CommitMoveSelection"); }

    void BeginTransform(const ui::TransformRequest& request) override {
        ops.push_back("BeginTransform(target=" + std::to_string(static_cast<int>(request.target)) + ",page=" + std::to_string(request.pageIndex)
            + ",index=" + std::to_string(request.objectIndex) + ",handle=" + std::to_string(static_cast<int>(request.handle)) + ")");
    }
    void UpdateTransform(const QPointF& logical, Qt::KeyboardModifiers) override { ops.push_back("UpdateTransform(" + Format(logical.x()) + "," + Format(logical.y()) + ")"); }
    void CommitTransform() override { ops.push_back("CommitTransform"); }

    void UpdatePendingErase(const std::vector<ui::ObjectRef>& pending) override {
        ops.push_back("UpdatePendingErase(" + std::to_string(pending.size()) + ")");
    }
    void CommitErase(const std::vector<ui::ObjectRef>& pending) override {
        ops.push_back("CommitErase(" + std::to_string(pending.size()) + ")");
    }

    void BeginTextEdit(std::size_t pageIndex, std::size_t textIndex) override {
        ops.push_back("BeginTextEdit(" + std::to_string(pageIndex) + "," + std::to_string(textIndex) + ")");
    }
    void CreateTextAt(std::size_t pageIndex, Vec2 pagePoint) override {
        ops.push_back("CreateTextAt(" + std::to_string(pageIndex) + "," + Format(pagePoint) + ")");
    }
    void BeginLatexEdit(std::size_t pageIndex, std::size_t latexIndex) override {
        ops.push_back("BeginLatexEdit(" + std::to_string(pageIndex) + "," + std::to_string(latexIndex) + ")");
    }
    void CreateLatexAt(std::size_t pageIndex, Vec2 pagePoint) override {
        ops.push_back("CreateLatexAt(" + std::to_string(pageIndex) + "," + Format(pagePoint) + ")");
    }

    void SetPointerCapture(bool captured) override {
        capture = captured;
        ops.push_back(std::string("SetPointerCapture(") + (captured ? "1)" : "0)"));
    }

    void Reset() {
        ops.clear();
        invalidateCount = 0;
    }

    [[nodiscard]] bool HasOp(const std::string& prefix) const {
        for (const std::string& op : ops) {
            if (op.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int CountOp(const std::string& prefix) const {
        int count = 0;
        for (const std::string& op : ops) {
            if (op.rfind(prefix, 0) == 0) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] const std::string* FirstOp(const std::string& prefix) const {
        for (const std::string& op : ops) {
            if (op.rfind(prefix, 0) == 0) {
                return &op;
            }
        }
        return nullptr;
    }

    [[nodiscard]] int IndexOfOp(const std::string& prefix) const {
        for (std::size_t index = 0; index < ops.size(); ++index) {
            if (ops[index].rfind(prefix, 0) == 0) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    [[nodiscard]] std::string Dump() const {
        std::string result;
        for (const std::string& op : ops) {
            result += "  " + op + "\n";
        }
        return result;
    }
};

ui::PointerSample Press(double x, double y, Qt::MouseButton button = Qt::LeftButton) {
    ui::PointerSample sample;
    sample.position = QPointF(x, y);
    sample.button = button;
    sample.buttons = button == Qt::MiddleButton ? Qt::MiddleButton : Qt::LeftButton;
    return sample;
}

ui::PointerSample Move(double x, double y, Qt::MouseButtons held = Qt::LeftButton) {
    ui::PointerSample sample;
    sample.position = QPointF(x, y);
    sample.buttons = held;
    return sample;
}

ui::PointerSample Release(double x, double y, Qt::MouseButton button = Qt::LeftButton) {
    ui::PointerSample sample;
    sample.position = QPointF(x, y);
    sample.button = button;
    return sample;
}

// MessageLoop.cpp:209-243 tool routing: exactly one interaction starts, and the
// first emitted operation identifies which one.
void TestToolRouting() {
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Pan);
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(router.ActiveInteraction() == ui::InteractionKind::Pan, "pan tool starts a pan");
        Check(host.IndexOfOp("SetCursor(") >= 0 && host.CountOp("BeginStroke") == 0, "pan tool does not begin a stroke");
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Draw);
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(router.ActiveInteraction() == ui::InteractionKind::Stroke, "draw tool begins a stroke");
        Check(host.HasOp("BeginStroke(page=0,point=(0.500,0.500),dotted=0)"), "stroke starts at the pressed page point\n" + host.Dump());
        Check(host.HasOp("SetPointerCapture(1)"), "stroke captures the pointer");
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Zoom);
        router.HandlePointerPress(Press(50.0, 50.0));
        // MessageLoop.cpp:241-243: the reference press dispatch has no zoom
        // branch, so the zoom tool falls through to the stroke path.
        Check(router.ActiveInteraction() == ui::InteractionKind::Stroke, "zoom tool falls through to the stroke path like the reference");
    }
    {
        RecordingHost host;
        host.textHit = std::make_pair<std::size_t, std::size_t>(0U, 3U);
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Text);
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("BeginTextEdit(0,3)"), "text tool edits the text under the cursor\n" + host.Dump());
        Check(router.ActiveInteraction() == ui::InteractionKind::None, "text hit does not leave an interaction active");
        const int clear = host.IndexOfOp("ClearSelection");
        const int edit = host.IndexOfOp("BeginTextEdit");
        Check(clear >= 0 && clear < edit, "opening the editor clears the selection frame first\n" + host.Dump());
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Text);
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("CreateTextAt(0,(0.500,0.500))"), "text tool creates a text box on the page\n" + host.Dump());
        const int clear = host.IndexOfOp("ClearSelection");
        const int create = host.IndexOfOp("CreateTextAt");
        Check(clear >= 0 && clear < create, "creating a text clears the selection frame first\n" + host.Dump());
    }
    {
        RecordingHost host;
        host.latexHit = std::make_pair<std::size_t, std::size_t>(1U, 2U);
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Latex);
        router.HandlePointerPress(Press(150.0, 50.0));
        Check(host.HasOp("BeginLatexEdit(1,2)"), "latex tool edits the equation under the cursor\n" + host.Dump());
        const int clear = host.IndexOfOp("ClearSelection");
        const int edit = host.IndexOfOp("BeginLatexEdit");
        Check(clear >= 0 && clear < edit, "opening the latex editor clears the selection frame first\n" + host.Dump());
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Latex);
        router.HandlePointerPress(Press(150.0, 50.0));
        Check(host.HasOp("CreateLatexAt(0,(1.500,0.500))"), "latex tool creates an equation on the page\n" + host.Dump());
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Draw);
        router.HandlePointerPress(Press(50.0, 1200.0));
        Check(router.ActiveInteraction() == ui::InteractionKind::None && host.ops.empty(), "a press outside every page starts nothing");
    }
}

// MessageLoop.cpp:250-256: the middle button pans with any tool and wins over
// the left-button routing.
void TestMiddleButtonPanPrecedence() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(50.0, 50.0, Qt::MiddleButton));
    Check(router.ActiveInteraction() == ui::InteractionKind::Pan, "middle press begins a pan");
    Check(router.PanFromMiddleButton(), "middle press pan is marked as middle-button");
    Check(host.CountOp("BeginStroke") == 0, "middle press never begins a stroke");

    router.HandlePointerMove(Move(80.0, 70.0, Qt::MiddleButton));
    Check(host.HasOp("PanBy(30.000,20.000)"), "middle drag pans by the pointer delta\n" + host.Dump());

    // A left press while the middle pan is live must not start a second
    // interaction (one pointer at a time).
    host.Reset();
    router.HandlePointerPress(Press(90.0, 90.0));
    Check(router.ActiveInteraction() == ui::InteractionKind::Pan && host.ops.empty(), "left press during a middle pan is ignored");

    router.HandlePointerRelease(Release(90.0, 90.0, Qt::MiddleButton));
    Check(router.ActiveInteraction() == ui::InteractionKind::None, "middle release ends the pan");
    Check(host.HasOp("SetPointerCapture(0)"), "releasing the pan releases capture");
}

// App.cpp:5810-5841 BeginPan/UpdatePan/EndPan.
void TestPanToolDeltas() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Pan);
    host.hasSelection = true;
    router.HandlePointerPress(Press(100.0, 100.0));
    Check(host.HasOp("ClearSelection"), "a pan press clears the selection (App.cpp:5816)");

    host.Reset();
    router.HandlePointerMove(Move(90.0, 110.0));
    Check(host.HasOp("PanBy(-10.000,10.000)"), "pan moves by the pointer delta\n" + host.Dump());

    host.Reset();
    router.HandlePointerMove(Move(90.0, 110.0));
    Check(host.ops.empty(), "an unchanged pointer position pans nothing");

    router.HandlePointerRelease(Release(90.0, 110.0));
    Check(router.ActiveInteraction() == ui::InteractionKind::None, "left release ends a pan-tool pan");
    Check(host.HasOp("SetPointerCapture(0)"), "pan release releases capture");
}

// App.cpp:4490-4496: crossing a page boundary commits the current stroke and
// starts a new segment on the new page.
void TestStrokePageSegmentation() {
    RecordingHost host;
    // Two pages side by side: x < 100 is page 0, x >= 100 is page 1.
    host.hitPage = [](const QPointF& logical) -> std::optional<ui::PageHit> {
        return ui::PageHit { logical.x() < 100.0 ? 0U : 1U, 1.0f };
    };
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(50.0, 50.0));
    host.Reset();

    router.HandlePointerMove(Move(150.0, 50.0));
    const int commitIndex = host.IndexOfOp("CommitStroke");
    const int beginIndex = host.IndexOfOp("BeginStroke(page=1");
    Check(commitIndex >= 0, "crossing a page boundary commits the first segment\n" + host.Dump());
    Check(beginIndex > commitIndex, "the new page segment begins after the commit\n" + host.Dump());
    Check(router.ActiveInteraction() == ui::InteractionKind::Stroke, "the stroke stays active on the new page");

    host.Reset();
    router.HandlePointerRelease(Release(160.0, 50.0));
    Check(host.CountOp("CommitStroke") == 1, "releasing commits exactly the active segment");
    Check(router.ActiveInteraction() == ui::InteractionKind::None, "release ends the stroke");
}

// App.cpp:4497-4559 sampling: the smoothed follow point is dropped below the
// input spacing and resampled above it (App.cpp:1958-1979).
void TestStrokeSpacingAndResampling() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(0.0, 0.0));
    host.Reset();

    // follow = clamp(1 - 0.3, 0.05, 0.95) = 0.7; a 100 px move is 1.0 page unit,
    // the filtered point travels 0.7 page units, below the 1.75 spacing floor
    // (App.cpp:2050-2055), so nothing is appended.
    router.HandlePointerMove(Move(100.0, 0.0));
    Check(host.CountOp("AppendStrokePoint") == 0, "a sub-spacing move appends no point\n" + host.Dump());

    // A 400 px move is 4.0 page units; the filtered point travels 2.8, which is
    // resampled into ceil(2.8 / 1.75) = 2 appended points.
    host.Reset();
    router.HandlePointerMove(Move(400.0, 0.0));
    Check(host.CountOp("AppendStrokePoint") == 2, "a long move is resampled into spacing-sized steps\n" + host.Dump());

    host.Reset();
    router.HandlePointerRelease(Release(400.0, 0.0));
    Check(host.CountOp("CommitStroke") == 1, "the resampled segment commits once");
}

// App.cpp:1981-2006 and 4532-4544: 'A' switches to a straight line, Shift snaps
// the endpoint to 15-degree steps, Ctrl builds the oval/circle, Space sets the
// dotted style.
void TestStrokeStraightLineSnapAndOval() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(0.0, 0.0));

    ui::InteractionModifiers modifiers;
    modifiers.straightLine = true;
    router.SetModifiers(modifiers);
    host.Reset();
    router.HandlePointerMove(Move(100.0, 36.4));
    Check(host.CountOp("ReplaceStrokePoints(2,") == 1, "straight-line mode replaces the point list with one segment\n" + host.Dump());

    host.Reset();
    modifiers.snapLine = true;
    router.SetModifiers(modifiers);
    router.HandlePointerMove(Move(100.0, 36.4));
    Check(host.CountOp("ReplaceStrokePoints(2,") == 1, "snapped straight-line mode still emits one segment");
    Check(Near(ui::InteractionRouter::SnapLineEndpoint(Vec2 { 0.0f, 0.0f }, Vec2 { 1.0f, 0.364f }).y, 0.27543, 1e-4),
        "SnapLineEndpoint snaps a 20-degree direction to 15 degrees (App.cpp:1997-2011)");

    host.Reset();
    modifiers.oval = true;
    router.SetModifiers(modifiers);
    router.HandlePointerMove(Move(100.0, 20.0));
    Check(host.CountOp("ReplaceStrokePoints(73,") == 1, "ctrl builds the 73-point oval (App.cpp:2013-2046)\n" + host.Dump());

    host.Reset();
    modifiers.snapLine = false;
    router.SetModifiers(modifiers);
    router.HandlePointerMove(Move(100.0, 20.0));
    Check(host.CountOp("ReplaceStrokePoints(73,") == 1, "oval without shift stays an ellipse");

    host.Reset();
    modifiers.dotted = true;
    router.SetModifiers(modifiers);
    router.HandlePointerMove(Move(100.0, 20.0));
    Check(host.HasOp("SetStrokeDotted(1)"), "the space modifier switches the live stroke to dotted\n" + host.Dump());

    const std::vector<Vec2> circle = ui::InteractionRouter::BuildOvalStrokePoints(Vec2 { 0.0f, 0.0f }, Vec2 { 1.0f, 0.2f }, true);
    Check(circle.size() == 73U, "circle mode returns 73 points");
    Check(Near(circle[0].x, 1.0) && Near(circle[0].y, 0.5, 1e-6), "the oval starts at the right edge of the snapped bounds");

    router.HandlePointerRelease(Release(100.0, 20.0));
}

// MessageLoop.cpp:915-925 / 996-1006: key changes re-apply the stroke modes
// without pointer motion.
void TestStrokeDottedModifierWithoutMotion() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(0.0, 0.0));
    host.Reset();

    ui::InteractionModifiers modifiers;
    modifiers.dotted = true;
    router.SetModifiers(modifiers);
    router.HandlePointerMove(Move(0.0, 0.0));
    Check(host.CountOp("SetStrokeDotted(1)") == 1, "a dotted mode change with no motion still updates the live stroke\n" + host.Dump());
    Check(host.CountOp("AppendStrokePoint") == 0, "the same-position update appends no point");

    host.Reset();
    router.HandlePointerMove(Move(0.0, 0.0));
    Check(host.ops.empty(), "an unchanged state emits nothing");

    router.HandlePointerRelease(Release(0.0, 0.0));
    Check(host.CountOp("CommitStroke") == 1, "the dotted stroke commits once");
}

// MessageLoop.cpp:451-458: releasing outside the canvas (or over another page)
// still extends the active segment before committing.
void TestStrokeReleaseOutsideCanvas() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(50.0, 50.0));
    host.Reset();

    router.HandlePointerRelease(Release(50.0, 1200.0));
    Check(host.CountOp("CommitStroke") == 1, "release over no page still commits the segment\n" + host.Dump());
    Check(router.ActiveInteraction() == ui::InteractionKind::None, "the stroke ends");
}

// App.cpp:5539-5720: the whole erase drag is one transaction, shown at
// kPendingEraseOpacity until the release.
void TestErasePendingSingleCommit() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Erase);
    host.eraseTargets = { ui::ObjectRef { 0U, ui::ObjectKind::Stroke, 4U } };
    router.HandlePointerPress(Press(50.0, 50.0));
    Check(router.ActiveInteraction() == ui::InteractionKind::Erase, "the eraser starts an erase drag");
    Check(host.HasOp("UpdatePendingErase(1)"), "the first mark is pending\n" + host.Dump());

    host.Reset();
    host.eraseTargets = { ui::ObjectRef { 0U, ui::ObjectKind::Stroke, 7U } };
    router.HandlePointerMove(Move(60.0, 60.0));
    Check(host.HasOp("UpdatePendingErase(2)"), "a second object joins the pending set\n" + host.Dump());

    host.Reset();
    host.eraseTargets = { ui::ObjectRef { 0U, ui::ObjectKind::Stroke, 7U } };
    router.HandlePointerMove(Move(70.0, 70.0));
    Check(host.ops.empty(), "re-hitting a pending object changes nothing (deduplicated)");

    host.Reset();
    router.HandlePointerRelease(Release(70.0, 70.0));
    Check(host.CountOp("CommitErase(") == 1, "the drag commits exactly once");
    Check(host.HasOp("CommitErase(2)"), "one commit carries every pending object\n" + host.Dump());
    Check(ui::kPendingEraseOpacity == 0.5f, "pending erase previews at 50% opacity (App.cpp:172)");

    // An erase press with nothing under the pointer still arms the drag but
    // commits nothing.
    RecordingHost empty;
    ui::InteractionRouter emptyRouter(empty);
    emptyRouter.SetTool(ui::Tool::Erase);
    emptyRouter.HandlePointerPress(Press(50.0, 50.0));
    emptyRouter.HandlePointerRelease(Release(50.0, 50.0));
    Check(empty.CountOp("CommitErase(") == 0 && empty.CountOp("UpdatePendingErase(") == 0, "an erase drag over nothing commits nothing");
}

// Selection.cpp:1269: within the 18-px click radius the gesture is a click,
// beyond it a marquee; Selection.cpp:1581 marks the click-selection box
// override.
void TestSelectionClickVersusMarquee() {
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        router.SetSelectionMode(ui::SelectionMode::Box);
        router.HandlePointerPress(Press(100.0, 100.0));
        Check(host.HasOp("UpdateSelectionOverlay(lasso=0,points=1)"), "a box selection updates the overlay");
        router.HandlePointerMove(Move(105.0, 105.0));
        router.HandlePointerRelease(Release(105.0, 105.0));
        Check(host.HasOp("CommitSelection(anchor=100.000,100.000,current=105.000,105.000,lasso=0,boxOverride=0,click=1)"),
            "a 7-px gesture is a click\n" + host.Dump());
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        router.SetSelectionMode(ui::SelectionMode::Box);
        router.HandlePointerPress(Press(100.0, 100.0));
        router.HandlePointerMove(Move(140.0, 160.0));
        router.HandlePointerRelease(Release(140.0, 160.0));
        Check(host.HasOp("CommitSelection(anchor=100.000,100.000,current=140.000,160.000,lasso=0,boxOverride=0,click=0)"),
            "a 72-px gesture is a marquee\n" + host.Dump());
    }
    {
        // Selection.cpp:1221-1231: a press outside the canvas clears instead of
        // starting a selection.
        RecordingHost host;
        host.pointInCanvas = false;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        router.HandlePointerPress(Press(10.0, 10.0));
        Check(router.ActiveInteraction() == ui::InteractionKind::None && host.HasOp("ClearSelection"),
            "a press outside the canvas clears the selection and starts nothing");
    }
}

// Selection.cpp:1250-1256: lasso points accumulate no closer than 6 px.
void TestSelectionLassoAccumulation() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Select);
    router.SetSelectionMode(ui::SelectionMode::Lasso);
    router.HandlePointerPress(Press(0.0, 0.0));
    router.HandlePointerMove(Move(3.0, 0.0));
    Check(host.HasOp("UpdateSelectionOverlay(lasso=1,points=1)"), "a 3-px move adds no lasso point\n" + host.Dump());
    router.HandlePointerMove(Move(7.0, 0.0));
    Check(host.HasOp("UpdateSelectionOverlay(lasso=1,points=2)"), "a 7-px move adds a lasso point\n" + host.Dump());
    router.HandlePointerRelease(Release(7.0, 0.0));
    Check(host.HasOp("CommitSelection(anchor=0.000,0.000,current=7.000,0.000,lasso=1,boxOverride=0,click=1)"),
        "the lasso commit reports the effective mode\n" + host.Dump());
}

// Selection.cpp:1269: the click radius is measured against the whole lasso path,
// so a loop that travels away and returns commits as a polygon even though its
// release lands back near the anchor.
void TestSelectionLassoClickVersusLoop() {
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        router.SetSelectionMode(ui::SelectionMode::Lasso);
        router.HandlePointerPress(Press(100.0, 100.0));
        router.HandlePointerMove(Move(160.0, 100.0));
        router.HandlePointerMove(Move(160.0, 160.0));
        // The router tracks the release position through the pointer moves.
        router.HandlePointerMove(Move(102.0, 102.0));
        router.HandlePointerRelease(Release(102.0, 102.0));
        Check(host.HasOp("CommitSelection(anchor=100.000,100.000,current=102.000,102.000,lasso=1,boxOverride=0,click=0)"),
            "a lasso loop that returns near its anchor is not a click\n" + host.Dump());
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        router.SetSelectionMode(ui::SelectionMode::Lasso);
        router.HandlePointerPress(Press(100.0, 100.0));
        router.HandlePointerMove(Move(104.0, 103.0));
        router.HandlePointerRelease(Release(104.0, 103.0));
        Check(host.HasOp("CommitSelection(anchor=100.000,100.000,current=104.000,103.000,lasso=1,boxOverride=0,click=1)"),
            "a lasso tap inside the click radius is still a click\n" + host.Dump());
        // The 5-px move is under the 6-px sampling spacing, so the release point
        // only reaches the polygon through the final append.
        Check(host.HasOp("UpdateSelectionOverlay(lasso=1,points=2)"),
            "the release point joins the lasso polygon\n" + host.Dump());
    }
}

// MessageLoop.cpp:213-236 precedence: PDF text beats the move path; an existing
// selection interception wins over starting a new selection.
void TestSelectionPrecedence() {
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.pdfTextPage = 0U;
        host.hasSelection = true;
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.IndexOfOp("ClearSelection") >= 0, "move-tool PDF text press clears the old selection");
        Check(host.HasOp("UpdateSelectionOverlay(lasso=1,points=1)"),
            "move-tool PDF text press starts a lasso selection (the reference default mode)\n" + host.Dump());
        Check(host.CountOp("BeginTransform") == 0 && host.CountOp("BeginMoveSelection") == 0, "PDF text press never moves objects");
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        host.selectionHandle = ui::TransformRequest { ui::TransformTarget::Stroke, 0U, 2U, ui::TransformHandle::TopLeft };
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("BeginTransform(target=0,page=0,index=2,handle=1)"), "a selection handle starts a transform\n" + host.Dump());
        router.HandlePointerMove(Move(70.0, 80.0));
        Check(host.HasOp("UpdateTransform(70.000,80.000)"), "the transform follows the pointer\n" + host.Dump());
        router.HandlePointerRelease(Release(70.0, 80.0));
        Check(host.CountOp("CommitTransform") == 1, "the transform commits exactly once");
    }
    {
        // Selection.cpp:1553-1582: pressing inside the selection bounds with no
        // object under the cursor moves the whole selection instead of clearing
        // it and starting a fresh marquee.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Select);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("BeginMoveSelection(0)"), "a Select press inside the selection bounds starts a move\n" + host.Dump());
        Check(router.ActiveInteraction() == ui::InteractionKind::MoveSelection, "the inside-selection press is a move interaction");
        Check(host.CountOp("ClearSelection") == 0, "the inside-selection press keeps the existing selection");
        Check(host.CountOp("UpdateSelectionOverlay") == 0, "the inside-selection press never starts a marquee");

        host.Reset();
        router.HandlePointerMove(Move(70.0, 80.0));
        Check(host.HasOp("UpdateMoveSelection(20.000,30.000)"), "the whole selection follows the pointer\n" + host.Dump());

        host.Reset();
        router.HandlePointerRelease(Release(70.0, 80.0));
        Check(host.CountOp("CommitMoveSelection") == 1, "releasing inside the selection commits exactly one move");
    }
    {
        // The Move tool routes unconditionally into BeginMoveSelection
        // (MessageLoop.cpp:228), so the inside-selection check must live there.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("BeginMoveSelection(0)"), "a Move press inside the selection bounds starts a move\n" + host.Dump());
        Check(router.ActiveInteraction() == ui::InteractionKind::MoveSelection, "the Move-tool inside-selection press is a move interaction");
        Check(host.CountOp("ClearSelection") == 0 && host.CountOp("UpdateSelectionOverlay") == 0,
            "the Move-tool inside-selection press neither clears nor marquees");
    }
    {
        // A press outside the selection bounds still clears it and starts the
        // box-override marquee (Selection.cpp:1578-1582).
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.CountOp("ClearSelection") == 1, "a press outside the selection bounds clears it");
        Check(host.HasOp("UpdateSelectionOverlay(lasso=0,points=1)"), "a press outside the selection bounds starts a box override\n" + host.Dump());
        Check(router.ActiveInteraction() == ui::InteractionKind::Select, "a press outside the selection bounds starts a selection drag");
    }
    {
        // Selection.cpp:1578-1582: no object under the cursor in the move path
        // falls back to a box-override selection.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        router.SetSelectionMode(ui::SelectionMode::Lasso);
        router.HandlePointerPress(Press(50.0, 50.0));
        router.HandlePointerMove(Move(200.0, 50.0));
        router.HandlePointerRelease(Release(200.0, 50.0));
        Check(host.HasOp("CommitSelection(anchor=50.000,50.000,current=200.000,50.000,lasso=0,boxOverride=1,click=0)"),
            "the empty-space move fallback is a box selection regardless of the lasso mode\n" + host.Dump());
    }
}

// Selection.cpp:1605-1613 and 1650-1687: an already-selected text under the Move
// tool is a click-to-edit until the pointer travels 5 px.  An unselected text is
// selected and moved instead (the reference only treats an already-selected text
// as a click target).
void TestTextClickVersusDrag() {
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        host.selectionObject = ui::SelectionHit { ui::ObjectKind::Text, 2U, true, false };
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(router.ActiveInteraction() == ui::InteractionKind::PendingTextClick, "pressing a selected text arms a text click");
        Check(host.CountOp("BeginMoveSelection") == 0 && host.CountOp("BeginTransform") == 0, "the pending click does not move the object yet");

        host.Reset();
        router.HandlePointerMove(Move(53.0, 53.0));
        Check(host.ops.empty(), "a 4-px move stays a click\n" + host.Dump());

        host.Reset();
        router.HandlePointerRelease(Release(53.0, 53.0));
        Check(host.HasOp("BeginTextEdit(0,2)"), "releasing inside the threshold opens the editor\n" + host.Dump());
        const int clear = host.IndexOfOp("ClearSelection");
        const int edit = host.IndexOfOp("BeginTextEdit");
        Check(clear >= 0 && clear < edit, "click-to-edit clears the selection frame first\n" + host.Dump());
        Check(router.ActiveInteraction() == ui::InteractionKind::None, "the click interaction ends");
    }
    {
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        host.selectionObject = ui::SelectionHit { ui::ObjectKind::Text, 2U, true, false };
        router.HandlePointerPress(Press(50.0, 50.0));
        host.Reset();

        router.HandlePointerMove(Move(58.0, 50.0));
        Check(host.HasOp("BeginMoveSelection(0)"), "crossing the threshold turns the click into a move\n" + host.Dump());
        Check(host.HasOp("UpdateMoveSelection(8.000,0.000)"), "the move delta is measured from the press position\n" + host.Dump());

        host.Reset();
        router.HandlePointerRelease(Release(58.0, 50.0));
        Check(host.CountOp("CommitMoveSelection") == 1 && host.CountOp("BeginTextEdit") == 0, "the drag commits a move and never opens the editor");
    }
    {
        // Unselected text with the Move tool: select it and move it (the
        // reference's pending-click branch requires an already-selected text).
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.selectionObject = ui::SelectionHit { ui::ObjectKind::Text, 5U, false, false };
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("AddObjectToSelection(kind=2,index=5)"), "an unselected text is selected first\n" + host.Dump());
        Check(router.ActiveInteraction() == ui::InteractionKind::MoveSelection, "an unselected text starts a move drag");
        Check(host.CountOp("BeginTextEdit") == 0, "an unselected text does not open the editor");
    }
}

// Alt turns a move drag into clone-and-move: the press routes to
// BeginCloneMoveSelection instead of BeginMoveSelection, and on an already-
// selected text it skips the click-to-edit arming entirely.
void TestAltCloneMove() {
    {
        // Inside the selection bounds with no object under the cursor.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        ui::PointerSample press = Press(50.0, 50.0);
        press.modifiers = Qt::AltModifier;
        router.HandlePointerPress(press);
        Check(host.HasOp("BeginCloneMoveSelection(0)"), "an Alt press inside the selection starts a clone move\n" + host.Dump());
        Check(host.CountOp("BeginMoveSelection") == 0, "the Alt press does not start a plain move");
        Check(router.ActiveInteraction() == ui::InteractionKind::MoveSelection, "the clone press is still a move interaction");

        host.Reset();
        router.HandlePointerMove(Move(70.0, 80.0));
        Check(host.HasOp("UpdateMoveSelection(20.000,30.000)"), "the clones follow the pointer\n" + host.Dump());
        host.Reset();
        router.HandlePointerRelease(Release(70.0, 80.0));
        Check(host.CountOp("CommitMoveSelection") == 1, "the clone move commits through the shared end path");
    }
    {
        // Pressing an unselected object selects it, then clones.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.selectionObject = ui::SelectionHit { ui::ObjectKind::Stroke, 4U, false, false };
        ui::PointerSample press = Press(50.0, 50.0);
        press.modifiers = Qt::AltModifier;
        router.HandlePointerPress(press);
        Check(host.HasOp("AddObjectToSelection(kind=0,index=4)"), "an unselected object is selected before cloning\n" + host.Dump());
        const int add = host.IndexOfOp("AddObjectToSelection");
        const int clone = host.IndexOfOp("BeginCloneMoveSelection");
        Check(clone > add && add >= 0, "the clone move starts after the object joins the selection\n" + host.Dump());
    }
    {
        // An already-selected text under Alt: a clone move, not click-to-edit.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        host.selectionObject = ui::SelectionHit { ui::ObjectKind::Text, 2U, true, false };
        ui::PointerSample press = Press(50.0, 50.0);
        press.modifiers = Qt::AltModifier;
        router.HandlePointerPress(press);
        Check(router.ActiveInteraction() == ui::InteractionKind::MoveSelection, "Alt skips the pending text click\n" + host.Dump());
        Check(host.HasOp("BeginCloneMoveSelection(0)"), "Alt on a selected text clones it\n" + host.Dump());

        host.Reset();
        router.HandlePointerRelease(Release(50.0, 50.0));
        Check(host.CountOp("BeginTextEdit") == 0, "releasing without a drag never opens the editor\n" + host.Dump());
        Check(host.CountOp("CommitMoveSelection") == 1, "the Alt click still commits the clone");
    }
    {
        // Without Alt the same presses keep the plain move behavior.
        RecordingHost host;
        ui::InteractionRouter router(host);
        router.SetTool(ui::Tool::Move);
        host.hasSelection = true;
        host.cursorInteractsWithSelection = true;
        router.HandlePointerPress(Press(50.0, 50.0));
        Check(host.HasOp("BeginMoveSelection(0)") && host.CountOp("BeginCloneMoveSelection") == 0,
            "a press without Alt keeps the plain move\n" + host.Dump());
    }
}


// Session.cpp:95-108: finishing is idempotent, commits the partial interaction
// once, and releases capture once.
void TestFinishInteractionIdempotent() {
    RecordingHost host;
    ui::InteractionRouter router(host);
    router.SetTool(ui::Tool::Draw);
    router.HandlePointerPress(Press(50.0, 50.0));
    router.HandlePointerMove(Move(400.0, 50.0));
    host.Reset();

    router.FinishInteraction();
    Check(host.CountOp("CommitStroke") == 1, "focus loss commits the partial stroke once\n" + host.Dump());
    Check(host.CountOp("SetPointerCapture(0)") == 1, "focus loss releases capture once");
    Check(!router.IsInteractionActive(), "no interaction stays active after finishing");

    host.Reset();
    router.FinishInteraction();
    Check(host.ops.empty(), "a second finish emits nothing\n" + host.Dump());

    // Capture is also released on the normal path and re-acquired per gesture.
    RecordingHost second;
    ui::InteractionRouter secondRouter(second);
    secondRouter.SetTool(ui::Tool::Draw);
    secondRouter.HandlePointerPress(Press(50.0, 50.0));
    secondRouter.HandlePointerRelease(Release(50.0, 50.0));
    Check(second.CountOp("SetPointerCapture(1)") == 1 && second.CountOp("SetPointerCapture(0)") == 1,
        "a normal gesture captures and releases once\n" + second.Dump());
}

// MessageLoop.cpp:492-540 wheel routing: ctrl zooms at the cursor, plain wheel
// scrolls with three 18-px lines per notch, horizontal wheel scrolls sideways.
void TestWheelIntents() {
    RecordingHost host;
    ui::InteractionRouter router(host);

    ui::WheelSample ctrl;
    ctrl.position = QPointF(300.0, 200.0);
    ctrl.angleDeltaY = 120;
    ctrl.modifiers = Qt::ControlModifier;
    Check(router.HandleWheel(ctrl), "ctrl-wheel is consumed");
    Check(host.HasOp("ZoomByWheelSteps(1,300.000,200.000)"), "ctrl-wheel zooms at the cursor (Viewport.cpp:675)\n" + host.Dump());

    host.Reset();
    ui::WheelSample vertical;
    vertical.position = QPointF(300.0, 200.0);
    vertical.angleDeltaY = 120;
    Check(router.HandleWheel(vertical), "wheel is consumed");
    Check(host.HasOp("ScrollBy(0.000,54.000)"), "one notch scrolls three 18-px lines\n" + host.Dump());

    host.Reset();
    ui::WheelSample backward;
    backward.position = QPointF(300.0, 200.0);
    backward.angleDeltaY = -240;
    router.HandleWheel(backward);
    Check(host.HasOp("ScrollBy(0.000,-108.000)"), "two backward notches scroll the other way\n" + host.Dump());

    host.Reset();
    ui::WheelSample horizontal;
    horizontal.position = QPointF(300.0, 200.0);
    horizontal.angleDeltaX = 120;
    router.HandleWheel(horizontal);
    // Qt negates WM_MOUSEHWHEEL's delta, so the reference's
    // SmoothPanBy(-WheelScrollPixels(wmDelta)) is +scrollPixels(angleDeltaX) in
    // Qt's convention (MessageLoop.cpp:533-539; Qt 6.11
    // qwindowspointerhandler.cpp:748-752).
    Check(host.HasOp("ScrollBy(54.000,0.000)"), "horizontal wheel keeps the native direction for a positive delta\n" + host.Dump());

    host.Reset();
    ui::WheelSample horizontalBack;
    horizontalBack.position = QPointF(300.0, 200.0);
    horizontalBack.angleDeltaX = -240;
    router.HandleWheel(horizontalBack);
    Check(host.HasOp("ScrollBy(-108.000,0.000)"), "the opposite horizontal delta scrolls the other way\n" + host.Dump());

    host.Reset();
    ui::WheelSample precision;
    precision.position = QPointF(300.0, 200.0);
    precision.pixelDelta = QPointF(3.0, -4.0);
    router.HandleWheel(precision);
    // A precision device's pixel delta is already smooth and carries the OS
    // momentum: it pans the viewport 1:1 in this event (InteractionHost::PanBy,
    // the reference App::PanBy) and never goes through the animating
    // ScrollBy/WheelScrollPixels line model.
    Check(host.HasOp("PanBy(3.000,-4.000)"), "pixel deltas pan directly, never through the line model\n" + host.Dump());
    Check(!host.HasOp("ScrollBy("), "a precision wheel sample is not animated\n" + host.Dump());
    Check(host.IndexOfOp("PanBy(3.000,-4.000)") == static_cast<int>(host.ops.size()) - 1,
        "the pixel delta is applied once, within its own event\n" + host.Dump());

    host.Reset();
    ui::WheelSample empty;
    empty.position = QPointF(300.0, 200.0);
    Check(!router.HandleWheel(empty), "an empty wheel event is not consumed");
}

// The reference values the router carries, asserted directly: these are the
// quantities that would drift silently if someone edited a constant.
void TestPortedReferenceValues() {
    Check(ui::kSelectionClickRadius == 18.0, "selection click radius is 18 px (Selection.cpp:1269)");
    Check(ui::kLassoPointSpacing == 6.0, "lasso points are at least 6 px apart (Selection.cpp:1252)");
    Check(ui::kTextEditDragThreshold == 5.0, "text click-versus-drag threshold is 5 px (Selection.cpp:1652)");
    Check(ui::kWheelScrollLines == 3, "the wheel scroll-line fallback is 3 (MessageLoop.cpp:54)");
    Check(ui::kWheelScrollUnitPixels == 18, "one scroll line is 18 logical px (MessageLoop.cpp:57)");
    Check(Near(ui::InteractionRouter::InputSpacingPageUnits(0.85f, 1.0f), 1.75), "spacing is clamped to 1.75 screen px (App.cpp:2051)");
    Check(Near(ui::InteractionRouter::InputSpacingPageUnits(5.0f, 1.0f), 3.0), "spacing is clamped to 3.0 screen px (App.cpp:2052)");
    Check(Near(ui::InteractionRouter::InputSpacingPageUnits(5.0f, 2.0f), 1.5),
        "a clamped screen spacing converts back through the live page scale");
    Check(ui::InteractionRouter::BuildOvalStrokePoints(Vec2 { 0.0f, 0.0f }, Vec2 { 0.0f, 0.0f }, false).size() == 2U,
        "a degenerate oval falls back to the two endpoints (App.cpp:2027-2029)");

    const Vec2 snapped = ui::InteractionRouter::SnapLineEndpoint(Vec2 { 0.0f, 0.0f }, Vec2 { 1.0f, 0.364f });
    const double angleDegrees = std::atan2(snapped.y, snapped.x) * 180.0 / 3.14159265358979323846;
    Check(Near(std::fmod(std::abs(angleDegrees), 15.0), 0.0, 1e-3) || Near(std::fmod(std::abs(angleDegrees), 15.0), 15.0, 1e-3),
        "snapped endpoints land on the 15-degree grid (App.cpp:2000)");

    std::vector<Vec2> points;
    ui::InteractionRouter::AppendStrokePointWithResampling(points, Vec2 { 0.0f, 0.0f }, 1.0f);
    ui::InteractionRouter::AppendStrokePointWithResampling(points, Vec2 { 0.5f, 0.0f }, 1.0f);
    Check(points.size() == 1U, "resampling drops a point below the spacing (App.cpp:1968-1970)");
    ui::InteractionRouter::AppendStrokePointWithResampling(points, Vec2 { 4.0f, 0.0f }, 1.0f);
    Check(points.size() == 5U, "resampling splits a 4-unit gap into four 1-unit steps (starting point included)");
    Check(Near(points.back().x, 4.0) && Near(points.back().y, 0.0), "resampling ends exactly on the requested point");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("interaction router: start\n");

    // Pure computation: no Qt platform is needed.  The watchdog only guards
    // against a pathological loop in a future edit.
    std::atomic<bool> finished { false };
    std::thread watchdog([&finished] {
        for (int attempt = 0; attempt < 1200; ++attempt) {
            if (finished.load()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "interaction router: timed out after 120 s\n");
        std::_Exit(9);
    });
    watchdog.detach();

    TestToolRouting();
    TestMiddleButtonPanPrecedence();
    TestPanToolDeltas();
    TestStrokePageSegmentation();
    TestStrokeSpacingAndResampling();
    TestStrokeStraightLineSnapAndOval();
    TestStrokeDottedModifierWithoutMotion();
    TestStrokeReleaseOutsideCanvas();
    TestErasePendingSingleCommit();
    TestSelectionClickVersusMarquee();
    TestSelectionLassoAccumulation();
    TestSelectionLassoClickVersusLoop();
    TestSelectionPrecedence();
    TestTextClickVersusDrag();
    TestAltCloneMove();
    TestFinishInteractionIdempotent();
    TestWheelIntents();
    TestPortedReferenceValues();

    finished.store(true);
    std::printf("%s: %d checks, %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
