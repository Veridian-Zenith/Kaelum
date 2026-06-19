cmake_minimum_required(VERSION 3.18)

# ---- Ignore External Flags ----
# Ensuring build consistency by ignoring environment-inherited flags
set(ENV{CFLAGS} "")
set(ENV{CXXFLAGS} "")
set(ENV{LDFLAGS} "")
set(CMAKE_C_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG "" CACHE STRING "" FORCE)

project(Kaelum VERSION 0.1.0 LANGUAGES CXX)

# ---- Cache Acceleration ----
if(NOT DEFINED ENV{CCACHE_DISABLE})
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "ccache found: ${CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    else()
        message(WARNING "ccache not found. Proceeding with raw compilation.")
    endif()
endif()

# ---- Project Standards ----
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(KAELUM_USE_MOLD "Use MOLD linker instead of LLD" ON)
option(KAELUM_ENABLE_LEVEL_ZERO "Enable Intel Level Zero support" ON)
option(KAELUM_USE_SYCL "Enable SYCL compatibility" ON)

# ---- Architecture Selection ----
set(KAELUM_ARCH "native" CACHE STRING "Target architecture for compilation (default: native)")
message(STATUS "Targeting architecture: ${KAELUM_ARCH}")

# ---- Toolchain Validation ----
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Clang required. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ---- Compiler & Feature Flags ----
# Optimized for performance and security, mirroring Voix's professional standards
set(COMMON_FLAGS "-march=${KAELUM_ARCH} -pipe -fno-plt -fstack-protector-strong \
-fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -flto=thin -fvisibility=hidden \
-fexperimental-library -std=c++26")

set(DEBUG_FLAGS "-g -O1 -fstandalone-debug -fno-omit-frame-pointer")
