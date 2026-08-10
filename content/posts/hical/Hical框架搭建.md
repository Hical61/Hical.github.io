+++
title = '搭建 Hical HTTP 服务器 — 多平台环境搭建指南'
date = '2026-04-22'
draft = false
tags = ["C++", "Hical", "环境搭建", "vcpkg", "Conan", "跨平台"]
categories = ["Hical框架"]
description = "Hical v2.0.0 在所有支持平台上的环境搭建，包括 vcpkg / Conan / 源码编译三种安装方式。"
+++

# 搭建 Hical HTTP 服务器 — 多平台环境搭建指南

## 概述

本文档涵盖 Hical v2.0.0 在所有支持平台上的环境搭建，包括三种安装方式（vcpkg / Conan / 源码编译）和五个平台（Windows MSYS2、Windows MSVC、Ubuntu/Debian、Fedora/Arch、macOS）。

---

## 依赖要求

| 组件        | 版本要求                         | 用途                            |
| ----------- | -------------------------------- | ------------------------------- |
| C++ 编译器  | GCC 14+ / Clang 22+ / MSVC 2022+ | C++20 协程 + C++26 反射（可选） |
| CMake       | >= 3.20                          | 构建系统                        |
| Boost       | >= 1.70                          | Asio / Beast / JSON / System    |
| OpenSSL     | 必需                             | SSL/TLS 支持                    |
| Google Test | 必需                             | 单元测试                        |

---

## 安装方式一：vcpkg（推荐）

vcpkg 是最简单的安装方式，一行命令自动解决所有依赖。

### 安装 vcpkg（如未安装）

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && bootstrap-vcpkg.bat   # Windows
cd vcpkg && ./bootstrap-vcpkg.sh  # Linux / macOS
```

将 vcpkg 目录加入 `PATH`，或记住安装路径用于后续 `CMAKE_TOOLCHAIN_FILE`。

### 安装 Hical

```bash
vcpkg install Hical61-Hical
```

### CMakeLists.txt 配置

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Hical CONFIG REQUIRED)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE Hical::Hical_core)
```

### 编译

```bash
# 指定 vcpkg 工具链文件
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=[vcpkg根目录]/scripts/buildsystems/vcpkg.cmake
cmake --build build -j$(nproc)
```

> Windows MSVC 下 `nproc` 不可用，去掉 `-j$(nproc)` 即可，CMake 会使用默认并行度。

---

## 安装方式二：Conan

### 命令行安装

```bash
conan install --requires="Hical/2.0.0" --build=missing
```

### 使用 conanfile.txt

在项目根目录创建 `conanfile.txt`：

```ini
[requires]
Hical/2.0.0

[generators]
CMakeDeps
CMakeToolchain
```

然后执行：

```bash
conan install . --build=missing --output-folder=build
```

### CMakeLists.txt 配置

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Hical REQUIRED)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE Hical::Hical_core)
```

### 编译

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build -j$(nproc)
```

> **提示：** 若 Hical 尚未进入 Conan Center，可直接从仓库创建本地包：
> ```bash
> git clone https://github.com/Hical61/Hical.git
> conan create Hical/ --build=missing
> ```

---

## 安装方式三：源码编译

适合需要修改框架代码或无法使用包管理器的场景。

### 获取源码

```bash
git clone https://github.com/Hical61/Hical.git
cd Hical
```

---

### 平台 1：Windows（MSYS2 MINGW64）

**安装依赖：**

```bash
# MSYS2 MINGW64 终端
pacman -Syu
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-gtest
```

**配置 PATH：** 将 `C:\msys64\mingw64\bin` 添加到系统环境变量。

**编译命令：**

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**CMakeLists.txt 要点：**
- 需要 `add_compile_definitions(_WIN32_WINNT=0x0A00)`（Hical 根 CMakeLists.txt 已包含）
- 下游项目链接时需要额外链接 `ws2_32 mswsock`（Windows Socket 库）

**中文乱码修复：** PowerShell 执行 `chcp 65001`

---

### 平台 2：Windows（MSVC + vcpkg）

**前提：** 已安装 Visual Studio 2022（含 C++ 桌面开发工作负荷）和 vcpkg。

**安装依赖：**

```bash
vcpkg install boost:x64-windows openssl:x64-windows gtest:x64-windows
```

**编译命令：**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**运行测试：**

```bash
ctest --test-dir build --output-on-failure -C Release
```

> MSVC 构建是多配置生成器，运行和测试时必须加 `--config Release` 或 `-C Release`。

---

### 平台 3：Linux — Ubuntu / Debian

**安装依赖：**

```bash
sudo apt update
sudo apt install -y build-essential g++ cmake ninja-build \
                    libboost-all-dev libssl-dev libgtest-dev
```

> **版本要求：** Ubuntu 24.04+ 自带 GCC 14+，满足要求。Ubuntu 22.04 的 GCC 12 版本偏低，需升级。

**Ubuntu 22.04 升级 GCC（如需要）：**

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install g++-14
# 配置时指定编译器
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=g++-14
```

**验证版本：**

```bash
g++ --version        # 需要 14+
cmake --version      # 需要 3.20+
dpkg -s libboost-dev | grep Version   # 需要 1.70+
openssl version
```

**编译命令：**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

> Linux 下无需 `CMAKE_PREFIX_PATH`，系统包管理器安装的库默认在标准路径。

---

### 平台 4：Linux — Fedora / RHEL / CentOS

**安装依赖：**

```bash
# Fedora
sudo dnf install -y gcc-c++ cmake ninja-build \
                    boost-devel openssl-devel gtest-devel

# RHEL 8 / CentOS Stream 8（需启用 EPEL 和 PowerTools）
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled powertools
sudo dnf install -y gcc-c++ cmake ninja-build \
                    boost-devel openssl-devel gtest-devel
```

**编译命令：**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

### 平台 5：Linux — Arch Linux

**安装依赖：**

```bash
sudo pacman -S gcc cmake ninja boost openssl gtest
```

> Arch 滚动更新，软件包版本始终满足要求。

**编译命令：**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

### 平台 6：macOS（Homebrew）

**安装 Homebrew（如未安装）：**

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

**安装依赖：**

```bash
brew install cmake ninja boost openssl@3 googletest
```

**重要：macOS 自带的是 LibreSSL，不是 OpenSSL。** Homebrew 安装的 OpenSSL 不在默认搜索路径，CMake 需要额外指定：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
    -DCMAKE_PREFIX_PATH="$(brew --prefix boost);$(brew --prefix openssl@3)"
cmake --build build -j$(sysctl -n hw.ncpu)
```

**Apple Silicon（M1/M2/M3/M4）注意事项：**
- Homebrew 安装路径为 `/opt/homebrew`（非 Intel 的 `/usr/local`）
- `brew --prefix` 会自动返回正确路径，无需手动区分

**使用 Clang（Xcode 自带）：**
- macOS 默认使用 Apple Clang，需要 Xcode 15+（Clang 15+）以支持 C++20 协程
- 验证：`clang++ --version`
- 如需完整 C++26 反射支持，需安装 Clang 22+：`brew install llvm`

---

## 运行测试（所有平台通用）

源码编译完成后运行测试：

```bash
ctest --test-dir build --output-on-failure --timeout 60 -j4
# MSVC 需要加: -C Release
```

---

## 各平台编译命令速查

### 包管理器安装后编译

| 安装方式 | 配置命令                                                                         | 编译命令              |
| -------- | -------------------------------------------------------------------------------- | --------------------- |
| vcpkg    | `cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake` | `cmake --build build` |
| Conan    | `cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake`              | `cmake --build build` |

### 源码编译各平台

| 平台                   | 配置命令                                                                                  | 编译命令                                     |
| ---------------------- | ----------------------------------------------------------------------------------------- | -------------------------------------------- |
| Windows (MSYS2)        | `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`                                      | `cmake --build build`                        |
| Windows (MSVC + vcpkg) | `cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/.../vcpkg.cmake`                          | `cmake --build build --config Release`       |
| Ubuntu / Debian        | `cmake -B build -DCMAKE_BUILD_TYPE=Release`                                               | `cmake --build build -j$(nproc)`             |
| Fedora / Arch          | `cmake -B build -DCMAKE_BUILD_TYPE=Release`                                               | `cmake --build build -j$(nproc)`             |
| macOS (Homebrew)       | `cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)` | `cmake --build build -j$(sysctl -n hw.ncpu)` |

---

## 下游项目 CMakeLists.txt 示例

### 使用 vcpkg 或 Conan 安装的 Hical

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg 用 CONFIG, Conan 不需要
find_package(Hical CONFIG REQUIRED)    # vcpkg
# find_package(Hical REQUIRED)         # Conan

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE Hical::Hical_core)
```

### 使用 add_subdirectory 引入源码

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.70 REQUIRED COMPONENTS system json)
find_package(OpenSSL REQUIRED)

# 引入 Hical 源码（路径按实际修改）
add_subdirectory(/path/to/Hical ${CMAKE_BINARY_DIR}/Hical)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE Hical_core)

# Windows 平台需要额外链接 Winsock 库
if(WIN32)
    target_link_libraries(my_server PRIVATE ws2_32 mswsock)
endif()
```

> `_WIN32_WINNT` 宏已在 Hical 根 CMakeLists.txt 中通过 `if(MSVC)` 分支定义。
> 使用 MinGW 时不会自动定义，下游项目如遇相关警告可自行添加 `add_compile_definitions(_WIN32_WINNT=0x0A00)`。

---

## 启用 C++26 反射（可选）

需要支持 C++26 反射的编译器（Clang 实验性支持）：

```bash
cmake -B build -DHical_ENABLE_REFLECTION=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

当编译器不支持 P2996 时，框架自动回退到 C++20 宏方案（`Hical_JSON` / `Hical_ROUTES`），用户代码无需修改。

---

## 快速验证

编译完成后，运行示例服务器验证安装：

```bash
# Linux / macOS
./build/examples/http_server 8080

# Windows
./build/examples/http_server.exe 8080
```

```bash
curl http://localhost:8080/
# 预期输出: Hello, Hical!
```

---

## 常见问题

### Q: Ubuntu 22.04 编译报错 "协程相关特性不支持"

GCC 12 对 C++20 协程支持不完整，升级到 GCC 14+：

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt install g++-14
cmake -B build -DCMAKE_CXX_COMPILER=g++-14
```

### Q: macOS 找不到 OpenSSL

Homebrew 安装的 OpenSSL 不在系统默认路径，配置时需指定：

```bash
cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
```

### Q: Windows MSVC 测试运行失败

MSVC 是多配置生成器，运行测试时必须指定配置：

```bash
ctest --test-dir build -C Release --output-on-failure
```

### Q: PowerShell 中文乱码

```powershell
chcp 65001
```

### Q: vcpkg 找不到 Hical 包

确保 vcpkg 已更新到最新：

```bash
cd vcpkg && git pull && vcpkg x-update-baseline --add-initial-baseline
vcpkg install Hical61-Hical
```

### Q: Conan 找不到 Hical 包

若 Hical 尚未进入 Conan Center，从源码创建本地包：

```bash
git clone https://github.com/Hical61/Hical.git
conan create Hical/ --build=missing
```
