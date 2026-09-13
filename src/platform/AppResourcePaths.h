#pragma once

#include <filesystem>

namespace cnote::platform {

// Installed-application resource lookup (migration plan step 7.3).
//
// The packaged application must resolve its runtime resources from the location
// the application was installed to, never from the current working directory:
// the app is launched from arbitrary directories on user machines, so a
// cwd-relative lookup would silently disable equations (MicroTeX resources) or
// fail to find staged libraries.  Everything here is derived from
// QCoreApplication::applicationDirPath(), which Qt already resolves per
// platform.
//
// Installed layout:
//   * Windows:  <install>/cnote_qt.exe, <install>/latex-res/, <install>/pdfium.dll
//   * Linux:    <install>/bin/cnote_qt, <install>/bin/latex-res/, <install>/lib/libpdfium.so
//   * macOS:    cnote_qt.app/Contents/MacOS/cnote_qt,
//               cnote_qt.app/Contents/Resources/latex-res/ (a bundle may not
//               carry resources beside its executable)

// Directory containing the installed executable.
std::filesystem::path ApplicationDir();

// Directory holding the application's staged runtime resources (the parent of
// `latex-res`).  On macOS this is the bundle's Contents/Resources directory; in
// a non-bundle/build-tree layout it falls back to the executable directory.
std::filesystem::path AppResourceDir();

// MicroTeX/LaTeX resource tree: AppResourceDir()/"latex-res".
// The CNOTE_LATEX_RES_ROOT environment variable overrides the location, which
// keeps build-tree and test runs working from any directory.
std::filesystem::path LatexResourceDir();

// Applies the packaged cnote icon to the application.  The artwork is compiled
// into this library as a Qt resource (the same file the Windows .rc icon and the
// macOS bundle icon are built from).  Safe to call once a QGuiApplication
// exists; it is a no-op if the resource or the application instance is missing.
void ApplyApplicationIcon();

}  // namespace cnote::platform
