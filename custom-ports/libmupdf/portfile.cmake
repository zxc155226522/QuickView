if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ArtifexSoftware/mupdf
    REF "${VERSION}"
    SHA512 cf68c4a7c21ca7f4f854bf523bd01fdf8a9b2921cbb2cab1a6dca03004a800cf8b1300b66a0bc825ff736571a08cd76c1a113d739a9a6f9bda12daae864ab59e
    HEAD_REF master
)

# [mujs] MuPDF's regexp.c includes thirdparty/mujs/regexp.{c,h} from the mujs
# submodule, but the GitHub release tarball does not include submodules.
# Download the mujs 1.3.6 release and extract only the needed regexp files.
vcpkg_download_distfile(MUJS_ARCHIVE
    URLS "https://github.com/ArtifexSoftware/mujs/archive/refs/tags/1.3.6.tar.gz"
    FILENAME "mujs-1.3.6.tar.gz"
    SHA512 63df9155182a2744860a92603c492f744efd30170b0d60b860dfd75c10190123b07c04626ce2e38af9febe4b5982f09ed04ca3dd59a4b99ccb9a0e179b13acc6
)
vcpkg_extract_source_archive_ex(OUT_SOURCE_PATH MUJS_SOURCE_PATH ARCHIVE "${MUJS_ARCHIVE}")
file(MAKE_DIRECTORY "${SOURCE_PATH}/thirdparty/mujs")
# Copy all mujs source files (regexp.c includes utf.c which includes ucd.c)
file(GLOB MUJS_SOURCES "${MUJS_SOURCE_PATH}/*.c" "${MUJS_SOURCE_PATH}/*.h")
file(COPY ${MUJS_SOURCES} DESTINATION "${SOURCE_PATH}/thirdparty/mujs")

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/unofficial-libmupdf-config.cmake.in" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME "unofficial-libmupdf")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/manual-tools")

set(font_licenses "")
foreach(item IN ITEMS urw/OFL.txt noto/COPYING han/LICENSE.txt droid/NOTICE sil/OFL.txt)
    string(REPLACE "/" " " new_name "# Fonts - ${item}")
    set(file "${CURRENT_BUILDTREES_DIR}/${new_name}")
    file(COPY_FILE "${SOURCE_PATH}/resources/fonts/${item}" "${file}")
    list(APPEND font_licenses "${file}")
endforeach()

vcpkg_install_copyright(
    COMMENT [[
This software includes Base 14 PDF fonts from URW, Noto fonts from Google,
Source Han Serif from Adobe for CJK, DroidSansFallback from Android for CJK,
and Charis SIL from SIL.
]]
    FILE_LIST
        "${SOURCE_PATH}/COPYING"
        ${font_licenses}
)
