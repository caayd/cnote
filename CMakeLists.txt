cmake_minimum_required(VERSION 3.25)

project(ctrainer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ctrainer WIN32
    src/App.cpp
    src/Document.cpp
    src/PdfBackend.cpp
    src/main.cpp
)

target_include_directories(ctrainer PRIVATE src)

target_compile_definitions(ctrainer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_compile_options(ctrainer PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(ctrainer PRIVATE
    comdlg32
    gdi32
    gdiplus
    user32
)
