# Third-party notices

This file inventories every third-party component the cnote build uses or
distributes, its license, where the license text lives (or does not), and
whether it actually ships in the packaged artifacts. It is the reference for the
distribution review requested by the migration plan (step 7.6).

Nothing here is legal advice, and **compliance is not claimed**: the Qt
distribution obligations are an explicit open prerequisite at the end of this
document.

## Components

### MicroTeX (`third_party/microtex`)

- Revision: vendored from `NanoMichael/MicroTeX` at `0e3707f6`, with the Qt
  drawing backend restored at `third_party/microtex/src/platform/qt/graphic_qt.cpp`.
- License: MIT (`third_party/microtex/LICENSE`).
- Shipped: statically linked into the application (no separate artifact), and its
  runtime resource tree ships as `latex-res/` beside the executable
  (`Contents/Resources/latex-res` inside a macOS bundle) through
  `install(DIRECTORY third_party/microtex/res/ ...)`.
- Notice status: the MIT text is staged into the packages at
  `share/licenses/cnote/microtex/LICENSE`.

### MicroTeX resources and fonts (`third_party/microtex/res`)

- `res/fonts` ships inside `latex-res/fonts`, including its license texts:
  `Knuth_License.txt` (Computer Modern / TeX fonts), `License_for_dsrom.txt`,
  `OFL.txt` (SIL Open Font License fonts), shipped inside `latex-res/fonts`.
  The packages additionally carry this notice and the component license texts
  under `share/licenses/cnote/` (see the install rules and the packaging
  verification). Verified by listing the produced artifacts and by
  tools/verify-package.cmake, which fails when any of those files is missing.
- `res/greek` and `res/cyrillic` were **stripped because they carry GPLv3
  licenses** and are absent from the tree. Verified absent:
  `third_party/microtex/res/` contains no `greek/` or `cyrillic/` directory,
  and nothing in `CMakeLists.txt` or `tools/` stages them. The upstream
  MicroTeX sources still register those packages
  (`third_party/microtex/src/fonts/alphabet.cpp` references `greek` and
  `cyrillic`), so a formula that needs them fails to resolve its resource at
  runtime, which is the pre-existing cnote behaviour rather than a regression.
  No GPLv3 font resource is redistributed.

### tinyxml2 (`third_party/tinyxml2`)

- Version: 10.0.0 (`third_party/tinyxml2/tinyxml2.cpp`).
- License: zlib (`third_party/tinyxml2/LICENSE.txt`).
- Shipped: statically linked into MicroTeX's XML parsers; no separate
  artifact. The zlib license text is staged into the packages at
  `share/licenses/cnote/tinyxml2/LICENSE.txt`.

### mingw-w64 `interactioncontext.h` (`third_party/mingw-w64`)

- Revision: unmodified public-domain header from `mingw-w64/mingw-w64` commit
  `058bc941cb6b3a93fd00783827841536bc45ce58` (SHA-256
  `25f728091bc5baf7cdba2f86dc110a5649fdf59ccc3402fe2e2c3f13f7f3c252`).
- License: public domain, "no copyright assigned"
  (`third_party/mingw-w64/DISCLAIMER.PD`; the same statement heads
  `include/interactioncontext.h`).
- Shipped: compiled into the Windows precision-touchpad adapter only. No
  Microsoft or mingw-w64 binary is redistributed; `ninput.dll` APIs are resolved
  dynamically at runtime.

### MinGW-w64 compiler runtime (Windows packages only)

- Artifacts shipped beside the executable by the Qt deploy step:
  `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`.
- License: GCC Runtime Library Exception (GPLv3 + exception) for libgcc /
  libstdc++, MIT-style for libwinpthread.
- Notice status: no runtime license text is staged into the Windows package.

### PDFium 148.0.7749.0 (`third_party/pdfium`)

- Source: `bblanchon/pdfium-binaries` release tag `chromium/7749`, matching the
  revision pinned by `cmake/Pdfium.cmake` (`CNOTE_PDFIUM_REQUIRED_VERSION
  "148.0.7749.0"`).
- Licenses in the tree: the SDK packaging license
  (`third_party/pdfium/LICENSE`, MIT, © Benoit Blanchon), the PDFium license
  (`third_party/pdfium/licenses/pdfium.txt`, BSD-3-Clause) and the bundled
  third-party notices in `third_party/pdfium/licenses/`: `abseil.txt`,
  `agg23.txt`, `fast_float.txt`, `freetype.txt`, `icu.txt`, `lcms.txt`,
  `libjpeg_turbo.ijg`, `libjpeg_turbo.md`, `libopenjpeg.txt`, `libpng.txt`,
  `libtiff.txt`, `llvm-libc.txt`, `simdutf.txt`, `zlib.txt`.
- Shipped: `pdfium.dll` (Windows) / `libpdfium.so` (Linux) /
  `libpdfium.dylib` (macOS) beside or under the application.
- Notice status: the SDK packaging license and the bundled third-party
  license texts are staged into the packages under
  `share/licenses/cnote/pdfium/`.

### Qt 6.11.2 (Core, Gui, Widgets and the deployed plugins)

- Source: the official Qt 6.11.2 desktop packages (external dependency, not
  vendored in this tree; the migration build requires exactly 6.11.2).
- License route: **dynamically linked LGPL v3** is the selected distribution
  route.
- Shipped: the Qt libraries, platform/image/icon/tls/wayland/xcb plugins,
  translations and the generated `qt.conf`, deployed by
  `qt_generate_deploy_app_script` (windeployqt on Windows, the Qt deploy tool on
  Linux/macOS). On Windows the deployed set includes `Qt6Core.dll`,
  `Qt6Gui.dll`, `Qt6Widgets.dll`, `platforms/qwindows.dll`,
  `imageformats/qico.dll` and the rest of Qt's default plugin set.
- Notice status: no Qt license text is staged into the packages, and the LGPL
  obligations (license notice, the user's ability to relink the application
  against a modified/replacement Qt, and a source offer where required) are
  **pending an actual legal/distribution review**. See the open prerequisites.

### Project artwork (`assets`)

- `assets/cnote.ico`, `assets/icons/*.svg` (toolbar/tool icons) are embedded
  through the resource system (`qt_add_resources` in `CMakeLists.txt`) and the
  same `.ico` supplies the Windows resource and the macOS bundle icon.
- License: project artwork. There is **no license file or provenance record in
  the tree**; authorship is not documented (reported below as an open item).

### Generated tooling

- `tools/verify-package.cmake`, `tools/make-icns.py`,
  `tools/fixtures/blank.cnote`: project-owned scripts and test data, no
  third-party license involved.

### XPRESS / cabinet compression codec

- `src/document/XpressCodec.cpp` implements the Windows Compression API cabinet
  framing plus the MS-XCA XPRESS-Huffman algorithm directly, using only the C++
  standard library (`<algorithm>`, `<array>`, `<bit>`, `<cstddef>`, `<limits>`,
  `<new>`, `<queue>`, `<stdexcept>`, `<utility>`).
- No GPL-only compressor is used: there is no dependency on `ms-compress`,
  `libwim` or any other third-party compression library anywhere in `src/`,
  `cmake/` or `third_party/` (verified by search). Portable compression is
  cnote's own implementation, so no copyleft obligation is introduced.

## Open prerequisites (not compliance)

1. **Qt LGPL v3 distribution review.** Dynamic linking is the selected route,
   but the required steps have not been performed or verified: shipping the
   LGPL v3 text and Qt's own notices, ensuring the user can replace/relink the
   Qt libraries (the packaged application links them dynamically and the
   libraries are shipped as separate files, which is necessary but not by itself
   sufficient evidence), and offering Qt source where the license requires it.
   This must be signed off by the distribution/legal review before any public
   release; nothing in this document claims that review has happened.
2. **Qt LGPL v3 text is not present in the deployed Qt installation.** The
   install rules now stage `share/licenses/cnote/` with this notice and the
   MicroTeX (MIT) plus its font licenses, tinyxml2 (zlib), PDFium (BSD-3-Clause)
   plus its bundled licenses, and the MinGW-w64 public-domain disclaimer. The
   LGPL v3 text is taken from the Qt installation that is being deployed
   (`LICENSE.LGPL3`, `LICENSE.LGPLv3` or `LICENSES/LGPL-3.0-only.txt`); neither
   the Qt 6.11.2 MinGW kit nor the Linux kit used here ships any of those files,
   so the packaged notice set currently lacks the LGPL text and the build prints
   a configure-time warning. Supply the file from the Qt source distribution
   before publishing an artifact.
3. **MinGW runtime DLLs.** The Windows packages ship `libgcc_s_seh-1.dll`,
   `libstdc++-6.dll` and `libwinpthread-1.dll` from the Qt/MinGW toolchain.
   Those are covered by the GCC Runtime Library Exception (GPLv3 with the
   runtime exception), not by a separate license file staged here; the
   corresponding source offer is the toolchain's own, outside this repository.
4. **No root project license.** The repository has no `LICENSE`/`COPYING` file
   for cnote itself; the product's own license is undecided.
5. **Artwork provenance.** `assets/cnote.ico` and `assets/icons/*.svg` have no
   authorship/license record in the tree.
6. **macOS distribution.** The bundle, DMG, signing and notarization paths are
   configured but unverified; macOS artifacts are not part of the currently
   verified release set.
7. **Greek/Cyrillic MicroTeX packages** remain unavailable at runtime because
   their GPLv3 resource directories stay stripped; restoring them would require
   resolving their license first.
