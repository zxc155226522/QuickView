# ============================================================================
# libcdr - Static library for QuickView
# Parses CorelDRAW (.cdr) and Corel Exchange (.cmx) files
# Inherits compiler settings from parent project (no project() call)
# ============================================================================

set(LIBCDR_SOURCES
    # Internal library
    src/lib/CDRCollector.cpp
    src/lib/CDRContentCollector.cpp
    src/lib/CDRInternalStream.cpp
    src/lib/CDROutputElementList.cpp
    src/lib/CDRParser.cpp
    src/lib/CDRPath.cpp
    src/lib/CDRStylesCollector.cpp
    src/lib/CDRTransforms.cpp
    src/lib/CDRTypes.cpp
    src/lib/CMXParser.cpp
    src/lib/CommonParser.cpp
    src/lib/libcdr_utils.cpp
    # Public library
    src/lib/CDRDocument.cpp
    src/lib/CMXDocument.cpp
)

add_library(libcdr STATIC ${LIBCDR_SOURCES})

target_include_directories(libcdr PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
# ICU stub headers (charset detection disabled on Windows)
target_include_directories(libcdr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/icu_stub
)

target_compile_definitions(libcdr PRIVATE
    LIBCDR_BUILD=1
    BOOST_ERROR_CODE_HEADER_ONLY
    BOOST_SYSTEM_NO_DEPRECATED
)

# Suppress warnings for third-party code
target_compile_options(libcdr PRIVATE /W0)

# Inject MSVC system include paths for Clang-cl (needed for stdio.h etc.)
if(ADAPTIVE_MSVC_INCLUDE_PATHS)
    target_include_directories(libcdr SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
endif()

# zlib (libcdr's CDRInternalStream uses zlib for decompression)
find_package(ZLIB REQUIRED)
target_link_libraries(libcdr PUBLIC librevenge ZLIB::ZLIB)
