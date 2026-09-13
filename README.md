# cnote

Native Windows PDF annotation app in C++.

`cnote` is built around fast pen input, simple page navigation, and a minimal UI. It opens PDFs, lets you annotate them with strokes and pasted images, saves native `.cnote` projects, and exports back to PDF.

## What It Does

- Open a PDF or start from a blank document.
- Draw, erase, select, move, and transform annotations.
- Paste images from the clipboard onto pages.
- Save editable `.cnote` projects.
- Export annotated pages as PDF.
- Customize palette, brush behavior, auto-save, zoom presets, and shortcuts.
- Open linked views of a project with independent navigation and shared edits.

## Linked Project Views

Choose **New view of this project** from the title menu to open another window
on the same project, including an unsaved project. Each view keeps its own page,
zoom, scroll, tool, and selection. Completed edits update all linked views.
Switching to another linked view commits an unfinished stroke, drag, text edit,
or equation edit before editing continues there.

Undo/redo belongs to the project: undo in either window reverses the latest
project edit. Saving updates the path and saved state in every view, and one
window coordinates autosave. Closing a view leaves the others open; closing the
last view uses the normal save behavior.

**New window** starts a separate blank project in the same app instance. Opening
an already-open file from that window joins its existing session. Choosing
**New** or opening another file detaches only that window from its old project.

Linked views currently work within one app instance. Separate executable or
Explorer launches do not join existing sessions. Window layouts are not restored
after restarting, and live interaction previews are not broadcast to other views.

## Default Controls

- `D` draw
- `E` erase
- `S` select
- `M` move
- `H` pan
- `L` latex equation (edit source with live preview; type `\` for command suggestions, `Tab` or `Enter` completes the selected command, `Ctrl+Enter` commits)
- LaTeX editing auto-closes `{}`, `()`, and `[]`; completing `\frac` inserts both argument slots
- `Ctrl+Z` undo
- `Ctrl+Y` redo
- `Ctrl+O` open
- `Ctrl+S` save `.cnote`
- `Ctrl+B` toggle PDF sidebar
- `Ctrl+W` quick popup: brush size for pen tools, text formatting while editing text
- `Enter` inserts a new line while editing text
- `Ctrl+Enter` commits the active text edit
- `Escape` cancels the active text edit
- `Ctrl+Q` toggle tool wheel
- `Ctrl+Mouse Wheel` zoom
- `PgUp` / `PgDn` previous / next page

Shortcuts are editable in the settings window.

## Build

Requirements:

- CMake 3.25+
- A compiler matching the selected platform (MinGW-w64 GCC on Windows,
  GCC on Linux, or AppleClang on macOS)
- PDFium **148.0.7749.0** for the target platform and architecture

The Qt MinGW 13.1 kit does not ship `interactioncontext.h` or a MinGW
`ninput` import library. Windows builds use the unmodified public-domain
header from
`mingw-w64/mingw-w64` commit `058bc941cb6b3a93fd00783827841536bc45ce58`
(SHA-256
`25f728091bc5baf7cdba2f86dc110a5649fdf59ccc3402fe2e2c3f13f7f3c252`),
kept under `third_party/mingw-w64/include` with its upstream
`DISCLAIMER.PD`. The precision-touchpad adapter adds that include path only
for its Windows target and dynamically loads the documented `ninput.dll`
APIs, including the Windows 11 ordinal-2507 pointer-frame API; it does not
link or redistribute Windows system binaries.

`CNOTE_PDFIUM_ROOT` selects one complete PDFium SDK root and defaults to the
bundled `third_party/pdfium` Windows SDK. The root must contain
`PDFiumConfig.cmake`, `VERSION`, the PDFium headers, and the matching runtime
library. On Windows, MinGW uses `libpdfium.dll.a` when supplied; otherwise
`gendef` and `dlltool` generate an import library under the build directory
(never in the source tree). MSVC uses `pdfium.dll.lib`.

For the current legacy Windows build:

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCNOTE_PDFIUM_ROOT="$pwd/third_party/pdfium"
cmake --build build
```

The current legacy target is Windows-only. External Linux/macOS SDK roots are
for the migration targets and dependency probes; pass an absolute root when
those targets are enabled:

```text
cmake -S . -B build -G Ninja -DCNOTE_PDFIUM_ROOT=<pdfium-root>
cmake --build build
```

The configure step fails when the SDK version, binary format, architecture,
headers, runtime, or compiler does not match. The build copies the imported
`pdfium` target's runtime next to the executable automatically.
The initial shared build boundary is `cnote_model`, a plain C++20 static
library containing `Document`. It has only the public `src` include path and
does not inherit the legacy UI's Windows or Qt definitions. The legacy
`cnote_core` and existing tests consume this target while the Windows product
remains unchanged.

## Package (Qt migration build)

The Qt application (`cnote_qt`, migration target) installs and packages through
Qt's CMake deploy API. The legacy Windows `cnote` target keeps building but is
neither installed nor packaged; it is renamed at cutover.

```text
cmake -S . -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<qt-root> -DCNOTE_PDFIUM_ROOT=<pdfium-root>
cmake --build build-qt --target cnote_qt
cmake --install build-qt --prefix <stage-directory>
cpack --config build-qt/CPackConfig.cmake
```

CPack produces `cnote_qt-<version>-Windows-x64.zip`, a relocatable
`cnote_qt-<version>-Linux-x64.tar.gz` (extract anywhere; the executable and
plugins carry `$ORIGIN`-relative RPATHs), and a `DragNDrop` disk image on macOS.
The macOS configuration is present but unverified: no macOS machine is
available. CPack writes a `.sha256` checksum next to each artifact.

Each package carries the MicroTeX `latex-res` tree beside the executable
(`Contents/Resources` inside a macOS bundle), PDFium, the Qt platform and
image-format plugins, and - on Windows - the MinGW compiler runtime. The
application resolves `latex-res` relative to its installed location through
`cnote::platform::LatexResourceDir()`, not the working directory.

Verify a package before shipping it:

```text
cmake -DARTIFACT=<zip|tgz|install-directory> -DPLATFORM=windows|linux|macos \
  -DFIXTURE=tools/fixtures/blank.cnote \
  -DDEVELOPER_PREFIX=<build-machine SDK path> -DWORK_DIR=<scratch-directory> \
  [-DQT_QPA_PLATFORM=offscreen] -P tools/verify-package.cmake
```

The check fails loudly on any missing runtime piece. It asserts the Qt
libraries, platform/image plugins, PDFium, the MicroTeX resources (marker file
and fonts) and the Windows compiler runtime are present, that the executable and
`qt.conf` embed no build-machine path, and then launches the packaged
application from an unrelated working directory with a sanitized environment
(`--self-check=<fixture>`), requiring it to resolve its resources and open the
project.

## Run

```powershell
.\build\cnote.exe
```

Open a file directly:

```powershell
.\build\cnote.exe path\to\document.pdf
```

## Notes

- `.cnote` is the native project format for editable work.
- Auto-save applies to saved `.cnote` documents.
- This project is Windows-only and uses raw Win32, GDI, and GDI+ rather than a cross-platform UI framework.
