#include "shell/HeadlessFailures.h"

#include <csignal>
#include <cstdlib>
#include <cstring>

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cnote::shell {

namespace {

const char* g_tag = "cnote";

// Async-signal-safe: no stdio, no allocation.
void WriteError(const char* text) {
    const std::size_t length = std::strlen(text);
#if defined(Q_OS_WIN)
    static_cast<void>(_write(2, text, static_cast<unsigned int>(length)));
#else
    static_cast<void>(write(2, text, length));
#endif
}

[[noreturn]] void ReportCrash(const char* reason) {
    WriteError(g_tag);
    WriteError(": crashed: ");
    WriteError(reason);
    WriteError("\n");
    // _exit, not exit: nothing else of a crashed process may run, and the
    // platform's crash handling (the dialog) must never be reached.
    _exit(kHeadlessCrashExitCode);
}

extern "C" void OnFatalSignal(int signal) {
    switch (signal) {
    case SIGABRT:
        ReportCrash("abort (SIGABRT)");
    case SIGSEGV:
        ReportCrash("invalid memory access (SIGSEGV)");
    case SIGILL:
        ReportCrash("illegal instruction (SIGILL)");
    case SIGFPE:
        ReportCrash("arithmetic error (SIGFPE)");
#if !defined(Q_OS_WIN)
    case SIGBUS:
        ReportCrash("bus error (SIGBUS)");
#endif
    default:
        ReportCrash("fatal signal");
    }
}

#if defined(Q_OS_WIN)
// Every unhandled SEH exception (access violation, stack overflow, ...)
// lands here instead of in Windows Error Reporting.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    char reason[] = "unhandled exception 0x00000000";
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
    char* digit = reason + sizeof(reason) - 2;
    for (int shift = 0; shift < 32; shift += 4, --digit) {
        *digit = "0123456789ABCDEF"[(code >> shift) & 0xFU];
    }
    ReportCrash(reason);
}
#endif

QtMessageHandler g_qtHandler = nullptr;

// Qt's own handler prints the message (to stderr, see below); a fatal one is
// then reported here, because Qt ends the process with a fail-fast on
// Windows, which skips every handler and goes straight to Windows Error
// Reporting.
void OnQtMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    g_qtHandler(type, context, message);
    if (type == QtFatalMsg) {
        ReportCrash("qFatal");
    }
}

}  // namespace

void RouteHeadlessFailuresToStderr(const char* tag) {
    g_tag = tag;
    // Qt messages (warnings, qFatal's reason) go to stderr even without a
    // console attached.
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    g_qtHandler = qInstallMessageHandler(&OnQtMessage);
#if defined(Q_OS_WIN)
    // No system error box for a missing drive/DLL/plugin, no WER dialog.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // assert() and other C runtime diagnostics print instead of opening a
    // message box (a GUI-subsystem process defaults to the box).
    _set_error_mode(_OUT_TO_STDERR);
    ::SetUnhandledExceptionFilter(&OnUnhandledException);
#endif
    // abort() (std::terminate included) reaches SIGABRT before the C
    // runtime's own abort message box; the macOS crash reporter never sees a
    // process that exits from its signal handler.
    for (const int signal : { SIGABRT, SIGSEGV, SIGILL, SIGFPE }) {
        std::signal(signal, &OnFatalSignal);
    }
#if !defined(Q_OS_WIN)
    std::signal(SIGBUS, &OnFatalSignal);
#endif
}

}  // namespace cnote::shell
