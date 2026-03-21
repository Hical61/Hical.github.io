cmake_minimum_required(VERSION 3.15)
project(drogon-game-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── vcpkg 工具链集成（Drogon 安装在 C:/vcpkg）──────────────────────────
# 使用方式（推荐）：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
# 或者设置环境变量 VCPKG_ROOT=C:/vcpkg，CMake 将自动探测
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "")
endif()
if(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# ── 依赖：Drogon ──────────────────────────────────────────────────────────
find_package(Drogon REQUIRED)
# vcpkg 安装：vcpkg install drogon:x64-windows

# ── 源文件 ────────────────────────────────────────────────────────────────
file(GLOB_RECURSE SOURCES
    "src/*.cc"
    "src/*.cpp"
)

add_executable(${PROJECT_NAME} main.cc ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src/
    src/common/
    src/poker/
    src/reda/
)

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

# ── 编译优化 ──────────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJECT_NAME} PRIVATE -O3 -DNDEBUG)
endif()

# ── 测试 ──────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_subdirectory(tests)
endif()
