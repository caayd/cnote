# cnote

PDF annotation app in C++20 with a Qt 6 Widgets frontend for Windows, Linux and
macOS.

The application is the Qt 6 Widgets frontend (`cnote`, plan steps 7.1/7.2). The
original Win32 frontend and its GDI/GDI+/DirectComposition code were removed at
cutover; the model, codec and service layers are shared and portable. `THIRD-PARTY-NOTICES.md` inventories the licenses and
lists the open release prerequisites.

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

## Target and layer structure

| Layer | Targets | Contents |
|---|---|---|
| Model | `cnote_model` | `Document`, history values and the history reducer. Standard C++20 only: no Qt, no OS headers. |
| Services | `cnote_services` (`microtex_qt`, `tinyxml2`) | Text layout (`QTextDocument`), page renderer, PDF import/export, settings store and key bindings, selection clipboard codec, chrome geometry, interaction routing, resource lookup, MicroTeX with the Qt backend. Qt Core/Gui (plus PDFium, MicroTeX, tinyxml2) - no Qt Widgets, no native window handles. |
| UI | `cnote_ui` | Toolbar/sidebar chrome widgets, settings and new-page dialogs, the Windows precision-touchpad adapter, the canvas widget and the per-view controller. Depends on the services and Qt Widgets. |
| Executable | `cnote` | `QApplication` startup, session/window orchestration, deployment metadata. |

## Build the Qt application

Requirements:

- CMake 3.25+, Ninja, a C++20 compiler
- Qt **6.11.2** (Core, Gui, Widgets) for the target platform
- PDFium **148.0.7749.0** matching the target platform and architecture

`CNOTE_PDFIUM_ROOT` must point at one complete PDFium SDK root: a directory with
`PDFiumConfig.cmake`, `VERSION`, `include/` and the matching runtime library
(`bin/pdfium.dll`, `lib/libpdfium.so`, `lib/libpdfium.dylib`). Configure fails
when the version, binary format, architecture, headers, runtime or compiler do
not match. On Windows the MinGW kit's import library is used when supplied;
otherwise `gendef`/`dlltool` generate one under the build directory (never in the
source tree).

Windows x64, MinGW 13.1 from the Qt kit (verified locally):

```powershell
$qt = "C:/Qt/6.11.2/mingw_64"            # or the SDK location
$pdfium = "C:/pdfium-148.0.7749.0/pdfium-win-x64"
cmake -S . -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER="$qt/../../Tools/mingw1310_64/bin/g++.exe" `
  -DCMAKE_PREFIX_PATH="$qt" `
  -DCNOTE_PDFIUM_ROOT="$pdfium"
cmake --build build-qt
```

Linux x64 with GCC (verified locally under Ubuntu 24.04 / WSL):

```bash
cmake -S . -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/Qt/6.11.2/gcc_64 \
  -DCNOTE_PDFIUM_ROOT="$HOME/pdfium-148.0.7749.0"
cmake --build build-qt
```

macOS uses the same two commands with `-DCMAKE_PREFIX_PATH=<qt>/clang_64`,
`-DCMAKE_OSX_ARCHITECTURES=arm64` (or `x86_64`) and the matching
`pdfium-mac-<arch>` SDK. **The macOS build has not been executed** - see the
release status below.

## Test

The suite needs no display for the model/codec tests and the offscreen platform
plugin for the Qt tests (each Qt test already sets its own `QT_PLUGIN_PATH`
through its CTest environment, so a plain `ctest` works after configure):

```bash
# model/codec tests, no display
ctest --test-dir build-qt -R '^(latex_engine|text_model_round_trip|project_sessions|session_manager_qt|interaction_router_qt)$' --output-on-failure
# everything else, offscreen
QT_QPA_PLATFORM=offscreen ctest --test-dir build-qt -E '^(latex_engine|text_model_round_trip|project_sessions|session_manager_qt|interaction_router_qt)$' --output-on-failure
```

`latex_engine`, `text_model_round_trip` and `project_sessions` are the
legacy-named Windows-only model tests; they are absent from the Linux/macOS test
set. `layout_parity_qt` runs everywhere: on Windows it additionally compares the
Qt chrome geometry against the live Win32 reference, on other platforms against
the frozen reference table captured from it.

## Legacy Windows frontend (migration reference)

The legacy target still builds on Windows and remains the behavioural reference
until cutover. Do not extend it.

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCNOTE_PDFIUM_ROOT="$pwd/third_party/pdfium"
cmake --build build
```

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

## Package

The Qt application installs and packages through Qt's CMake deploy API
(`qt_generate_deploy_app_script` + CPack). The legacy Windows `cnote` target
keeps building but is neither installed nor packaged; it is renamed at cutover.

```bash
cmake --build build-qt --target cnote      # add -DCMAKE_CXX_COMPILER=... on Windows
cmake --install build-qt --prefix stage
cpack --config build-qt/CPackConfig.cmake -B dist
```

CPack produces `cnote-<version>-Windows-x64.zip`, a relocatable
`cnote-<version>-Linux-x64.tar.gz` (extract anywhere; the executable and
plugins carry `$ORIGIN`-relative RPATHs), and a `DragNDrop` disk image on macOS.
The macOS configuration is present but unverified: no macOS machine is
available. CPack writes a `.sha256` checksum next to each artifact.

Each package carries the MicroTeX `latex-res` tree beside the executable
(`Contents/Resources` inside a macOS bundle), PDFium, the Qt libraries and
platform/image plugins, and - on Windows - the MinGW compiler runtime. The
application resolves `latex-res` relative to its installed location through
`cnote::platform::LatexResourceDir()`, not the working directory. The verified
Windows ZIP contains 14 files under `bin/` (`cnote.exe`, `qt.conf`,
`Qt6Core/Gui/Widgets/Network/Svg.dll`, `opengl32sw.dll`, `D3Dcompiler_47.dll`,
`pdfium.dll`, the MinGW runtime DLLs), 7 plugin directories, 33 translation
files and 45 `latex-res` files. The verified Linux TGZ contains
`bin/cnote`, `bin/qt.conf`, 45 `latex-res` files, 34 libraries under `lib/`
and 13 plugin directories under `plugins/`.

Only the MicroTeX font licenses (`latex-res/fonts/licences/Knuth_License.txt`,
`OFL.txt`, `License_for_dsrom.txt`) ride along in the packages today.
`THIRD-PARTY-NOTICES.md` records the full inventory and lists the notices that
are still missing (a packaging change, not yet made).

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
project. It is invoked this way by `.github/workflows/package.yml`.

## Run

From the build tree (Qt application):

```powershell
.\build-qt\cnote.exe path\to\document.pdf
```

```bash
./build-qt/cnote path/to/document.pdf
```

From a package, run `bin/cnote.exe` (Windows) or extract the TGZ anywhere and
run `bin/cnote` - resources are resolved relative to the executable, so the
working directory does not matter. The legacy reference build runs as
`build\cnote.exe`.

## Release status

Verified locally (Windows MinGW 13.1 and WSL Ubuntu 24.04): configure, build,
the full CTest set, `cmake --install`, CPack ZIP/TGZ, and
`tools/verify-package.cmake` (17/17 checks on the ZIP, 16/16 on the TGZ,
including launching the packaged application headless).

Not verified, and not claimed as release-ready:

- **macOS build, bundle, signing and notarization** - no macOS machine is
  available; the DMG path is configured only.
- **Hosted CI runners** - `.github/workflows/` pins Qt 6.11.2 and PDFium
  148.0.7749.0 and was validated by running its exact command sequences locally,
  but no GitHub runner has executed it; the macOS jobs are entirely unexecuted.
- **Physical input hardware** - pen/tablet behaviour, pressure, and the Windows
  precision-touchpad adapter on real hardware.
- **Multi-monitor and mixed-DPI transitions**, IME/accessibility behaviour, and
  real-desktop rendering performance.
- **Qt LGPL distribution obligations** - dynamic linking is the selected route
  but the legal/distribution review has not happened (see
  `THIRD-PARTY-NOTICES.md`).

## Notes

- `.cnote` is the native project format for editable work; v14 files written by
  the legacy Windows application stay readable, and new files stay readable by
  it.
- Auto-save applies to saved `.cnote` documents.
- The product UI is Qt 6 Widgets (Core/Gui/Widgets, dynamically linked). The
  remaining raw Win32/GDI/GDI+ code belongs to the legacy `cnote` frontend that
  is deleted at cutover; new work belongs in the Qt targets.
