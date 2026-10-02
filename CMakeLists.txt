cmake_minimum_required(VERSION 3.16)
project(iso2rvz VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ISO2RVZ_STATIC "Link libstdc++/libgcc statically" ON)

# The bundled externals' CMake files come from Dolphin, which provides this helper.
function(dolphin_disable_warnings target)
  get_target_property(type ${target} TYPE)
  if(NOT type STREQUAL "INTERFACE_LIBRARY")
    target_compile_options(${target} PRIVATE -w)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Bundled third-party libraries (all static)
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/fmt EXCLUDE_FROM_ALL)
add_subdirectory(externals/zstd EXCLUDE_FROM_ALL)
add_subdirectory(externals/bzip2 EXCLUDE_FROM_ALL)
add_subdirectory(externals/liblzma EXCLUDE_FROM_ALL)

# Only AES and SHA-1 are needed from mbedtls.
add_library(mbedcrypto_min STATIC
  externals/mbedtls/library/aes.c
  externals/mbedtls/library/aesni.c
  externals/mbedtls/library/padlock.c
  externals/mbedtls/library/platform_util.c
  externals/mbedtls/library/sha1.c
)
target_include_directories(mbedcrypto_min PUBLIC externals/mbedtls/include)
dolphin_disable_warnings(mbedcrypto_min)

find_package(ZLIB REQUIRED)

# ---------------------------------------------------------------------------
# Dolphin's DiscIO and the parts of Common it needs
# ---------------------------------------------------------------------------
set(DOLPHIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/dolphin/Source/Core)

set(COMMON_SOURCES
  ${DOLPHIN_SRC}/Common/ColorUtil.cpp
  ${DOLPHIN_SRC}/Common/CommonFuncs.cpp
  ${DOLPHIN_SRC}/Common/Crypto/AES.cpp
  ${DOLPHIN_SRC}/Common/Crypto/SHA1.cpp
  ${DOLPHIN_SRC}/Common/DirectIOFile.cpp
  ${DOLPHIN_SRC}/Common/DynamicLibrary.cpp
  ${DOLPHIN_SRC}/Common/FileUtil.cpp
  ${DOLPHIN_SRC}/Common/Hash.cpp
  ${DOLPHIN_SRC}/Common/IOFile.cpp
  ${DOLPHIN_SRC}/Common/MsgHandler.cpp
  ${DOLPHIN_SRC}/Common/StringUtil.cpp
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  set(ISO2RVZ_ARCH_DEFINE _M_X86_64=1)
  list(APPEND COMMON_SOURCES ${DOLPHIN_SRC}/Common/x64CPUDetect.cpp)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(ISO2RVZ_ARCH_DEFINE _M_ARM_64=1)
  list(APPEND COMMON_SOURCES ${DOLPHIN_SRC}/Common/ArmCPUDetect.cpp)
else()
  set(ISO2RVZ_ARCH_DEFINE _M_GENERIC=1)
  list(APPEND COMMON_SOURCES ${DOLPHIN_SRC}/Common/GenericCPUDetect.cpp)
endif()

set(DISCIO_SOURCES
  ${DOLPHIN_SRC}/DiscIO/Blob.cpp
  ${DOLPHIN_SRC}/DiscIO/CachedBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/CISOBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/CompressedBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/DiscExtractor.cpp
  ${DOLPHIN_SRC}/DiscIO/DiscScrubber.cpp
  ${DOLPHIN_SRC}/DiscIO/DiscUtils.cpp
  ${DOLPHIN_SRC}/DiscIO/Enums.cpp
  ${DOLPHIN_SRC}/DiscIO/FileBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/Filesystem.cpp
  ${DOLPHIN_SRC}/DiscIO/FileSystemGCWii.cpp
  ${DOLPHIN_SRC}/DiscIO/LaggedFibonacciGenerator.cpp
  ${DOLPHIN_SRC}/DiscIO/NFSBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/ScrubbedBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/SplitFileBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/TGCBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/Volume.cpp
  ${DOLPHIN_SRC}/DiscIO/VolumeDisc.cpp
  ${DOLPHIN_SRC}/DiscIO/VolumeGC.cpp
  ${DOLPHIN_SRC}/DiscIO/VolumeWii.cpp
  ${DOLPHIN_SRC}/DiscIO/WbfsBlob.cpp
  ${DOLPHIN_SRC}/DiscIO/WIABlob.cpp
  ${DOLPHIN_SRC}/DiscIO/WIACompression.cpp
  ${DOLPHIN_SRC}/DiscIO/WiiEncryptionCache.cpp
)

set(COMPAT_SOURCES
  compat/Core/IOS/ES/Formats.cpp
  compat/Log.cpp
  compat/WiiSaveBanner.cpp
)

add_library(discio_min STATIC ${COMMON_SOURCES} ${DISCIO_SOURCES} ${COMPAT_SOURCES})
target_include_directories(discio_min PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/compat
  ${DOLPHIN_SRC}
)
target_compile_definitions(discio_min PUBLIC
  ISO2RVZ=1
  _ARCH_64=1
  ${ISO2RVZ_ARCH_DEFINE}
  _DEFAULT_SOURCE
  _FILE_OFFSET_BITS=64
  FMT_EXCEPTIONS=0
  DATA_DIR="${CMAKE_INSTALL_PREFIX}/share/iso2rvz/"
)
target_compile_options(discio_min PUBLIC -fno-strict-aliasing)
# Polyfills for older standard libraries; -idirafter lets the real headers win when present.
target_compile_options(discio_min PUBLIC
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-idirafter ${CMAKE_CURRENT_SOURCE_DIR}/compat/polyfill>")
target_link_libraries(discio_min PUBLIC
  fmt::fmt
  libzstd_static
  bzip2
  lzma
  mbedcrypto_min
  ZLIB::ZLIB
)
target_include_directories(discio_min PUBLIC externals/zstd/zstd/lib)

find_package(Threads REQUIRED)
target_link_libraries(discio_min PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# iso2rvz
# ---------------------------------------------------------------------------
add_executable(iso2rvz src/main.cpp)
target_link_libraries(iso2rvz PRIVATE discio_min)
target_compile_definitions(iso2rvz PRIVATE ISO2RVZ_VERSION="${PROJECT_VERSION}")
target_compile_options(iso2rvz PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(ISO2RVZ_STATIC)
  target_link_options(iso2rvz PRIVATE -static-libstdc++ -static-libgcc)
endif()

include(GNUInstallDirs)
install(TARGETS iso2rvz RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
