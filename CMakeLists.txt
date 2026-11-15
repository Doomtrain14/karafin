cmake_minimum_required(VERSION 3.24)

project(karafin VERSION 1.0.0 LANGUAGES C CXX)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Karafin targets Nintendo Switch only. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake (or just run ./build.ps1).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(KARAFIN_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/build/third_party" CACHE PATH
    "Vendored headers fetched by build.ps1 (json.hpp, stb_image.h, cacert.pem)")
foreach(_f json.hpp stb_image.h cacert.pem)
    if(NOT EXISTS "${KARAFIN_THIRD_PARTY_DIR}/${_f}")
        message(FATAL_ERROR "Missing ${KARAFIN_THIRD_PARTY_DIR}/${_f}; run ./build.ps1 to fetch vendored deps")
    endif()
endforeach()

# Third-party headers live in build/third_party; expose them as <nlohmann/json.hpp> and <stb_image.h>.
set(KARAFIN_VENDOR_INCLUDE "${CMAKE_BINARY_DIR}/vendor/include")
configure_file("${KARAFIN_THIRD_PARTY_DIR}/json.hpp" "${KARAFIN_VENDOR_INCLUDE}/nlohmann/json.hpp" COPYONLY)
configure_file("${KARAFIN_THIRD_PARTY_DIR}/stb_image.h" "${KARAFIN_VENDOR_INCLUDE}/stb_image.h" COPYONLY)

# -----------------------------------------------------------------------------------------------
# Dependencies via pkg-config (static). Everything is a static archive, so the whole set is
# linked inside one --start-group/--end-group to tolerate circular deps (freetype <-> harfbuzz,
# ffmpeg <-> mbedtls/ssh2, mesa <-> drm_nouveau).

find_package(PkgConfig REQUIRED)
pkg_check_modules(KDEPS REQUIRED
    mpv
    libavformat libavcodec libavfilter libavutil libswscale libswresample
    libass
    sdl2
    egl glesv2 glapi libdrm_nouveau
    libcurl mbedtls mbedx509 mbedcrypto
    freetype2 harfbuzz
    libwebp
    libpng libjpeg
    zlib
)

# -----------------------------------------------------------------------------------------------
# Sources: every .cpp/.c under src/ is compiled. Parallel module work never touches this file.

file(GLOB_RECURSE KARAFIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.c"
)

add_executable(karafin ${KARAFIN_SOURCES})

# Some portlib .pc files carry Makefile-style "$(DEVKITPRO)" paths; drop those.
set(_kdeps_includes ${KDEPS_STATIC_INCLUDE_DIRS})
list(FILTER _kdeps_includes EXCLUDE REGEX "\\$\\(")
target_include_directories(karafin PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_include_directories(karafin SYSTEM PRIVATE "${KARAFIN_VENDOR_INCLUDE}" ${_kdeps_includes})

target_compile_definitions(karafin PRIVATE
    KARAFIN_VERSION="${PROJECT_VERSION}"
    KARAFIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    KARAFIN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    KARAFIN_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    $<$<CONFIG:Debug>:KARAFIN_DEBUG=1>
)

# Drop flags pkg-config repeats from the platform toolchain (and the path half of sdl2's split
# "-isystem <dir>"); keep defines like -DCURL_STATICLIB.
set(_kdeps_cflags ${KDEPS_STATIC_CFLAGS_OTHER})
list(FILTER _kdeps_cflags INCLUDE REGEX "^-")
list(FILTER _kdeps_cflags EXCLUDE REGEX "^-(march|mtune|mtp|ftls-model|isystem|I)")
list(REMOVE_DUPLICATES _kdeps_cflags)
target_compile_options(karafin PRIVATE ${_kdeps_cflags})

target_compile_options(karafin PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-missing-field-initializers>
    $<$<COMPILE_LANGUAGE:C>:-Wall>
)

set(_kdeps_libs ${KDEPS_STATIC_LDFLAGS})
list(FILTER _kdeps_libs EXCLUDE REGEX "^-(L|fPIE$|pthread$)")
set(_kdeps_libdirs ${KDEPS_STATIC_LIBRARY_DIRS})
list(FILTER _kdeps_libdirs EXCLUDE REGEX "\\$\\(")
target_link_directories(karafin PRIVATE ${_kdeps_libdirs})
target_link_libraries(karafin PRIVATE
    -Wl,--start-group ${_kdeps_libs} -lstdc++ -lnx -lm -Wl,--end-group
    -pthread
)
target_link_options(karafin PRIVATE -Wl,--gc-sections -Wl,-Map,${CMAKE_BINARY_DIR}/karafin.map)

# -----------------------------------------------------------------------------------------------
# romfs: romfs/ from the repo plus the CA bundle, staged into the build tree.

set(KARAFIN_ROMFS_STAGE "${CMAKE_BINARY_DIR}/romfs")
set(KARAFIN_ROMFS_STAMP "${CMAKE_BINARY_DIR}/romfs.stamp")
file(GLOB_RECURSE KARAFIN_ROMFS_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/romfs/*")
add_custom_command(
    OUTPUT "${KARAFIN_ROMFS_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${KARAFIN_ROMFS_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/romfs" "${KARAFIN_ROMFS_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KARAFIN_ROMFS_STAGE}/certs"
    COMMAND "${CMAKE_COMMAND}" -E copy "${KARAFIN_THIRD_PARTY_DIR}/cacert.pem" "${KARAFIN_ROMFS_STAGE}/certs/cacert.pem"
    COMMAND "${CMAKE_COMMAND}" -E touch "${KARAFIN_ROMFS_STAMP}"
    DEPENDS ${KARAFIN_ROMFS_FILES} "${KARAFIN_THIRD_PARTY_DIR}/cacert.pem"
    COMMENT "Staging romfs"
    VERBATIM
)
add_custom_target(karafin_romfs DEPENDS "${KARAFIN_ROMFS_STAMP}")
# Same properties dkp_add_asset_target sets, so nx_create_nro depends on the staged files.
set_target_properties(karafin_romfs PROPERTIES
    DKP_ASSET_FOLDER "${KARAFIN_ROMFS_STAGE}"
    DKP_ASSET_FILES "${KARAFIN_ROMFS_STAMP}"
)

# -----------------------------------------------------------------------------------------------
# NRO

nx_generate_nacp(karafin.nacp
    NAME "Karafin"
    AUTHOR "Karafin"
    VERSION "${PROJECT_VERSION}"
)

nx_create_nro(karafin
    OUTPUT "${CMAKE_BINARY_DIR}/karafin.nro"
    ICON "${CMAKE_SOURCE_DIR}/assets/icon.jpg"
    NACP karafin.nacp
    ROMFS karafin_romfs
)
