# Packaging verification for the cnote Qt application (migration plan 7.4).
#
# Run with `cmake -P` on any platform; it inspects an already-built artifact
# (CPack ZIP/TGZ or an install prefix), then launches the packaged executable
# from a directory unrelated to the installation and with a sanitized
# environment.  It fails loudly on any missing runtime piece instead of
# reporting success on an incomplete package.
#
# Usage:
#   cmake -DARTIFACT=<zip|tgz|install-dir> \
#         -DPLATFORM=windows|linux|macos \
#         -DFIXTURE=<fixture.cnote> \
#         -DDEVELOPER_PREFIX=<build-machine SDK path> \
#         -DWORK_DIR=<scratch dir> \
#         [-DQT_QPA_PLATFORM=offscreen] \
#         [-DSELF_CHECK=OFF] \
#         -P tools/verify-package.cmake
#
# tools/fixtures/blank.cnote is a blank v14 project written by the
# application's own ProjectFile::Save (96 bytes, magic CNOTE1\0\0, version 14).
#
# What it verifies:
#   * the application executable, the Qt Core/Gui/Widgets libraries, the Qt
#     platform plugin, the Qt image-format plugins and (Windows) the MinGW
#     compiler runtime are all present in the artifact;
#   * PDFium and the MicroTeX/LaTeX resource tree (including the
#     .clatexmath-res_root marker and a real font file) are staged where the
#     installed application resolves them;
#   * neither the executable nor qt.conf embeds the build machine's SDK path;
#   * the packaged executable, launched with cwd unrelated to the install
#     directory and a PATH that contains no developer directory, resolves its
#     resources and opens a real .cnote project (`--self-check`).
#
# The macOS layout is inspected but the macOS artifacts cannot be produced or
# launched in this environment; a macOS run is only meaningful on a macOS host.

cmake_minimum_required(VERSION 3.25)

set(CNOTE_FAILURES "")
set(CNOTE_CHECKS 0)

macro(cnote_ok text)
    math(EXPR CNOTE_CHECKS "${CNOTE_CHECKS} + 1")
    message(STATUS "  [ok]   ${text}")
endmacro()

macro(cnote_fail text)
    list(APPEND CNOTE_FAILURES "${text}")
    message(STATUS "  [FAIL] ${text}")
endmacro()

macro(cnote_check_file file_path description)
    if(EXISTS "${file_path}")
        cnote_ok("${description}: ${file_path}")
    else()
        cnote_fail("${description} missing: ${file_path}")
    endif()
endmacro()

if(NOT DEFINED ARTIFACT)
    message(FATAL_ERROR "ARTIFACT is required (package archive or install directory)")
endif()
if(NOT DEFINED PLATFORM)
    message(FATAL_ERROR "PLATFORM is required (windows|linux|macos)")
endif()
if(NOT DEFINED BIN_NAME)
    set(BIN_NAME "cnote_qt")
endif()
if(NOT DEFINED SELF_CHECK)
    set(SELF_CHECK ON)
endif()

if(NOT DEFINED WORK_DIR OR WORK_DIR STREQUAL "")
    if(DEFINED ENV{TEMP})
        set(WORK_DIR "$ENV{TEMP}/cnote-package-verify")
    elseif(DEFINED ENV{TMPDIR})
        set(WORK_DIR "$ENV{TMPDIR}/cnote-package-verify")
    else()
        set(WORK_DIR "/tmp/cnote-package-verify")
    endif()
endif()

message(STATUS "cnote package verification")
message(STATUS "  artifact: ${ARTIFACT}")
message(STATUS "  platform: ${PLATFORM}")
message(STATUS "  work dir: ${WORK_DIR}")

# --- stage the artifact -------------------------------------------------------
get_filename_component(ARTIFACT_ABS "${ARTIFACT}" ABSOLUTE)
if(NOT EXISTS "${ARTIFACT_ABS}")
    message(FATAL_ERROR "artifact does not exist: ${ARTIFACT_ABS}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

if(IS_DIRECTORY "${ARTIFACT_ABS}")
    set(STAGE_DIR "${ARTIFACT_ABS}")
    cnote_ok("inspecting install directory ${ARTIFACT_ABS}")
else()
    set(STAGE_DIR "${WORK_DIR}/extracted")
    file(MAKE_DIRECTORY "${STAGE_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${ARTIFACT_ABS}" DESTINATION "${STAGE_DIR}")
    cnote_ok("extracted archive ${ARTIFACT_ABS} to ${STAGE_DIR}")
endif()

# --- locate the packaged application -----------------------------------------
set(_roots "${STAGE_DIR}")
file(GLOB _entries RELATIVE "${STAGE_DIR}" "${STAGE_DIR}/*")
foreach(_entry IN LISTS _entries)
    if(IS_DIRECTORY "${STAGE_DIR}/${_entry}")
        list(APPEND _roots "${STAGE_DIR}/${_entry}")
    endif()
endforeach()

set(APP_EXE "")
foreach(_root IN LISTS _roots)
    foreach(_candidate
        "${_root}/bin/${BIN_NAME}.exe"
        "${_root}/${BIN_NAME}.exe"
        "${_root}/bin/${BIN_NAME}"
        "${_root}/${BIN_NAME}")
        if(EXISTS "${_candidate}" AND NOT IS_DIRECTORY "${_candidate}")
            set(APP_EXE "${_candidate}")
            break()
        endif()
    endforeach()
    if(APP_EXE)
        break()
    endif()
    file(GLOB _bundles "${_root}/*.app")
    foreach(_bundle IN LISTS _bundles)
        file(GLOB _bundle_binaries "${_bundle}/Contents/MacOS/*")
        foreach(_candidate IN LISTS _bundle_binaries)
            if(EXISTS "${_candidate}" AND NOT IS_DIRECTORY "${_candidate}")
                set(APP_EXE "${_candidate}")
                break()
            endif()
        endforeach()
        if(APP_EXE)
            break()
        endif()
    endforeach()
    if(APP_EXE)
        break()
    endif()
endforeach()

if(NOT APP_EXE)
    message(FATAL_ERROR "no ${BIN_NAME} executable found under ${STAGE_DIR}")
endif()

get_filename_component(APP_BIN_DIR "${APP_EXE}" DIRECTORY)
get_filename_component(APP_ROOT "${APP_BIN_DIR}" DIRECTORY)
set(APP_LIB_DIR "${APP_ROOT}/lib")
set(APP_PLUGIN_DIR "${APP_ROOT}/plugins")
set(APP_RESOURCE_DIR "${APP_BIN_DIR}")

if(PLATFORM STREQUAL "macos" AND APP_EXE MATCHES "^(.*\\.app)/Contents/MacOS/")
    set(APP_BUNDLE "${CMAKE_MATCH_1}")
    set(APP_RESOURCE_DIR "${APP_BUNDLE}/Contents/Resources")
    set(APP_LIB_DIR "${APP_BUNDLE}/Contents/Frameworks")
    set(APP_PLUGIN_DIR "${APP_BUNDLE}/Contents/PlugIns")
endif()

cnote_ok("application executable: ${APP_EXE}")

# --- required runtime pieces --------------------------------------------------
if(PLATFORM STREQUAL "windows")
    foreach(_library Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll)
        cnote_check_file("${APP_BIN_DIR}/${_library}" "Qt library ${_library}")
    endforeach()
    cnote_check_file("${APP_BIN_DIR}/pdfium.dll" "PDFium runtime")
    # The MinGW runtime must ship with the package: the application may not
    # depend on the build machine's compiler bin directory being on PATH.
    foreach(_runtime libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        cnote_check_file("${APP_BIN_DIR}/${_runtime}" "MinGW compiler runtime ${_runtime}")
    endforeach()
    cnote_check_file("${APP_PLUGIN_DIR}/platforms/qwindows.dll" "Qt platform plugin")
    cnote_check_file("${APP_PLUGIN_DIR}/imageformats/qico.dll" "Qt image-format plugin (ico)")
elseif(PLATFORM STREQUAL "linux")
    foreach(_library libQt6Core.so.6 libQt6Gui.so.6 libQt6Widgets.so.6)
        cnote_check_file("${APP_LIB_DIR}/${_library}" "Qt library ${_library}")
    endforeach()
    cnote_check_file("${APP_LIB_DIR}/libpdfium.so" "PDFium runtime")
    cnote_check_file("${APP_PLUGIN_DIR}/platforms/libqxcb.so" "Qt xcb platform plugin")
    cnote_check_file("${APP_PLUGIN_DIR}/platforms/libqoffscreen.so" "Qt offscreen platform plugin")
    cnote_check_file("${APP_PLUGIN_DIR}/imageformats/libqico.so" "Qt image-format plugin (ico)")
    cnote_check_file("${APP_BIN_DIR}/qt.conf" "Qt deployment qt.conf")
elseif(PLATFORM STREQUAL "macos")
    cnote_check_file("${APP_LIB_DIR}/libpdfium.dylib" "PDFium runtime")
    cnote_check_file("${APP_PLUGIN_DIR}/platforms/libqcocoa.dylib" "Qt cocoa platform plugin")
else()
    message(FATAL_ERROR "unknown PLATFORM '${PLATFORM}'")
endif()

# --- MicroTeX / LaTeX resources ----------------------------------------------
set(LATEX_RES_DIR "")
foreach(_candidate
    "${APP_RESOURCE_DIR}/latex-res"
    "${APP_BIN_DIR}/latex-res"
    "${APP_ROOT}/latex-res")
    if(IS_DIRECTORY "${_candidate}")
        set(LATEX_RES_DIR "${_candidate}")
        break()
    endif()
endforeach()
if(LATEX_RES_DIR)
    cnote_ok("LaTeX resource tree: ${LATEX_RES_DIR}")
    cnote_check_file("${LATEX_RES_DIR}/.clatexmath-res_root" "MicroTeX resource root marker")
    cnote_check_file("${LATEX_RES_DIR}/fonts/base/cmmi10.ttf" "MicroTeX math font")
else()
    cnote_fail("LaTeX resource tree (latex-res) is not staged next to the application")
endif()

# --- no build-machine paths in the launcher ----------------------------------
if(DEFINED DEVELOPER_PREFIX AND NOT DEVELOPER_PREFIX STREQUAL "")
    string(HEX "${DEVELOPER_PREFIX}" _prefix_hex)
    foreach(_file "${APP_EXE}" "${APP_BIN_DIR}/qt.conf")
        if(EXISTS "${_file}")
            file(READ "${_file}" _hex HEX)
            string(FIND "${_hex}" "${_prefix_hex}" _position)
            if(_position GREATER_EQUAL 0)
                cnote_fail("${_file} embeds the build-machine prefix ${DEVELOPER_PREFIX}")
            else()
                cnote_ok("no build-machine prefix in ${_file}")
            endif()
        endif()
    endforeach()
endif()

# --- launch from an unrelated directory --------------------------------------
if(SELF_CHECK)
    if(NOT DEFINED FIXTURE OR FIXTURE STREQUAL "")
        cnote_fail("FIXTURE is required for the launch check")
    elseif(NOT EXISTS "${FIXTURE}")
        cnote_fail("fixture does not exist: ${FIXTURE}")
    else()
        set(_run_dir "${WORK_DIR}/unrelated-working-directory")
        set(_fixture_dir "${WORK_DIR}/fixture")
        file(MAKE_DIRECTORY "${_run_dir}")
        file(MAKE_DIRECTORY "${_fixture_dir}")
        get_filename_component(_fixture_name "${FIXTURE}" NAME)
        file(COPY "${FIXTURE}" DESTINATION "${_fixture_dir}")
        set(_fixture_copy "${_fixture_dir}/${_fixture_name}")

        if(PLATFORM STREQUAL "windows")
            # Only the OS system directory: no developer SDK, no Qt, no MinGW
            # directory.  The Windows loader finds system libraries through the
            # standard search order regardless of PATH, so a single directory
            # (no list separator) keeps this a single command-line argument.
            set(_sanitized_path "$ENV{SystemRoot}/system32")
        else()
            set(_sanitized_path "/usr/bin:/bin")
        endif()

        set(_env_args "PATH=${_sanitized_path}")
        if(PLATFORM STREQUAL "linux")
            list(APPEND _env_args "LD_LIBRARY_PATH=" "QT_PLUGIN_PATH=")
            if(DEFINED QT_QPA_PLATFORM AND NOT QT_QPA_PLATFORM STREQUAL "")
                list(APPEND _env_args "QT_QPA_PLATFORM=${QT_QPA_PLATFORM}")
            endif()
        endif()

        message(STATUS "  launching ${APP_EXE} --self-check=<fixture>")
        message(STATUS "    cwd  ${_run_dir}")
        message(STATUS "    PATH ${_sanitized_path}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${_env_args}
                "${APP_EXE}" "--self-check=${_fixture_copy}"
            WORKING_DIRECTORY "${_run_dir}"
            RESULT_VARIABLE _result
            OUTPUT_VARIABLE _stdout
            ERROR_VARIABLE _stderr
            TIMEOUT 180
        )
        if(_stdout)
            message(STATUS "  stdout: ${_stdout}")
        endif()
        if(_stderr)
            message(STATUS "  stderr: ${_stderr}")
        endif()
        if(_result EQUAL 0)
            cnote_ok("packaged executable launched from an unrelated directory with a sanitized environment and opened ${_fixture_name}")
        else()
            cnote_fail("packaged executable exited with ${_result} (see stdout/stderr above)")
        endif()

        # Second launch with a non-ASCII project name: that is the path a user
        # takes by opening an associated file, and on Windows it exercises the
        # application's command-line argument handling.  MinGW libstdc++ decodes
        # narrow path input as UTF-8 and throws filesystem_error on code-page
        # bytes, so a code-page-encoded argv used to abort here; the wide Qt
        # argument list must be what feeds the path construction.
        file(RENAME "${_fixture_copy}" "${_fixture_dir}/nöte-α-日本.cnote")
        set(_utf8_fixture "${_fixture_dir}/nöte-α-日本.cnote")
        message(STATUS "  launching ${APP_EXE} --self-check=<non-ASCII fixture>")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${_env_args}
                "${APP_EXE}" "--self-check=${_utf8_fixture}"
            WORKING_DIRECTORY "${_run_dir}"
            RESULT_VARIABLE _utf8_result
            OUTPUT_VARIABLE _utf8_stdout
            ERROR_VARIABLE _utf8_stderr
            TIMEOUT 180
        )
        if(_utf8_stdout)
            message(STATUS "  stdout: ${_utf8_stdout}")
        endif()
        if(_utf8_stderr)
            message(STATUS "  stderr: ${_utf8_stderr}")
        endif()
        if(_utf8_result EQUAL 0)
            cnote_ok("packaged executable opened a project whose path contains non-ASCII characters")
        else()
            cnote_fail("non-ASCII project path launch exited with ${_utf8_result} (see stdout/stderr above)")
        endif()
    endif()
endif()

# --- result -------------------------------------------------------------------
if(CNOTE_FAILURES)
    string(REPLACE ";" "\n  - " _failure_list "${CNOTE_FAILURES}")
    message(FATAL_ERROR
        "packaging verification FAILED (${CNOTE_CHECKS} checks passed):\n  - ${_failure_list}")
endif()
message(STATUS "packaging verification PASSED: ${CNOTE_CHECKS} checks on ${ARTIFACT_ABS}")
