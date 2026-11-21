// Headless failures reach the agent, not the desktop (shell/HeadlessFailures.h).
//
// The test runs itself as a child process that installs the headless routing
// and then fails in one way each: an invalid memory access, abort(), qFatal,
// an uncaught C++ exception and a plain qWarning.  For every case the parent
// asserts what the starting process gets back - the reason on stderr and the
// documented exit code - and that the child exited by itself in time: a
// crash dialog or message box would hold the child open until someone
// dismissed it.  On Windows it also asserts the child never shows a window.

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QString>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

#include "shell/HeadlessFailures.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    g_failures += condition ? 0 : 1;
}

int RunChild(const std::string& mode) {
    cnote::shell::RouteHeadlessFailuresToStderr("probe");
    if (mode == "segv") {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "qfatal") {
        qFatal("probe fatal message");
    } else if (mode == "throw") {
        throw std::runtime_error("probe exception");
    } else if (mode == "qwarning") {
        qWarning("probe warning message");
        return 0;
    }
    return 1;
}

#if defined(Q_OS_WIN)
struct WindowScan {
    DWORD processId = 0;
    bool visible = false;
};

BOOL CALLBACK ScanWindow(HWND window, LPARAM parameter) {
    auto* scan = reinterpret_cast<WindowScan*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    scan->visible = scan->visible || (owner == scan->processId && IsWindowVisible(window) != FALSE);
    return TRUE;
}
#endif

struct ChildResult {
    bool finished = false;
    bool showedWindow = false;
    int exitCode = -1;
    QString errors;
};

ChildResult Run(const QString& program, const QString& mode) {
    ChildResult result;
    QProcess process;
    process.start(program, { QStringLiteral("--child=") + mode });
    if (!process.waitForStarted(10000)) {
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 20000) {
#if defined(Q_OS_WIN)
        WindowScan scan { static_cast<DWORD>(process.processId()), false };
        EnumWindows(&ScanWindow, reinterpret_cast<LPARAM>(&scan));
        result.showedWindow = result.showedWindow || scan.visible;
#endif
        process.waitForFinished(5);
    }
    result.finished = process.state() == QProcess::NotRunning;
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.exitCode = process.exitCode();
    result.errors = QString::fromLocal8Bit(process.readAllStandardError());
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string first = argc > 1 ? argv[1] : "";
    if (first.rfind("--child=", 0) == 0) {
        return RunChild(first.substr(8));
    }
    QCoreApplication app(argc, argv);
    const QString self = QCoreApplication::applicationFilePath();

    struct Case {
        const char* mode;
        int exitCode;
        const char* reason;  // must appear on stderr
    };
    const Case cases[] = {
        { "segv", cnote::shell::kHeadlessCrashExitCode, "probe: crashed: " },
        { "abort", cnote::shell::kHeadlessCrashExitCode, "probe: crashed: abort" },
        { "qfatal", cnote::shell::kHeadlessCrashExitCode, "probe fatal message" },
        { "throw", cnote::shell::kHeadlessCrashExitCode, "probe exception" },
        { "qwarning", 0, "probe warning message" },
    };
    for (const Case& test : cases) {
        const ChildResult result = Run(self, QString::fromLatin1(test.mode));
        const std::string name = test.mode;
        std::printf("%s: exit %d, stderr:\n%s", test.mode, result.exitCode, result.errors.toLocal8Bit().constData());
        Check(result.finished, name + ": the child exits by itself (no dialog holds it open)");
        Check(!result.showedWindow, name + ": the child shows no window");
        Check(result.exitCode == test.exitCode, name + ": exit code " + std::to_string(test.exitCode));
        Check(result.errors.contains(QString::fromLatin1(test.reason)),
            name + ": stderr names the failure (\"" + test.reason + "\")");
    }
    std::printf(g_failures == 0 ? "all headless failure checks passed\n" : "%d headless failure check(s) failed\n",
        g_failures);
    return g_failures == 0 ? 0 : 1;
}
