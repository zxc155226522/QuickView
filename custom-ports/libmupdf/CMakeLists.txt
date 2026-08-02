cmake_minimum_required(VERSION 3.25)
project(libmupdf LANGUAGES C)

find_package(freetype CONFIG REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(OpenJPEG CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

find_path(JBIG2DEC_INCLUDE_DIR NAMES jbig2.h REQUIRED)
find_library(JBIG2DEC_LIBRARY NAMES jbig2decd jbig2dec NAMES_PER_DIR REQUIRED)

if(CMAKE_HOST_WIN32 AND WIN32)
    add_executable(bin2coff EXCLUDE_FROM_ALL scripts/bin2coff.c)

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86")
        set(coff_arch "Win32")
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
        set(coff_arch "aarch64")
    else()
        set(coff_arch "x64")
    endif()
endif()

file(GLOB fonts
    "${PROJECT_SOURCE_DIR}/resources/fonts/urw/*.cff"
    "${PROJECT_SOURCE_DIR}/resources/fonts/han/*.ttc"
    "${PROJECT_SOURCE_DIR}/resources/fonts/droid/DroidSansFallbackFull.ttf"
    "${PROJECT_SOURCE_DIR}/resources/fonts/droid/DroidSansFallback.ttf"
    "${PROJECT_SOURCE_DIR}/resources/fonts/noto/*.otf"
    "${PROJECT_SOURCE_DIR}/resources/fonts/noto/*.ttf"
    "${PROJECT_SOURCE_DIR}/resources/fonts/sil/*.cff"
)

set(FONT_SOURCES "")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")
foreach(font IN LISTS fonts)
    cmake_path(GET font FILENAME name)
    if(CMAKE_HOST_WIN32 AND WIN32)
        string(REGEX REPLACE "[-.]" "_" identifier "_binary_${name}")
        set(font_object "${CMAKE_CURRENT_BINARY_DIR}/resources/${name}.obj")
        set_source_files_properties("${font_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        add_custom_command(
            OUTPUT "${font_object}"
            COMMAND $<TARGET_FILE:bin2coff> "${font}" "${font_object}" "${identifier}" ${coff_arch}
            DEPENDS bin2coff "${font}"
            VERBATIM
        )
        list(APPEND FONT_SOURCES "${font_object}")
    endif()
endforeach()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "source/fitz/*.c"
    "source/pdf/*.c"
    "source/svg/*.c"
    "source/cbz/*.c"
)

list(FILTER SOURCES EXCLUDE REGEX "/fitz/tessocr\\.cpp$")

add_library(libmupdf STATIC ${SOURCES} ${FONT_SOURCES})
set_target_properties(libmupdf PROPERTIES PREFIX "")

# [Clang-CL Fix] Enable SSE4.1 for deskew_sse.h intrinsics (_mm_cvtepu8_epi32, _mm_mullo_epi32)
# clang-cl does not enable SSE4.1 by default (only SSE2 for x64), causing compilation failures.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(libmupdf PRIVATE /clang:-msse4.1 /clang:-mavx)
endif()

target_include_directories(libmupdf
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${JBIG2DEC_INCLUDE_DIR}
)

target_compile_definitions(libmupdf
    PRIVATE
        FZ_ENABLE_BROTLI=0
        FZ_ENABLE_DOCX_OUTPUT=0
        FZ_ENABLE_ODT_OUTPUT=0
        FZ_ENABLE_OCR_OUTPUT=0
        FZ_ENABLE_JS=0
        FZ_ENABLE_BARCODE=0
        FZ_ENABLE_XPS=0
        FZ_ENABLE_HTML=0
        FZ_ENABLE_EPUB=0
        FZ_ENABLE_FB2=0
        FZ_ENABLE_MOBI=0
        FZ_ENABLE_OFFICE=0
        FZ_ENABLE_TXT=0
        FZ_ENABLE_HTML_ENGINE=0
        FZ_ENABLE_SVG=1
        FZ_ENABLE_CBZ=1
        FZ_ENABLE_IMG=1
        FZ_ENABLE_PDF=1
        FZ_ENABLE_ICC=1
        FZ_ENABLE_JPX=1
        FZ_ENABLE_SPOT_RENDERING=1
        FZ_PLOTTERS_G=1
        FZ_PLOTTERS_RGB=1
        FZ_PLOTTERS_CMYK=1
        FZ_PLOTTERS_N=1
        OCR_DISABLED
        MEMENTO_MUPDF_HACKS
        SHARE_JPEG
)

target_link_libraries(libmupdf
    PUBLIC
        freetype
        lcms2::lcms2
        ${JBIG2DEC_LIBRARY}
        JPEG::JPEG
        openjp2
        ZLIB::ZLIB
)

install(DIRECTORY include/mupdf DESTINATION include)
install(TARGETS libmupdf
    EXPORT unofficial-libmupdf-targets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(EXPORT unofficial-libmupdf-targets
    NAMESPACE unofficial::libmupdf::
    DESTINATION share/unofficial-libmupdf
)
configure_file(unofficial-libmupdf-config.cmake.in unofficial-libmupdf-config.cmake @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-libmupdf-config.cmake"
    DESTINATION share/unofficial-libmupdf
)
