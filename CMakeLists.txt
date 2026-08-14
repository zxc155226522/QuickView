cmake_minimum_required(VERSION 3.25)

# ============================================================================
# Adaptive Build Tool Discovery (Handled by Toolchain)
# ============================================================================

# Force-define CXX features to bypass CMake's Clang 22 feature detection bug
project(QuickView LANGUAGES C CXX RC)

set(CMAKE_CXX_COMPILE_FEATURES "cxx_std_11;cxx_std_14;cxx_std_17;cxx_std_20;cxx_std_23;cxx_std_26" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX98_COMPILE_FEATURES "cxx_template_template_parameters" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX11_COMPILE_FEATURES "cxx_alias_templates;cxx_alignas;cxx_alignof;cxx_attributes;cxx_auto_type;cxx_constexpr;cxx_decltype;cxx_defaulted_functions;cxx_defaulted_move_initializers;cxx_delegating_constructors;cxx_deleted_functions;cxx_explicit_conversions;cxx_extended_friend_declarations;cxx_extern_templates;cxx_final;cxx_func_identifier;cxx_generalized_initializers;cxx_inheriting_constructors;cxx_inline_namespaces;cxx_lambdas;cxx_local_type_template_args;cxx_long_long_type;cxx_noexcept;cxx_nonstatic_member_init;cxx_nullptr;cxx_override;cxx_range_for;cxx_raw_string_literals;cxx_reference_qualified_functions;cxx_right_angle_brackets;cxx_rvalue_references;cxx_sizeof_member;cxx_static_assert;cxx_strong_enums;cxx_thread_local;cxx_trailing_return_types;cxx_unicode_literals;cxx_uniform_initialization;cxx_unrestricted_unions;cxx_user_literals;cxx_variadic_macros;cxx_variadic_templates" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX14_COMPILE_FEATURES "cxx_aggregate_default_initializers;cxx_attribute_deprecated;cxx_binary_literals;cxx_contextual_conversions;cxx_decltype_auto;cxx_digit_separators;cxx_generic_lambdas;cxx_lambda_init_captures;cxx_local_type_template_args;cxx_relaxed_constexpr;cxx_return_type_deduction;cxx_variable_templates" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX17_COMPILE_FEATURES "cxx_std_17" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX20_COMPILE_FEATURES "cxx_std_20" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX23_COMPILE_FEATURES "cxx_std_23" CACHE INTERNAL "" FORCE)
set(CMAKE_CXX26_COMPILE_FEATURES "cxx_std_26" CACHE INTERNAL "" FORCE)

# Set normal variables to override the compiler detection's empty normal variables
set(CMAKE_CXX_COMPILE_FEATURES "${CMAKE_CXX_COMPILE_FEATURES}")
set(CMAKE_CXX98_COMPILE_FEATURES "${CMAKE_CXX98_COMPILE_FEATURES}")
set(CMAKE_CXX11_COMPILE_FEATURES "${CMAKE_CXX11_COMPILE_FEATURES}")
set(CMAKE_CXX14_COMPILE_FEATURES "${CMAKE_CXX14_COMPILE_FEATURES}")
set(CMAKE_CXX17_COMPILE_FEATURES "${CMAKE_CXX17_COMPILE_FEATURES}")
set(CMAKE_CXX20_COMPILE_FEATURES "${CMAKE_CXX20_COMPILE_FEATURES}")
set(CMAKE_CXX23_COMPILE_FEATURES "${CMAKE_CXX23_COMPILE_FEATURES}")
set(CMAKE_CXX26_COMPILE_FEATURES "${CMAKE_CXX26_COMPILE_FEATURES}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force Static CRT for single-binary portability
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ensure headers from vcpkg-imported targets are treated as SYSTEM to suppress their warnings
set(CMAKE_NO_SYSTEM_FROM_IMPORTED FALSE)

# Automatically handle vcpkg (if not specified via command line)
if(NOT CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# ============================================================================
# Options & Matrix Build Logic
# ============================================================================
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)
option(ENABLE_LTO  "Enable Link Time Optimization (ThinLTO)" OFF)

# ============================================================================
# Source Files Definition
# ============================================================================
set(QUICKVIEW_SOURCES
    QuickView/main.cpp
    QuickView/ThumbnailWorker.cpp
    QuickView/ThumbnailWorker.h
    QuickView/PrintManager.cpp
    QuickView/PrintPreviewUI.cpp
    QuickView/PrintManager.h
    QuickView/FileNavigator.cpp
    QuickView/AppContext.cpp
    QuickView/CompareController.cpp
    QuickView/ImageViewportLayout.cpp
    QuickView/DialogController.cpp
    QuickView/ZoomAnimation.cpp
    QuickView/ColorMath.cpp
    QuickView/pch.cpp
    QuickView/AppStrings.cpp
    QuickView/GeekGlass.cpp
    QuickView/GeekContextMenu.cpp
    QuickView/GeekIconLibrary.cpp
    QuickView/GeekIconRenderer.cpp
    QuickView/GeekIconData.cpp
    QuickView/exif.cpp
    QuickView/RenderEngine.cpp
    QuickView/ImageLoader.cpp
    QuickView/MiniTiff.cpp
    QuickView/MiniTiffLzw.cpp
    QuickView/MiniTiffCmyk.cpp
    QuickView/LosslessTransform.cpp
    QuickView/StbLoader.cpp
    QuickView/TinyExrLoader.cpp
    QuickView/WuffsImpl.cpp
    QuickView/ArchiveVFS.cpp
    QuickView/ContextMenu.cpp
    QuickView/GalleryOverlay.cpp
    QuickView/ThumbnailManager.cpp
    QuickView/Toolbar.cpp
    QuickView/SettingsOverlay.cpp
    QuickView/ColorPickerPopup.cpp
    QuickView/HelpOverlay.cpp
    QuickView/UpdateManager.cpp
    QuickView/PreviewExtractor.cpp
    QuickView/ImageEngine.cpp
    QuickView/CompositionEngine.cpp
    QuickView/UIRenderer.cpp
    QuickView/HeavyLanePool.cpp
    QuickView/TileMemoryManager.cpp
    QuickView/TileManager.cpp
    QuickView/TileScheduler.cpp
    QuickView/ComputeEngine.cpp
    QuickView/DisplayColorInfo.cpp
    QuickView/ProcessRouter.cpp
    QuickView/ThemeSystem.cpp
    QuickView/ImageLoaderSimd.cpp
    QuickView/WebPAnimator.cpp
    QuickView/AvifAnimator.cpp
    QuickView/JxlAnimator.cpp
    QuickView/QuickViewETW.cpp
    QuickView/MuPdfDocument.cpp
    QuickView/DocumentRenderController.cpp
    QuickView/VectorLoader.cpp
    QuickView/ThumbDiskCache.cpp
    QuickView/ThumbDiskCache.h
    

    # Third party manually included
    third_party/yyjson/yyjson.c
    
    # Resource files
    QuickView/QuickView.rc
)

# ============================================================================
# Executable Target
# ============================================================================
add_subdirectory(third_party/unrar-mini)

add_executable(QuickView WIN32 ${QUICKVIEW_SOURCES})

# Explicitly set the subsystem and machine type
set(QUICKVIEW_LINK_FLAGS "/SUBSYSTEM:WINDOWS")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(QUICKVIEW_LINK_FLAGS "${QUICKVIEW_LINK_FLAGS} /machine:ARM64")
else()
    set(QUICKVIEW_LINK_FLAGS "${QUICKVIEW_LINK_FLAGS} /machine:x64")
endif()
set_target_properties(QuickView PROPERTIES LINK_FLAGS "${QUICKVIEW_LINK_FLAGS}")

# Include paths
target_include_directories(QuickView PRIVATE QuickView third_party/yyjson third_party/unrar-mini third_party/resvg/include)

# Precompiled Headers (PCH) - CXX only to avoid conflict with C source (yyjson.c)
target_precompile_headers(QuickView PRIVATE $<$<COMPILE_LANGUAGE:CXX>:pch.h>)

# Disable PCH for specific files (as defined in .vcxproj)
set_source_files_properties(QuickView/WuffsImpl.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(QuickView/ImageLoaderSimd.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)
set_source_files_properties(third_party/yyjson/yyjson.c PROPERTIES SKIP_PRECOMPILED_HEADERS ON)

# ============================================================================
# Compiler & Linker Flags (Clang-CL Matrix)
# ============================================================================
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Common strict warnings, encoding, standard conformance, and function-level linking
    target_compile_options(QuickView PRIVATE /W4 /WX /utf-8 /Gw /Gy /permissive- /clang:-Wno-cast-function-type-mismatch)
    
    # [Optimization] Disable compiler identification string to save bytes
    target_compile_options(QuickView PRIVATE /clang:-fno-ident)

    # [Stability] Lock MSVC compatibility version to prevent PCH mismatch in mixed environments
    if(QUICKVIEW_MSVC_COMPAT_VER)
        add_compile_options("/clang:-fms-compatibility-version=${QUICKVIEW_MSVC_COMPAT_VER}")
    endif()

    # Disable automatic manifest generation (we provide it via .rc)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
    
    if(ENABLE_ASAN)
        message(STATUS "Build Matrix: Enabling ASan (Debug-ASan Mode)")
        set(ASAN_FLAGS "-fsanitize=address" "/Oy-" "-D_DISABLE_STRING_ANNOTATION" "-D_DISABLE_VECTOR_ANNOTATION")
        # Keep precise floating point and default security checks in ASan environment
        target_compile_options(QuickView PRIVATE ${ASAN_FLAGS} /Od /fp:precise)
        
        # [ASAN FIX] Manually link ASan runtime for lld-link
        if(DEFINED ENV{LLVM_INSTALL_DIR} AND DEFINED ENV{LLVM_VERSION})
            set(ASAN_LIB_DIR "$ENV{LLVM_INSTALL_DIR}/lib/clang/$ENV{LLVM_VERSION}/lib/windows")
            target_link_directories(QuickView PRIVATE ${ASAN_LIB_DIR})
        endif()

        # [ARM64 FIX] Detect architecture for ASan library naming
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
                set(ASAN_ARCH "aarch64")
            else()
                set(ASAN_ARCH "x86_64")
            endif()
        else()
            set(ASAN_ARCH "i386")
        endif()

        target_link_libraries(QuickView PRIVATE 
            clang_rt.asan_dynamic-${ASAN_ARCH}.lib 
            clang_rt.asan_dynamic_runtime_thunk-${ASAN_ARCH}.lib
        )
    endif()

    if(ENABLE_LTO)
        message(STATUS "Build Matrix: Enabling Full LTO (Release-LTO Mode)")
        # Peak performance flags: 
        # /fp:fast (maximum floating point speed) 
        # /GS- (disable security checks for raw performance)
        # /EHs-c- (disable exceptions)
        target_compile_options(QuickView PRIVATE 
            /O2 /Gw /Zc:inline /GR- /fp:fast /GS- /EHs-c- /Z7
            /clang:-flto=full /clang:-mllvm /clang:-inline-threshold=250
            /clang:-fwhole-program-vtables
            /clang:-fno-ident)
        target_link_options(QuickView PRIVATE /DEBUG /OPT:REF /OPT:ICF /opt:lldlto=3 "/MAP:${CMAKE_BINARY_DIR}/QuickView.map")
    endif()
endif()

# ============================================================================
# Manifest & Entry Point
# ============================================================================
set_target_properties(QuickView PROPERTIES 
    WIN32_EXECUTABLE TRUE
)
target_compile_definitions(QuickView PRIVATE UNICODE _UNICODE)
# Note: Manifest is handled via QuickView.rc

# ============================================================================
# Third-Party Subprojects (Source-Integrated Static Libraries)
# ============================================================================
add_subdirectory(third_party/librevenge)
add_subdirectory(third_party/libcdr)

# ============================================================================
# Dependencies (vcpkg)
# ============================================================================
# libavif
find_package(libavif CONFIG REQUIRED)

# highway
find_package(hwy CONFIG REQUIRED)

# LibRaw
find_package(libraw CONFIG REQUIRED)

# libjxl (manual find since vcpkg port lacks config)
find_library(JXL_LIB_RELEASE NAMES jxl PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(JXL_LIB_DEBUG NAMES jxl PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIB_RELEASE NAMES jxl_threads PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(JXL_THREADS_LIB_DEBUG NAMES jxl_threads PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(JXL_CMS_LIB_RELEASE NAMES jxl_cms PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(JXL_CMS_LIB_DEBUG NAMES jxl_cms PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_path(JXL_INC jxl/decode.h REQUIRED)

add_library(libjxl::jxl STATIC IMPORTED)
set_target_properties(libjxl::jxl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${JXL_INC}"
    INTERFACE_COMPILE_DEFINITIONS "JXL_STATIC_DEFINE"
)
set_property(TARGET libjxl::jxl PROPERTY IMPORTED_LOCATION_RELEASE "${JXL_LIB_RELEASE}")
set_property(TARGET libjxl::jxl PROPERTY IMPORTED_LOCATION_DEBUG "${JXL_LIB_DEBUG}")

add_library(libjxl::jxl_threads STATIC IMPORTED)
set_property(TARGET libjxl::jxl_threads PROPERTY IMPORTED_LOCATION_RELEASE "${JXL_THREADS_LIB_RELEASE}")
set_property(TARGET libjxl::jxl_threads PROPERTY IMPORTED_LOCATION_DEBUG "${JXL_THREADS_LIB_DEBUG}")

add_library(libjxl::jxl_cms STATIC IMPORTED)
set_property(TARGET libjxl::jxl_cms PROPERTY IMPORTED_LOCATION_RELEASE "${JXL_CMS_LIB_RELEASE}")
set_property(TARGET libjxl::jxl_cms PROPERTY IMPORTED_LOCATION_DEBUG "${JXL_CMS_LIB_DEBUG}")

# Brotli (required by libjxl)
find_library(BROTLI_COMMON_LIB_RELEASE NAMES brotlicommon PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(BROTLI_COMMON_LIB_DEBUG NAMES brotlicommon PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIB_RELEASE NAMES brotlidec PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(BROTLI_DEC_LIB_DEBUG NAMES brotlidec PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)

# dav1d (required by avif)
find_library(DAV1D_LIB_RELEASE NAMES dav1d PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(DAV1D_LIB_DEBUG NAMES dav1d PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)

# lcms2
find_package(lcms2 CONFIG REQUIRED)

# WebP
find_package(WebP CONFIG REQUIRED)

# libjpeg-turbo
find_package(libjpeg-turbo CONFIG REQUIRED)

# libyuv
find_package(libyuv CONFIG REQUIRED)

# ZLIB
find_package(ZLIB REQUIRED)

# libmupdf (MuPDF - PDF/AI rendering)
find_package(unofficial-libmupdf CONFIG REQUIRED)

# libdxfrw (DXF/DWG vector format support)
find_package(libdxfrw CONFIG REQUIRED)

# Create targets for Brotli and Dav1d
add_library(brotli::common STATIC IMPORTED)
set_property(TARGET brotli::common PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_COMMON_LIB_RELEASE}")
set_property(TARGET brotli::common PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_COMMON_LIB_DEBUG}")

add_library(brotli::dec STATIC IMPORTED)
set_property(TARGET brotli::dec PROPERTY IMPORTED_LOCATION_RELEASE "${BROTLI_DEC_LIB_RELEASE}")
set_property(TARGET brotli::dec PROPERTY IMPORTED_LOCATION_DEBUG "${BROTLI_DEC_LIB_DEBUG}")

add_library(dav1d::dav1d STATIC IMPORTED)
set_property(TARGET dav1d::dav1d PROPERTY IMPORTED_LOCATION_RELEASE "${DAV1D_LIB_RELEASE}")
set_property(TARGET dav1d::dav1d PROPERTY IMPORTED_LOCATION_DEBUG "${DAV1D_LIB_DEBUG}")

# resvg C-API：Rust 静态库（预编译于 third_party/resvg/lib/resvg.lib）。
# 用 IMPORTED 目标并指向 junction 源目录（CMAKE_SOURCE_DIR=E:/qv_build_tmp），
# 既避免 CMake 把 "resvg.lib" 当成库名又补一次 .lib 变成 "resvg.lib.lib"，
# 也避免真实中文路径泄漏进链接响应文件。
add_library(resvg_static STATIC IMPORTED)
set_property(TARGET resvg_static PROPERTY IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/third_party/resvg/lib/resvg.lib")

target_link_libraries(QuickView PRIVATE
    avif
    libjpeg-turbo::turbojpeg-static
    hwy::hwy
    libjxl::jxl
    libjxl::jxl_threads
    libjxl::jxl_cms
    brotli::common
    brotli::dec
    dav1d::dav1d
    lcms2::lcms2
    WebP::webpdemux
    WebP::webpdecoder
    libraw::raw
    yuv
    ZLIB::ZLIB
    unrar-mini
    librevenge
    libcdr
    resvg_static
    unofficial::libmupdf::libmupdf
    libdxfrw::libdxfrw
    # Windows system libraries
    imm32.lib
    dwrite.lib
    windowscodecs.lib
    shlwapi.lib
    shell32.lib
    mscms.lib
    # resvg C-API 是 Rust 静态库，其 Rust std 依赖以下 Windows 导入库（否则 lld-link 报 undefined symbol）
    ntdll.lib
    userenv.lib
    advapi32.lib
    kernel32.lib
    user32.lib
    gdi32.lib
    ws2_32.lib
    bcrypt.lib
    crypt32.lib
    iphlpapi.lib
    normaliz.lib
)

# ============================================================================
# Unit Tests (Optional - requires gtest)
# ============================================================================
find_package(GTest CONFIG QUIET)
if(GTest_FOUND)
    enable_testing()

    add_executable(QuickViewTests
        tests/ColorMathTests.cpp
        tests/FileNavigatorTests.cpp
        tests/HotkeyTests.cpp
        tests/StringUtilsTests.cpp
        tests/SupportedExtensionsTests.cpp
        tests/MiniTiffTests.cpp
        QuickView/ImageLoaderSimd.cpp
        QuickView/MiniTiff.cpp
        QuickView/MiniTiffLzw.cpp
        QuickView/MiniTiffCmyk.cpp
        QuickView/ColorMath.cpp 
        QuickView/FileNavigator.cpp 
        QuickView/ArchiveVFS.cpp 
        QuickView/exif.cpp
        QuickView/QuickViewETW.cpp
        QuickView/pch.cpp
    )
    target_precompile_headers(QuickViewTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:pch.h>)
    target_link_libraries(QuickViewTests PRIVATE 
        GTest::gtest_main 
        GTest::gtest 
        d2d1.lib 
        dwrite.lib 
        windowscodecs.lib 
        dxgi.lib 
        d3d11.lib 
        shlwapi.lib 
        shell32.lib 
        mscms.lib
        unrar-mini
        ZLIB::ZLIB
    )
    target_include_directories(QuickViewTests PRIVATE QuickView third_party/unrar-mini)
    target_compile_definitions(QuickViewTests PRIVATE UNICODE _UNICODE)
    add_test(NAME QuickViewTests COMMAND QuickViewTests)
endif()

# ============================================================================
# IDE IntelliSense & Build Environment Helper (Inject Adaptive SDK Include Paths)
# ============================================================================
if(WIN32 AND ADAPTIVE_MSVC_INCLUDE_PATHS)
    target_include_directories(QuickView SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    if(TARGET unrar-mini)
        target_include_directories(unrar-mini SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    endif()
    if(TARGET QuickViewTests)
        target_include_directories(QuickViewTests SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    endif()
    if(TARGET QuickViewThumbnailProvider)
        target_include_directories(QuickViewThumbnailProvider SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
    endif()
endif()

# ============================================================================
# QuickViewThumbnailProvider COM DLL
# Windows Shell IThumbnailProvider: thin out-of-process shim that delegates
# rendering to "QuickView.exe --thumbnail" (full app pipeline reuse).
# ============================================================================
add_library(QuickViewThumbnailProvider SHARED
    QuickView/ThumbnailProvider/ThumbnailProvider.cpp
    QuickView/ThumbDiskCache.cpp
    QuickView/ThumbnailProvider/ThumbnailProvider.def
)
set_target_properties(QuickViewThumbnailProvider PROPERTIES
    OUTPUT_NAME "QuickViewThumbnailProvider"
    PREFIX ""
    LINK_FLAGS ""
)
target_include_directories(QuickViewThumbnailProvider PRIVATE
    QuickView
)
target_compile_options(QuickViewThumbnailProvider PRIVATE
    /EHsc /GR /W4 /utf-8 /Gw
    $<$<CONFIG:Release>:/O2 /Gy>
)
target_link_libraries(QuickViewThumbnailProvider PRIVATE
    ole32.lib
    oleaut32.lib
    shell32.lib
    shlwapi.lib
    user32.lib
    gdi32.lib
    gdiplus.lib
)
target_compile_definitions(QuickViewThumbnailProvider PRIVATE
    UNICODE _UNICODE
)
# Inject MSVC/Windows SDK include paths (required for windows.h etc.)
if(ADAPTIVE_MSVC_INCLUDE_PATHS)
    target_include_directories(QuickViewThumbnailProvider SYSTEM PRIVATE ${ADAPTIVE_MSVC_INCLUDE_PATHS})
endif()

# Force-define CXX features to bypass CMake's Clang 22 feature detection bug
