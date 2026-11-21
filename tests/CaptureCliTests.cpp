// End-to-end contract test for the headless capture mode (`cnote --capture`
// / `--script`, src/shell/CaptureDriver.h) through the real executable.
//
// Agents rely on it to exercise the app without a desktop session, so it
// must hold:
//   * no window of the process ever becomes visible and none becomes the
//     foreground window (Windows: polled with EnumWindows while it runs);
//   * scripted input reaches the app the way platform input does: a drag
//     draws a stroke, Ctrl+W opens the quick-size popup at the pointer, a click
//     opens the title menu and Escape closes it (popup mode on hidden windows);
//   * captures have the requested logical size times --scale;
//   * the startup document is never modified, even by a script that edits and
//     saves it, and the same parsed path is copied and opened;
//   * unknown startup options are rejected, not ignored;
//   * a step after a click runs only once that click returns, and a drag leaves
//     no mouse grab behind;
//   * Finish stops the run instead of starting another step;
//   * script errors fail fast with the line number.
//
// Every wait is bounded, so a regression can never hang the suite.

#include <algorithm>
#include <cstdio>
#include <string>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) {
        ++gFailures;
    }
}

struct RunResult {
    int exitCode = -1;
    QString output;
    // Windows only: how many of the process's top-level windows were ever
    // visible, and whether one of them was ever the foreground window.
    int visibleWindows = 0;
    bool tookForeground = false;
};

#if defined(Q_OS_WIN)
struct WindowScan {
    DWORD processId = 0;
    int visible = 0;
};

BOOL CALLBACK ScanWindow(HWND window, LPARAM parameter) {
    auto* scan = reinterpret_cast<WindowScan*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == scan->processId && IsWindowVisible(window) != FALSE) {
        ++scan->visible;
    }
    return TRUE;
}
#endif

RunResult RunCnote(const QStringList& arguments, const QString& workingDirectory) {
    RunResult result;
    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral(CNOTE_EXECUTABLE), arguments);
    if (!process.waitForStarted(10000)) {
        result.output = QStringLiteral("cannot start ") + QStringLiteral(CNOTE_EXECUTABLE);
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 60000) {
#if defined(Q_OS_WIN)
        WindowScan scan { static_cast<DWORD>(process.processId()), 0 };
        EnumWindows(&ScanWindow, reinterpret_cast<LPARAM>(&scan));
        result.visibleWindows = std::max(result.visibleWindows, scan.visible);
        DWORD foregroundOwner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundOwner);
        result.tookForeground = result.tookForeground || foregroundOwner == scan.processId;
#endif
        process.waitForFinished(5);
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(5000);
        result.output = QStringLiteral("timed out\n");
    }
    result.output += QString::fromLocal8Bit(process.readAll());
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}

bool IsInk(const QImage& image, int x, int y) {
    return qGray(image.pixel(x, y)) < 128;
}

bool IsPaper(const QImage& image, int x, int y) {
    return qGray(image.pixel(x, y)) > 240;
}

// Whether any pixel of the box differs between the two images.
bool BoxChanged(const QImage& before, const QImage& after, const QRect& box) {
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            if (before.pixel(x, y) != after.pixel(x, y)) {
                return true;
            }
        }
    }
    return false;
}

QByteArray FileHash(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

void WriteText(const QString& path, const QByteArray& text) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(text);
    }
}

void ScriptedSessionStaysOffScreen(const QDir& work) {
    const QString document = work.filePath(QStringLiteral("blank.cnote"));
    QFile::remove(document);
    Check(QFile::copy(QStringLiteral(CNOTE_FIXTURE), document), "copy the blank fixture");
    const QByteArray documentHash = FileHash(document);

    // 1000x700 logical, canvas below the 75 px toolbar band.  How wide the
    // page is follows the screen (100% zoom fits the monitor width), so strokes
    // stay near the window centre, which is always on the page, and the menu
    // check compares captures instead of expecting paper.
    WriteText(work.filePath(QStringLiteral("session.txt")),
        "# stroke, quick-size popup, title menu, then edit and save\n"
        "drag 420 300 580 300\n"
        "capture stroke.png\n"
        "move 700 500\n"
        "key Ctrl+W\n"
        "wait 50\n"
        "capture popup.png\n"
        "key Escape\n"
        "click 30 17\n"
        "wait 50\n"
        "capture menu.png\n"
        "key Escape\n"
        "wait 50\n"
        "capture closed.png\n"
        "drag 420 500 580 500\n"
        "key Ctrl+S\n");

    const RunResult run = RunCnote(
        { QStringLiteral("--script=session.txt"), QStringLiteral("--size=1000x700"), document }, work.path());
    std::printf("%s", run.output.toLocal8Bit().constData());
    Check(run.exitCode == 0, "the scripted session exits 0");
#if defined(Q_OS_WIN)
    Check(run.visibleWindows == 0, "no window of the process is ever visible");
    Check(!run.tookForeground, "no window of the process ever takes the foreground");
#endif

    const QImage stroke(work.filePath(QStringLiteral("stroke.png")));
    const QImage popup(work.filePath(QStringLiteral("popup.png")));
    const QImage menu(work.filePath(QStringLiteral("menu.png")));
    const QImage closed(work.filePath(QStringLiteral("closed.png")));
    Check(stroke.size() == QSize(1000, 700) && popup.size() == stroke.size() && menu.size() == stroke.size()
            && closed.size() == stroke.size(),
        "captures are 1000x700 at scale 1");
    if (stroke.size() != QSize(1000, 700) || popup.size() != stroke.size() || menu.size() != stroke.size()
        || closed.size() != stroke.size()) {
        return;
    }
    Check(IsInk(stroke, 500, 300) && IsPaper(stroke, 500, 400), "the drag drew a stroke along its path");
    Check(BoxChanged(stroke, popup, QRect(600, 420, 200, 160)), "Ctrl+W opened the quick-size popup at the pointer");
    // Below the band, where the title menu drops down from its left edge.
    const QRect menuBox(20, 90, 150, 100);
    Check(BoxChanged(stroke, menu, menuBox), "the title menu is composited over the canvas");
    Check(!BoxChanged(stroke, closed, menuBox), "Escape closed the title menu");
    Check(FileHash(document) == documentHash, "the startup document is unchanged after edit and Ctrl+S");
}

// Script coordinates are logical at every scale: a drag at --scale=1.5 must
// land where the same drag lands at scale 1, times 1.5 in the image.
void ScaleKeepsLogicalCoordinates(const QDir& work) {
    WriteText(work.filePath(QStringLiteral("scaled.txt")), "drag 420 300 580 300\n");
    const RunResult run = RunCnote({ QStringLiteral("--script=scaled.txt"), QStringLiteral("--capture=scaled.png"),
                                       QStringLiteral("--size=1000x700"), QStringLiteral("--scale=1.5") },
        work.path());
    Check(run.exitCode == 0, "the scaled run exits 0");
    const QImage scaled(work.filePath(QStringLiteral("scaled.png")));
    Check(scaled.size() == QSize(1500, 1050), "--scale=1.5 captures 1000x700 logical as 1500x1050 pixels");
    if (scaled.size() == QSize(1500, 1050)) {
        Check(IsInk(scaled, 750, 450) && IsPaper(scaled, 750, 600), "the scaled drag drew at its logical position");
    }
}

void ScriptErrorsNameTheLine(const QDir& work) {
    WriteText(work.filePath(QStringLiteral("broken.txt")), "wait 10\nclick 10\n");
    const RunResult run = RunCnote({ QStringLiteral("--script=broken.txt") }, work.path());
    Check(run.exitCode == 2, "a malformed script exits 2");
    Check(run.output.contains(QStringLiteral("line 2")), "the error names the failing line");
}

// One parse supplies both paths: the file copied into the scratch directory and
// the file the window opens.  A non-ASCII name only survives when both come
// from the same parsed path.
void StartupPathIsParsedOnce(const QDir& work) {
    const QString document = work.filePath(QStringLiteral("café notes.cnote"));
    QFile::remove(document);
    Check(QFile::copy(QStringLiteral(CNOTE_FIXTURE), document), "copy the fixture to a non-ASCII path");
    const QByteArray documentHash = FileHash(document);

    WriteText(work.filePath(QStringLiteral("single-parse.txt")),
        "drag 420 300 580 300\n"
        "capture opened.png\n"
        "key Ctrl+S\n");

    const RunResult run = RunCnote(
        { QStringLiteral("--script=single-parse.txt"), QStringLiteral("--size=1000x700"), document }, work.path());
    std::printf("%s", run.output.toLocal8Bit().constData());
    Check(run.exitCode == 0, "a non-ASCII startup path is copied and opened from one parse");
    Check(QFileInfo::exists(work.filePath(QStringLiteral("opened.png"))), "the capture step ran");
    Check(FileHash(document) == documentHash, "Ctrl+S saved the scratch copy, not the parsed startup path");
}

void UnknownStartupFlagsAreRejected(const QDir& work) {
    const RunResult run = RunCnote({ QStringLiteral("--not-a-cnote-option") }, work.path());
    Check(run.exitCode == 2, "an unknown startup option exits 2");
    Check(run.output.contains(QStringLiteral("unknown option --not-a-cnote-option")),
        "the error names the unknown option");
}

// The click opens the title menu; the next step is scheduled only after the
// click returns, and suspension does not mean "wait until the popup closes", so
// the later Escape still closes it.
void StepsAfterAClickWaitForTheClick(const QDir& work) {
    WriteText(work.filePath(QStringLiteral("scheduling.txt")),
        "capture scheduling-before.png\n"
        "click 30 17\n"
        "wait 50\n"
        "capture scheduling-menu.png\n"
        "key Escape\n"
        "wait 50\n"
        "capture scheduling-closed.png\n");

    const RunResult run = RunCnote(
        { QStringLiteral("--script=scheduling.txt"), QStringLiteral("--size=1000x700") }, work.path());
    std::printf("%s", run.output.toLocal8Bit().constData());
    Check(run.exitCode == 0, "steps after a click that opens a menu still run");

    const QImage before(work.filePath(QStringLiteral("scheduling-before.png")));
    const QImage menu(work.filePath(QStringLiteral("scheduling-menu.png")));
    const QImage closed(work.filePath(QStringLiteral("scheduling-closed.png")));
    if (before.size() != QSize(1000, 700) || menu.size() != before.size() || closed.size() != before.size()) {
        Check(false, "the scheduling captures are 1000x700");
        return;
    }
    const QRect menuBox(20, 90, 150, 100);
    Check(BoxChanged(before, menu, menuBox), "the step after the click sees the menu the click opened");
    Check(!BoxChanged(before, closed, menuBox), "the later Escape closes that menu");
}

// The drag's release clears the implicit grab (the same clear Finish performs
// for a script that stops mid-gesture).  A stale grab would route the next
// click to the canvas, so the title menu would never open.
void DragReleasesTheMouseGrab(const QDir& work) {
    WriteText(work.filePath(QStringLiteral("grab.txt")),
        "capture grab-before.png\n"
        "drag 420 300 580 300\n"
        "click 30 17\n"
        "wait 50\n"
        "capture grab-menu.png\n");

    const RunResult run = RunCnote(
        { QStringLiteral("--script=grab.txt"), QStringLiteral("--size=1000x700") }, work.path());
    std::printf("%s", run.output.toLocal8Bit().constData());
    Check(run.exitCode == 0, "a click after a drag still reaches the title bar");

    const QImage before(work.filePath(QStringLiteral("grab-before.png")));
    const QImage menu(work.filePath(QStringLiteral("grab-menu.png")));
    if (before.size() != QSize(1000, 700) || menu.size() != before.size()) {
        Check(false, "the grab captures are 1000x700");
        return;
    }
    Check(BoxChanged(before, menu, QRect(20, 90, 150, 100)),
        "the drag released its grab, so the next click opened the title menu");
}

// Finish is reported once and never runs another step.  A directory where the
// capture file should go makes the first step fail.
void FinishStopsTheRun(const QDir& work) {
    Check(work.mkdir(QStringLiteral("blocked.png")), "create a directory where the capture would go");
    WriteText(work.filePath(QStringLiteral("finish.txt")),
        "capture blocked.png\n"
        "capture unreachable.png\n");

    const RunResult run = RunCnote({ QStringLiteral("--script=finish.txt") }, work.path());
    Check(run.exitCode == 4, "a capture that cannot be written exits 4");
    Check(!QFileInfo::exists(work.filePath(QStringLiteral("unreachable.png"))),
        "Finish never runs another step");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL cannot create a scratch directory\n");
        return 1;
    }
    const QDir work(scratch.path());

    ScriptedSessionStaysOffScreen(work);
    ScaleKeepsLogicalCoordinates(work);
    ScriptErrorsNameTheLine(work);
    StartupPathIsParsedOnce(work);
    UnknownStartupFlagsAreRejected(work);
    StepsAfterAClickWaitForTheClick(work);
    DragReleasesTheMouseGrab(work);
    FinishStopsTheRun(work);

    std::printf(gFailures == 0 ? "ALL CAPTURE CLI CHECKS PASSED\n" : "%d CAPTURE CLI CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
