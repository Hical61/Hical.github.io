cmake_minimum_required(VERSION 3.15)
project(drogon-game-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── vcpkg 工具链集成（Drogon 安装在 C:/vcpkg）──────────────────────────
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()
if(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# ── 依赖：Drogon ──────────────────────────────────────────────────────────
find_package(Drogon REQUIRED)

# ── 第一阶段：极简服务器（仅 main.cc，验证平台是否正常）──────────────────
# 确认云托管可以运行后，改为 PHASE2=ON 启用完整游戏逻辑
option(PHASE2_FULL_GAME "Enable full game logic (Poker + REDA)" OFF)

if(PHASE2_FULL_GAME)
    # 完整游戏逻辑
    file(GLOB_RECURSE GAME_SOURCES "src/*.cc" "src/*.cpp")
    add_executable(${PROJECT_NAME} main.cc ${GAME_SOURCES})
    target_include_directories(${PROJECT_NAME} PRIVATE src/ src/common/ src/poker/ src/reda/)
else()
    # 极简：只有 main.cc
    add_executable(${PROJECT_NAME} main.cc)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJECT_NAME} PRIVATE -O2 -DNDEBUG)
endif()
