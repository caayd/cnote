#pragma once

// Headless runs (--capture/--script, --self-check) are started by agents and
// tests, often on a desktop someone is using.  Whatever goes wrong in one is
// reported to the process that started it - a line on stderr and an exit
// code - and never to the person at the machine: no Windows Error Reporting or
// macOS crash-reporter dialog, no C runtime assert box, no system error box
// for a DLL or plugin that fails to load (each with its error sound), and no
// Qt message that only a debugger would see (a GUI-subsystem process sends
// them to OutputDebugString by default).

namespace cnote::shell {

// Exit code of a headless run that crashed (a fatal signal, abort(),
// qFatal, an uncaught C++ exception or, on Windows, an unhandled SEH
// exception).  The run prints "<tag>: crashed: <reason>" on stderr first.
inline constexpr int kHeadlessCrashExitCode = 5;

// Call once, before QApplication exists: Qt reads its logging target when
// the first message is written, and an earlier failure must already be
// covered.  `tag` prefixes the crash line ("capture", "self-check") and must
// outlive the process (a string literal).
void RouteHeadlessFailuresToStderr(const char* tag);

}  // namespace cnote::shell
