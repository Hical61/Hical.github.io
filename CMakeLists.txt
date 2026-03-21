cmake_minimum_required(VERSION 3.15)
project(drogon-game-server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── vcpkg 工具链（仅 Windows 本地开发需要，Linux Docker 环境中忽略）──
if(WIN32)
    if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
    if(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()

# ── 依赖：Drogon（在 drogonframework/drogon Docker 镜像中已预装）──
find_package(Drogon REQUIRED)

# ── 编译阶段控制 ──
option(PHASE2_FULL_GAME "Enable full game logic (Poker + REDA)" ON)

if(PHASE2_FULL_GAME)
    # 完整游戏逻辑：包含所有 RoomManager 和 WS 控制器
    # HTTP 路由在 main.cc 中直接注册（lambda 方式，不依赖 HttpController 自动注册）
    file(GLOB_RECURSE GAME_SOURCES "src/*.cc" "src/*.cpp")
    add_executable(${PROJECT_NAME} main.cc ${GAME_SOURCES})
    target_include_directories(${PROJECT_NAME} PRIVATE src/ src/common/ src/poker/ src/reda/)
else()
    # 极简模式：只有 main.cc
    add_executable(${PROJECT_NAME} main.cc)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJECT_NAME} PRIVATE -O2 -DNDEBUG)
endif()
