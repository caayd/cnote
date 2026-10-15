# cnote

PDF annotation app in C++20 with a Qt 6 Widgets frontend for Windows, Linux and
macOS.

The application is a Qt 6 Widgets frontend (`cnote`). The original Win32
frontend and its GDI/GDI+/DirectComposition code have been removed; the
model, codec and service layers are portable. `THIRD-PARTY-NOTICES.md`
inventories the licenses and lists the open release prerequisites.

`cnote` is built around fast pen input, simple page navigation, and a minimal UI. It opens PDFs, lets you annotate them with strokes and pasted images, saves native `.cnote` projects, and exports back to PDF.

## What It Does

- Open a PDF or start from a blank document.
- Draw, erase, select, move, and transform annotations.
- Copy, cut, and paste annotation objects; text retains rich formatting and rotation.
- Paste images from the clipboard onto pages.
- Save editable `.cnote` projects.
- Export annotated pages as PDF.
- Customize palette, brush behavior, text size presets, auto-save, zoom presets, and shortcuts.
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

## Window chrome

On Windows the window has **no native title bar**: the band across the top of
the client area is the application's own title bar. `QtMainWindow::nativeEvent`
answers `WM_NCCALCSIZE` with the whole window rectangle as client area (what the
removed Win32 frontend did) and `WM_NCACTIVATE` with `TRUE`, while the
`WS_OVERLAPPEDWINDOW` style is deliberately kept - that is what preserves the
DWM shadow, Aero snap, drag-to-move and double-click maximize/restore, the
edge/corner resize zones and the work-area clamp when maximizing.
`WM_NCHITTEST` supplies the DPI-scaled resize zones from the real frame metrics
and splits the band into a drag strip (`HTCAPTION`) and the clickable controls
(`HTCLIENT`), using the same hit test the band widget uses for its own mouse
routing (`ResolveWindowChromeHit`, `src/ui/WindowBehaviorQt.h`).

The band carries the title/file menu, the zoom indicator, undo/redo, the tool
buttons, the palette swatch and - where the native caption would have been - the
application's own minimize, maximize/restore (the glyph switches to the
reference two-square restore glyph while maximized) and close controls. Its
height is the reference 34 logical pixels, 39 while maximized with a 5-pixel top
inset and the caption cluster inset from the right edge by the native frame
width; the two pixels above the canvas show the window background, matching the
reference's clipped toolbar surface.

With **Auto-hide toolbar** enabled in Settings the entire band, window controls
included, animates away and comes back when the pointer reaches the top edge
(12 logical pixels normally, 22 while maximized); with the option disabled the
band is always visible. Linux and macOS keep their native decorations, so the
band there never draws the in-client window buttons.

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
- `Ctrl+W` quick popup: brush size for pen tools, text formatting for the Text tool and while editing text
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

The suite needs no display for the model/codec tests. Every Qt test selects
its own platform through its CTest `ENVIRONMENT` properties (offscreen
everywhere except the three Windows tests that need the native platform), so
a plain `ctest` works after configure - do not force `QT_QPA_PLATFORM`
globally, because the offscreen plugin's placeholder font metrics break the
Windows font tests:

```bash
# model/codec tests, no display
ctest --test-dir build-qt -R '^(xpress_codec|session_manager_qt|interaction_router_qt|precision_input_router_qt)$' --output-on-failure
# everything else (offscreen, or the native platform where a test requires it)
ctest --test-dir build-qt -E '^(xpress_codec|session_manager_qt|interaction_router_qt|precision_input_router_qt)$' --output-on-failure
```

The headless set is `xpress_codec` (pure C++), `session_manager_qt`
(`QCoreApplication` only), `interaction_router_qt` and
`precision_input_router_qt` (no Qt application object). On Windows the suite
has 28 tests; Linux and macOS have 27 because `precision_touchpad_qt` is
Windows-only. On Windows, `selection_clipboard_qt` (real clipboard bridge),
`layout_parity_qt` and `font_fallback_qt` (real font metrics) run on the
native platform; `layout_parity_qt` compares the Qt chrome geometry against
the frozen reference table captured from the removed Win32 frontend.
`shortcut_routing_qt` includes a selected rich-text object's Ctrl+C/Ctrl+V
round trip through the window, preserving Unicode, formatting, dimensions and
rotation, with undo/redo. `selection_clipboard_qt` also checks mixed-object
round trips and reading legacy CTS8 data into the current CTS9 format.
`window_chrome_qt` covers the borderless window chrome offscreen (band
geometry under the native frame inset, the maximize/restore glyph, the
auto-hide band state machine and the caption-button intents); its native half
(`WM_NCCALCSIZE`/`WM_NCHITTEST`) needs a real desktop and is verified there.
`canvas_geometry_dpr1_qt` and `canvas_geometry_dpr175_qt` run the same binary
offscreen at `QT_SCALE_FACTOR` 1.0 and 1.75, asserting widget/page mapping,
committed stroke page coordinates, preview-vs-committed pixels and the
zoom-session snapshot scale at zooms 0.5/1.0/2.0.  `input_feel_qt` and
`input_feel_dpr175_qt` run the same binary at both scale factors for the
precision-scroll router and the configured draw cursor; and
`quick_popup_placement_qt`/`quick_popup_placement_dpr175_qt` drive the real
window offscreen to assert the Ctrl+W quick-size popup anchors at the cursor
(with the reference's out-of-canvas fallback and edge clamping) at both scale
factors.

## Windows precision-touchpad header

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

The application installs and packages through Qt's CMake deploy API
(`qt_generate_deploy_app_script` + CPack); `cnote` is the only product
target.

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
Windows ZIP contains 13 files under `bin/` (`cnote.exe`, `qt.conf`,
`Qt6Core/Gui/Widgets/Network/Svg.dll`, `opengl32sw.dll`, `D3Dcompiler_47.dll`,
`pdfium.dll`, the MinGW runtime DLLs), a 36-file `latex-res` tree, 11 plugin
files under 7 plugin directories, 33 translation files and 23 license files.
The verified Linux TGZ contains `bin/cnote`, `bin/qt.conf`, the same 36-file
`latex-res` tree, 33 libraries under `lib/`, 39 plugin files under 13 plugin
directories, 32 translation files and 23 license files.

Every package stages `share/licenses/cnote/` with `THIRD-PARTY-NOTICES.md`
and the license texts of the redistributed components: MicroTeX (MIT plus the
font licenses), tinyxml2 (zlib), PDFium (BSD-3-Clause plus its bundled
licenses) and the MinGW-w64 public-domain disclaimer. The MicroTeX font
licenses also ride inside `latex-res/fonts/licences/`. The Qt LGPL v3 text is
taken from the deployed Qt installation when it ships one; the kits used here
do not, so it must be supplied before publishing (see
`THIRD-PARTY-NOTICES.md`, "Open prerequisites").

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

From the build tree (Qt application) - the build stages Qt's platform,
image-format and style plugins beside the executable, so no environment
variables are needed:

```powershell
.\build\cnote.exe path\to\document.pdf
```

```bash
./build/cnote path/to/document.pdf
```

From a package, run `bin/cnote.exe` (Windows) or extract the TGZ anywhere and
run `bin/cnote` - resources and plugins are resolved relative to the
executable, so the working directory does not matter and no environment
variables are needed.

The Windows executable is a GUI-subsystem application, so a double-click from
Explorer or the Start menu opens the window without a console window, and
launching it from a shell returns the prompt immediately.  `--self-check`
diagnostics still reach stdout/stderr whenever those handles are redirected
(a shell or `tools/verify-package.cmake`), and the process attaches to a
parent console when one exists without ever allocating a new one.  DPI
awareness is deliberately not declared in `src/cnote.manifest`: Qt applies its
default PerMonitorV2 context at startup.

## Release status

Verified locally (Windows MinGW 13.1 and WSL Ubuntu 24.04): configure, build,
the full CTest set (28 tests on Windows, 27 on Linux), `cmake --install`,
CPack ZIP/TGZ, and `tools/verify-package.cmake` (25/25 checks on the ZIP,
24/24 on the TGZ, including launching the packaged application headless).

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
- **No project-level license** - the repository has no `LICENSE`/`COPYING`
  for cnote itself; the product's own license is undecided.

## Notes

- `.cnote` is the native project format for editable work; v14 files written
  by the former Win32 application stay readable, and new files remain v14 so
  they stay readable by it.
- Auto-save applies to saved `.cnote` documents.
- The product UI is Qt 6 Widgets (Core/Gui/Widgets, dynamically linked). The
  Win32/GDI/GDI+ frontend is deleted; the only remaining raw Win32 code is
  the precision-touchpad adapter, which loads `ninput.dll` dynamically.
