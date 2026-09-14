// Deterministic offscreen contract test for the Ctrl+W quick-size popup
// placement (QtMainWindow::ToggleQuickPopup's shortcut path).
//
// The reference (App::ToggleQuickSizePopup, ToolbarState.cpp:230-260) anchors
// the popup at the cursor in client coordinates, falls back to the canvas
// centre 120 px below its top when the cursor is outside the canvas (exclusive
// right/bottom edges), and BuildQuickSizePopupLayout (Layout.cpp:228-256)
// centres the 188-wide panel on the anchor, hangs it cursorGap=14 above the
// cursor and clamps it 8 px inside the canvas.  The Qt port regressed this by
// passing a fixed (0,0) host anchor for the shortcut path, so the popup always
// landed at the canvas top-left.
//
// This test drives the real window: QCursor::setPos places the pointer (the
// offscreen platform honours setPos/pos), QTest::keyClick fires the real
// Ctrl+W shortcut through the QWindowSystemInterface -> QShortcutMap path, and
// ToolbarWidget::QuickSizePopupBounds() reports the popup's host-space rect.
// Expected rects come from the public LayoutQt entry points fed with the same
// anchor the reference resolves, so the assertion covers the whole chain:
// cursor -> anchor resolution -> layout -> widget geometry.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QWindow>
#include <QtTest/QTest>

#include "Unicode.h"
#include "qt/QtMainWindow.h"
#include "settings/SettingsStoreQt.h"
#include "shell/AppManager.h"
#include "ui/CanvasWidget.h"
#include "ui/LayoutQt.h"
#include "ui/ToolbarWidget.h"

namespace {

namespace lq = cnote::appui::qt;

int gFailures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("ok   %s\n", what.c_str());
    } else {
        std::printf("FAIL %s\n", what.c_str());
        ++gFailures;
    }
}

std::string DescribeRect(const QRect& rect) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%d,%d %dx%d", rect.x(), rect.y(), rect.width(), rect.height());
    return buffer;
}

// The reference preview extent (App::QuickSizePreviewDiameterForTool +
// ToolbarWidget.cpp PreviewExtent) for the draw tool: the popup's preview
// square is max(28, ceil(diameter) + 2) with diameter = max(1.25, size*scale/2.15).
qreal ExpectedPreviewExtent(float brushSize, qreal pageScale) {
    const qreal diameter = std::max(1.25, static_cast<qreal>(brushSize) * std::max(0.1, pageScale) / 2.15);
    return std::max(4.0, static_cast<qreal>(static_cast<int>(std::ceil(diameter)) + 2));
}

// The anchor the reference resolves for a cursor position: the cursor itself
// inside the canvas (exclusive right/bottom), otherwise the canvas centre
// 120 px below its top.
QPointF ReferenceAnchor(const QRectF& canvas, const QPointF& cursor) {
    const bool inside = cursor.x() >= canvas.left() && cursor.x() < canvas.right()
        && cursor.y() >= canvas.top() && cursor.y() < canvas.bottom();
    if (inside) {
        return cursor;
    }
    return QPointF((canvas.left() + canvas.right()) / 2.0, canvas.top() + 120.0);
}

QRect ExpectedPopupBounds(const QRectF& canvas, const QPointF& cursor, float brushSize, qreal pageScale) {
    const lq::QuickSizePopupLayout layout = lq::BuildQuickSizePopupLayout(
        canvas, ReferenceAnchor(canvas, cursor), ExpectedPreviewExtent(brushSize, pageScale));
    return lq::QuickSizePopupBounds(layout).toAlignedRect();
}

struct Harness {
    cnote::AppManager manager;
    std::unique_ptr<cnote::qt::QtMainWindow> window;
    cnote::ui::ToolbarWidget* toolbar = nullptr;
    cnote::ui::CanvasWidget* canvas = nullptr;

    explicit Harness(const QSize& size) {
        window = std::make_unique<cnote::qt::QtMainWindow>(manager);
        window->show();
        // showEvent snaps the first show to PreferredStartSize(screen); resize
        // afterwards so the requested size wins.
        window->resize(size);
        window->activateWindow();
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        QApplication::setActiveWindow(window.get());
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        QApplication::processEvents();
        QTest::qWait(50);
        toolbar = window->findChild<cnote::ui::ToolbarWidget*>();
        canvas = window->Canvas();
    }

    ~Harness() {
        if (window != nullptr) {
            window->hide();
        }
    }

    // The canvas rect the popup anchors/clamps against, in host coordinates.
    [[nodiscard]] QRectF CanvasRect() const { return toolbar->HostCanvasRect(); }

    // Places the cursor at a host position and fires the real Ctrl+W path.
    void ToggleAt(const QPointF& hostPosition) {
        QCursor::setPos(window->mapToGlobal(hostPosition.toPoint()));
        QApplication::processEvents();
        QTest::keyClick(window->windowHandle(), Qt::Key_W, Qt::ControlModifier);
        QApplication::processEvents();
    }
};

void PopupFollowsCursor(const QSize& windowSize, const char* label) {
    Harness harness(windowSize);
    Check(harness.toolbar != nullptr && harness.canvas != nullptr, std::string(label) + ": chrome exists");

    const QRectF canvas = harness.CanvasRect();
    const float brushSize = harness.toolbar->Settings().drawBrushSize;
    const qreal pageScale = harness.toolbar->PageScale();

    const std::vector<std::pair<std::string, QPointF>> cases = {
        { "top-left", QPointF(canvas.left() + 120.0, canvas.top() + 60.0) },
        { "centre", canvas.center() },
        { "bottom-right", QPointF(canvas.right() - 60.0, canvas.bottom() - 40.0) },
        { "left-edge", QPointF(canvas.left() + 4.0, canvas.top() + 300.0) },
        { "right-edge", QPointF(canvas.right() - 4.0, canvas.top() + 300.0) },
        { "top-edge", QPointF(canvas.left() + 500.0, canvas.top() + 2.0) },
        { "bottom-edge", QPointF(canvas.left() + 500.0, canvas.bottom() - 2.0) },
        // Inside the window but above the canvas (on the toolbar band): the
        // reference's out-of-canvas fallback.
        { "toolbar-band", QPointF(canvas.center().x(), canvas.top() - 10.0) },
        // Fully outside the window.
        { "outside", QPointF(-400.0, -300.0) },
        // Exactly on the exclusive right/bottom edges: still the fallback.
        { "right-exclusive", QPointF(canvas.right(), canvas.center().y()) },
        { "bottom-exclusive", QPointF(canvas.center().x(), canvas.bottom()) },
    };

    for (const auto& [name, hostPosition] : cases) {
        harness.ToggleAt(hostPosition);
        const std::string what = std::string(label) + " " + name;
        Check(harness.toolbar->QuickSizePopupVisible(), what + ": popup opens");
        const QRect got = harness.toolbar->QuickSizePopupBounds();
        const QRect want = ExpectedPopupBounds(canvas, hostPosition, brushSize, pageScale);
        Check(got == want, what + ": popup rect " + DescribeRect(got) + " == expected " + DescribeRect(want));
        Check(harness.window->rect().contains(got), what + ": popup stays inside the window");

        // Ctrl+W again toggles it off (same tool).
        harness.ToggleAt(hostPosition);
        Check(!harness.toolbar->QuickSizePopupVisible(), what + ": second Ctrl+W closes the popup");
    }
}

void PopupTracksLargerBrushPreview() {
    Harness harness(QSize(1000, 700));
    // A large brush makes the preview square exceed the 28 px floor, so the
    // popup's total height (and therefore its anchor offset) grows.
    cnote::settings::Settings settings = harness.toolbar->Settings();
    settings.drawBrushSize = 60.0f;
    harness.toolbar->SetSettings(settings);

    const QRectF canvas = harness.CanvasRect();
    const QPointF centre = canvas.center();
    harness.ToggleAt(centre);
    const QRect got = harness.toolbar->QuickSizePopupBounds();
    const QRect want = ExpectedPopupBounds(canvas, centre, 60.0f, harness.toolbar->PageScale());
    Check(got == want, "large brush: popup rect " + DescribeRect(got) + " == expected " + DescribeRect(want));
    Check(got.bottom() < centre.y(), "large brush: popup still hangs above the cursor");
    harness.ToggleAt(centre);
}

std::filesystem::path MakeTempDir(const std::string& name) {
    std::error_code error;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(error) / cnote::Utf8ToPath("cnote-quick-popup-" + name);
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

// Points the settings store at a scratch directory before any window loads it,
// so the test never reads or writes the developer's real settings file.
void RedirectSettingsStore() {
    const std::filesystem::path scratch = MakeTempDir("settings");
    const QByteArray value(reinterpret_cast<const char*>(scratch.u8string().data()),
        static_cast<qsizetype>(scratch.u8string().size()));
#if defined(Q_OS_WIN)
    qputenv("APPDATA", value);
#else
    qputenv("XDG_CONFIG_HOME", value);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cnote"));
    RedirectSettingsStore();

    // Normal and compact widths; the compact case exercises the horizontal
    // clamp where the panel nearly spans the canvas.
    PopupFollowsCursor(QSize(1200, 800), "normal");
    PopupFollowsCursor(QSize(420, 500), "compact");
    PopupTracksLargerBrushPreview();

    if (gFailures != 0) {
        std::printf("%d quick-popup placement check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("all quick-popup placement checks passed\n");
    return 0;
}
