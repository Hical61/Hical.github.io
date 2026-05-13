+++
title = '现代 CMake 课程学习：从「面向目录」到「面向目标」'
date = '2025-06-01'
draft = false
tags = ["CMake", "C++", "构建系统", "学习笔记", "工程实践"]
categories = ["C++"]
description = "系统总结现代 CMake 的核心理念：target-based 设计、属性传播、generator expressions、find_package、FetchContent、Presets、toolchain 等，附实战代码和常见陷阱。"
+++

# 现代 CMake 课程学习：从「面向目录」到「面向目标」

> 现代 CMake 不是新语法，是新思维。

---

## 写在前面

### 这篇文章适合谁？

- 用过 CMake 但只会 `add_executable` + `target_link_libraries` 的人
- 从 Makefile / Visual Studio 工程迁移过来，想系统学 CMake 的人
- 看别人 CMakeLists.txt 里一堆 `PUBLIC`、`$<BUILD_INTERFACE:...>` 一头雾水的人

### 什么是 CMake？（30 秒版本）

CMake **不是编译器**，它是一个**构建系统生成器**。你写一份 `CMakeLists.txt`，CMake 帮你生成对应平台的构建文件：

- Linux → Makefile 或 Ninja
- Windows → Visual Studio .sln 或 Ninja
- macOS → Xcode 或 Ninja

类比：CMake 就像一个"翻译官"，你用一种语言描述"我要编译什么"，它翻译成各平台编译器能理解的指令。

### 为什么要学"现代" CMake？

CMake 从 2000 年诞生至今，经历了巨大变化。2014 年的 CMake 3.0 是分水岭——引入了 **target-based**（面向目标）设计。此后的版本持续完善这套体系。

如果你还在用 `include_directories()`、`link_libraries()` 这套"传统写法"，那你用的是 2014 年之前的思路——就像 2025 年还在写 C++98 一样。

这篇文章是我系统学习现代 CMake 后的总结，涵盖核心概念、最佳实践和踩坑经验。

---

## 一、核心理念转变：面向目录 → 面向目标

### 前置知识

你只需要知道：
- `add_executable(name src.cpp)` — 告诉 CMake "我要编译一个可执行文件"
- `add_library(name src.cpp)` — 告诉 CMake "我要编译一个库"
- `target_link_libraries(A B)` — 告诉 CMake "A 需要链接 B"

### 1.1 传统 CMake 的问题

```cmake
# ❌ 传统写法（CMake 2.x 风格）—— 千万别再写了

# 设置头文件搜索路径 —— 问题：这会影响当前目录下的「所有」目标！
include_directories(${PROJECT_SOURCE_DIR}/include)

# 设置库搜索路径 —— 问题：同上，全局生效，后面的目标都会受影响
link_directories(/usr/local/lib)

# 添加编译宏 —— 问题：同上，所有目标都会定义这个宏
add_definitions(-DUSE_SSL)

add_executable(myapp main.cpp)
target_link_libraries(myapp ssl crypto)
```

**类比理解**：传统写法就像在办公室里大喊"所有人都去开会！"——不管谁需要不需要，全被影响了。

问题总结：

- `include_directories()` 是**目录级**的，影响当前 `CMakeLists.txt` 下**所有**目标
- 依赖关系隐式传播，子目录互相"污染"
- 无法表达"A 依赖 B，B 的头文件路径应该自动传递给 A"
- 项目一大，根本搞不清楚哪个目标用了哪些路径/宏/选项

### 1.2 现代 CMake：一切围绕 target

```cmake
# ✅ 现代写法 —— 每个目标管好自己的事

# 创建一个库目标叫 mylib
add_library(mylib src/mylib.cpp)

# 给 mylib 设置头文件路径：
#   PUBLIC  = 自己编译时用 + 别人链接我时也能用（公共头文件在这）
#   PRIVATE = 只有自己编译时用（内部实现细节，外部不需要知道）
target_include_directories(mylib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 给 mylib 定义编译宏，PUBLIC 表示链接我的人也会获得这个宏
target_compile_definitions(mylib PUBLIC USE_SSL)

# 创建可执行文件
add_executable(myapp main.cpp)

# myapp 链接 mylib（PRIVATE = myapp 的使用者不需要知道 mylib 的存在）
target_link_libraries(myapp PRIVATE mylib)
# 此时 myapp 自动获得：
#   - mylib 的 PUBLIC include 路径（include/）
#   - mylib 的 PUBLIC 宏定义（USE_SSL）
#   - 但看不到 mylib 的 PRIVATE 路径（src/）
```

**类比理解**：现代写法就像给每个人发一封精确的邮件——只有需要的人收到相关信息，不会打扰其他人。

**核心原则：每个 target 自己声明自己的需求，通过 `target_link_libraries` 让依赖自动传播。就像搭积木——每块积木自带接口说明，拼在一起时自然契合。**

---

## 二、属性传播：PUBLIC / PRIVATE / INTERFACE

### 为什么需要这个？

上一节你已经见过 `PUBLIC` 和 `PRIVATE` 了。这一节我们彻底讲清楚——这是现代 CMake **最核心的机制**，理解了它，后面的内容都是水到渠成。

### 生活类比

想象你开了一家餐厅（你的库）：

- **PRIVATE**（后厨用的调料）：顾客不需要知道你用了什么牌子的酱油。→ 仅自己编译时用，别人链接你时看不到。
- **PUBLIC**（菜单上的菜名）：顾客必须知道才能点菜。→ 自己编译时用 + 链接你的人也会获得。
- **INTERFACE**（外卖包装说明）：你自己不吃外卖，但顾客需要这个信息。→ 自己不用，仅提供给链接你的人。

### 具体使用场景

| 关键字      | 什么时候用？ | 典型举例 |
| ----------- | ------------ | -------- |
| `PRIVATE`   | 这个东西**只出现在我的 .cpp 里**（实现细节） | 你的 .cpp 用了 `#include "detail/impl.h"`，外部不需要知道 |
| `PUBLIC`    | 这个东西**出现在我的公共 .h 里**（别人 include 我时也得有它） | 你的公共头 `mylib.h` 里写了 `#include <boost/json.hpp>`，外部也得能找到 Boost |
| `INTERFACE` | 我自己不编译（纯头文件库），只需**告诉使用者**去哪找东西 | header-only 库没有 .cpp，只有头文件 |

### 2.1 实战示例：header-only 库

header-only 库是 `INTERFACE` 最常见的用法。这类库没有 .cpp 文件需要编译，只有头文件——使用者直接 `#include` 就行。

```cmake
# 创建一个 INTERFACE 库（不会生成 .a 或 .so 文件，仅作为"配置容器"）
add_library(json_utils INTERFACE)

# 对于 INTERFACE 库，只能用 INTERFACE 关键字
# （因为它自己不编译，不存在"自己用"的概念）
target_include_directories(json_utils
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include
    #         ^^^^^^^^^ 意思：谁链接我，谁就把这个路径加到自己的头文件搜索路径
)

# 要求使用者至少用 C++20 编译（因为我的头文件里用了 C++20 语法）
target_compile_features(json_utils INTERFACE cxx_std_20)
#                                  ^^^^^^^^^ 意思：谁链接我，谁就得开启 C++20
```

### 2.2 传播链条（重点！画个图就懂了）

属性传播就像接力赛传棒——但 PRIVATE 会"截断"传递：

**场景 1：PRIVATE 截断传播**

```
你的 App ──(PRIVATE 依赖)──▶ LibB ──(PUBLIC 依赖)──▶ LibC
```

- App 编译链接时：能用 LibB 和 LibC（正常编译通过）
- 但如果有个 LibX 依赖 App：LibX **看不到** LibB 和 LibC
- **类比**：你知道餐厅用了某品牌的酱油（LibC），但你不会在自己的广告里提这事

**场景 2：PUBLIC 全程透传**

```
你的 App ──(PUBLIC 依赖)──▶ LibB ──(PUBLIC 依赖)──▶ LibC
```

- 任何依赖 App 的目标，都会**自动获得** LibB 和 LibC 的 PUBLIC 属性
- **类比**：你在广告里写了"使用某品牌酱油"，你的加盟商也得跟着用

### 2.3 选择口诀（背下来）

> - 这个依赖**只出现在 .cpp 里** → `PRIVATE`
> - 这个依赖**出现在公共 .h 里** → `PUBLIC`
> - 我**自己不编译**，只是转交给别人 → `INTERFACE`

不确定时，**默认用 PRIVATE**。只有当你的公共头文件确实需要它时，才升级为 PUBLIC。这是"最小暴露"原则。

---

## 三、Generator Expressions（生成表达式）

### 为什么需要这个？

CMake 有两个阶段：
1. **配置阶段**（`cmake -B build`）：读取 CMakeLists.txt，确定构建方案
2. **构建阶段**（`cmake --build build`）：真正执行编译链接

有些信息在配置阶段还不确定（比如"当前是 Debug 还是 Release？"——在 MSVC 的多配置生成器中，配置时并不知道最终用哪个配置）。

**Generator Expressions（生成表达式）就是在构建阶段才求值的"延迟表达式"。**

### 语法长什么样？

```
$<条件:值>        ← 如果条件为真，展开为"值"；否则展开为空字符串
$<条件:真值,假值>  ← 三元表达式
```

第一次看会觉得很丑，但习惯后非常强大。我们逐个看常用模式：

### 3.1 按构建类型设置编译选项

**场景**：Debug 模式开 sanitizer 方便调试，Release 模式开优化。

```cmake
target_compile_options(mylib PRIVATE
    # $<$<CONFIG:Debug>:...> 意思是：如果当前构建类型是 Debug，就使用冒号后面的选项
    $<$<CONFIG:Debug>:-O0 -g -fsanitize=address>
    #                  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^ Debug 时：不优化 + 调试信息 + 地址检查

    # 如果当前构建类型是 Release，就使用这些选项
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
    #                   ^^^^^^^^^^^^^ Release 时：最大优化 + 关闭 assert
)
```

**为什么不用 `if(CMAKE_BUILD_TYPE STREQUAL "Debug")`？**

因为对于 MSVC 等多配置生成器（一次配置生成 Debug 和 Release 两套），配置阶段 `CMAKE_BUILD_TYPE` 是空的！生成表达式才能正确处理这种情况。

### 3.2 按编译器选择警告选项

**场景**：GCC/Clang 和 MSVC 的警告选项写法完全不同。

```cmake
target_compile_options(mylib PRIVATE
    # GCC 和 Clang 用 -W 系列
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>

    # MSVC 用 /W 系列
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
```

### 3.3 安装时的路径切换（发布库的经典写法）

**场景**：你开发时源码在 `D:/myproject/include/`，但别人安装后在 `/usr/local/include/`。怎么让同一份 CMake 配置在两种场景都正确？

```cmake
target_include_directories(mylib
    PUBLIC
        # BUILD_INTERFACE：在本项目内编译时，用源码目录的绝对路径
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>

        # INSTALL_INTERFACE：别人 find_package 找到我之后，用安装目录的相对路径
        $<INSTALL_INTERFACE:include>
        #                   ^^^^^^^ 相对于安装前缀（如 /usr/local/）
)
```

**类比**：就像你在公司里用工牌号找人（BUILD），出了公司得用手机号（INSTALL）。同一个人，不同场景用不同的寻址方式。

### 3.4 按平台链接不同的系统库

```cmake
target_link_libraries(myapp PRIVATE
    # Windows 需要链接 Winsock 库
    $<$<PLATFORM_ID:Windows>:ws2_32 mswsock>

    # Linux 需要链接 pthread
    $<$<PLATFORM_ID:Linux>:pthread>
)
# 其他平台（如 macOS）什么都不链——生成表达式为空就是不添加
```

---

## 四、find_package：现代依赖发现

### 为什么需要这个？

你的项目要用 Boost、OpenSSL、GoogleTest……这些第三方库可能安装在系统的各种位置。`find_package` 就是 CMake 的"找库神器"——你告诉它"我需要 Boost"，它帮你找到 Boost 装在哪、头文件在哪、要链接哪些 .a/.lib 文件。

**类比**：`find_package` 就像手机上的"查找附近餐厅"——你说"我要吃火锅"，它帮你定位到具体地址，你直接导航过去就行。

### 4.1 两种模式（了解即可）

| 模式 | 谁提供查找逻辑？ | 查找的文件名 | 类比 |
|------|-----------------|-------------|------|
| **Config 模式** | 库自己安装时附带 | `<Pkg>Config.cmake` | 餐厅自己在地图 App 上注册了地址 |
| **Module 模式** | CMake 内置或用户写的脚本 | `Find<Pkg>.cmake` | 有人帮餐厅录入了地址 |

**现代做法优先走 Config 模式**——大部分主流库（Boost、Qt、gRPC 等）安装后都自带 Config 文件。你不需要关心具体用的哪种模式，直接 `find_package(Xxx)` 就行。

### 4.2 使用示例

```cmake
# find_package(包名 版本 REQUIRED COMPONENTS 子模块...)
#   包名      —— 库的名字，大小写敏感（一般首字母大写）
#   版本      —— 最低版本要求（可选）
#   REQUIRED  —— 找不到就报错终止（否则只是警告，继续配置）
#   COMPONENTS —— 大型库（如 Boost）有很多子模块，只列你需要的
find_package(Boost 1.82 REQUIRED COMPONENTS system json)
find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp
    PRIVATE
        # 这些 "Xxx::yyy" 格式叫 "imported targets"（导入目标）
        # 它们是 find_package 找到库后自动创建的"虚拟目标"
        # 链接它们时，头文件路径、编译选项等会自动传播——不需要你手动设置！
        Boost::system       # Boost 的 system 子模块
        Boost::json         # Boost 的 JSON 子模块
        OpenSSL::SSL        # OpenSSL 的 SSL 部分
        OpenSSL::Crypto     # OpenSSL 的 Crypto 部分
        GTest::gtest_main   # GoogleTest + 自动生成 main 函数
)
```

### 4.3 imported targets vs 老式变量（重要区别）

```cmake
# ❌ 老式写法——find_package 设置一堆变量，你手动拼接
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})    # 手动加头文件路径
target_link_libraries(myapp ${Boost_LIBRARIES})  # 手动加库文件

# ✅ 现代写法——直接用 imported target，一切自动传播
find_package(Boost REQUIRED COMPONENTS json)
target_link_libraries(myapp PRIVATE Boost::json)
# 头文件路径？自动有。编译宏？自动有。依赖的其他库？也自动有。
```

**为什么 imported targets 更好？** 因为它复用了第二章的属性传播机制——Boost::json 这个 target 身上已经设好了所有 PUBLIC 属性，你一链接就全拿到了。

### 4.4 编写自己的 Config 文件（库作者专属）

> 如果你只是使用别人的库，这一节可以跳过。如果你要发布自己的库让别人 `find_package`，继续看。

目标：让别人写一句 `find_package(mylib)` + `target_link_libraries(app mylib::mylib)` 就能用你的库。

```cmake
# ===== 第一步：安装你的库目标 =====
install(TARGETS mylib
    EXPORT mylib-targets           # 把这个目标记录到一个"导出集"
    ARCHIVE DESTINATION lib        # .a/.lib 静态库文件放这
    LIBRARY DESTINATION lib        # .so/.dylib 动态库文件放这
    INCLUDES DESTINATION include   # 安装后头文件在这（给 INTERFACE 用）
)

# ===== 第二步：导出 targets 文件（核心！）=====
install(EXPORT mylib-targets
    FILE mylibTargets.cmake        # 生成的文件名
    NAMESPACE mylib::              # 给目标加命名空间前缀（最佳实践）
    DESTINATION lib/cmake/mylib    # 安装到这个相对路径
)
# 安装后，别人的 CMake 能在这找到 mylibTargets.cmake
# 里面定义了 mylib::mylib 这个 imported target

# ===== 第三步：生成 Config 文件（入口）=====
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/mylibConfig.cmake.in                    # 输入模板
    ${CMAKE_CURRENT_BINARY_DIR}/mylibConfig.cmake # 输出
    INSTALL_DESTINATION lib/cmake/mylib
)

# ===== 第四步：生成版本兼容文件 =====
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/mylibConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion  # 主版本号相同就认为兼容
)

# ===== 第五步：安装 Config 和 Version 文件 =====
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/mylibConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/mylibConfigVersion.cmake
    DESTINATION lib/cmake/mylib
)
```

`cmake/mylibConfig.cmake.in` 模板文件内容：

```cmake
# @PACKAGE_INIT@ 会被 CMake 自动替换为初始化代码
@PACKAGE_INIT@

# 如果你的库依赖其他库，在这里声明
# find_dependency 和 find_package 类似，但用于 Config 文件内部
include(CMakeFindDependencyMacro)
find_dependency(Boost 1.82 COMPONENTS system json)
find_dependency(OpenSSL)

# 加载实际的 targets 定义
include("${CMAKE_CURRENT_LIST_DIR}/mylibTargets.cmake")

# 检查所有请求的组件是否存在
check_required_components(mylib)
```

**流程总结**：安装后目录长这样：
```
/usr/local/
├── lib/
│   ├── libmylib.a                    ← 库文件本体
│   └── cmake/mylib/                  ← CMake 配置目录
│       ├── mylibConfig.cmake         ← find_package 的入口
│       ├── mylibConfigVersion.cmake  ← 版本检查
│       └── mylibTargets.cmake        ← imported target 定义
└── include/
    └── mylib/                        ← 公共头文件
        └── mylib.h
```

---

## 五、FetchContent：编译时拉取依赖

### 为什么需要这个？

有时候你需要的库既不在系统包管理器里（apt/brew 没有），也没有用 vcpkg/Conan 打包。这时候 `FetchContent` 就派上用场——它在 CMake 配置阶段自动从 Git 仓库下载源码，然后作为子目录直接参与编译。

**类比**：`find_package` 是"去超市买食材"，`FetchContent` 是"从农场直接快递原材料到你家厨房"。

### 5.1 基本用法

```cmake
# 引入 FetchContent 模块（CMake 自带，不需要额外安装）
include(FetchContent)

# 声明依赖：告诉 CMake 去哪下载、下载哪个版本
FetchContent_Declare(
    googletest                      # 依赖的名字（你自己起，后面引用用）
    GIT_REPOSITORY https://github.com/google/googletest.git  # Git 仓库地址
    GIT_TAG        v1.14.0          # 具体版本标签（切忌用 master/main！）
    GIT_SHALLOW    TRUE             # 浅克隆：只下载这个 tag 的快照，不拉全部历史
)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
)

# 让 CMake 去下载并把它们加入构建（相当于 add_subdirectory）
FetchContent_MakeAvailable(googletest fmt)

# 下载完成后，就能直接用它们提供的 targets 了
target_link_libraries(myapp PRIVATE fmt::fmt GTest::gtest_main)
```

### 5.2 与 find_package 配合（推荐模式）

**问题**：每次配置都要联网下载太慢了。如果系统上已经装了这个库，能不能优先用系统的？

CMake 3.24+ 支持 `FIND_PACKAGE_ARGS`——先在系统里找，找不到再下载：

```cmake
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
    FIND_PACKAGE_ARGS   # 这一行的含义：先执行 find_package(fmt)
                        # 如果系统上找到了，就直接用，不下载
                        # 如果找不到，再走 Git 下载流程
)
FetchContent_MakeAvailable(fmt)
# 不管是系统找到的还是下载的，后面都统一用 fmt::fmt
```

### 5.3 常见坑和注意事项

| 坑 | 说明 | 解决方案 |
|---|------|---------|
| 用了 `main` 分支 | 每次配置可能下到不同代码，构建不可复现 | 永远锁定具体 tag 或 commit hash |
| 配置速度慢 | 每次重新配置都要联网检查 | 设置 `FETCHCONTENT_FULLY_DISCONNECTED=ON` 离线用缓存 |
| 下载的库"污染"构建 | 有些库的 CMakeLists.txt 写得不好，会注册全局目标或覆盖你的选项 | 加 `EXCLUDE_FROM_ALL` 参数 |
| 网络环境差 | 公司内网 / CI 环境下载 GitHub 慢 | 配合 mirror 或预下载到本地目录 |

---

## 六、CMake Presets：团队统一构建配置

### 为什么需要这个？

典型痛点：

> "你帮我看一下为什么编译不过？"
> "你用的什么编译器？什么构建类型？开了什么选项？"
> "不知道，我就跟着 README 敲的命令……"

每个人的本地环境不同（编译器版本、构建目录、开启的选项），导致"在我这能编译"的经典问题。

**CMake Presets** 就是一个 JSON 文件，把所有构建配置固化下来，团队共享。

**类比**：就像 `.editorconfig` 统一代码风格一样，`CMakePresets.json` 统一构建配置。

### 6.1 文件结构（带注释说明）

```json
{
    "version": 6,   // Preset 文件格式版本（不是你项目的版本）
    "cmakeMinimumRequired": { "major": 3, "minor": 25 },

    // ===== 配置阶段 preset（cmake -B build 时用哪些选项）=====
    "configurePresets": [
        {
            "name": "dev-gcc",              // preset 的唯一标识符
            "displayName": "GCC Debug",     // 给人看的友好名称
            "generator": "Ninja",           // 用 Ninja 构建（比 Make 快）
            "binaryDir": "${sourceDir}/build/gcc-debug",  // 构建产物放哪
            "cacheVariables": {             // 等价于 -DXXX=YYY 参数
                "CMAKE_BUILD_TYPE": "Debug",
                "CMAKE_CXX_COMPILER": "g++-14",
                "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
            }
        },
        {
            "name": "dev-msvc",
            "displayName": "MSVC Release",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build/msvc-release",
            "toolchainFile": "C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Release"
            }
        },
        {
            "name": "ci-linux",
            "displayName": "CI Linux",
            "inherits": "dev-gcc",          // 继承 dev-gcc 的所有配置，然后覆盖部分
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "Release",  // 只改这两个，其他沿用 dev-gcc
                "BUILD_TESTING": "ON"
            }
        }
    ],

    // ===== 构建阶段 preset（cmake --build 时的选项）=====
    "buildPresets": [
        {
            "name": "dev-gcc",
            "configurePreset": "dev-gcc",   // 关联到哪个 configure preset
            "jobs": 8                       // 并行编译数（-j8）
        }
    ],

    // ===== 测试阶段 preset（ctest 时的选项）=====
    "testPresets": [
        {
            "name": "dev-gcc",
            "configurePreset": "dev-gcc",
            "output": { "outputOnFailure": true },  // 失败时显示详细输出
            "execution": { "timeout": 60 }          // 单个测试超时 60 秒
        }
    ]
}
```

### 6.2 使用方式

有了 Preset 文件后，所有人只需要三条命令：

```bash
# 配置（替代：cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++-14 ...）
cmake --preset dev-gcc

# 构建（替代：cmake --build build -j8）
cmake --build --preset dev-gcc

# 测试（替代：ctest --test-dir build --output-on-failure --timeout 60）
ctest --preset dev-gcc
```

### 6.3 最佳实践

| 文件 | 是否提交到仓库 | 用途 |
|------|--------------|------|
| `CMakePresets.json` | 提交 | 团队共享的官方配置（CI 也用这个） |
| `CMakeUserPresets.json` | 加入 `.gitignore` | 个人的本地覆盖（比如你用了特殊编译器路径） |

- 用 `inherits` 实现继承，避免大量重复配置
- CI 环境专门建一个 `ci-*` 系列 preset
- IDE（VSCode、CLion）都支持自动读取 Presets 文件

---

## 七、Toolchain 文件：交叉编译的钥匙

### 为什么需要这个？

正常编译：你在 x86 Windows 上编译，生成的 .exe 也在 x86 Windows 上跑。

但有时候你需要：
- 在 x86 机器上编译 ARM 的程序（比如树莓派、手机 APP 的 native 库）
- 指定特殊的编译器（不是系统默认的 gcc）
- 使用包管理器（vcpkg / Conan）的编译器配置

这就需要 **Toolchain 文件**——一个告诉 CMake "用什么编译器、目标平台是什么"的配置文件。

**类比**：正常编译是"在自家厨房做菜"，交叉编译是"按照别人家厨房的规格做菜，然后打包寄过去"。Toolchain 文件就是那份"对方厨房的设备清单"。

### 7.1 什么时候需要

| 场景 | 需要 Toolchain 吗？ |
|------|-------------------|
| 本机编译本机运行（最常见） | 不需要 |
| x86 机器编译 ARM 程序 | 需要 |
| 想用 clang 而不是系统默认 gcc | 可以用（也可以直接设 CXX） |
| 使用 vcpkg 管理依赖 | 需要（vcpkg 自带一个） |
| 使用 Conan 2.x 管理依赖 | 需要（Conan 自动生成一个） |

### 7.2 示例：ARM 交叉编译

```cmake
# 文件名：arm-toolchain.cmake
# 用途：在 x86 机器上编译 ARM64 Linux 的程序

# 告诉 CMake：目标系统是 Linux（不是当前的 Windows/macOS）
set(CMAKE_SYSTEM_NAME Linux)
# 目标 CPU 架构是 aarch64（ARM 64 位）
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 指定交叉编译器（这些编译器生成 ARM 代码，而非 x86 代码）
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# 告诉 CMake 去哪找目标平台的库和头文件
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)

# 查找策略：
#   NEVER = 不在 sysroot 里找可执行程序（用宿主机的 cmake/make 等）
#   ONLY  = 只在 sysroot 里找库/头文件（不要找到宿主机的 x86 库）
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

使用方式：

```bash
# 在配置阶段通过 -DCMAKE_TOOLCHAIN_FILE 传入
cmake -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake
cmake --build build
# 生成的二进制文件是 ARM 架构的，不能在当前 x86 机器上直接运行
```

### 7.3 vcpkg 的 Toolchain 机制

vcpkg 的核心原理就是一个 toolchain 文件，它 hook 了 CMake 的 `find_package` 流程：

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

效果：你写 `find_package(Boost)` 时，CMake 会优先在 vcpkg 安装目录里找 Boost，而不是去系统路径找。对你来说完全透明——CMakeLists.txt 不需要为 vcpkg 做任何修改。

### 7.4 Toolchain 与 Presets 结合

在实际项目中，通常把 toolchain 路径写进 Preset，避免每次手敲：

```json
{
    "name": "vcpkg-release",
    "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
    "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
    }
}
```

---

## 八、项目结构最佳实践

### 为什么需要关注目录结构？

CMake 项目的目录布局直接影响：
- 别人能不能快速理解你的项目
- `find_package` 导出是否正常工作
- 子目录之间的依赖关系是否清晰

一个好的目录结构就像一个整洁的文件柜——东西放在该放的地方，找起来自然快。

### 8.1 推荐目录布局

```
project/
├── CMakeLists.txt              # 顶层入口：定义项目、选项、引入子目录
├── CMakePresets.json           # 构建配置 preset（团队共享）
├── cmake/                      # 自定义 CMake 模块（可复用的 .cmake 脚本）
│   ├── projectConfig.cmake.in  #   → 第四章讲的 Config 模板
│   └── CompilerWarnings.cmake  #   → 封装编译器警告设置的函数
├── src/                        # 库的源码（.cpp 和内部 .h）
│   ├── CMakeLists.txt          #   → 定义库 target
│   ├── core/                   #   → 按功能模块划分子目录
│   └── utils/
├── include/                    # 公共头文件（别人 include 的入口）
│   └── project/                #   → 加一层项目名前缀，避免冲突
│       └── project.h           #   → #include <project/project.h>
├── tests/                      # 测试代码
│   └── CMakeLists.txt          #   → 定义测试 target
├── examples/                   # 使用示例
│   └── CMakeLists.txt
└── docs/                       # 文档
```

**为什么 include 下要加项目名子目录？**

避免头文件名冲突。比如你有个 `utils.h`，如果直接放在 `include/utils.h`，别人也有个 `utils.h` 就炸了。放在 `include/project/utils.h`，用户写 `#include <project/utils.h>`，就不会冲突。

### 8.2 顶层 CMakeLists.txt 模板（带详细注释）

```cmake
# ===== 这一行必须是文件的第一条命令 =====
# 声明最低 CMake 版本要求（3.20 是目前比较安全的起点）
cmake_minimum_required(VERSION 3.20)

# 声明项目信息
# VERSION：语义化版本号，后面可以通过 PROJECT_VERSION_MAJOR/MINOR/PATCH 引用
# LANGUAGES：本项目用到的语言（CXX = C++）
project(myproject
    VERSION 1.2.0
    LANGUAGES CXX
    DESCRIPTION "A modern C++ project"
)

# ===== 仅当本项目是顶层项目时，才设置这些选项 =====
# 为什么？因为如果别人通过 FetchContent 把你的项目拉进去当子目录，
# 你不应该覆盖他们的全局设置
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    # 禁止 GNU 扩展（如 typeof、typeof 等），确保代码可移植
    set(CMAKE_CXX_EXTENSIONS OFF)

    # 定义构建选项（用户可以通过 -DBUILD_TESTING=OFF 关闭）
    option(BUILD_TESTING "Build tests" ON)
    option(BUILD_EXAMPLES "Build examples" ON)
endif()

# ===== 声明外部依赖 =====
find_package(Boost 1.82 REQUIRED COMPONENTS system json)
find_package(OpenSSL REQUIRED)

# ===== 添加子目录（每个子目录有自己的 CMakeLists.txt）=====
add_subdirectory(src)   # 核心库

# 测试：仅在开启时编译（CI 可能 OFF 以节省时间）
if(BUILD_TESTING)
    enable_testing()          # 告诉 CMake "本项目有测试"
    add_subdirectory(tests)
endif()

# 示例
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

---

## 九、常见陷阱与避坑

### 为什么要单独列这一节？

因为 CMake 的历史包袱重，很多教程和 StackOverflow 答案还在用老写法。以下是我踩过的坑，记录下来免得重复犯错。

### 9.1 不要用 `file(GLOB)` 收集源文件

```cmake
# ❌ 看起来很方便，实际上是定时炸弹
file(GLOB SOURCES "src/*.cpp")
add_library(mylib ${SOURCES})
```

**为什么有问题？**

`file(GLOB)` 在配置阶段扫描目录。之后你新增了一个 `src/newfile.cpp`，CMake **不会自动重新配置**——它不知道有新文件！导致：
- 新文件没参与编译，链接时报"undefined reference"
- 删除文件后可能还在用缓存的旧文件列表

**正确做法：显式列出每个源文件**

```cmake
# ✅ 虽然啰嗦，但安全可靠
add_library(mylib
    src/core.cpp
    src/router.cpp
    src/middleware.cpp
)
```

**如果文件实在太多（50+）**，可以用 `CONFIGURE_DEPENDS` 缓解（CMake 3.12+）：

```cmake
# 折中方案：每次构建前重新扫描目录
# 注意：官方说这"不保证在所有生成器上都有效"，但实际上 Ninja/Make 都支持
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
```

### 9.2 不要在 target 外设置编译选项

```cmake
# ❌ 修改全局变量——影响所有 target（包括第三方库！）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# ✅ 只影响指定目标
target_compile_options(mylib PRIVATE -Wall)
```

**类比**：全局 `CMAKE_CXX_FLAGS` 就像在公司邮件里群发所有人——哪怕第三方库不需要 `-Wall`，它也被强制加上了，可能产生一堆你不想看到的警告。

### 9.3 cmake_minimum_required 必须是第一条命令

```cmake
# ✅ 正确顺序
cmake_minimum_required(VERSION 3.20)
project(myapp)

# ❌ 错误——project() 在前，CMake 会用默认的旧版 policy 行为
project(myapp)
cmake_minimum_required(VERSION 3.20)
```

**为什么？** `cmake_minimum_required` 除了检查版本，还会设置 CMake 的 **policy（策略）** 行为。不同版本的 CMake 对同一命令可能有不同默认行为（为了向后兼容）。这条命令告诉 CMake "请用 3.20 的行为模式"。

### 9.4 VERSION 范围写法（CMake 3.12+）

```cmake
# 意思是：我最低需要 3.20，但请启用到 3.30 的所有 policy 改进
cmake_minimum_required(VERSION 3.20...3.30)
```

如果用户的 CMake 版本是 3.25，它会启用 3.20~3.25 的所有 policy（不会超过自身版本）。这比单写 `VERSION 3.20` 更明确。

### 9.5 target_link_libraries 必须写访问修饰符

```cmake
# ❌ 老代码常见写法——含义模糊，CMake 会猜测传播行为
target_link_libraries(myapp Boost::system)

# ✅ 现代写法——显式声明：这个依赖是 myapp 的私有实现细节
target_link_libraries(myapp PRIVATE Boost::system)
```

不写修饰符时，CMake 会根据 `cmake_minimum_required` 的版本选择不同的默认行为——这就是隐式依赖，迟早出 bug。

### 9.6 不要用 `link_directories()`

```cmake
# ❌ 全局设置库搜索路径——和 include_directories 一样是全局污染
link_directories(/opt/mylibs/lib)

# ✅ 正确方式：通过 find_package 或 imported target 传递
find_package(MyLib REQUIRED)
target_link_libraries(myapp PRIVATE MyLib::MyLib)
```

---

## 十、实用技巧集锦

### 10.1 封装编译器警告为可复用函数

**问题**：每个 target 都要写一堆 `target_compile_options`，重复又容易遗漏。

**解决**：写一个 `.cmake` 模块，封装成函数，一行调用。

```cmake
# 文件：cmake/CompilerWarnings.cmake
# 用法：include(cmake/CompilerWarnings.cmake)
#       set_project_warnings(mylib)

function(set_project_warnings target)
    # GCC / Clang 的警告选项
    set(GCC_WARNINGS
        -Wall                   # 基本警告全开
        -Wextra                 # 额外警告
        -Wpedantic              # 严格标准兼容
        -Wshadow                # 变量遮蔽（外层同名变量被内层覆盖）
        -Wnon-virtual-dtor      # 基类析构不是 virtual（可能内存泄漏）
        -Wold-style-cast        # C 风格强转（建议用 static_cast 等）
        -Wcast-align            # 可能导致未对齐访问的转换
        -Woverloaded-virtual    # 重载（而非覆写）了虚函数
        -Wconversion            # 隐式类型转换可能丢失数据
        -Wsign-conversion       # signed/unsigned 隐式转换
        -Wnull-dereference      # 可能解引用空指针
        -Wformat=2              # printf 格式串检查
        -Wimplicit-fallthrough  # switch case 缺少 break/[[fallthrough]]
    )

    # MSVC 的警告选项（/W4 已经相当于 GCC 的 -Wall -Wextra）
    set(MSVC_WARNINGS
        /W4             # 开启到第 4 级警告
        /permissive-    # 严格标准模式
        /w14242         # 类型转换精度丢失
        /w14254         # 逻辑位运算符优先级
        /w14263         # 成员函数不覆写基类虚函数
        /w14265         # 类有虚函数但析构不是 virtual
        /w14287         # 无符号/负常量不匹配
        /w14296         # 表达式永远为 true/false
    )

    # 根据编译器选择对应的警告集
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE ${MSVC_WARNINGS})
    else()
        target_compile_options(${target} PRIVATE ${GCC_WARNINGS})
    endif()
endfunction()
```

使用时：

```cmake
include(cmake/CompilerWarnings.cmake)

add_library(mylib src/mylib.cpp)
set_project_warnings(mylib)  # 一行搞定
```

### 10.2 导出 compile_commands.json

```cmake
# 在顶层 CMakeLists.txt 或 Preset 里设置
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

**这是什么？** 一个 JSON 文件，记录了每个源文件的完整编译命令（编译器路径、所有 -I/-D 选项等）。

**谁需要它？**
- **clangd**（代码补全、跳转定义） — 没有这个文件，clangd 几乎无法工作
- **clang-tidy**（静态分析） — 需要它来理解你的编译配置
- **IDE**（VSCode/CLion） — 用它来提供准确的 IntelliSense

建议在 `CMakePresets.json` 里统一开启，确保团队所有人都有。

### 10.3 条件选项（Option 与 CMakeDependentOption）

```cmake
# 基本选项：用户可以通过 -DMYPROJECT_WITH_SSL=OFF 关闭
# option(变量名 "描述文字" 默认值)
option(MYPROJECT_WITH_SSL "Enable SSL support" ON)

# 依赖选项：只有当 SSL 开启时，HTTP/2 选项才有意义
include(CMakeDependentOption)
cmake_dependent_option(
    MYPROJECT_WITH_HTTP2            # 选项变量名
    "Enable HTTP/2 (requires SSL)" # 描述
    ON                              # 默认值（在条件满足时）
    "MYPROJECT_WITH_SSL"            # 前置条件（为真时选项才可用）
    OFF                             # 条件不满足时强制设为 OFF
)
# 效果：如果用户关了 SSL，HTTP/2 自动关闭，cmake-gui 里也不显示这个选项
```

### 10.4 版本号单一来源（避免多处维护）

**问题**：版本号出现在 CMakeLists.txt、头文件、打包脚本里，改一处忘改另一处。

**解决**：只在 `project(VERSION ...)` 里定义一次，其他地方自动引用。

```cmake
# CMakeLists.txt
project(mylib VERSION 2.6.0)   # 唯一的版本号来源

# 用 configure_file 自动生成版本头文件
configure_file(
    src/Version.h.in                                    # 输入模板
    ${CMAKE_CURRENT_BINARY_DIR}/include/mylib/Version.h # 输出到构建目录
)
```

`src/Version.h.in` 模板内容：

```cpp
#pragma once

// @xxx@ 会被 CMake 替换为实际值
#define MYLIB_VERSION_MAJOR @PROJECT_VERSION_MAJOR@   // → 2
#define MYLIB_VERSION_MINOR @PROJECT_VERSION_MINOR@   // → 6
#define MYLIB_VERSION_PATCH @PROJECT_VERSION_PATCH@   // → 0
#define MYLIB_VERSION       "@PROJECT_VERSION@"       // → "2.6.0"
```

---

## 十一、与包管理器集成

### 为什么需要包管理器？

手动管理依赖（下载源码、编译、安装、设置路径）太痛苦了。包管理器帮你：
- 一条命令安装所有依赖
- 自动处理依赖的依赖（传递依赖）
- 保证版本一致性

C++ 生态最主流的两个包管理器：**vcpkg**（微软出品）和 **Conan**（JFrog 出品）。

### 11.1 vcpkg（Manifest 模式）

**什么是 Manifest 模式？** 在项目根目录放一个 `vcpkg.json`，声明所需依赖。CMake 配置时自动安装。

```json
{
    "name": "myproject",
    "version-semver": "1.0.0",
    "dependencies": [
        "boost-asio",       // 只装 Boost 的 Asio 模块（不是整个 Boost）
        "boost-json",
        "openssl",
        "gtest"
    ]
}
```

使用方式：

```bash
# 通过 toolchain 文件告诉 CMake "用 vcpkg 找依赖"
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

**效果**：配置时 vcpkg 自动下载编译所有依赖；你的 CMakeLists.txt 里正常写 `find_package(Boost)` 就行。

### 11.2 Conan 2.x

Conan 用 Python 写配置文件，灵活度更高。

```python
# conanfile.py
from conan import ConanFile
from conan.tools.cmake import cmake_layout

class MyProjectConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"  # 影响编译的环境因素
    generators = "CMakeDeps", "CMakeToolchain"          # 生成 CMake 能理解的文件

    def requirements(self):
        self.requires("boost/1.84.0")    # 声明依赖和精确版本
        self.requires("openssl/3.2.0")

    def layout(self):
        cmake_layout(self)  # 使用标准 CMake 目录布局
```

使用流程：

```bash
# 第一步：安装依赖（Conan 下载/编译依赖，生成 CMake toolchain）
conan install . --output-folder=build --build=missing

# 第二步：正常 CMake 配置（使用 Conan 生成的 toolchain）
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake

# 第三步：正常构建
cmake --build build
```

**Conan 2.x 的 `CMakeDeps` 生成器做了什么？** 为每个依赖生成 `xxxConfig.cmake` 文件。所以你的 CMakeLists.txt 里正常写 `find_package(Boost)` 就行，完全不需要为 Conan 做任何特殊处理。

### 11.3 vcpkg vs Conan 怎么选？

| 维度 | vcpkg | Conan |
|------|-------|-------|
| 上手难度 | 简单（一个 JSON + 一个环境变量） | 中等（需要 Python + 学 conanfile 语法） |
| 包数量 | 2000+（微软主力维护） | 更多（社区贡献活跃） |
| 自定义打包 | 写 portfile（CMake 脚本） | 写 conanfile.py（Python） |
| 适合场景 | Windows 为主、VS/VSCode 集成好 | 跨平台、CI/CD 集成灵活 |

---

## 十二、CTest 与测试集成

### 为什么需要 CTest？

你已经有了 GoogleTest 来写测试用例。CTest 是 CMake 自带的**测试运行器**——它不关心你用什么测试框架（GTest、Catch2、自己写的都行），它负责：
- 发现所有注册的测试
- 并行执行、设置超时、捕获输出
- 按标签（Label）过滤运行
- 集成到 CI 流水线

**类比**：GoogleTest 是考试的题目和答案，CTest 是负责安排考场、计时、收卷的监考老师。

### 12.1 基础用法

```cmake
# 在顶层 CMakeLists.txt 里开启测试支持
enable_testing()

# 创建测试可执行文件
add_executable(test_router tests/test_router.cpp)
target_link_libraries(test_router PRIVATE mylib GTest::gtest_main)
#                                               ^^^^^^^^^^^^^^^^
# gtest_main 提供了 main 函数，你的测试文件不需要自己写 main()

# 自动发现测试用例（CMake 3.10+，推荐方式）
include(GoogleTest)  # 引入 CMake 自带的 GoogleTest 集成模块
gtest_discover_tests(test_router
    # gtest_discover_tests 会在构建后运行你的测试程序加 --gtest_list_tests 参数
    # 自动把每个 TEST/TEST_F 注册为一个 CTest 测试用例
    PROPERTIES TIMEOUT 60  # 每个测试用例超时 60 秒
)
```

### 12.2 按标签分组运行

```cmake
gtest_discover_tests(test_integration
    PROPERTIES
        TIMEOUT 120                         # 集成测试通常更慢，给更多时间
        LABELS "integration"                # 打标签，方便过滤
        ENVIRONMENT "TEST_DB_HOST=localhost" # 注入环境变量
)
```

运行测试的几种方式：

```bash
# 运行所有测试
ctest --test-dir build --output-on-failure

# 只运行带 "integration" 标签的测试
ctest --test-dir build -L integration --output-on-failure

# 只运行名字匹配 "Router" 的测试
ctest --test-dir build -R Router --output-on-failure

# 并行运行（-j4 = 4 个测试同时跑）
ctest --test-dir build -j4 --output-on-failure

# MSVC 多配置生成器需要指定配置
ctest --test-dir build -C Release --output-on-failure
```

---

## 总结：现代 CMake 核心原则速查

回顾全文，用一张表把最重要的原则串起来：

| 原则 | 做法 | 对应章节 |
|------|------|----------|
| 面向目标 | 所有配置通过 `target_xxx()` 设置，不用全局命令 | 第一章 |
| 显式传播 | 始终写 `PUBLIC` / `PRIVATE` / `INTERFACE`，不留模糊空间 | 第二章 |
| 延迟求值 | 需要条件逻辑时用 Generator Expressions，不用 `if()` + 全局变量 | 第三章 |
| 声明式依赖 | `find_package` + imported targets，不手动拼路径 | 第四章 |
| 锁定版本 | FetchContent 锁定 tag，vcpkg.json 声明依赖 | 第五、十一章 |
| 统一配置 | CMakePresets.json 消除"在我这能编译"问题 | 第六章 |
| 最小暴露 | 默认 `PRIVATE`，只有公共头文件需要的才 `PUBLIC` | 第二章 |
| 单一来源 | 版本号、编译器选项等只在一处定义，其他地方引用 | 第十章 |

### 如果你只记一件事

> **每个 target 管好自己的事，通过 `target_link_libraries` 让依赖关系自动传播。**

这一句话就是现代 CMake 的全部哲学。

---

## 推荐学习资源

| 资源 | 适合谁 | 说明 |
|------|--------|------|
| [It's Time To Do CMake Right](https://pabloariasal.github.io/2018/02/19/its-time-to-do-cmake-right/) | 入门 | 经典短文，20 分钟读完就能理解核心理念 |
| [More Modern CMake (HSF)](https://hsf-training.github.io/hsf-training-cmake-webpage/) | 入门→进阶 | 交互式教程，有动手练习 |
| [Effective Modern CMake](https://gist.github.com/mbinna/c61dbb39bca0e4fb7d1f73b0d66a4fd1) | 进阶 | 社区最佳实践清单，适合查阅 |
| [Professional CMake (Craig Scott)](https://crascit.com/professional-cmake/) | 进阶→专家 | 最权威的 CMake 书籍，覆盖所有细节 |
| [CMake 官方文档 cmake-buildsystem(7)](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html) | 参考 | 官方手册，概念解释最准确 |

---

## 写在最后

现代 CMake 的学习曲线确实陡峭——语法不美观、概念有门槛、历史包袱重。但一旦理解了 target-based 的思维模型，一切都会变得自然。

从传统 CMake 迁移到现代 CMake，本质上就是一个转变：

```
从 "我在这个目录下需要什么" → "这个目标需要什么、暴露什么"
```

把这个原则贯彻到底，你的 CMakeLists.txt 会变得：
- **清晰** — 每个 target 的依赖一目了然
- **可维护** — 改一个库的配置，所有使用者自动更新
- **可组合** — 别人能一行代码引入你的库
- **跨平台** — Presets + Toolchain 消除环境差异

不再有全局污染，不再有隐式依赖，不再有"在我这能编译你的不行"。

这就是现代 CMake 的力量。
