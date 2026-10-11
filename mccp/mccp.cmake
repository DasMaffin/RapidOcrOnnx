# mccp.cmake - builds ocr.dll, the MCCP native module (cmake -DOCR_OUTPUT=MCCP).
#
# Included from the top-level CMakeLists.txt, which returns right after, so
# everything the MCCP build needs is in this one file:
#   1. static ONNX Runtime and OpenCV from RapidAI's builder releases,
#   2. the PP-OCR models, embedded into the DLL as Windows resources,
#   3. the ocr target (upstream sources + mccp/module.cpp), and
#   4. ocr_host, the test program in tools/host.cpp.
# Every download is pinned by SHA-256; a file that does not match fails the
# configure step. Nothing is downloaded at run time.

if (NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "OCR_OUTPUT=MCCP builds a Windows x64 DLL: use Visual Studio with -A x64.")
endif ()
if (CMAKE_VERSION VERSION_LESS 3.18)
    message(FATAL_ERROR "OCR_OUTPUT=MCCP needs CMake 3.18 or newer (for file(ARCHIVE_EXTRACT)).")
endif ()

set(MCCP_DOWNLOADS "${CMAKE_BINARY_DIR}/mccp-downloads")

# Downloads <url> into MCCP_DOWNLOADS unless a file with that hash is already
# there, and sets <out_var> to its path.
function(mccp_fetch out_var url sha256)
    get_filename_component(name "${url}" NAME)
    set(path "${MCCP_DOWNLOADS}/${name}")
    message(STATUS "MCCP: ${name}")
    file(DOWNLOAD "${url}" "${path}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if (NOT code EQUAL 0)
        message(FATAL_ERROR "MCCP: download failed: ${url}\n${status}")
    endif ()
    set(${out_var} "${path}" PARENT_SCOPE)
endfunction()

# Writes a generated file, leaving it untouched when the content is the same so
# that a re-configure does not rebuild the resources.
function(mccp_write path content)
    file(WRITE "${path}.tmp" "${content}")
    configure_file("${path}.tmp" "${path}" COPYONLY)
endfunction()

# Unpacks an archive once into <dir>.
function(mccp_extract archive dir)
    get_filename_component(name "${archive}" NAME)
    if (NOT EXISTS "${dir}/.extracted-${name}")
        file(REMOVE_RECURSE "${dir}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dir}")
        file(TOUCH "${dir}/.extracted-${name}")
    endif ()
endfunction()

# ---- 1. ONNX Runtime and OpenCV ------------------------------------------------
# The versions upstream's Windows CI (.github/workflows/windows.yml) builds
# with: static libraries, static CRT (/MT), built with VS 2022.

mccp_fetch(ORT_7Z
    https://github.com/RapidAI/OnnxruntimeBuilder/releases/download/1.15.1/onnxruntime-v1.15.1-windows-vs2022-static-mt.7z
    3b02dc5ee8f1e38c8f16c4170e8ec3101a1a33c3fe08c56687c253f7de4febff)
mccp_fetch(OPENCV_7Z
    https://github.com/RapidAI/OpenCVBuilder/releases/download/4.8.1/opencv-4.8.1-windows-vs2022-mt.7z
    9b647b479aad111f685efc0612a533c86c3ed1dc145c6d3c9408430fb9c0a019)
mccp_extract("${ORT_7Z}" "${CMAKE_BINARY_DIR}/onnxruntime")
mccp_extract("${OPENCV_7Z}" "${CMAKE_BINARY_DIR}/opencv")

set(ORT_ROOT "${CMAKE_BINARY_DIR}/onnxruntime/windows-x64")
add_library(mccp_onnxruntime STATIC IMPORTED)
set_target_properties(mccp_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ORT_ROOT}/lib/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${ORT_ROOT}/include")

# Upstream includes <onnxruntime/core/session/onnxruntime_cxx_api.h>, the
# header's place in older ONNX Runtime packages; this one ships it flat. A
# one-line forwarding header keeps the upstream sources unchanged.
set(ORT_SHIM "${CMAKE_BINARY_DIR}/ort-shim")
mccp_write("${ORT_SHIM}/onnxruntime/core/session/onnxruntime_cxx_api.h"
    "#pragma once\n#include <onnxruntime_cxx_api.h>\n")

# Straight at the vc17 static libraries: the pack's own top-level config only
# knows Visual Studio versions up to 2022.
set(OpenCV_STATIC ON)
find_package(OpenCV 4.8.1 EXACT REQUIRED COMPONENTS core imgproc imgcodecs
    PATHS "${CMAKE_BINARY_DIR}/opencv/windows-x64/x64/vc17/staticlib" NO_DEFAULT_PATH)

# Both packages are built with the static CRT; so is everything here.
foreach (flag_var CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE
                  CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO
                  CMAKE_C_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_MINSIZEREL)
    string(REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
endforeach ()

# ---- 2. Models -------------------------------------------------------------------
# PP-OCRv4 ONNX conversions as RapidOCR publishes them (the URLs and hashes of
# RapidOCR's default_models.yaml). Each becomes an RCDATA resource of ocr.dll.

set(MODELS https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0)
set(MCCP_RESOURCES "")
set(MCCP_LANGS "")

mccp_fetch(DET_MODEL ${MODELS}/onnx/PP-OCRv4/det/ch_PP-OCRv4_det_mobile.onnx
    d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9)
string(APPEND MCCP_RESOURCES "DET RCDATA \"${DET_MODEL}\"\n")

# One recognition language: its model and key file, embedded as REC_<CODE> and
# KEYS_<CODE>, and listed by ocr.languages(). Adding a language is one more
# call below; module.cpp needs no change.
macro(mccp_language code model_url model_sha256 keys_url keys_sha256)
    string(TOUPPER "${code}" _up)
    mccp_fetch(_model "${model_url}" ${model_sha256})
    mccp_fetch(_keys "${keys_url}" ${keys_sha256})
    string(APPEND MCCP_RESOURCES "REC_${_up} RCDATA \"${_model}\"\nKEYS_${_up} RCDATA \"${_keys}\"\n")
    list(APPEND MCCP_LANGS "${code}")
endmacro()

# English (PP-OCRv4 en): ASCII only, 95 characters.
mccp_language(en
    ${MODELS}/onnx/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile.onnx
    e8770c967605983d1570cdf5352041dfb68fa0c21664f49f47b155abd3e0e318
    ${MODELS}/paddle/PP-OCRv4/rec/en_PP-OCRv4_rec_mobile/en_dict.txt
    5662df9d2d03f0e8ca0d3b0649d6acbab904b6a14b3d3521463c71c37c668ce3)

# Chinese and English (PP-OCRv4 ch): 6623 characters, among them ’ — é.
option(MCCP_OCR_CH "Embed the Chinese/multilingual recognition model (+11 MB)" ON)
if (MCCP_OCR_CH)
    mccp_language(ch
        ${MODELS}/onnx/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile.onnx
        48fc40f24f6d2a207a2b1091d3437eb3cc3eb6b676dc3ef9c37384005483683b
        ${MODELS}/paddle/PP-OCRv4/rec/ch_PP-OCRv4_rec_mobile/ppocr_keys_v1.txt
        28b2362ad4ab2dc38769aa72feb535e3a9ddb3fd2a7585a05920e6393b1dc7f7)
endif ()

# More languages follow the same pattern, e.g. RapidOCR's PP-OCRv4 entries
# latin_PP-OCRv3_rec_mobile + latin_dict.txt, japan_PP-OCRv4_rec_mobile +
# japan_dict.txt, korean_PP-OCRv4_rec_mobile + korean_dict.txt.

set(MCCP_GEN "${CMAKE_BINARY_DIR}/mccp-generated")
mccp_write("${MCCP_GEN}/models.rc" "// Generated by mccp/mccp.cmake.\n${MCCP_RESOURCES}")
list(TRANSFORM MCCP_LANGS PREPEND "\"" OUTPUT_VARIABLE _quoted)
list(TRANSFORM _quoted APPEND "\"")
list(JOIN _quoted ", " _quoted)
mccp_write("${MCCP_GEN}/languages.h"
    "// Generated by mccp/mccp.cmake: the embedded recognition languages, first is the default.\n#pragma once\nstatic const char *const kLanguages[] = {${_quoted}};\n")

# ---- 3. ocr.dll ------------------------------------------------------------------
# Only the upstream sources detection and recognition need: no main, JNI or C
# API. AngleNet.cpp is compiled because OcrLiteImpl holds one, but it never
# loads a model.

add_library(ocr SHARED
    mccp/module.cpp
    mccp/mccp_module.h
    "${MCCP_GEN}/models.rc"
    src/AngleNet.cpp
    src/clipper.cpp
    src/CrnnNet.cpp
    src/DbNet.cpp
    src/OcrLiteImpl.cpp
    src/OcrUtils.cpp)
target_include_directories(ocr PRIVATE include mccp "${MCCP_GEN}" "${ORT_SHIM}")
target_compile_features(ocr PRIVATE cxx_std_17)
target_compile_definitions(ocr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ocr PRIVATE /utf-8 /EHsc)
target_link_libraries(ocr PRIVATE mccp_onnxruntime ${OpenCV_LIBS})
set_target_properties(ocr PROPERTIES OUTPUT_NAME ocr PREFIX "")

# ---- 4. ocr_host -----------------------------------------------------------------
# Loads ocr.dll like MCCP does and runs it on PNG screenshots (see README).

option(MCCP_OCR_HOST "Build tools/host.cpp, the test program" ON)
if (MCCP_OCR_HOST)
    add_executable(ocr_host tools/host.cpp)
    target_include_directories(ocr_host PRIVATE mccp)
    target_compile_features(ocr_host PRIVATE cxx_std_17)
    target_compile_definitions(ocr_host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(ocr_host PRIVATE /utf-8 /EHsc)
    target_link_libraries(ocr_host PRIVATE ${OpenCV_LIBS})
    add_dependencies(ocr_host ocr)
endif ()
