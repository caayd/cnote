cmake_minimum_required(VERSION 3.25)

project(cnote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Pdfium.cmake")


add_library(cnote_core STATIC
    src/App.cpp
    src/AppSettings.cpp
    src/ui/Layout.cpp
    src/DpiUtil.cpp
    src/Document.cpp
    src/NewPageSettingsWindow.cpp
    src/PdfBackend.cpp
    src/ProjectFile.cpp
    src/SettingsWindow.cpp
    src/ShortcutConfig.cpp
    src/document/Clipboard.cpp
    src/document/FileIO.cpp
    src/document/History.cpp
    src/document/Pages.cpp
    src/document/Session.cpp
    src/latex/LatexEditing.cpp
    src/latex/LatexCompletion.cpp
    src/latex/LatexEngine.cpp
    src/render/Paint.cpp
    src/selection/Selection.cpp
    src/selection/Geometry.cpp
    src/selection/Transforms.cpp
    src/shell/Startup.cpp
    src/ui/UiUtil.cpp
    src/shell/MessageLoop.cpp
    src/shell/ToolbarState.cpp
    src/text/TextEditing.cpp
    src/text/TextGeometry.cpp
    src/viewport/Viewport.cpp
)

add_executable(cnote WIN32 src/main.cpp src/cnote.rc)
target_link_libraries(cnote PRIVATE cnote_core)

target_include_directories(cnote_core PUBLIC src)

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
target_compile_definitions(microtex PUBLIC BUILD_WIN32 _HAS_STD_BYTE=0)
target_link_libraries(microtex PUBLIC tinyxml2 gdiplus)

target_link_libraries(cnote_core PUBLIC
    cabinet
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
    ninput
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
    src/Document.cpp
    src/latex/LatexEngine.cpp
    src/latex/LatexCompletion.cpp
)
target_include_directories(cnote_latex_engine_tests PRIVATE src)
target_compile_definitions(cnote_latex_engine_tests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602
    LATEX_RES_ROOT=L"${CMAKE_CURRENT_SOURCE_DIR}/third_party/microtex/res"
)
target_link_libraries(cnote_latex_engine_tests PRIVATE microtex gdiplus)
add_test(NAME latex_engine COMMAND cnote_latex_engine_tests)

add_executable(cnote_text_model_tests
    tests/TextModelTests.cpp
    src/DpiUtil.cpp
    src/Document.cpp
    src/latex/LatexEngine.cpp
    src/PdfBackend.cpp
    src/ProjectFile.cpp
    src/selection/Geometry.cpp
    src/text/TextGeometry.cpp
    src/ui/UiUtil.cpp
)
target_include_directories(cnote_text_model_tests PRIVATE src)
target_compile_definitions(cnote_text_model_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0602)
target_compile_options(cnote_text_model_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(cnote_text_model_tests PRIVATE
    cabinet
    comctl32
    gdi32
    gdiplus
    microtex
    msimg32
    pdfium
    user32
)
add_test(NAME text_model_round_trip COMMAND cnote_text_model_tests)

add_custom_command(TARGET cnote POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:pdfium>"
        "$<TARGET_FILE_DIR:cnote>/pdfium.dll"
)

add_executable(cnote_project_session_tests tests/ProjectSessionTests.cpp)
target_link_libraries(cnote_project_session_tests PRIVATE cnote_core)
add_test(NAME project_sessions COMMAND cnote_project_session_tests)
