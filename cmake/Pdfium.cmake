# Project-owned PDFium dependency discovery.
#
# PDFium is selected as one self-contained SDK.  The SDK's PDFiumConfig.cmake
# remains the package convention and exports the imported target named pdfium.
# Header, runtime, and import-library paths are all forced from the selected
# root so a reconfigure cannot silently mix two SDKs.

if(DEFINED CNOTE_PDFIUM_MODULE_INCLUDED)
    return()
endif()
set(CNOTE_PDFIUM_MODULE_INCLUDED TRUE)

set(CNOTE_PDFIUM_REQUIRED_VERSION "148.0.7749.0")
get_filename_component(_cnote_pdfium_default_root
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/pdfium" ABSOLUTE)
set(CNOTE_PDFIUM_ROOT "${_cnote_pdfium_default_root}" CACHE PATH
    "Root of the exact PDFium SDK used by cnote")

if(NOT CNOTE_PDFIUM_ROOT)
    message(FATAL_ERROR "CNOTE_PDFIUM_ROOT is empty; provide the PDFium SDK root")
endif()
get_filename_component(_cnote_pdfium_root "${CNOTE_PDFIUM_ROOT}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT IS_DIRECTORY "${_cnote_pdfium_root}")
    message(FATAL_ERROR "PDFium SDK root is not a directory: ${CNOTE_PDFIUM_ROOT}")
endif()
file(REAL_PATH "${_cnote_pdfium_root}" _cnote_pdfium_root)

set(_cnote_pdfium_config "${_cnote_pdfium_root}/PDFiumConfig.cmake")
set(_cnote_pdfium_version_file "${_cnote_pdfium_root}/VERSION")
if(NOT EXISTS "${_cnote_pdfium_config}" OR NOT EXISTS "${_cnote_pdfium_version_file}")
    message(FATAL_ERROR
        "PDFium SDK root must contain PDFiumConfig.cmake and VERSION: ${_cnote_pdfium_root}")
endif()

# Validate both version declarations before executing the package config.
file(STRINGS "${_cnote_pdfium_version_file}" _cnote_pdfium_version_lines)
set(_cnote_pdfium_major "")
set(_cnote_pdfium_minor "")
set(_cnote_pdfium_build "")
set(_cnote_pdfium_patch "")
foreach(_cnote_pdfium_line IN LISTS _cnote_pdfium_version_lines)
    if(_cnote_pdfium_line MATCHES "^[ \t]*MAJOR=([0-9]+)[ \t]*$")
        set(_cnote_pdfium_major "${CMAKE_MATCH_1}")
    elseif(_cnote_pdfium_line MATCHES "^[ \t]*MINOR=([0-9]+)[ \t]*$")
        set(_cnote_pdfium_minor "${CMAKE_MATCH_1}")
    elseif(_cnote_pdfium_line MATCHES "^[ \t]*BUILD=([0-9]+)[ \t]*$")
        set(_cnote_pdfium_build "${CMAKE_MATCH_1}")
    elseif(_cnote_pdfium_line MATCHES "^[ \t]*PATCH=([0-9]+)[ \t]*$")
        set(_cnote_pdfium_patch "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(_cnote_pdfium_major STREQUAL "" OR _cnote_pdfium_minor STREQUAL "" OR
   _cnote_pdfium_build STREQUAL "" OR _cnote_pdfium_patch STREQUAL "")
    message(FATAL_ERROR "PDFium VERSION is missing MAJOR/MINOR/BUILD/PATCH metadata")
endif()
set(_cnote_pdfium_metadata_version
    "${_cnote_pdfium_major}.${_cnote_pdfium_minor}.${_cnote_pdfium_build}.${_cnote_pdfium_patch}")
if(NOT _cnote_pdfium_metadata_version STREQUAL CNOTE_PDFIUM_REQUIRED_VERSION)
    message(FATAL_ERROR
        "PDFium SDK version ${_cnote_pdfium_metadata_version} is not the required ${CNOTE_PDFIUM_REQUIRED_VERSION}")
endif()

file(READ "${_cnote_pdfium_config}" _cnote_pdfium_config_text)
string(REGEX MATCH
    "set\\([ \t]*PDFium_VERSION[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)\""
    _cnote_pdfium_config_match "${_cnote_pdfium_config_text}")
if(NOT _cnote_pdfium_config_match OR
   NOT CMAKE_MATCH_1 STREQUAL CNOTE_PDFIUM_REQUIRED_VERSION OR
   NOT CMAKE_MATCH_1 STREQUAL _cnote_pdfium_metadata_version)
    message(FATAL_ERROR
        "PDFiumConfig.cmake version disagrees with required ${CNOTE_PDFIUM_REQUIRED_VERSION}")
endif()

if(NOT WIN32 AND NOT UNIX)
    message(FATAL_ERROR "cnote PDFium supports desktop Windows, Linux, and macOS only")
endif()
if(WIN32 AND NOT MSVC AND NOT MINGW)
    message(FATAL_ERROR "cnote PDFium on Windows requires MSVC or MinGW")
endif()
if(UNIX AND NOT APPLE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "cnote PDFium on Linux requires GCC")
endif()
if(APPLE AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "cnote PDFium on macOS requires AppleClang")
endif()

# CMAKE_OSX_ARCHITECTURES is the target on macOS and must win over the host
# processor (for example, an x64 build made on an arm64 machine).
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _cnote_pdfium_osx_arch_count)
    if(NOT _cnote_pdfium_osx_arch_count EQUAL 1)
        message(FATAL_ERROR "PDFium requires one CMAKE_OSX_ARCHITECTURES value; universal builds are not supported")
    endif()
    set(_cnote_pdfium_processor "${CMAKE_OSX_ARCHITECTURES}")
elseif(CMAKE_CXX_COMPILER_TARGET)
    set(_cnote_pdfium_processor "${CMAKE_CXX_COMPILER_TARGET}")
else()
    set(_cnote_pdfium_processor "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_cnote_pdfium_processor}" _cnote_pdfium_processor_lower)
if(_cnote_pdfium_processor_lower MATCHES "(^|[-_])((x86_64)|(amd64)|(x64))($|[-_])")
    set(_cnote_pdfium_arch x86_64)
elseif(_cnote_pdfium_processor_lower MATCHES "(^|[-_])((aarch64)|(arm64))($|[-_])")
    set(_cnote_pdfium_arch arm64)
elseif(_cnote_pdfium_processor_lower MATCHES "(^|[-_])(i[3-6]86|x86)($|[-_])")
    set(_cnote_pdfium_arch x86)
else()
    message(FATAL_ERROR "Cannot determine PDFium target architecture from '${_cnote_pdfium_processor}'")
endif()
if(_cnote_pdfium_arch STREQUAL x86_64 OR _cnote_pdfium_arch STREQUAL arm64)
    if(DEFINED CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "PDFium ${_cnote_pdfium_arch} requires a 64-bit toolchain")
    endif()
elseif(DEFINED CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "PDFium x86 requires a 32-bit toolchain")
endif()

set(_cnote_pdfium_include "${_cnote_pdfium_root}/include")
if(WIN32)
    set(_cnote_pdfium_runtime "${_cnote_pdfium_root}/bin/pdfium.dll")
elseif(APPLE)
    set(_cnote_pdfium_runtime "${_cnote_pdfium_root}/lib/libpdfium.dylib")
else()
    set(_cnote_pdfium_runtime "${_cnote_pdfium_root}/lib/libpdfium.so")
endif()
set(_cnote_pdfium_required_headers
    fpdfview.h fpdf_attachment.h fpdf_edit.h fpdf_ppo.h fpdf_save.h fpdf_text.h)
foreach(_cnote_pdfium_header IN LISTS _cnote_pdfium_required_headers)
    if(NOT EXISTS "${_cnote_pdfium_include}/${_cnote_pdfium_header}")
        message(FATAL_ERROR "PDFium SDK is missing required header ${_cnote_pdfium_header}")
    endif()
endforeach()
if(NOT EXISTS "${_cnote_pdfium_include}/cpp/fpdf_scopers.h" OR
   NOT EXISTS "${_cnote_pdfium_include}/cpp/fpdf_deleters.h")
    message(FATAL_ERROR "PDFium SDK is missing required C++ helper headers")
endif()
if(NOT EXISTS "${_cnote_pdfium_runtime}" OR IS_DIRECTORY "${_cnote_pdfium_runtime}")
    message(FATAL_ERROR "PDFium SDK is missing its runtime library: ${_cnote_pdfium_runtime}")
endif()

# Inspect object headers directly.  This catches a Linux ELF renamed to .dll,
# and does not depend on GNU objdump (important for MSVC installations).
function(_cnote_pdfium_validate_binary _artifact _description)
    file(SIZE "${_artifact}" _cnote_pdfium_size)
    if(_cnote_pdfium_size LESS 32)
        message(FATAL_ERROR "PDFium ${_description} is too small to be a valid binary: ${_artifact}")
    endif()
    file(READ "${_artifact}" _cnote_pdfium_hex OFFSET 0 LIMIT 4096 HEX)
    string(TOLOWER "${_cnote_pdfium_hex}" _cnote_pdfium_hex)
    string(SUBSTRING "${_cnote_pdfium_hex}" 0 4 _cnote_pdfium_magic)
    set(_cnote_pdfium_match FALSE)

    if(WIN32)
        if(NOT _cnote_pdfium_magic STREQUAL "4d5a")
            message(FATAL_ERROR "PDFium ${_description} is not a PE/COFF binary: ${_artifact}")
        endif()
        string(SUBSTRING "${_cnote_pdfium_hex}" 120 2 _cnote_pdfium_b0)
        string(SUBSTRING "${_cnote_pdfium_hex}" 122 2 _cnote_pdfium_b1)
        string(SUBSTRING "${_cnote_pdfium_hex}" 124 2 _cnote_pdfium_b2)
        string(SUBSTRING "${_cnote_pdfium_hex}" 126 2 _cnote_pdfium_b3)
        math(EXPR _cnote_pdfium_pe_offset "0x${_cnote_pdfium_b3}${_cnote_pdfium_b2}${_cnote_pdfium_b1}${_cnote_pdfium_b0}")
        math(EXPR _cnote_pdfium_machine_offset "${_cnote_pdfium_pe_offset} * 2 + 8")
        string(LENGTH "${_cnote_pdfium_hex}" _cnote_pdfium_hex_length)
        if(_cnote_pdfium_machine_offset GREATER_EQUAL _cnote_pdfium_hex_length OR
           _cnote_pdfium_machine_offset LESS 0)
            message(FATAL_ERROR "PDFium ${_description} has an invalid PE header: ${_artifact}")
        endif()
        string(SUBSTRING "${_cnote_pdfium_hex}" ${_cnote_pdfium_machine_offset} 4 _cnote_pdfium_machine)
        if(_cnote_pdfium_arch STREQUAL x86_64 AND _cnote_pdfium_machine STREQUAL 6486)
            set(_cnote_pdfium_match TRUE)
        elseif(_cnote_pdfium_arch STREQUAL arm64 AND _cnote_pdfium_machine STREQUAL aa64)
            set(_cnote_pdfium_match TRUE)
        elseif(_cnote_pdfium_arch STREQUAL x86 AND _cnote_pdfium_machine STREQUAL 4c01)
            set(_cnote_pdfium_match TRUE)
        endif()
    elseif(UNIX AND NOT APPLE)
        if(NOT _cnote_pdfium_magic STREQUAL "7f454c46")
            message(FATAL_ERROR "PDFium ${_description} is not an ELF binary: ${_artifact}")
        endif()
        string(SUBSTRING "${_cnote_pdfium_hex}" 8 2 _cnote_pdfium_class)
        string(SUBSTRING "${_cnote_pdfium_hex}" 20 2 _cnote_pdfium_data)
        string(SUBSTRING "${_cnote_pdfium_hex}" 36 4 _cnote_pdfium_machine)
        if(_cnote_pdfium_data STREQUAL 01 AND _cnote_pdfium_arch STREQUAL x86_64 AND
           _cnote_pdfium_class STREQUAL 02 AND _cnote_pdfium_machine STREQUAL 3e00)
            set(_cnote_pdfium_match TRUE)
        elseif(_cnote_pdfium_data STREQUAL 01 AND _cnote_pdfium_arch STREQUAL arm64 AND
               _cnote_pdfium_class STREQUAL 02 AND _cnote_pdfium_machine STREQUAL b700)
            set(_cnote_pdfium_match TRUE)
        elseif(_cnote_pdfium_data STREQUAL 01 AND _cnote_pdfium_arch STREQUAL x86 AND
               _cnote_pdfium_class STREQUAL 01 AND _cnote_pdfium_machine STREQUAL 0300)
            set(_cnote_pdfium_match TRUE)
        endif()
    elseif(APPLE)
        if(_cnote_pdfium_magic STREQUAL "cffaedfe")
            string(SUBSTRING "${_cnote_pdfium_hex}" 8 8 _cnote_pdfium_cpu)
            if(_cnote_pdfium_arch STREQUAL x86_64 AND _cnote_pdfium_cpu STREQUAL 07000001)
                set(_cnote_pdfium_match TRUE)
            elseif(_cnote_pdfium_arch STREQUAL arm64 AND _cnote_pdfium_cpu STREQUAL 0c000001)
                set(_cnote_pdfium_match TRUE)
            endif()
        elseif(_cnote_pdfium_magic STREQUAL "cafebabe" OR _cnote_pdfium_magic STREQUAL "bebafeca")
            set(_cnote_pdfium_fat_little FALSE)
            if(_cnote_pdfium_magic STREQUAL "bebafeca")
                set(_cnote_pdfium_fat_little TRUE)
            endif()
            string(SUBSTRING "${_cnote_pdfium_hex}" 8 8 _cnote_pdfium_nfat_hex)
            if(_cnote_pdfium_fat_little)
                string(SUBSTRING "${_cnote_pdfium_nfat_hex}" 0 2 _cnote_pdfium_n0)
                string(SUBSTRING "${_cnote_pdfium_nfat_hex}" 2 2 _cnote_pdfium_n1)
                string(SUBSTRING "${_cnote_pdfium_nfat_hex}" 4 2 _cnote_pdfium_n2)
                string(SUBSTRING "${_cnote_pdfium_nfat_hex}" 6 2 _cnote_pdfium_n3)
                set(_cnote_pdfium_nfat_hex "${_cnote_pdfium_n3}${_cnote_pdfium_n2}${_cnote_pdfium_n1}${_cnote_pdfium_n0}")
            endif()
            math(EXPR _cnote_pdfium_nfat "0x${_cnote_pdfium_nfat_hex}")
            math(EXPR _cnote_pdfium_entry_offset 16)
            foreach(_cnote_pdfium_entry RANGE 1 ${_cnote_pdfium_nfat})
                string(SUBSTRING "${_cnote_pdfium_hex}" ${_cnote_pdfium_entry_offset} 8 _cnote_pdfium_cpu)
                if((_cnote_pdfium_arch STREQUAL x86_64 AND _cnote_pdfium_cpu STREQUAL 01000007) OR
                   (_cnote_pdfium_arch STREQUAL arm64 AND _cnote_pdfium_cpu STREQUAL 0100000c))
                    set(_cnote_pdfium_match TRUE)
                endif()
                math(EXPR _cnote_pdfium_entry_offset "${_cnote_pdfium_entry_offset} + 40")
            endforeach()
        endif()
    endif()
    if(NOT _cnote_pdfium_match)
        message(FATAL_ERROR "PDFium ${_description} architecture does not match ${_cnote_pdfium_arch}: ${_artifact}")
    endif()
endfunction()

_cnote_pdfium_validate_binary("${_cnote_pdfium_runtime}" "runtime")

get_filename_component(_cnote_pdfium_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
if(WIN32)
    if(MSVC)
        set(_cnote_pdfium_implib "${_cnote_pdfium_root}/lib/pdfium.dll.lib")
        if(NOT EXISTS "${_cnote_pdfium_implib}" OR IS_DIRECTORY "${_cnote_pdfium_implib}")
            message(FATAL_ERROR "MSVC PDFium SDK is missing its import library: ${_cnote_pdfium_implib}")
        endif()
    else()
        set(_cnote_pdfium_implib "${_cnote_pdfium_root}/lib/libpdfium.dll.a")
        if(NOT EXISTS "${_cnote_pdfium_implib}" OR IS_DIRECTORY "${_cnote_pdfium_implib}")
            find_program(_cnote_pdfium_gendef NAMES gendef gendef.exe
                HINTS "${_cnote_pdfium_compiler_dir}")
            find_program(_cnote_pdfium_dlltool NAMES dlltool dlltool.exe
                HINTS "${_cnote_pdfium_compiler_dir}")
            if(NOT _cnote_pdfium_gendef OR NOT _cnote_pdfium_dlltool)
                message(FATAL_ERROR "MinGW PDFium import library is missing; gendef and dlltool are required")
            endif()
            set(_cnote_pdfium_import_dir "${CMAKE_CURRENT_BINARY_DIR}/pdfium-import")
            file(MAKE_DIRECTORY "${_cnote_pdfium_import_dir}")
            execute_process(COMMAND "${_cnote_pdfium_gendef}" "${_cnote_pdfium_runtime}"
                WORKING_DIRECTORY "${_cnote_pdfium_import_dir}"
                RESULT_VARIABLE _cnote_pdfium_gendef_result
                ERROR_VARIABLE _cnote_pdfium_gendef_error)
            if(NOT _cnote_pdfium_gendef_result EQUAL 0 OR
               NOT EXISTS "${_cnote_pdfium_import_dir}/pdfium.def")
                message(FATAL_ERROR "gendef failed to create the PDFium import definition: ${_cnote_pdfium_gendef_error}")
            endif()
            set(_cnote_pdfium_implib "${_cnote_pdfium_import_dir}/libpdfium.dll.a")
            execute_process(COMMAND "${_cnote_pdfium_dlltool}" -d "${_cnote_pdfium_import_dir}/pdfium.def"
                -D pdfium.dll -l "${_cnote_pdfium_implib}"
                WORKING_DIRECTORY "${_cnote_pdfium_import_dir}"
                RESULT_VARIABLE _cnote_pdfium_dlltool_result
                ERROR_VARIABLE _cnote_pdfium_dlltool_error)
            if(NOT _cnote_pdfium_dlltool_result EQUAL 0 OR
               NOT EXISTS "${_cnote_pdfium_implib}")
                message(FATAL_ERROR "dlltool failed to create the PDFium MinGW import library: ${_cnote_pdfium_dlltool_error}")
            endif()
        endif()
    endif()

elseif(APPLE)
    set(_cnote_pdfium_implib "")
else()
    set(_cnote_pdfium_implib "")
endif()
if(WIN32 AND MINGW)
    # GNU import archives contain COFF members.  Check their reported
    # architecture using objdump from the selected compiler kit.  This is
    # intentionally not used for MSVC, which need not ship GNU tools.
    find_program(_cnote_pdfium_objdump NAMES objdump objdump.exe
        HINTS "${_cnote_pdfium_compiler_dir}")
    if(NOT _cnote_pdfium_objdump)
        message(FATAL_ERROR "MinGW PDFium import validation requires objdump from the selected compiler kit")
    endif()
    execute_process(COMMAND "${_cnote_pdfium_objdump}" -f "${_cnote_pdfium_implib}"
        RESULT_VARIABLE _cnote_pdfium_objdump_result
        OUTPUT_VARIABLE _cnote_pdfium_objdump_output
        ERROR_VARIABLE _cnote_pdfium_objdump_error)
    if(NOT _cnote_pdfium_objdump_result EQUAL 0)
        message(FATAL_ERROR "Cannot inspect MinGW PDFium import library: ${_cnote_pdfium_objdump_error}")
    endif()
    if(_cnote_pdfium_arch STREQUAL x86_64)
        set(_cnote_pdfium_import_arch_regex "architecture:[ \t]*i386:x86-64")
    elseif(_cnote_pdfium_arch STREQUAL arm64)
        set(_cnote_pdfium_import_arch_regex "architecture:[ \t]*(aarch64|arm64)")
    else()
        set(_cnote_pdfium_import_arch_regex "architecture:[ \t]*i386([^:-]|$)")
    endif()
    if(NOT _cnote_pdfium_objdump_output MATCHES "${_cnote_pdfium_import_arch_regex}")
        message(FATAL_ERROR "PDFium MinGW import library architecture does not match ${_cnote_pdfium_arch}: ${_cnote_pdfium_implib}")
    endif()
endif()

if(TARGET pdfium)
    message(FATAL_ERROR "A pdfium target already exists; refusing to mix PDFium SDK roots")
endif()

# These are the cache variables consumed by the bundled config.  FORCE avoids
# stale paths when CNOTE_PDFIUM_ROOT is changed in an existing build directory.
set(PDFium_DIR "${_cnote_pdfium_root}" CACHE PATH "PDFium package directory" FORCE)
set(PDFium_INCLUDE_DIR "${_cnote_pdfium_include}" CACHE PATH "PDFium include directory" FORCE)
set(PDFium_LIBRARY "${_cnote_pdfium_runtime}" CACHE FILEPATH "PDFium runtime library" FORCE)
if(WIN32)
    set(PDFium_IMPLIB "${_cnote_pdfium_implib}" CACHE FILEPATH "PDFium import library" FORCE)
else()
    unset(PDFium_IMPLIB CACHE)
endif()
unset(PDFium_FOUND CACHE)

find_package(PDFium CONFIG REQUIRED PATHS "${_cnote_pdfium_root}" NO_DEFAULT_PATH)
if(NOT TARGET pdfium)
    message(FATAL_ERROR "PDFiumConfig.cmake did not provide the imported target pdfium")
endif()

# The package config chooses the MSVC import library by default.  Override only
# the selected target's locations so MinGW can use its .dll.a or generated one.
if(WIN32)
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${_cnote_pdfium_runtime}"
        IMPORTED_IMPLIB "${_cnote_pdfium_implib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_cnote_pdfium_include};${_cnote_pdfium_include}/cpp")
else()
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${_cnote_pdfium_runtime}"
        INTERFACE_INCLUDE_DIRECTORIES "${_cnote_pdfium_include};${_cnote_pdfium_include}/cpp")
endif()

set(CNOTE_PDFIUM_VERSION "${CNOTE_PDFIUM_REQUIRED_VERSION}")
mark_as_advanced(PDFium_DIR PDFium_INCLUDE_DIR PDFium_LIBRARY PDFium_IMPLIB)
