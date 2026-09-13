cmake_minimum_required(VERSION 3.25)

project(cnote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pdfium.cmake")

find_package(Qt6 6.11.2 REQUIRED COMPONENTS Core)


add_library(cnote_model STATIC
    src/Document.cpp
    src/document/HistoryReducer.cpp
)
target_include_directories(cnote_model PUBLIC src)
target_compile_features(cnote_model PUBLIC cxx_std_20)
target_compile_options(cnote_model PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)


# Vendored tinyxml2 10.0.0 (zlib license), required by MicroTeX's XML parsers.
add_library(tinyxml2 STATIC third_party/tinyxml2/tinyxml2.cpp)
target_include_directories(tinyxml2 PUBLIC third_party/tinyxml2)

# Vendored MicroTeX (NanoMichael/MicroTeX @ 0e3707f6, MIT).
# res/greek and res/cyrillic were stripped: they carry GPLv3 licenses and only
# back lazy-loaded text-mode Greek/Cyrillic alphabets; math-mode symbols come
# from the compiled-in Computer Modern metrics below.
# Source list mirrors upstream CMakeLists.txt (their target also builds a
# sample exe and requires pkg-config; we want neither).
#
# The engine sources are shared by the legacy Windows GDI target (`microtex`,
# Windows only) and the Qt target (`microtex_qt`, all platforms).  The two
# platform backends must never be compiled into one target (plan step 1.5).
set(CNOTE_MICROTEX_ENGINE_SOURCES
    third_party/microtex/src/atom/atom_basic.cpp
    third_party/microtex/src/atom/atom_char.cpp
    third_party/microtex/src/atom/atom_impl.cpp
    third_party/microtex/src/atom/atom_matrix.cpp
    third_party/microtex/src/atom/atom_row.cpp
    third_party/microtex/src/atom/atom_space.cpp
    third_party/microtex/src/atom/colors_def.cpp
    third_party/microtex/src/atom/unit_conversion.cpp
    third_party/microtex/src/box/box.cpp
    third_party/microtex/src/box/box_factory.cpp
    third_party/microtex/src/box/box_group.cpp
    third_party/microtex/src/box/box_single.cpp
    third_party/microtex/src/core/core.cpp
    third_party/microtex/src/core/formula.cpp
    third_party/microtex/src/core/formula_def.cpp
    third_party/microtex/src/core/glue.cpp
    third_party/microtex/src/core/localized_num.cpp
    third_party/microtex/src/core/macro.cpp
    third_party/microtex/src/core/macro_def.cpp
    third_party/microtex/src/core/macro_impl.cpp
    third_party/microtex/src/core/parser.cpp
    third_party/microtex/src/fonts/alphabet.cpp
    third_party/microtex/src/fonts/font_basic.cpp
    third_party/microtex/src/fonts/font_info.cpp
    third_party/microtex/src/fonts/fonts.cpp
    third_party/microtex/src/utils/string_utils.cpp
    third_party/microtex/src/utils/utf.cpp
    third_party/microtex/src/utils/utils.cpp
    third_party/microtex/src/res/builtin/formula_mappings.res.cpp
    third_party/microtex/src/res/builtin/symbol_mapping.res.cpp
    third_party/microtex/src/res/builtin/tex_param.res.cpp
    third_party/microtex/src/res/builtin/tex_symbols.res.cpp
    third_party/microtex/src/res/font/bi10.def.cpp
    third_party/microtex/src/res/font/bx10.def.cpp
    third_party/microtex/src/res/font/cmbsy10.def.cpp
    third_party/microtex/src/res/font/cmbx10.def.cpp
    third_party/microtex/src/res/font/cmbxti10.def.cpp
    third_party/microtex/src/res/font/cmex10.def.cpp
    third_party/microtex/src/res/font/cmmi10.def.cpp
    third_party/microtex/src/res/font/cmmi10_unchanged.def.cpp
    third_party/microtex/src/res/font/cmmib10.def.cpp
    third_party/microtex/src/res/font/cmmib10_unchanged.def.cpp
    third_party/microtex/src/res/font/cmr10.def.cpp
    third_party/microtex/src/res/font/cmss10.def.cpp
    third_party/microtex/src/res/font/cmssbx10.def.cpp
    third_party/microtex/src/res/font/cmssi10.def.cpp
    third_party/microtex/src/res/font/cmsy10.def.cpp
    third_party/microtex/src/res/font/cmti10.def.cpp
    third_party/microtex/src/res/font/cmti10_unchanged.def.cpp
    third_party/microtex/src/res/font/cmtt10.def.cpp
    third_party/microtex/src/res/font/dsrom10.def.cpp
    third_party/microtex/src/res/font/eufb10.def.cpp
    third_party/microtex/src/res/font/eufm10.def.cpp
    third_party/microtex/src/res/font/i10.def.cpp
    third_party/microtex/src/res/font/moustache.def.cpp
    third_party/microtex/src/res/font/msam10.def.cpp
    third_party/microtex/src/res/font/msbm10.def.cpp
    third_party/microtex/src/res/font/r10.def.cpp
    third_party/microtex/src/res/font/r10_unchanged.def.cpp
    third_party/microtex/src/res/font/rsfs10.def.cpp
    third_party/microtex/src/res/font/sb10.def.cpp
    third_party/microtex/src/res/font/sbi10.def.cpp
    third_party/microtex/src/res/font/si10.def.cpp
    third_party/microtex/src/res/font/special.def.cpp
    third_party/microtex/src/res/font/ss10.def.cpp
    third_party/microtex/src/res/font/stmary10.def.cpp
    third_party/microtex/src/res/font/tt10.def.cpp
    third_party/microtex/src/res/parser/font_parser.cpp
    third_party/microtex/src/res/parser/formula_parser.cpp
    third_party/microtex/src/res/reg/builtin_font_reg.cpp
    third_party/microtex/src/res/reg/builtin_syms_reg.cpp
    third_party/microtex/src/res/sym/amsfonts.def.cpp
    third_party/microtex/src/res/sym/amssymb.def.cpp
    third_party/microtex/src/res/sym/base.def.cpp
    third_party/microtex/src/res/sym/stmaryrd.def.cpp
    third_party/microtex/src/res/sym/symspecial.def.cpp
    third_party/microtex/src/latex.cpp
    third_party/microtex/src/render.cpp
)




enable_testing()


# --- Qt MicroTeX backend (migration plan step 3.8) ----------------------------
# `microtex_qt` carries the MicroTeX engine sources plus the Qt platform
# adapter; the GDI backend was removed at cutover (plan step 7.1).
#
# third_party/microtex/src/platform/qt/graphic_qt.{h,cpp} are vendored from
# NanoMichael/MicroTeX @ 0e3707f6 (MIT; third_party/microtex/LICENSE) with local
# cnote changes recorded in the file headers.
#
# Resource notes (unchanged from the legacy target): tinyxml2 is retained, the
# MicroTeX res/ tree - including its `.clatexmath-res_root` marker - is staged as
# real files next to the executable, and the GPLv3 res/greek + res/cyrillic
# directories stay stripped.
find_package(Qt6 6.11.2 REQUIRED COMPONENTS Gui)

add_library(microtex_qt STATIC
    ${CNOTE_MICROTEX_ENGINE_SOURCES}
    third_party/microtex/src/platform/qt/graphic_qt.cpp
)
target_include_directories(microtex_qt PUBLIC third_party/microtex/src)
target_compile_definitions(microtex_qt PUBLIC BUILD_QT)
# _HAS_STD_BYTE=0 (an MSVC workaround for MicroTeX's `using namespace std;`
# headers) is not needed here: this target's Qt adapter TU includes Qt headers,
# which require std::byte.
target_link_libraries(microtex_qt PUBLIC tinyxml2 Qt6::Gui)

# Portable MicroTeX core plus the Qt QPainter drawing backend
# (cnote::latex::Draw(FormulaHandle, QPainter&, ...)).
add_library(cnote_latex_qt STATIC
    src/latex/LatexEngine.cpp
    src/latex/LatexEngineQt.cpp
)
target_include_directories(cnote_latex_qt PUBLIC src)
target_compile_features(cnote_latex_qt PUBLIC cxx_std_20)
target_compile_definitions(cnote_latex_qt PUBLIC CNOTE_LATEX_QT_BACKEND)
target_link_libraries(cnote_latex_qt PUBLIC cnote_model microtex_qt Qt6::Gui)

# --- Qt canvas services (migration plan steps 3.1/3.3/3.6) --------------------
# Portable rendering layer: QTextDocument/QTextLayout text layout and the
# QPainter/QImage committed-object page renderer. No widgets, no App/HWND/HDC.
add_library(cnote_render_qt STATIC
    src/text/TextLayoutQt.cpp
    src/text/TextEditingQt.cpp
    src/latex/LatexCompletion.cpp
    src/render/PageRenderer.cpp
    src/selection/StrokeSmoothing.cpp
)
target_include_directories(cnote_render_qt PUBLIC src)
target_compile_features(cnote_render_qt PUBLIC cxx_std_20)
target_link_libraries(cnote_render_qt PUBLIC cnote_model cnote_latex_qt Qt6::Gui)

# Qt PDF export: the portable PDFium core plus the Qt overlay composition.
add_library(cnote_pdf_qt STATIC
    src/PdfBackend.cpp
    src/render/PdfExportQt.cpp
    src/render/PdfiumBackgroundProvider.cpp
)
target_include_directories(cnote_pdf_qt PUBLIC src)
target_compile_features(cnote_pdf_qt PUBLIC cxx_std_20)
target_link_libraries(cnote_pdf_qt PUBLIC cnote_render_qt pdfium)

# Offscreen Qt page-renderer test: layer order, transparent text backgrounds,
# span application, cache reuse without re-layout, model stability.
add_executable(cnote_page_renderer_tests tests/PageRendererTests.cpp)
target_compile_definitions(cnote_page_renderer_tests PRIVATE
    CNOTE_LATEX_RES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res")
target_link_libraries(cnote_page_renderer_tests PRIVATE cnote_render_qt)
add_test(NAME render_qt_offscreen COMMAND cnote_page_renderer_tests)
get_target_property(_cnote_qmake Qt6::qmake IMPORTED_LOCATION)
execute_process(
    COMMAND "${_cnote_qmake}" -query QT_INSTALL_PLUGINS
    OUTPUT_VARIABLE _cnote_plugins
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set_tests_properties(render_qt_offscreen PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# Self-contained Qt PDF-export contract test (no external fixtures).
add_executable(cnote_pdf_export_tests
    tests/PdfExportTests.cpp
    src/ProjectFile.cpp
    src/document/LoadDocumentFile.cpp
    src/document/XpressCodec.cpp
    src/Unicode.cpp
)
target_compile_definitions(cnote_pdf_export_tests PRIVATE
    CNOTE_LATEX_RES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res")
target_link_libraries(cnote_pdf_export_tests PRIVATE cnote_pdf_qt Qt6::Core)
add_test(NAME pdf_export_qt COMMAND cnote_pdf_export_tests)
set_tests_properties(pdf_export_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# Self-contained Qt editing-services test (offsets, formatting, LaTeX editing).
add_executable(cnote_text_editing_tests
    tests/TextEditingTests.cpp
)
target_compile_definitions(cnote_text_editing_tests PRIVATE
    CNOTE_LATEX_RES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res")
target_link_libraries(cnote_text_editing_tests PRIVATE cnote_render_qt)
add_test(NAME text_editing_qt COMMAND cnote_text_editing_tests)
set_tests_properties(text_editing_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# Self-contained lazy PDF-background provider test.
add_executable(cnote_pdf_background_tests
    tests/PdfBackgroundTests.cpp
)
target_link_libraries(cnote_pdf_background_tests PRIVATE cnote_pdf_qt)
add_test(NAME pdf_background_qt COMMAND cnote_pdf_background_tests)
set_tests_properties(pdf_background_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# Self-contained Qt session-lifetime / autosave manager test.
add_executable(cnote_session_manager_tests
    tests/SessionManagerTests.cpp
    src/shell/AppManager.cpp
    src/Unicode.cpp
)
target_link_libraries(cnote_session_manager_tests PRIVATE cnote_model cnote_latex_qt Qt6::Core)
add_test(NAME session_manager_qt COMMAND cnote_session_manager_tests)

# --- Qt canvas and runnable migration frontend (plan steps 3.1/3.2/3.3/3.9) ---
# CanvasWidget owns per-view viewport/transient state and its raster caches and
# delegates all committed-object drawing to cnote_render_qt; the controller owns
# the shared ProjectSession (document + undo/redo) behind ui::CanvasController.
# No App/HWND/HDC and no second rendering or text path.
find_package(Qt6 6.11.2 REQUIRED COMPONENTS Widgets)

add_library(cnote_ui STATIC
    src/ui/CanvasWidget.cpp
    src/qt/QtDocumentController.cpp
    src/qt/QtDocumentIo.cpp
    src/qt/QtMainWindow.cpp
    # Portable project decode used by the Qt file service.
    src/ProjectFile.cpp
    src/document/XpressCodec.cpp
    src/document/LoadDocumentFile.cpp
    src/Unicode.cpp
    # Shared session/view registry and autosave timers (Qt Core only).
    src/shell/AppManager.cpp
    # New-page draft/defaults model used by the window's page commands (the
    # dialogs that also use it are separate targets).
    src/ui/PageDefaultsQt.cpp
    # Ported chrome widgets instantiated by the window (plan 6.1-6.3).
    src/ui/ToolbarWidget.cpp
    src/ui/SidebarWidget.cpp
    src/ui/ChromeAssetsQt.cpp
    src/settings/KeyBindingQt.cpp
    # Clipboard flavors for the selection commands (Ctrl+C/X/V).
    src/document/SelectionClipboardQt.cpp
    src/document/SelectionClipboardCodec.cpp
    # Settings and new-page dialogs the window owns (modeless, live model).
    src/ui/SettingsDialogQt.cpp
    src/ui/SettingsFormatQt.cpp
    src/ui/NewPageDialogQt.cpp
)
set_target_properties(cnote_ui PROPERTIES AUTOMOC ON)
target_include_directories(cnote_ui PUBLIC src)
target_compile_features(cnote_ui PUBLIC cxx_std_20)
target_compile_options(cnote_ui PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
# cnote_pdf_qt provides PdfiumBackend plus the portable file-loader service.
target_link_libraries(cnote_ui PUBLIC cnote_model cnote_render_qt cnote_pdf_qt Qt6::Widgets)

add_executable(cnote src/main.cpp)
target_link_libraries(cnote PRIVATE cnote_ui)

# MicroTeX's font/XML resources ship as real files next to the executable.
add_custom_command(TARGET cnote POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res"
        "$<TARGET_FILE_DIR:cnote>/latex-res"
)
if(WIN32)
    add_custom_command(TARGET cnote POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:pdfium>"
            "$<TARGET_FILE_DIR:cnote>/pdfium.dll"
    )
endif()

# --- Qt settings store and key bindings (migration plan steps 6.4/6.5) --------
# Portable settings storage: same keys, defaults, clamping, saved-palette
# semantics, zoom presets and recent files as src/AppSettings.cpp; the Windows
# %APPDATA%/cnote/settings.ini rule plus the working-directory fallback are kept
# and non-Windows resolves QStandardPaths::AppConfigLocation for "cnote".
# Persistence goes through QSaveFile and never touches document state.
add_library(cnote_settings_qt STATIC
    src/settings/SettingsStoreQt.cpp
    src/settings/KeyBindingQt.cpp
    src/Unicode.cpp
)
target_include_directories(cnote_settings_qt PUBLIC src)
target_compile_features(cnote_settings_qt PUBLIC cxx_std_20)
target_compile_options(cnote_settings_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_settings_qt PUBLIC Qt6::Gui)

# Self-contained contract test: legacy-record -> QKeySequence -> PortableText
# round trips for every capturable action, malformed/unknown-record fallback to
# the action default, real payload parsing, Windows/XDG path resolution and
# failed-persist behavior.
add_executable(cnote_settings_tests tests/SettingsStoreQtTests.cpp)
target_compile_options(cnote_settings_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_settings_tests PRIVATE cnote_settings_qt)
add_test(NAME settings_qt COMMAND cnote_settings_tests)
set_tests_properties(settings_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# --- Qt clipboard bridge (migration plan step 2.6) ----------------------------
# Platform-neutral CTS selection codec plus the QClipboard bridge: structured
# selections travel as application/x-cnote-selection and, on Windows, through
# Qt's Windows MIME bridge under the legacy registered format
# cnote.selection.v1.  Ordinary image data and plain text are the paste
# fallbacks in that order.  Qt Gui only - no widgets, no App/HWND/HDC - and
# nothing here mutates a document.
add_library(cnote_clipboard_qt STATIC
    src/document/SelectionClipboardCodec.cpp
    src/document/SelectionClipboardQt.cpp
)
target_include_directories(cnote_clipboard_qt PUBLIC src)
target_compile_features(cnote_clipboard_qt PUBLIC cxx_std_20)
target_compile_options(cnote_clipboard_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_clipboard_qt PUBLIC cnote_model Qt6::Gui)

# Self-contained contract test over the real OS clipboard: CTS round trip,
# structured > image > text priority, invalid/empty payloads and - on Windows -
# the registered-format bridge in both directions.
add_executable(cnote_selection_clipboard_tests tests/SelectionClipboardQtTests.cpp)
target_compile_options(cnote_selection_clipboard_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_selection_clipboard_tests PRIVATE cnote_clipboard_qt)
add_test(NAME selection_clipboard_qt COMMAND cnote_selection_clipboard_tests)
# The Windows bridge evidence needs the real (non-offscreen) platform; the
# other platforms run this service test on the offscreen platform like the
# rest of the Qt suite.
if(WIN32)
    set_tests_properties(selection_clipboard_qt PROPERTIES
        ENVIRONMENT "QT_PLUGIN_PATH=${_cnote_plugins}")
else()
    set_tests_properties(selection_clipboard_qt PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")
endif()

# --- Qt layout consolidation (migration plan step 6.1) ------------------------
# Qt logical-pixel port of the reference chrome geometry (toolbar, toolbar
# title/zoom, sidebar metrics and header, quick-size and text-format popups,
# palette menu/wheel, toolbar auto-hide).  Qt Gui only - no widgets, no
# App/HWND/HDC.  Every constant in src/ui/LayoutQt.cpp
# records the call path it came from; the duplicate appui/App.cpp definitions
# and their single disagreement (palette-wheel hover grow) are resolved in the
# header comment.
add_library(cnote_layout_qt STATIC
    src/ui/LayoutQt.cpp
)
target_include_directories(cnote_layout_qt PUBLIC src)
target_compile_features(cnote_layout_qt PUBLIC cxx_std_20)
target_compile_options(cnote_layout_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_layout_qt PUBLIC Qt6::Gui)

# The Qt shell is the product consumer of the consolidated layout.
if(TARGET cnote_ui)
    target_link_libraries(cnote_ui PUBLIC cnote_layout_qt)
endif()

# Layout parity evidence (plan step 6.1): one input matrix (client sizes
# including compact widths, palette sizes, maximized states, tool variants,
# title/zoom text lengths, popup-edge clamping, auto-hide boundaries) drives
# cnote::appui and cnote::appui::qt and asserts matching rectangles - exactly
# for geometry and within the stated text-metric tolerance for title/zoom.
# Windows additionally runs the live legacy comparison; every platform compares
# against the frozen reference table captured from that legacy path (the
# executable's own --emit-reference mode verifies the table on Windows).
add_executable(cnote_layout_parity_tests tests/LayoutParityTests.cpp)
target_include_directories(cnote_layout_parity_tests PRIVATE src)
target_compile_features(cnote_layout_parity_tests PRIVATE cxx_std_20)
# The harness must run with or without an externally set platform plugin path:
# a missing Qt platform plugin makes QGuiApplication pop an invisible modal
# message box on Windows and hang forever (observed under a stale CTest file).
target_compile_definitions(cnote_layout_parity_tests PRIVATE
    CNOTE_QT_PLUGIN_PATH="${_cnote_plugins}"
)
target_compile_options(cnote_layout_parity_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_layout_parity_tests PRIVATE cnote_layout_qt Qt6::Gui)
add_test(NAME layout_parity_qt COMMAND cnote_layout_parity_tests)
# Windows needs the native platform: the offscreen plugin's font database
# returns placeholder metrics there, while the harness compares real text
# advances.  Other platforms only need geometry and run offscreen.
if(WIN32)
    set_tests_properties(layout_parity_qt PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_PLUGIN_PATH=${_cnote_plugins}")
else()
    set_tests_properties(layout_parity_qt PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")
endif()

# --- Qt packaging: installed resources, deployment and CPack ------------------
# Migration plan steps 7.3/7.4.
#
# The Qt application is the only product target; it is installed and packaged.
#
# Layout produced by `cmake --install <prefix>` and by CPack:
#   Windows: <prefix>/bin/cnote.exe, Qt DLLs, the MinGW compiler runtime and
#            platform/image plugins under <prefix>/plugins, pdfium.dll and
#            latex-res/ beside the executable (windeployqt layout).
#   Linux:   <prefix>/bin/cnote + bin/latex-res, <prefix>/lib/libQt6*.so and
#            libpdfium.so, <prefix>/plugins/..., <prefix>/bin/qt.conf.  RPATHs
#            are $ORIGIN-relative, so the tree can be extracted anywhere.
#   macOS:   <prefix>/cnote.app with Contents/Resources/latex-res and
#            Contents/Frameworks/libpdfium.dylib.  Present but UNVERIFIED: there
#            is no macOS machine in this environment (see the packaging report).
#
# `include(CPack)` must remain the last statement of this project: it captures
# the install rules above.
if(TARGET cnote)
    include(GNUInstallDirs)

    # -- installed-location resource lookup and packaged application icon -----
    # The helper resolves latex-res from the installed application directory
    # (Contents/Resources inside a macOS bundle) instead of the current working
    # directory; main.cpp calls it, and tests may link it too.
    add_library(cnote_platform_qt STATIC
        src/platform/AppResourcePaths.cpp
    )
    target_include_directories(cnote_platform_qt PUBLIC src)
    target_compile_features(cnote_platform_qt PUBLIC cxx_std_20)
    target_compile_options(cnote_platform_qt PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(cnote_platform_qt PUBLIC Qt6::Gui)
    # The same artwork as the Windows .rc icon and the macOS bundle icon, made
    # available to Qt on every platform.
    qt_add_resources(cnote_platform_qt "cnote_app_icons"
        PREFIX "/cnote"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/cnote.ico"
    )
    target_link_libraries(cnote_ui PUBLIC cnote_platform_qt)

    # The Windows icon resource stays conditional; no other platform compiles it.
    if(WIN32)
        target_sources(cnote PRIVATE src/cnote.rc)
    endif()

    # -- install layout -------------------------------------------------------
    get_target_property(_cnote_output_name cnote OUTPUT_NAME)
    if(NOT _cnote_output_name)
        set(_cnote_output_name "cnote")
    endif()
    set(_cnote_app_bundle "${_cnote_output_name}.app")
    if(APPLE)
        set(_cnote_resource_dest "${_cnote_app_bundle}/Contents/Resources")
    else()
        set(_cnote_resource_dest "${CMAKE_INSTALL_BINDIR}")
    endif()

    install(TARGETS cnote
        BUNDLE DESTINATION .
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    # MicroTeX's XML/TTF resources must remain real files (plan step 3.8): stage
    # them beside the executable, or in Contents/Resources inside a bundle.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res/"
        DESTINATION "${_cnote_resource_dest}/latex-res"
    )

    # PDFium: the exact runtime library the build linked, installed where the
    # installed application loads it without any developer PATH entry.
    if(APPLE)
        install(FILES "$<TARGET_FILE:pdfium>"
            DESTINATION "${_cnote_app_bundle}/Contents/Frameworks")
    elseif(WIN32)
        install(FILES "$<TARGET_FILE:pdfium>" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    else()
        install(FILES "$<TARGET_FILE:pdfium>" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()

    # Relocatable runtime paths: the installed executable finds the deployed
    # libraries relative to itself, never through a build-machine path.
    if(APPLE)
        set_target_properties(cnote PROPERTIES
            INSTALL_RPATH "@executable_path/../Frameworks")
    elseif(UNIX)
        set_target_properties(cnote PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib")
    endif()

    # -- macOS bundle (configured, explicitly unverified) ---------------------
    if(APPLE)
        set_target_properties(cnote PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "cnote"
            # Placeholder reverse-DNS identifier: macOS signing/notarization and
            # the real distribution identifier are release prerequisites that
            # cannot be verified or chosen without a macOS machine.
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.cnote.app"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        )
        # Bundle icon generated from the same assets/cnote.ico artwork.  The
        # generator is only needed on macOS; skipping it (no Python) leaves the
        # bundle without CFBundleIconFile rather than failing the build.
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            set(_cnote_icns "${CMAKE_CURRENT_BINARY_DIR}/cnote.icns")
            add_custom_command(OUTPUT "${_cnote_icns}"
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-icns.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/cnote.ico"
                    "${_cnote_icns}"
                DEPENDS
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/cnote.ico"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-icns.py"
                COMMENT "Generating cnote.icns from assets/cnote.ico"
                VERBATIM
            )
            add_custom_target(cnote_bundle_icon ALL DEPENDS "${_cnote_icns}")
            install(FILES "${_cnote_icns}"
                DESTINATION "${_cnote_app_bundle}/Contents/Resources")
            set_target_properties(cnote PROPERTIES
                MACOSX_BUNDLE_ICON_FILE "cnote.icns")
        endif()
    endif()

    # -- Qt runtime deployment (plan step 7.3) --------------------------------
    # Windows: windeployqt (Qt DLLs, platform/image plugins, MinGW compiler
    # runtime).  Linux/macOS: the Qt deploy tool copies the Qt libraries (and
    # plugins on Linux) and writes a relative qt.conf.  Libraries that cannot be
    # resolved are left to the verification script, which fails loudly.
    #
    # On Linux the offscreen platform plugin ships too: it lets the packaged
    # application (and tools/verify-package.cmake) run the real binary headless
    # on a machine without a display.  Windows' default windeployqt set already
    # carries everything the verifier requires; macOS is handled by macdeployqt.
    set(_cnote_deploy_plugin_args "")
    if(UNIX AND NOT APPLE)
        set(_cnote_deploy_plugin_args INCLUDE_PLUGINS offscreen)
    endif()
    qt_generate_deploy_app_script(
        TARGET cnote
        OUTPUT_SCRIPT _cnote_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
        ${_cnote_deploy_plugin_args}
    )
    install(SCRIPT "${_cnote_deploy_script}")

    # -- third-party notices (plan 7.4/7.6) ------------------------------------
    # The shipped archives carry the notice file plus the license texts of every
    # component that is redistributed: MicroTeX (MIT and its font licenses),
    # tinyxml2 (zlib), PDFium (BSD-3-Clause and its bundled licenses) and the
    # MinGW-w64 public-domain disclaimer (see THIRD-PARTY-NOTICES.md; the MinGW
    # runtime DLLs shipped on Windows are covered by the GCC Runtime Library
    # Exception rather than a separate license file).
    set(_cnote_notice_dir "${CMAKE_INSTALL_DATADIR}/licenses/cnote")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
        DESTINATION "${_cnote_notice_dir}")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/LICENSE"
        DESTINATION "${_cnote_notice_dir}/microtex")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res/fonts/licences/"
        DESTINATION "${_cnote_notice_dir}/microtex/fonts-licences")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyxml2/LICENSE.txt"
        DESTINATION "${_cnote_notice_dir}/tinyxml2")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/LICENSE"
        DESTINATION "${_cnote_notice_dir}/pdfium")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/licenses/"
        DESTINATION "${_cnote_notice_dir}/pdfium/licenses")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mingw-w64/DISCLAIMER.PD"
        DESTINATION "${_cnote_notice_dir}/mingw-w64")

    # The LGPL v3 text is taken from the Qt installation that is being deployed.
    # Qt's binary installers do not always carry it; when it is missing the
    # release must supply LICENSES/LGPL-3.0-only.txt from the Qt source, so this
    # is reported at configure time instead of being silently omitted.
    get_target_property(_cnote_core_location_local Qt6::Core IMPORTED_LOCATION)
    get_filename_component(_cnote_root_local "${_cnote_core_location_local}/../.." ABSOLUTE)
    set(_cnote_lgpl_license "")
    foreach(_cnote_license_candidate
        "${_cnote_root_local}/LICENSE.LGPL3"
        "${_cnote_root_local}/LICENSE.LGPLv3"
        "${_cnote_root_local}/LICENSES/LGPL-3.0-only.txt"
        "${_cnote_root_local}/LICENSES/LGPL-3.0-or-later.txt")
        if(EXISTS "${_cnote_license_candidate}")
            set(_cnote_lgpl_license "${_cnote_license_candidate}")
            break()
        endif()
    endforeach()
    if(_cnote_lgpl_license)
        install(FILES "${_cnote_lgpl_license}"
            DESTINATION "${_cnote_notice_dir}/qt" RENAME "LICENSE.LGPL3")
    else()
        message(WARNING
            "Qt LGPL v3 text not found under ${_cnote_root_local}: the packaged "
            "notices cannot include it. Supply LICENSES/LGPL-3.0-only.txt from the Qt "
            "source distribution before publishing an artifact (see "
            "THIRD-PARTY-NOTICES.md, 'Open prerequisites').")
    endif()

    # -- CPack: Windows x64 ZIP, Linux x64 relocatable TGZ --------------------
    set(CPACK_PACKAGE_NAME "cnote")
    set(CPACK_PACKAGE_VENDOR "cnote")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "cnote note-taking application (Qt ${Qt6_VERSION} build)")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            set(_cnote_package_arch "arm64")
        else()
            set(_cnote_package_arch "x64")
        endif()
    else()
        set(_cnote_package_arch "x86")
    endif()
    # Deterministic artifact name; the archive gets the same name as its
    # top-level directory (CPACK_INCLUDE_TOPLEVEL_DIRECTORY below).
    set(CPACK_PACKAGE_FILE_NAME
        "cnote-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${_cnote_package_arch}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_PACKAGE_CHECKSUM "SHA256")
    if(WIN32)
        set(CPACK_GENERATOR "ZIP")
    elseif(APPLE)
        # Configured but unverified: drag-and-drop DMG for the (unsigned,
        # un-notarized) bundle.  macOS artifacts are not produced here.
        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_VOLUME_NAME "cnote")
    else()
        set(CPACK_GENERATOR "TGZ")
    endif()
    include(CPack)
endif()

# --- Qt settings dialogs (migration plan step 6.2) ----------------------------
# QDialog ports of SettingsWindow and NewPageSettingsWindow driven by the
# portable settings service: same categories, controls, draft/apply semantics
# and palette-remapping policy as the legacy windows.  Widgets only, no
# App/HWND/HDC.
add_library(cnote_dialogs_qt STATIC
    src/ui/SettingsFormatQt.cpp
    src/ui/PageDefaultsQt.cpp
    src/ui/SettingsDialogQt.cpp
    src/ui/NewPageDialogQt.cpp
)
set_target_properties(cnote_dialogs_qt PROPERTIES AUTOMOC ON)
target_include_directories(cnote_dialogs_qt PUBLIC src)
target_compile_features(cnote_dialogs_qt PUBLIC cxx_std_20)
target_compile_options(cnote_dialogs_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_dialogs_qt PUBLIC cnote_model cnote_settings_qt Qt6::Widgets)

# Self-contained offscreen evidence for the settings dialog: control inventory,
# palette/quick-size/brush-preset/zoom clamps, shortcut capture, persistence,
# refresh-on-open, modeless ownership, tab order and grab() screenshots.
add_executable(cnote_settings_dialog_tests tests/SettingsDialogQtTests.cpp)
target_compile_options(cnote_settings_dialog_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_settings_dialog_tests PRIVATE cnote_dialogs_qt)
add_test(NAME settings_dialog_qt COMMAND cnote_settings_dialog_tests)
set_tests_properties(settings_dialog_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# Self-contained offscreen evidence for the new-page dialog: draft clamping and
# presets, swatch/hex/HSL colour edits, profile selection, remap toggle,
# current/all/future apply semantics, recent-colour bookkeeping, reset, refresh
# on open, cancel-discards-draft, tab order and grab() screenshots.
add_executable(cnote_new_page_dialog_tests tests/NewPageDialogQtTests.cpp)
target_compile_options(cnote_new_page_dialog_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_new_page_dialog_tests PRIVATE cnote_dialogs_qt)
add_test(NAME new_page_dialog_qt COMMAND cnote_new_page_dialog_tests)
set_tests_properties(new_page_dialog_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# --- Qt precision input: Windows touchpad and native gestures (plan 5.3/5.4) --
# Portable decision core plus the Qt event filter the canvas installs on itself,
# and the Windows-only QAbstractNativeEventFilter that reuses the touchpad
# context adapter.  Qt Gui/Core only for the portable half; no widgets, no App.
add_library(cnote_precision_input_qt STATIC
    src/ui/PrecisionInputRouter.cpp
    src/ui/PrecisionInputFilterQt.cpp
)
set_target_properties(cnote_precision_input_qt PROPERTIES AUTOMOC ON)
target_include_directories(cnote_precision_input_qt PUBLIC src)
target_compile_features(cnote_precision_input_qt PUBLIC cxx_std_20)
target_compile_options(cnote_precision_input_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_precision_input_qt PUBLIC Qt6::Gui)
if(WIN32)
    target_sources(cnote_precision_input_qt PRIVATE
        src/platform/windows/PrecisionTouchpadQt.cpp
        src/platform/windows/PrecisionTouchpad.cpp
    )
    target_compile_definitions(cnote_precision_input_qt PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
    target_link_libraries(cnote_precision_input_qt PUBLIC user32)
endif()
if(MINGW)
    # The adapter uses touchpad declarations newer than the default MinGW SDK.
    set_source_files_properties(
        src/platform/windows/PrecisionTouchpad.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mingw-w64/include")
    set_source_files_properties(
        src/platform/windows/PrecisionTouchpadQt.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mingw-w64/include")
endif()

# Portable decision-core evidence: routing, consume/pass-through, duplicate
# suppression, availability fallback, inertia transitions and thresholds.
add_executable(cnote_precision_router_tests tests/PrecisionInputRouterTests.cpp)
target_compile_options(cnote_precision_router_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_precision_router_tests PRIVATE cnote_precision_input_qt)
add_test(NAME precision_input_router_qt COMMAND cnote_precision_router_tests)

# Widget-level harness: real QWidget + real Qt events through the installed
# filter, proving ordinary wheel/mouse input is unchanged and that native
# pinch/pan reaches the sink.
add_executable(cnote_precision_filter_tests tests/PrecisionInputFilterTests.cpp)
target_compile_options(cnote_precision_filter_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_precision_filter_tests PRIVATE cnote_precision_input_qt Qt6::Widgets)
add_test(NAME precision_input_filter_qt COMMAND cnote_precision_filter_tests)
set_tests_properties(precision_input_filter_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

if(WIN32)
    # Windows adapter lifecycle, availability fallback and inertia timer wiring.
    add_executable(cnote_precision_touchpad_tests tests/PrecisionTouchpadQtTests.cpp)
    target_compile_options(cnote_precision_touchpad_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(cnote_precision_touchpad_tests PRIVATE cnote_precision_input_qt Qt6::Widgets)
    add_test(NAME precision_touchpad_qt COMMAND cnote_precision_touchpad_tests)
    set_tests_properties(precision_touchpad_qt PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")
    foreach(_cnote_precision_test cnote_precision_filter_tests cnote_precision_touchpad_tests)
        foreach(_cnote_runtime Qt6::Core Qt6::Gui Qt6::Widgets)
            add_custom_command(TARGET ${_cnote_precision_test} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:${_cnote_runtime}>"
                    "$<TARGET_FILE_DIR:${_cnote_precision_test}>"
            )
        endforeach()
    endforeach()
endif()

# The dialog tests need QtWidgets at runtime; stage the Qt runtime next to the
# executables on Windows (the other Qt test targets rely on a manual copy).
if(WIN32)
    foreach(_cnote_dialog_test cnote_settings_dialog_tests cnote_new_page_dialog_tests)
        foreach(_cnote_runtime Qt6::Core Qt6::Gui Qt6::Widgets)
            add_custom_command(TARGET ${_cnote_dialog_test} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:${_cnote_runtime}>"
                    "$<TARGET_FILE_DIR:${_cnote_dialog_test}>"
            )
        endforeach()
    endforeach()
endif()


# --- Qt compact chrome widgets (migration plan steps 6.1/6.2/6.3) -------------
# ToolbarWidget reproduces the reference compact toolbar (34 logical-pixel base
# height, 30 icon buttons, 22 color swatch, palette swatch menu and palette/tool
# wheel, quick-size and text-format popups, 120 ms auto-hide) and SidebarWidget
# reproduces the left-edge PDF sidebar (176 logical pixels, lazily composed page
# thumbnails, bottom header, local wheel scrolling, 150 ms slide).  Both take
# model/settings data and report navigation/command intents as signals; neither
# contains App/HWND/HDC state or command logic.  Reference SVG artwork is shipped
# through Qt resources with a filesystem fallback for out-of-tree harnesses;
# the SVGs are parsed with the ported reference path parser, so the chrome needs
# only Qt Core/Gui/Widgets (plan step 1.3), never the optional Qt SVG module.

add_library(cnote_chrome_qt STATIC
    src/ui/ChromeAssetsQt.cpp
    src/ui/ToolbarWidget.cpp
    src/ui/SidebarWidget.cpp
)
set_target_properties(cnote_chrome_qt PROPERTIES AUTOMOC ON)
target_include_directories(cnote_chrome_qt PUBLIC src)
target_compile_features(cnote_chrome_qt PUBLIC cxx_std_20)
target_compile_options(cnote_chrome_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
qt_add_resources(cnote_chrome_qt "cnote_chrome_assets"
    PREFIX "/cnote"
    BASE "assets"
    FILES
        assets/icons/zoom.svg
        assets/icons/text.svg
        assets/icons/new-page.svg
        assets/icons/pan.svg
        assets/icons/settings.svg
        assets/icons/cursor.svg
        assets/icons/draw.svg
        assets/icons/erase.svg
        assets/icons/lasso.svg
        assets/icons/redo.svg
        assets/icons/undo.svg
        assets/icons/text/bold.svg
        assets/icons/text/italics.svg
        assets/icons/text/underline.svg
        assets/icons/text/left-or.svg
        assets/icons/text/cent-or.svg
        assets/icons/text/right-or.svg
)
target_link_libraries(cnote_chrome_qt PUBLIC
    cnote_layout_qt cnote_settings_qt cnote_render_qt Qt6::Widgets)

# The Qt shell is the product consumer of the chrome widgets.
if(TARGET cnote_ui)
    target_link_libraries(cnote_ui PUBLIC cnote_chrome_qt)
endif()

# Deterministic offscreen contract test (plan step 6.1): toolbar control rects
# and hit regions against cnote::appui::qt for a width matrix including compact
# widths, hover state, palette-wheel color/tool intents, popup-edge clamping,
# sidebar page count/highlight/navigation intent, sidebar-local wheel isolation
# and elapsed-time auto-hide/slide transitions.  Every wait is bounded, so the
# test cannot block the suite.
add_executable(cnote_chrome_tests tests/ChromeWidgetTests.cpp)
target_compile_options(cnote_chrome_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_chrome_tests PRIVATE cnote_chrome_qt cnote_model Qt6::Widgets)
add_test(NAME chrome_qt COMMAND cnote_chrome_tests)
set_tests_properties(chrome_qt PROPERTIES TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# --- Qt interaction router (migration plan steps 5.1-5.3) ---------------------
# Widget-free pointer/tool routing ported from src/shell/MessageLoop.cpp (tool
# routing, drag state, capture) and the App interaction methods it calls.  Qt
# Gui plus the model's Vec2 only: no QWidget, App, HWND or HDC.  The canvas
# adopts it; src/ui/CanvasWidget.cpp stays with the canvas slice.
add_library(cnote_interaction_qt STATIC
    src/ui/InteractionRouter.cpp
)
target_include_directories(cnote_interaction_qt PUBLIC src)
target_compile_features(cnote_interaction_qt PUBLIC cxx_std_20)
target_compile_options(cnote_interaction_qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_interaction_qt PUBLIC cnote_model Qt6::Gui)

# The Qt shell is the consumer; linking the library here is the single
# preparation step the canvas slice needs before it adopts the router.
if(TARGET cnote_ui)
    target_link_libraries(cnote_ui PUBLIC cnote_interaction_qt)
endif()

# Deterministic behavior test over PointerSample sequences: tool routing,
# precedence, modifier changes, page-boundary segmentation, pending-erase
# commit-once, click-versus-drag thresholds and cancel-once.  Plain computation
# over model types - no Qt platform or window is created.
add_executable(cnote_interaction_router_tests tests/InteractionRouterTests.cpp)
target_compile_features(cnote_interaction_router_tests PRIVATE cxx_std_20)
target_compile_options(cnote_interaction_router_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_interaction_router_tests PRIVATE cnote_interaction_qt)
add_test(NAME interaction_router_qt COMMAND cnote_interaction_router_tests)
set_tests_properties(interaction_router_qt PROPERTIES TIMEOUT 120)


# --- Portable selection math (plan steps 5.2/5.3) ----------------------------
# src/selection/Geometry.cpp is bound to App/POINT/RECT and cannot be linked
# into Qt targets, so the Qt canvas uses this portable port.  On Windows the
# test also links the legacy translation unit and compares every case against
# it live; other platforms compare against the frozen table captured from that
# same legacy run.
add_executable(cnote_selection_math_tests
    tests/SelectionMathTests.cpp
)
target_include_directories(cnote_selection_math_tests PRIVATE src)
target_compile_features(cnote_selection_math_tests PRIVATE cxx_std_20)
target_compile_definitions(cnote_selection_math_tests PRIVATE
    CNOTE_LATEX_RES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res")
target_link_libraries(cnote_selection_math_tests PRIVATE cnote_model cnote_latex_qt cnote_render_qt)
add_test(NAME selection_math_qt COMMAND cnote_selection_math_tests)
set_tests_properties(selection_math_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")

# --- Cross-platform portability tests (plan 7.1/7.6, assumptions section) -----
# Path/file-name behavior the Windows build alone cannot catch: case-only path
# differences (same file on Windows, different files on POSIX), non-ASCII
# project names through save/open/export, extension case folding, a destination
# directory created after startup, and the platform settings location rule.
add_executable(cnote_portability_tests
    tests/PortabilityPathTests.cpp
    src/shell/AppManager.cpp
    src/ProjectFile.cpp
    src/document/XpressCodec.cpp
    src/document/LoadDocumentFile.cpp
)
target_include_directories(cnote_portability_tests PRIVATE src)
target_compile_features(cnote_portability_tests PRIVATE cxx_std_20)
target_compile_options(cnote_portability_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_portability_tests PRIVATE cnote_pdf_qt cnote_settings_qt Qt6::Core)
add_test(NAME portability_paths_qt COMMAND cnote_portability_tests)
set_tests_properties(portability_paths_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")
if(WIN32)
    # PDFium is a runtime dependency of the export path; stage it next to the
    # test executable so a clean build directory can run this test directly.
    add_custom_command(TARGET cnote_portability_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:pdfium>"
            "$<TARGET_FILE_DIR:cnote_portability_tests>/pdfium.dll"
    )
endif()

# Font fallback (plan 3.7): none of the reference Windows families is installed
# on Linux, so the style-hint substitution must keep every paragraph, every span
# and a non-empty measured box, and the metrics are printed for comparison.
add_executable(cnote_font_fallback_tests tests/FontFallbackTests.cpp)
target_compile_options(cnote_font_fallback_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_font_fallback_tests PRIVATE cnote_render_qt)
add_test(NAME font_fallback_qt COMMAND cnote_font_fallback_tests)
# Windows runs this on the native platform: the offscreen plugin's font
# database there returns placeholder metrics with no installed families, so it
# could not prove anything about the real font substitution.  Other platforms
# have a real font database offscreen.
if(WIN32)
    set_tests_properties(font_fallback_qt PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_PLUGIN_PATH=${_cnote_plugins}")
else()
    set_tests_properties(font_fallback_qt PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")
endif()

# --- Portable XPRESS codec tests (plan step 2.4) ------------------------------
# tests/XpressCodecTests.cpp existed but was registered nowhere; the codec is
# load-bearing for v14 project compatibility (Windows cabinet buffer framing plus
# MS-XCA XPRESS-Huffman), so it runs as its own CTest.
add_executable(cnote_xpress_codec_tests
    tests/XpressCodecTests.cpp
    src/document/XpressCodec.cpp
)
target_include_directories(cnote_xpress_codec_tests PRIVATE src)
target_compile_features(cnote_xpress_codec_tests PRIVATE cxx_std_20)
target_compile_options(cnote_xpress_codec_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
add_test(NAME xpress_codec COMMAND cnote_xpress_codec_tests)

# --- Persistence and error semantics (migration plan 6.6, 4.5/7.2) -----------
# Drives the real controller, manager, window and settings store: save and PDF
# export failure/replace semantics, recent-document rules, and the close/auto-
# save interplay on the last view.
add_executable(cnote_persistence_tests tests/PersistenceSemanticsTests.cpp)
target_include_directories(cnote_persistence_tests PRIVATE src)
target_compile_features(cnote_persistence_tests PRIVATE cxx_std_20)
target_compile_options(cnote_persistence_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_persistence_tests PRIVATE cnote_ui cnote_settings_qt)
add_test(NAME persistence_semantics_qt COMMAND cnote_persistence_tests)
set_tests_properties(persistence_semantics_qt PROPERTIES TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_plugins}")
if(WIN32)
    # PDFium is a runtime dependency of the export path.
    add_custom_command(TARGET cnote_persistence_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:pdfium>"
            "$<TARGET_FILE_DIR:cnote_persistence_tests>/pdfium.dll"
    )
endif()

# --- Test runtime staging (test hygiene) --------------------------------------
# A fresh build directory has no Qt, PDFium or MinGW runtime on PATH, so every Qt
# test executable gets the DLLs it needs next to it.  Without this the suite
# result depends on the ambient PATH and on the order ctest happens to use
# (observed as STATUS_ENTRYPOINT_NOT_FOUND / DLL-not-found flips between runs).
if(WIN32)
    get_target_property(_cnote_core_location Qt6::Core IMPORTED_LOCATION)
    get_filename_component(_cnote_bin_dir "${_cnote_core_location}" DIRECTORY)

    function(cnote_stage_test_runtime target)
        foreach(_cnote_runtime_target Qt6::Core Qt6::Gui Qt6::Widgets pdfium)
            if(TARGET ${_cnote_runtime_target})
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:${_cnote_runtime_target}>"
                        "$<TARGET_FILE_DIR:${target}>")
            endif()
        endforeach()
        foreach(_cnote_runtime_dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
            if(EXISTS "${_cnote_bin_dir}/${_cnote_runtime_dll}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${_cnote_bin_dir}/${_cnote_runtime_dll}"
                        "$<TARGET_FILE_DIR:${target}>")
            endif()
        endforeach()
    endfunction()

    foreach(_cnote_test_target
        cnote_page_renderer_tests
        cnote_pdf_export_tests
        cnote_text_editing_tests
        cnote_pdf_background_tests
        cnote_session_manager_tests
        cnote_settings_tests
        cnote_selection_clipboard_tests
        cnote_layout_parity_tests
        cnote_settings_dialog_tests
        cnote_new_page_dialog_tests
        cnote_precision_router_tests
        cnote_precision_filter_tests
        cnote_precision_touchpad_tests
        cnote_chrome_tests
        cnote_interaction_router_tests
        cnote_selection_math_tests
        cnote_portability_tests
        cnote_font_fallback_tests
        cnote_xpress_codec_tests
        cnote_persistence_tests)
        if(TARGET ${_cnote_test_target})
            cnote_stage_test_runtime(${_cnote_test_target})
        endif()
    endforeach()
endif()
