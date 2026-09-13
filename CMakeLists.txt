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

add_library(cnote_core STATIC
    src/App.cpp
    src/AppSettings.cpp
    src/Unicode.cpp
    src/ui/Layout.cpp
    src/DpiUtil.cpp
    src/NewPageSettingsWindow.cpp
    src/PdfBackend.cpp
    src/ProjectFile.cpp
    src/SettingsWindow.cpp
    src/ShortcutConfig.cpp
    src/document/Clipboard.cpp
    src/document/FileIO.cpp
    src/document/History.cpp
    src/document/LoadDocumentFile.cpp
    src/document/Pages.cpp
    src/document/SelectionClipboardCodec.cpp
    src/document/Session.cpp
    src/document/XpressCodec.cpp
    src/latex/LatexEditing.cpp
    src/latex/LatexCompletion.cpp
    src/latex/LatexEngine.cpp
    src/latex/LatexEngineGdi.cpp
    src/PdfOverlayGdi.cpp
    src/render/Paint.cpp
    src/selection/Selection.cpp
    src/selection/Geometry.cpp
    src/selection/StrokeSmoothing.cpp
    src/selection/Transforms.cpp
    src/shell/Startup.cpp
    src/shell/AppManager.cpp
    src/ui/UiUtil.cpp
    src/shell/MessageLoop.cpp
    src/shell/ToolbarState.cpp
    src/text/TextEditing.cpp
    src/text/TextGeometry.cpp
    src/viewport/Viewport.cpp
)
if(WIN32)
    target_sources(cnote_core PRIVATE src/platform/windows/PrecisionTouchpad.cpp)
endif()

if(MINGW)
    set_source_files_properties(
        src/platform/windows/PrecisionTouchpad.cpp
        PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mingw-w64/include")
endif()


add_executable(cnote WIN32 src/main.cpp src/cnote.rc)
target_link_libraries(cnote PRIVATE cnote_core)

target_include_directories(cnote_core PUBLIC src)
target_link_libraries(cnote_core PUBLIC cnote_model Qt6::Core)

target_compile_definitions(cnote_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)

target_compile_options(cnote_core PRIVATE
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
add_library(microtex STATIC
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
    third_party/microtex/src/platform/gdi_win/graphic_win32.cpp
)
target_include_directories(microtex PUBLIC third_party/microtex/src)
target_compile_definitions(microtex PUBLIC BUILD_WIN32)
# _HAS_STD_BYTE=0 is only needed while compiling MicroTeX's own sources; it must
# not leak to consumers, where Qt requires std::byte.
target_compile_definitions(microtex PRIVATE _HAS_STD_BYTE=0)
target_link_libraries(microtex PUBLIC tinyxml2 gdiplus)

target_link_libraries(cnote_core PUBLIC
    comdlg32
    comctl32
    d3d11
    dcomp
    dwmapi
    dxgi
    gdi32
    gdiplus
    microtex
    msimg32
    pdfium
    uxtheme
    user32
    winmm
)

# The MicroTeX font TTFs are external files; stage them next to the app.
add_custom_command(TARGET cnote POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res"
        "$<TARGET_FILE_DIR:cnote>/latex-res"
)

enable_testing()

add_executable(cnote_latex_engine_tests
    tests/LatexEngineTests.cpp
    src/latex/LatexEngine.cpp
    src/latex/LatexEngineGdi.cpp
    src/latex/LatexCompletion.cpp
)
target_include_directories(cnote_latex_engine_tests PRIVATE src)
target_compile_definitions(cnote_latex_engine_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602
    LATEX_RES_ROOT=L"${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res"
)
target_link_libraries(cnote_latex_engine_tests PRIVATE cnote_model microtex gdiplus)
add_test(NAME latex_engine COMMAND cnote_latex_engine_tests)

add_executable(cnote_text_model_tests
    tests/TextModelTests.cpp
    src/DpiUtil.cpp
    src/Unicode.cpp
    src/document/SelectionClipboardCodec.cpp
    src/latex/LatexEngine.cpp
    src/latex/LatexEngineGdi.cpp
    src/PdfBackend.cpp
    src/PdfOverlayGdi.cpp
    src/ProjectFile.cpp
    src/selection/Geometry.cpp
    src/selection/StrokeSmoothing.cpp
    src/text/TextGeometry.cpp
    src/ui/UiUtil.cpp
    src/document/XpressCodec.cpp
)
target_include_directories(cnote_text_model_tests PRIVATE src)
target_compile_definitions(cnote_text_model_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
target_compile_options(cnote_text_model_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_text_model_tests PRIVATE
    comctl32
    gdi32
    gdiplus
    microtex
    msimg32
    pdfium
    user32
)
target_link_libraries(cnote_text_model_tests PRIVATE cnote_model Qt6::Core)
add_test(NAME text_model_round_trip COMMAND cnote_text_model_tests)

add_custom_command(TARGET cnote POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:pdfium>"
        "$<TARGET_FILE_DIR:cnote>/pdfium.dll"
)

add_executable(cnote_project_session_tests tests/ProjectSessionTests.cpp)
target_link_libraries(cnote_project_session_tests PRIVATE cnote_core)
add_test(NAME project_sessions COMMAND cnote_project_session_tests)

# --- Qt MicroTeX backend (migration plan step 3.8) ----------------------------
# `microtex_qt` is built from the same engine sources as `microtex`, but with the
# Qt platform adapter instead of the GDI one.  The two platform backends must
# never be compiled into one target (plan step 1.5), so nothing links both
# `microtex` and `microtex_qt`.
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

get_target_property(_cnote_microtex_sources microtex SOURCES)
list(FILTER _cnote_microtex_sources EXCLUDE REGEX "platform/gdi_win/graphic_win32\\.cpp$")

add_library(microtex_qt STATIC
    ${_cnote_microtex_sources}
    third_party/microtex/src/platform/qt/graphic_qt.cpp
)
target_include_directories(microtex_qt PUBLIC third_party/microtex/src)
target_compile_definitions(microtex_qt PUBLIC BUILD_QT)
# The legacy `microtex` target sets _HAS_STD_BYTE=0 for MicroTeX's own sources
# (an MSVC workaround for its `using namespace std;` headers).  This target's
# Qt adapter TU includes Qt headers, which require std::byte, so the define
# stays off here; it is unnecessary for MinGW/GCC/AppleClang.
target_link_libraries(microtex_qt PUBLIC tinyxml2 Qt6::Gui)

# Portable MicroTeX core plus the Qt QPainter drawing backend: the Qt
# counterpart of the legacy `microtex` usage in cnote_core.  This is the home of
# cnote::latex::Draw(FormulaHandle, QPainter&, ...) until the services layer
# (plan steps 3-4) absorbs it.
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

# Qt PDF export: the portable PDFium core plus the Qt overlay composition. The
# legacy cnote_core keeps the GDI+ overlay (src/PdfOverlayGdi.cpp) until
# cutover; the two overlay TUs are never linked into one target.
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
    OUTPUT_VARIABLE _cnote_qt_plugins
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set_tests_properties(render_qt_offscreen PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")

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
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")

# Self-contained Qt editing-services test (offsets, formatting, LaTeX editing).
add_executable(cnote_text_editing_tests
    tests/TextEditingTests.cpp
)
target_compile_definitions(cnote_text_editing_tests PRIVATE
    CNOTE_LATEX_RES_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res")
target_link_libraries(cnote_text_editing_tests PRIVATE cnote_render_qt)
add_test(NAME text_editing_qt COMMAND cnote_text_editing_tests)
set_tests_properties(text_editing_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")

# Self-contained lazy PDF-background provider test.
add_executable(cnote_pdf_background_tests
    tests/PdfBackgroundTests.cpp
)
target_link_libraries(cnote_pdf_background_tests PRIVATE cnote_pdf_qt)
add_test(NAME pdf_background_qt COMMAND cnote_pdf_background_tests)
set_tests_properties(pdf_background_qt PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")

# Self-contained Qt session-lifetime / autosave manager test.
add_executable(cnote_session_manager_tests
    tests/SessionManagerTests.cpp
    src/shell/AppManager.cpp
    src/Unicode.cpp
)
target_link_libraries(cnote_session_manager_tests PRIVATE cnote_model Qt6::Core)
add_test(NAME session_manager_qt COMMAND cnote_session_manager_tests)

# --- Qt canvas and runnable migration frontend (plan steps 3.1/3.2/3.3/3.9) ---
# CanvasWidget owns per-view viewport/transient state and its raster caches and
# delegates all committed-object drawing to cnote_render_qt; the controller owns
# the shared ProjectSession (document + undo/redo) behind ui::CanvasController.
# No App/HWND/HDC and no second rendering or text path.
find_package(Qt6 6.11.2 REQUIRED COMPONENTS Widgets)

add_library(cnote_qt_ui STATIC
    src/ui/CanvasWidget.cpp
    src/qt/QtDocumentController.cpp
    src/qt/QtDocumentIo.cpp
    src/qt/QtMainWindow.cpp
    # Portable project decode used by the Qt file service.
    src/ProjectFile.cpp
    src/document/XpressCodec.cpp
    src/document/LoadDocumentFile.cpp
    src/Unicode.cpp
)
set_target_properties(cnote_qt_ui PROPERTIES AUTOMOC ON)
target_include_directories(cnote_qt_ui PUBLIC src)
target_compile_features(cnote_qt_ui PUBLIC cxx_std_20)
target_compile_options(cnote_qt_ui PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
# cnote_pdf_qt provides PdfiumBackend plus the portable file-loader service.
target_link_libraries(cnote_qt_ui PUBLIC cnote_model cnote_render_qt cnote_pdf_qt Qt6::Widgets)

add_executable(cnote_qt src/qt/main.cpp)
target_link_libraries(cnote_qt PRIVATE cnote_qt_ui)

# MicroTeX's font/XML resources ship as real files next to the executable.
add_custom_command(TARGET cnote_qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res"
        "$<TARGET_FILE_DIR:cnote_qt>/latex-res"
)
if(WIN32)
    add_custom_command(TARGET cnote_qt POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:pdfium>"
            "$<TARGET_FILE_DIR:cnote_qt>/pdfium.dll"
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
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")

# Windows-only parity harness: runs the real legacy App::LoadAppSettings and the
# Qt parser over the same payload and diffs every parsed field, checks the Qt
# shortcut table against src/ShortcutConfig.cpp, and compares both writers.
if(WIN32 AND TARGET cnote_core)
    add_executable(cnote_settings_parity_tests tests/SettingsParityTests.cpp)
    target_compile_options(cnote_settings_parity_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(cnote_settings_parity_tests PRIVATE cnote_core cnote_settings_qt)
    add_test(NAME settings_parity_qt COMMAND cnote_settings_parity_tests)
    set_tests_properties(settings_parity_qt PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")
endif()

# --- Qt clipboard bridge (migration plan step 2.6) ----------------------------
# Platform-neutral CTS selection codec plus the QClipboard bridge: structured
# selections travel as application/x-cnote-selection and, on Windows, through
# Qt's Windows MIME bridge under the legacy registered format
# cnote.selection.v1.  Ordinary image data and plain text are the paste
# fallbacks in that order.  Qt Gui only - no widgets, no App/HWND/HDC - and
# nothing here mutates a document.  The legacy Win32 clipboard path
# (src/document/Clipboard.cpp) stays in cnote_core until cutover.
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
        ENVIRONMENT "QT_PLUGIN_PATH=${_cnote_qt_plugins}")
else()
    set_tests_properties(selection_clipboard_qt PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_cnote_qt_plugins}")
endif()

