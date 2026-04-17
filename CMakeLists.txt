cmake_minimum_required(VERSION 3.25)

project(cnote VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PDFIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium")
set(PDFIUM_DLL "${PDFIUM_ROOT}/bin/pdfium.dll")
set(PDFIUM_IMPLIB "${PDFIUM_ROOT}/lib/libpdfium.dll.a")

add_executable(cnote WIN32
    src/App.cpp
    src/AppSettings.cpp
    src/ui/Layout.cpp
    src/cnote.rc
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
    src/render/Paint.cpp
    src/selection/Selection.cpp
    src/selection/Transforms.cpp
    src/shell/Startup.cpp
    src/ui/UiUtil.cpp
    src/shell/MessageLoop.cpp
    src/shell/ToolbarState.cpp
    src/text/TextEditing.cpp
    src/text/TextGeometry.cpp
    src/viewport/Viewport.cpp
    src/main.cpp
)

target_include_directories(cnote PRIVATE src)

if(MINGW AND EXISTS "${PDFIUM_DLL}" AND NOT EXISTS "${PDFIUM_IMPLIB}")
    find_program(GENDEF_EXECUTABLE gendef REQUIRED)
    find_program(DLLTOOL_EXECUTABLE dlltool REQUIRED)

    execute_process(
        COMMAND "${GENDEF_EXECUTABLE}" ../bin/pdfium.dll
        WORKING_DIRECTORY "${PDFIUM_ROOT}/lib"
        COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND "${DLLTOOL_EXECUTABLE}" -d pdfium.def -D pdfium.dll -l libpdfium.dll.a
        WORKING_DIRECTORY "${PDFIUM_ROOT}/lib"
        COMMAND_ERROR_IS_FATAL ANY
    )
endif()

add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFIUM_DLL}"
    IMPORTED_IMPLIB "${PDFIUM_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_ROOT}/include;${PDFIUM_ROOT}/include/cpp"
)

target_compile_definitions(cnote PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_compile_options(cnote PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(cnote PRIVATE
    comdlg32
    comctl32
    d3d11
    dcomp
    dwmapi
    dxgi
    gdi32
    gdiplus
    pdfium
    uxtheme
    user32
    winmm
)

add_custom_command(TARGET cnote POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PDFIUM_DLL}"
        "$<TARGET_FILE_DIR:cnote>/pdfium.dll"
)
