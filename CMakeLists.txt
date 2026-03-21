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
    file(GLOB_RECURSE GAME_SOURCES "src/*.cc" "src/*.cpp")

    if(UNIX)
        # ── Linux（云托管环境）──
        # 将游戏源码编译为静态库，并用 --whole-archive 强制链接所有符号
        # 防止链接器优化掉 Drogon WebSocketController 的静态初始化代码（WS 路由注册）
        add_library(game_static STATIC ${GAME_SOURCES})
        target_include_directories(game_static PUBLIC src/ src/common/ src/poker/ src/reda/)
        target_link_libraries(game_static PUBLIC Drogon::Drogon)

        add_executable(${PROJECT_NAME} main.cc)
        target_include_directories(${PROJECT_NAME} PRIVATE src/ src/common/ src/poker/ src/reda/)
        # --whole-archive：强制包含 game_static 中所有符号（包括未被直接引用的静态初始化代码）
        target_link_libraries(${PROJECT_NAME} PRIVATE
            -Wl,--whole-archive game_static -Wl,--no-whole-archive
            Drogon::Drogon
        )
    else()
        # ── Windows 本地开发 ──
        add_executable(${PROJECT_NAME} main.cc ${GAME_SOURCES})
        target_include_directories(${PROJECT_NAME} PRIVATE src/ src/common/ src/poker/ src/reda/)
        target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)
    endif()
else()
    # 极简模式：只有 main.cc
    add_executable(${PROJECT_NAME} main.cc)
    target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${PROJECT_NAME} PRIVATE -O2 -DNDEBUG)
endif()
