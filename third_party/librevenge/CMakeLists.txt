# ============================================================================
# librevenge - Static library for QuickView
# Combines core + stream libraries (SVG generator is in core)
# Inherits compiler settings from parent project (no project() call)
# ============================================================================

set(LIBREVENGE_SOURCES
    # Core library (includes RVNGSVGDrawingGenerator)
    src/lib/RVNGBinaryData.cpp
    src/lib/RVNGMemoryStream.cpp
    src/lib/RVNGProperty.cpp
    src/lib/RVNGPropertyList.cpp
    src/lib/RVNGPropertyListVector.cpp
    src/lib/RVNGString.cpp
    src/lib/RVNGStringVector.cpp
    src/lib/RVNGSVGDrawingGenerator.cpp
    # Stream library (file/OLE/Zip streams)
    src/lib/RVNGStreamImplementation.cpp
    src/lib/RVNGDirectoryStream.cpp
    src/lib/RVNGOLEStream.cpp
    src/lib/RVNGZipStream.cpp
)

add_library(librevenge STATIC ${LIBREVENGE_SOURCES})

target_include_directories(librevenge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_definitions(librevenge PRIVATE
    LIBREVENGE_BUILD
    LIBREVENGE_STREAM_BUILD
    BOOST_ERROR_CODE_HEADER_ONLY
)

# Generate Windows compat header in build dir (defines S_ISREG/S_ISDIR)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/win_compat.h"
"#pragma once\n"
"#ifndef S_ISREG\n#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)\n#endif\n"
"#ifndef S_ISDIR\n#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)\n#endif\n"
)

# Suppress warnings; downgrade to C++17 (shared_ptr::unique() removed in C++20+);
# Force-include win_compat.h for POSIX stat macros on Windows.
target_compile_options(librevenge PRIVATE
    /W0
    /clang:-std=c++17
    /FIwin_compat.h
)
target_include_directories(librevenge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Inject MSVC system include paths for Clang-cl (needed for stdio.h etc.)
if(ADAPTIVE_MSVC_INCLUDE_PATHS)
    target_include_directories(librevenge SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
endif()

# zlib for ZipStream
find_package(ZLIB REQUIRED)
target_link_libraries(librevenge PRIVATE ZLIB::ZLIB)
