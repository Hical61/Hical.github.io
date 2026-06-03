+++
title = 'CMake 原理深度解析：从脚本到可执行文件的完整旅程'
date = '2025-06-02'
draft = false
tags = ["CMake", "C++", "构建系统", "编译原理", "工程实践"]
categories = ["C++"]
description = "从底层原理出发，系统讲解 CMake 的工作机制：配置阶段、生成阶段、构建阶段三段式流程，核心语法参数逐一拆解，附大量实例。读完这篇你会真正理解 CMake 在干什么，而不只是会复制粘贴。"
+++

# CMake 原理深度解析：从脚本到可执行文件的完整旅程

> CMake 不难，难的是不知道它在干什么。这篇文章的目标就是把黑盒变白盒。

---

## 写在前面

大多数人学 CMake 的路径是：搜到一份 `CMakeLists.txt`，改改能用就行，出了问题就再搜。结果用了好几年，遇到稍微复杂一点的场景（比如交叉编译、多配置、发布库）就卡住了。

根本原因是**没有建立原理模型**——不知道 CMake 执行 `cmake -B build` 时到底发生了什么，`project()`、`target_link_libraries()` 这些命令在内部做了什么。

这篇文章就是要填这个坑。

**前置要求**：用过 CMake 写过几行命令，知道"能编译就行"是什么感觉。

---

## 一、CMake 是什么？（真正的答案）

### 1.1 一句话定义

CMake 是一个**元构建系统（Meta Build System）**，它不直接编译代码，而是**生成其他构建系统的配置文件**。

这句话很关键，很多人把 CMake 当编译器用，但它跟编译器没有任何直接关系。

### 1.2 构建工具链全景

一个 C++ 项目从源码到可执行文件，经历了这些工具：

```
你写的代码
    │
    ▼
CMakeLists.txt（你告诉 CMake 项目结构是什么）
    │
    ▼ cmake -B build（配置 + 生成）
    │
    ├── Linux/macOS → Makefile 或 build.ninja
    ├── Windows     → Visual Studio .sln + .vcxproj 或 build.ninja
    └── macOS       → Xcode .xcodeproj
    │
    ▼ cmake --build build（或直接 make / ninja / msbuild）
    │
    ├── 编译器（gcc / clang / MSVC cl.exe）
    │     → .cpp → .o（目标文件）
    │
    └── 链接器（ld / lld / link.exe）
          → .o + .a/.lib → 最终的可执行文件或库
```

**类比**：CMake 就像建筑师画的图纸（蓝图）。施工队（make/ninja）按图纸干活，工人（编译器）才是真正拿锤子的人。图纸本身不盖楼。

### 1.3 为什么需要 CMake？

直接写 Makefile 不行吗？可以，但——

| 问题         | Makefile 的痛点                         | CMake 的解法                              |
| ------------ | --------------------------------------- | ----------------------------------------- |
| 跨平台       | Windows 的 Makefile 语法和 Linux 不一样 | 一份 CMakeLists.txt，生成各平台的构建文件 |
| 依赖管理     | 手写头文件依赖，漏了就无法增量编译      | CMake 自动追踪依赖                        |
| 编译器差异   | GCC 和 MSVC 的命令行参数完全不同        | CMake 屏蔽差异，统一接口                  |
| 项目结构复杂 | 大型项目手写 Makefile 的维护成本极高    | 子目录、模块、外部依赖都有成熟方案        |

---

## 二、三阶段执行模型（最重要的一节）

CMake 的执行分为三个阶段，理解这三个阶段是理解一切 CMake 问题的基础。

### 2.1 三阶段概览

```
阶段一：配置（Configure）
命令：cmake -B build
做什么：读取 CMakeLists.txt，执行 CMake 脚本，收集项目信息，写入 CMakeCache.txt

阶段二：生成（Generate）
命令：（自动，跟在配置后面）
做什么：根据 CMakeCache.txt 的信息，生成目标构建系统的文件（Makefile / .sln / build.ninja）

阶段三：构建（Build）
命令：cmake --build build
做什么：调用实际的构建工具（make/ninja/msbuild），它们再调用编译器和链接器
```

### 2.2 阶段一：配置（Configure）

这是 CMake 读你写的脚本的阶段。

**发生了什么：**

1. 从顶层 `CMakeLists.txt` 开始，逐行执行
2. 遇到 `add_subdirectory(src)` 就进入 `src/CMakeLists.txt` 递归执行
3. 在内存里建立目标（Target）的数据结构，记录每个目标的属性
4. 把关键变量写入 `CMakeCache.txt`（缓存文件）

```bash
# 完整的配置命令
cmake -B build -S . -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# 参数解释：
#   -B build       构建目录（CMake 生成的文件放这里，不污染源码目录）
#   -S .           源码目录（CMakeLists.txt 在哪，不写默认是当前目录）
#   -G "Ninja"     使用哪个生成器（Ninja / "Unix Makefiles" / "Visual Studio 17 2022"）
#   -D             定义 CMake 缓存变量（相当于给脚本传参数）
```

**CMakeCache.txt 是什么：**

配置完成后，`build/CMakeCache.txt` 里记录了所有的配置决策：

```
# 这是 CMakeCache.txt 的部分内容（简化）
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++
CMAKE_INSTALL_PREFIX:PATH=/usr/local
OPENSSL_ROOT_DIR:PATH=/opt/openssl
```

这个文件的核心作用：**下次运行 `cmake -B build` 时，已经缓存的值不需要重新探测**。

常见操作：

```bash
# 删掉缓存，强制重新配置（相当于"清除所有设置重来"）
rm -rf build/CMakeCache.txt
# 或者直接删整个 build 目录
rm -rf build/

# 修改缓存变量（不用改 CMakeLists.txt）
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```

### 2.3 阶段二：生成（Generate）

这个阶段通常跟配置阶段合并在一起，不需要单独触发。

生成器（Generator）决定了生成什么格式的构建文件：

| 生成器                   | 生成的文件          | 适合场景              |
| ------------------------ | ------------------- | --------------------- |
| `Unix Makefiles`（默认） | `Makefile`          | Linux/macOS 通用      |
| `Ninja`（推荐）          | `build.ninja`       | 快、跨平台，CI 首选   |
| `Visual Studio 17 2022`  | `.sln` + `.vcxproj` | Windows，要用 VS IDE  |
| `Xcode`                  | `.xcodeproj`        | macOS，要用 Xcode IDE |

**为什么推荐 Ninja？**

Ninja 是专门为速度设计的构建工具。与 Make 相比：
- 启动更快（不解析通用 Makefile 语法，只执行）
- 并行调度更高效
- 错误信息更清晰

```bash
# 指定生成器
cmake -B build -G Ninja
cmake -B build -G "Unix Makefiles"
cmake -B build -G "Visual Studio 17 2022" -A x64
#                                           ^^^^ 指定目标架构（x64 / Win32 / ARM64）
```

**单配置 vs 多配置生成器：**

这是个很容易踩坑的地方：

| 类型   | 例子                   | Debug/Release 怎么选                     |
| ------ | ---------------------- | ---------------------------------------- |
| 单配置 | Ninja / Unix Makefiles | 配置时就确定：`-DCMAKE_BUILD_TYPE=Debug` |
| 多配置 | Visual Studio / Xcode  | 配置时不确定，构建时选：`--config Debug` |

```bash
# 单配置：配置时指定
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 多配置（MSVC）：配置时不指定 BUILD_TYPE
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug    # 构建时选 Debug
cmake --build build --config Release  # 构建时选 Release
```

这就是为什么在 [现代 CMake 实践](现代cmake实践.md) 里提到要用 Generator Expressions 而不是 `if(CMAKE_BUILD_TYPE STREQUAL "Debug")`——多配置生成器下，`CMAKE_BUILD_TYPE` 在配置时是空的！

### 2.4 阶段三：构建（Build）

```bash
# 构建命令
cmake --build build

# 完整参数
cmake --build build          \
  --config Release           \  # 多配置生成器选 Release（单配置忽略）
  --target myapp             \  # 只构建 myapp 这个目标（不写就构建所有）
  --parallel 8               \  # 8 线程并行（等价于 make -j8）
  --verbose                     # 打印完整的编译命令（调试时很有用）
```

**clean 操作：**

```bash
# 清除构建产物（不删除 CMake 生成的 Makefile/build.ninja）
cmake --build build --target clean

# 完全重头来过（删掉整个 build 目录）
rm -rf build/
cmake -B build
cmake --build build
```

---

## 三、CMake 脚本语言基础

CMake 是一门真正的脚本语言，有变量、条件、循环、函数。理解这些基础是看懂复杂 CMakeLists.txt 的前提。

### 3.1 变量：一切都是字符串

CMake 的变量系统简单粗暴：**所有变量的值都是字符串（或字符串列表）**，没有整数类型、没有布尔类型（虽然有"假值"的概念）。

```cmake
# 设置变量
set(MY_VAR "hello world")

# 引用变量（用 ${} 包起来）
message("变量的值是: ${MY_VAR}")   # 输出：变量的值是: hello world

# 列表变量（用分号分隔，但通常写成多行）
set(SOURCE_FILES
    src/main.cpp
    src/utils.cpp
    src/router.cpp
)
# 上面等价于：set(SOURCE_FILES "src/main.cpp;src/utils.cpp;src/router.cpp")

# 引用列表（展开成空格分隔的多个参数）
add_executable(myapp ${SOURCE_FILES})
# 等价于：add_executable(myapp src/main.cpp src/utils.cpp src/router.cpp)
```

**引用 vs 展开（细节，重要）：**

```cmake
set(FILES "a.cpp" "b.cpp" "c.cpp")

# 带引号：整个列表作为一个字符串参数传入
message("${FILES}")    # 输出：a.cpp;b.cpp;c.cpp（一个字符串）

# 不带引号：列表展开成多个参数
message(${FILES})      # 输出：a.cpp b.cpp c.cpp（三个独立参数传给 message）
```

### 3.2 变量作用域

CMake 的变量有三个作用域层次：

```cmake
# 函数作用域（function scope）
function(my_func)
    set(LOCAL_VAR "only inside function")
    # PARENT_SCOPE：让变量"泄露"到调用者的作用域
    set(SHARED_VAR "visible to caller" PARENT_SCOPE)
endfunction()

my_func()
message(${SHARED_VAR})   # 能看到：visible to caller
message(${LOCAL_VAR})    # 空的，LOCAL_VAR 作用域已结束

# 目录作用域（directory scope）
# 父 CMakeLists.txt 里 set 的变量，子目录的 CMakeLists.txt 能继承（只读副本）
# 但子目录修改后，父目录看不到（除非用 CACHE）

# 缓存变量（全局，持久化到 CMakeCache.txt）
set(MY_OPTION "default_value" CACHE STRING "这是个可被用户覆盖的选项")
#                              ^^^^^ 类型（STRING/BOOL/PATH/FILEPATH）
```

### 3.3 条件语句

```cmake
# 基本 if
if(MY_VAR STREQUAL "hello")
    message("值是 hello")
elseif(MY_VAR MATCHES "^hel")    # 正则匹配
    message("以 hel 开头")
else()
    message("其他情况")
endif()

# 常用的条件判断方式：

# 字符串比较
if(A STREQUAL B)
if(NOT A STREQUAL B)

# 数值比较
if(VERSION GREATER_EQUAL 3)
if(NUM LESS 10)
if(NUM EQUAL 5)

# 文件/目录是否存在
if(EXISTS "/path/to/file")
if(IS_DIRECTORY "/path/to/dir")

# 变量是否有值（非空）
if(MY_VAR)          # MY_VAR 非空且不是 "0"/"FALSE"/"NO"/"OFF" 就为真
if(NOT MY_VAR)      # 反过来

# 目标是否已定义
if(TARGET mylib)

# 命令/函数是否存在
if(COMMAND my_function)
```

**"假值"有哪些（CMake 的布尔规则）：**

```cmake
# 以下所有情况 if() 判定为"假"：
if("")          # 空字符串
if("0")         # 字符串 "0"
if("FALSE")
if("NO")
if("OFF")
if("IGNORE")
if("NOTFOUND")
if("xxx-NOTFOUND")  # find_package 找不到时返回的格式
# 其他非空字符串都是"真"
```

### 3.4 循环

```cmake
# foreach 遍历列表
set(PLATFORMS "Linux" "Windows" "macOS")

foreach(platform IN LISTS PLATFORMS)
    message("构建平台: ${platform}")
endforeach()

# 遍历范围（生成 0 1 2 3 4）
foreach(i RANGE 4)
    message("i = ${i}")
endforeach()

# 遍历范围（指定起止和步长）
foreach(i RANGE 1 10 2)    # 1 3 5 7 9
    message("i = ${i}")
endforeach()

# while 循环（不太常用）
set(counter 0)
while(counter LESS 5)
    message("counter = ${counter}")
    math(EXPR counter "${counter} + 1")
endwhile()
```

### 3.5 函数和宏

```cmake
# 函数（有独立作用域，推荐）
function(add_project_library name)
    # ARGN 包含函数调用时传入的额外参数（name 之外的参数）
    add_library(${name} ${ARGN})
    target_compile_features(${name} PUBLIC cxx_std_20)
endfunction()

# 调用
add_project_library(mylib src/a.cpp src/b.cpp)
# 等价于：
# add_library(mylib src/a.cpp src/b.cpp)
# target_compile_features(mylib PUBLIC cxx_std_20)


# 宏（没有独立作用域，set 的变量会影响调用方）
macro(print_var varname)
    message("${varname} = ${${varname}}")
    #                      ^^^^^^^^^^^ 二次展开：先展开 varname 得到变量名，再展开变量值
endmacro()

set(MY_PATH "/usr/local/lib")
print_var(MY_PATH)   # 输出：MY_PATH = /usr/local/lib
```

**函数 vs 宏怎么选：**

- 默认用**函数**，作用域隔离，副作用可控
- 需要修改调用方变量时才用**宏**

---

## 四、核心命令深度解析

### 4.1 cmake_minimum_required

```cmake
cmake_minimum_required(VERSION 3.20)
cmake_minimum_required(VERSION 3.20...3.30)   # 范围写法（推荐）
```

**它做了两件事：**

1. 检查当前 CMake 版本是否满足要求，不满足就报错退出
2. 设置**策略（Policy）**行为——CMake 的行为随版本演进，`cmake_minimum_required` 告诉 CMake 用哪个版本的行为模式

```cmake
# 范围写法的含义：
cmake_minimum_required(VERSION 3.20...3.30)
# 意思：
# - 最低要求 3.20（低于这个版本直接报错）
# - 如果用户的 CMake 是 3.25，就启用 3.20~3.25 的所有 policy 改进
# - 如果用户的 CMake 是 3.35（高于 3.30），只启用到 3.30 的 policy
# 效果：你的脚本在 3.20~3.30+ 的 CMake 上都能正确运行
```

> 必须是 CMakeLists.txt 的**第一条命令**。先写 `project()` 再写 `cmake_minimum_required()` 是常见错误，会导致用旧版 policy 解析 project()。

### 4.2 project()

```cmake
project(myapp
    VERSION   1.2.3          # 语义化版本
    LANGUAGES CXX C          # 用到的语言（不写默认 CXX C，会探测编译器）
    DESCRIPTION "My App"     # 项目描述（可选）
    HOMEPAGE_URL "https://example.com"  # 主页（可选）
)
```

**它做了什么：**

1. 设置 `PROJECT_NAME`、`CMAKE_PROJECT_NAME` 变量
2. 设置 `PROJECT_VERSION`、`PROJECT_VERSION_MAJOR/MINOR/PATCH/TWEAK` 变量
3. **探测编译器**（这是开销最大的步骤之一）——CMake 会编译一个简单的测试程序来确认编译器可用
4. 设置 `PROJECT_SOURCE_DIR`、`PROJECT_BINARY_DIR` 等路径变量

**常用变量：**

```cmake
project(myapp VERSION 1.2.3 LANGUAGES CXX)

# project() 之后可以用这些变量：
message(${PROJECT_NAME})            # myapp
message(${PROJECT_VERSION})         # 1.2.3
message(${PROJECT_VERSION_MAJOR})   # 1
message(${PROJECT_VERSION_MINOR})   # 2
message(${PROJECT_VERSION_PATCH})   # 3
message(${PROJECT_SOURCE_DIR})      # /path/to/your/project
message(${PROJECT_BINARY_DIR})      # /path/to/build

# 区分顶层项目和子项目（通过 FetchContent 引入时很有用）
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    message("这是顶层项目")
endif()
```

### 4.3 add_executable / add_library

```cmake
# 可执行文件
add_executable(target_name source1.cpp source2.cpp ...)

# 普通库
add_library(target_name [STATIC|SHARED|MODULE] source1.cpp ...)
#   STATIC  → .a / .lib（静态库，链接时把代码打包进可执行文件）
#   SHARED  → .so / .dll（动态库，运行时动态加载）
#   MODULE  → 动态加载的插件（不用于链接，用于 dlopen）
#   不写    → 由 BUILD_SHARED_LIBS 变量决定（默认 STATIC）

# 接口库（没有源文件，只是配置的容器）
add_library(target_name INTERFACE)
# 用途：header-only 库、公共编译选项的"配置包"

# 对象库（编译但不链接成库，可以被多个目标复用）
add_library(target_name OBJECT source1.cpp ...)
# 链接时：target_link_libraries(myapp PRIVATE $<TARGET_OBJECTS:obj_lib>)
```

**STATIC vs SHARED 怎么选：**

| 场景                       | 选 STATIC                       | 选 SHARED                     |
| -------------------------- | ------------------------------- | ----------------------------- |
| 分发单个可执行文件         | ✅ 不需要依赖动态库就能运行      | ❌ 用户得自己装对应的 .so/.dll |
| 多个程序复用同一个库       | ❌ 每个程序都复制一份代码        | ✅ 内存里只有一份，节省空间    |
| 插件系统（运行时动态加载） | ❌ 不支持                        | ✅（MODULE 类型）              |
| 编译速度（大型项目）       | ❌ 链接慢（每次都要合并所有 .o） | ✅ 链接快（只链接头部信息）    |

### 4.4 target_include_directories

```cmake
target_include_directories(target_name
    [SYSTEM]          # 标记为系统头文件（编译器不报这些头文件里的警告）
    [BEFORE]          # 把这些路径插到搜索路径最前面（优先级最高）
    <PUBLIC|PRIVATE|INTERFACE>
    dir1 dir2 ...
)
```

**参数逐一解释：**

```cmake
add_library(mylib src/impl.cpp)

target_include_directories(mylib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    # PUBLIC 含义：
    # 1. mylib 编译时，编译器在 include/ 里找头文件
    # 2. 任何 target_link_libraries(xxx PRIVATE/PUBLIC mylib) 的目标
    #    也自动获得这个 include 路径

    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    # PRIVATE 含义：
    # 1. mylib 编译时，编译器也在 src/ 里找头文件（impl 目录）
    # 2. 但链接 mylib 的其他目标看不到这个 src/ 路径
    #    ——实现细节，不暴露给外部

    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party_headers
    # INTERFACE 含义：
    # 1. mylib 自己编译时不用这个路径
    # 2. 但链接 mylib 的目标会自动获得这个路径
    # 用途：header-only 库（自己没有 .cpp，不需要找头文件）
)

# SYSTEM 参数：把 include 路径标记为"系统路径"
# 效果：这些路径里的头文件的警告会被编译器静默
target_include_directories(mylib SYSTEM PRIVATE /opt/legacy_library/include)
# 常用于引入你不打算修改的第三方库
```

**一个容易搞混的细节——绝对路径 vs 相对路径：**

```cmake
# CMake 里的相对路径是相对于「当前 CMakeLists.txt 所在目录」
# 用下面这些变量来构造绝对路径（推荐）：
target_include_directories(mylib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # ^^^^^^^^^^^^^^^^^^^^^^^^ 当前 CMakeLists.txt 所在的源码目录（绝对路径）
)

# 对比：
#   CMAKE_SOURCE_DIR      → 整个项目最顶层的源码目录（根 CMakeLists.txt 在哪）
#   CMAKE_CURRENT_SOURCE_DIR → 当前正在处理的 CMakeLists.txt 所在目录（子目录时不同）
#   CMAKE_BINARY_DIR      → 构建目录的根
#   CMAKE_CURRENT_BINARY_DIR → 当前子目录对应的构建目录
```

### 4.5 target_link_libraries

这是现代 CMake 最核心的命令，几乎所有依赖关系都通过它表达。

```cmake
target_link_libraries(target_name
    <PUBLIC|PRIVATE|INTERFACE>  library1 library2 ...
    [<PUBLIC|PRIVATE|INTERFACE> library3 ...]  # 可以有多组
)
```

**传播规则图解：**

```
假设有这样的依赖关系：
  App → (PRIVATE) → LibB → (PUBLIC) → LibC

target_link_libraries(LibB PUBLIC  LibC)   # LibC 对外公开
target_link_libraries(App  PRIVATE LibB)   # LibB 是 App 的私有依赖

结果：
  App 能找到 LibC 的头文件、能链接 LibC ✓（因为 LibB 把 LibC PUBLIC 传出去了）
  LibX 依赖 App → LibX 看不到 LibB 和 LibC ✓（PRIVATE 截断了传播）
```

**链接什么类型的东西：**

```cmake
# 1. 链接你自己定义的目标
target_link_libraries(myapp PRIVATE mylib another_lib)

# 2. 链接 find_package 找到的导入目标（最常见）
find_package(OpenSSL REQUIRED)
target_link_libraries(myapp PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# 3. 链接系统库（不推荐，但有时必要）
target_link_libraries(myapp PRIVATE pthread dl)

# 4. 链接具体的库文件路径（更不推荐，绑死平台）
target_link_libraries(myapp PRIVATE /usr/local/lib/libfoo.a)

# 5. 传递编译器选项（极少数场景）
# 某些老库没有 CMake 支持，可以用字符串形式
target_link_libraries(myapp PRIVATE "-framework CoreFoundation")  # macOS 专用
```

### 4.6 target_compile_options

```cmake
target_compile_options(target_name
    <PUBLIC|PRIVATE|INTERFACE>
    option1 option2 ...
)
```

**常用编译选项：**

```cmake
target_compile_options(mylib PRIVATE
    # === GCC/Clang 选项 ===
    -Wall           # 开启大部分警告
    -Wextra         # 更多警告
    -Wpedantic      # 严格 ISO C++ 合规
    -Werror         # 警告当错误处理（CI 推荐）

    -O0             # 不优化（Debug）
    -O2             # 中等优化（通常的 Release）
    -O3             # 全力优化
    -Os             # 优化代码体积
    -Oz             # 极致压缩（clang 支持）

    -g              # 包含调试信息（GDB 需要）
    -ggdb3          # GDB 专用调试信息（更详细）
    -fno-omit-frame-pointer  # 保留帧指针（性能采样需要）

    -fsanitize=address       # AddressSanitizer（内存错误检测）
    -fsanitize=undefined     # UBSanitizer（未定义行为检测）
    -fsanitize=thread        # ThreadSanitizer（数据竞争检测）

    # === MSVC 选项 ===
    /W4             # 警告级别 4
    /WX             # 警告当错误
    /O2             # 优化
    /Od             # 禁止优化（Debug）
    /Zi             # 调试信息（PDB 格式）
    /MP             # 多处理器并行编译
    /permissive-    # 严格标准模式
    /utf-8          # 源文件 UTF-8 编码
)
```

**最佳实践：配合生成表达式按编译器选择：**

```cmake
target_compile_options(mylib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Debug>:-O0 -g -fsanitize=address>
        $<$<CONFIG:Release>:-O3>
    >
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4 /permissive-
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<CONFIG:Release>:/O2>
    >
)
```

### 4.7 target_compile_definitions

```cmake
target_compile_definitions(target_name
    <PUBLIC|PRIVATE|INTERFACE>
    DEFINE1             # 相当于 -DDEFINE1（定义宏，值为 1）
    DEFINE2=value       # 相当于 -DDEFINE2=value（定义宏并赋值）
    DEFINE3="hello"     # 字符串值（注意引号可能需要转义）
)
```

**实际例子：**

```cmake
add_library(net_lib src/network.cpp)

target_compile_definitions(net_lib
    PUBLIC
        NET_LIB_VERSION=2          # 公共宏（使用者也能 #ifdef 这个）

    PRIVATE
        _WIN32_WINNT=0x0A00        # 目标 Windows 10（仅内部编译用）
        OPENSSL_NO_DEPRECATED      # 禁用 OpenSSL 废弃 API 的警告

    INTERFACE
        USE_NET_LIB                # 使用者会自动获得这个宏定义
)
```

### 4.8 target_compile_features

比直接指定 `-std=c++20` 更好的方式：

```cmake
# 指定 C++ 标准
target_compile_features(mylib PUBLIC cxx_std_20)
#                                    ^^^^^^^^^^ 要求 C++20
# 效果：CMake 会自动加上对应的 -std=c++20 或 /std:c++20（根据编译器）

# 可用的值：
# cxx_std_11 / cxx_std_14 / cxx_std_17 / cxx_std_20 / cxx_std_23 / cxx_std_26

# 还可以指定具体的特性（CMake 帮你检查编译器是否支持）
target_compile_features(mylib PRIVATE
    cxx_auto_type       # auto 关键字
    cxx_range_for       # range-based for
    cxx_nullptr         # nullptr
    cxx_lambdas         # lambda 表达式
)
# 用具体特性的好处：你写的代码能在所有支持该特性的编译器上跑
# 缺点：CMake 的特性列表比较老，新特性没有细粒度支持
# 实践：大多数人直接用 cxx_std_XX，不细化到具体特性
```

---

## 五、变量系统详解

### 5.1 内置变量分类

CMake 有大量内置变量，按用途分几类：

**路径类：**

```cmake
# 源码路径
CMAKE_SOURCE_DIR          # 顶层 CMakeLists.txt 所在目录（项目根）
CMAKE_CURRENT_SOURCE_DIR  # 当前处理的 CMakeLists.txt 所在目录
PROJECT_SOURCE_DIR        # 最近一次 project() 调用处的源码目录

# 构建路径
CMAKE_BINARY_DIR          # 构建根目录（-B 指定的那个）
CMAKE_CURRENT_BINARY_DIR  # 当前子目录对应的构建目录
PROJECT_BINARY_DIR        # 最近一次 project() 调用处的构建目录

# 安装路径
CMAKE_INSTALL_PREFIX      # install() 的安装根目录（默认 /usr/local）
```

**编译器类：**

```cmake
CMAKE_CXX_COMPILER        # C++ 编译器完整路径（/usr/bin/g++）
CMAKE_CXX_COMPILER_ID     # 编译器标识（GNU / Clang / MSVC / AppleClang）
CMAKE_CXX_COMPILER_VERSION # 编译器版本（13.2.0）
CMAKE_CXX_STANDARD        # 全局 C++ 标准（但推荐用 target_compile_features）
CMAKE_CXX_EXTENSIONS      # 是否允许 GNU 扩展（建议设 OFF）
```

**构建类型：**

```cmake
CMAKE_BUILD_TYPE          # Debug / Release / RelWithDebInfo / MinSizeRel
BUILD_SHARED_LIBS         # 全局控制 add_library 默认生成动态库还是静态库
```

**系统信息：**

```cmake
CMAKE_SYSTEM_NAME         # 目标系统（Linux / Windows / Darwin）
CMAKE_SYSTEM_PROCESSOR    # 目标 CPU（x86_64 / aarch64 / arm）
UNIX / WIN32 / APPLE      # 布尔值，快速判断平台
MSVC / GCC / CLANG        # 编译器快捷判断（不推荐，用 CMAKE_CXX_COMPILER_ID）
```

### 5.2 变量缓存（Cache）

缓存变量存在 `CMakeCache.txt` 里，跨次调用保持值：

```cmake
# 定义缓存变量
set(MY_OPTION "default" CACHE STRING "这是一个可配置的选项")
#                        ^^^^^ 类型：STRING / BOOL / PATH / FILEPATH / INTERNAL

# FORCE：强制覆盖已有缓存值（谨慎使用）
set(MY_OPTION "forced_value" CACHE STRING "" FORCE)

# option() 是 BOOL 型缓存变量的简写
option(ENABLE_FEATURE "启用某功能" ON)
# 等价于：
set(ENABLE_FEATURE ON CACHE BOOL "启用某功能")
```

**用户怎么修改缓存变量：**

```bash
# 命令行（推荐，CI 里用这个）
cmake -B build -DMY_OPTION=new_value

# 交互式 TUI（本地调试用）
ccmake build/

# GUI
cmake-gui
```

### 5.3 环境变量

```cmake
# 读取环境变量
message("HOME = $ENV{HOME}")
message("PATH = $ENV{PATH}")

# 在 CMake 里设置环境变量（只影响 CMake 进程，不影响外部）
set(ENV{MYVAR} "some_value")

# 实际用途：读取用户设置的路径
if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()
```

---

## 六、目标（Target）系统深度解析

### 6.1 目标是什么

目标（Target）是 CMake 的核心数据结构，你对目标做的所有配置（include 路径、编译选项、链接库）都作为**属性（Property）**存在目标上。

```cmake
add_library(mylib src/impl.cpp)

# 以下命令本质都是在设置 mylib 这个 target 的属性
target_include_directories(mylib ...)   # 设置 INCLUDE_DIRECTORIES 属性
target_compile_options(mylib ...)       # 设置 COMPILE_OPTIONS 属性
target_link_libraries(mylib ...)        # 设置 LINK_LIBRARIES 属性
target_compile_definitions(mylib ...)   # 设置 COMPILE_DEFINITIONS 属性
```

### 6.2 手动读写目标属性

```cmake
# 读取属性
get_target_property(value target_name property_name)
# 例：
get_target_property(inc_dirs mylib INCLUDE_DIRECTORIES)
message("include dirs: ${inc_dirs}")

# 设置属性（底层操作，一般用 target_xxx 命令代替）
set_target_properties(mylib PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "libmylib"     # 改变输出文件名
    POSITION_INDEPENDENT_CODE ON  # 相当于 -fPIC
)

# 单次设置
set_property(TARGET mylib PROPERTY CXX_STANDARD 20)
```

**有用的目标属性：**

```cmake
set_target_properties(mylib PROPERTIES
    CXX_STANDARD 20               # C++ 标准
    CXX_STANDARD_REQUIRED ON      # 编译器不支持就报错（而非降级）
    CXX_EXTENSIONS OFF            # 禁止 GNU 扩展

    POSITION_INDEPENDENT_CODE ON  # 生成位置无关代码（共享库必须）
    VISIBILITY_INLINES_HIDDEN ON  # 隐藏内联函数符号（减少 .so 体积）
    CXX_VISIBILITY_PRESET hidden  # 默认隐藏所有符号（白名单式导出）

    OUTPUT_NAME "myrename"        # 改输出文件名（不影响 target 名）
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"  # .exe/.dll 放哪
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"  # .so 放哪
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"  # .a 放哪

    DEBUG_POSTFIX "d"             # Debug 版加后缀，比如 libmylib d.a
)
```

### 6.3 Imported Targets（导入目标）

`find_package` 找到的库，CMake 会创建**导入目标（Imported Target）**——它是一个虚拟目标，没有对应的 CMakeLists.txt，但设置了完整的属性。

```cmake
find_package(OpenSSL REQUIRED)

# find_package 执行后，CMake 创建了这些导入目标（具体取决于库）：
#   OpenSSL::SSL
#   OpenSSL::Crypto

# 你可以像普通目标一样使用它们
target_link_libraries(myapp PRIVATE OpenSSL::SSL)

# 还可以查看它的属性（调试用）
get_target_property(ssl_include OpenSSL::SSL INTERFACE_INCLUDE_DIRECTORIES)
message("OpenSSL include: ${ssl_include}")
```

**手动创建 Imported Target（给没有 CMake 支持的老库用）：**

```cmake
# 创建一个静态库的导入目标
add_library(legacy_lib STATIC IMPORTED)

set_target_properties(legacy_lib PROPERTIES
    IMPORTED_LOCATION "/opt/legacy/lib/liblegacy.a"   # 实际文件路径
    INTERFACE_INCLUDE_DIRECTORIES "/opt/legacy/include"
)

# 使用方式和普通 target 一样
target_link_libraries(myapp PRIVATE legacy_lib)
```

---

## 七、install() 命令详解

### 7.1 为什么需要 install()

`cmake --build` 只是把产物放在 `build/` 目录里。`cmake --install` 才是把文件安装到系统目录（或你指定的目录），让别人能用。

```bash
# 构建
cmake --build build

# 安装（把头文件、库文件、可执行文件复制到系统目录）
cmake --install build
cmake --install build --prefix /opt/myapp   # 自定义安装路径（覆盖 CMAKE_INSTALL_PREFIX）
cmake --install build --component dev        # 只安装某个"组件"
```

### 7.2 install() 的几种用法

```cmake
# 安装可执行文件
install(TARGETS myapp
    RUNTIME DESTINATION bin     # .exe 放到 <prefix>/bin/
)

# 安装库
install(TARGETS mylib
    ARCHIVE DESTINATION lib     # .a/.lib 静态库 → <prefix>/lib/
    LIBRARY DESTINATION lib     # .so/.dylib 动态库 → <prefix>/lib/
    RUNTIME DESTINATION bin     # .dll（Windows 动态库）→ <prefix>/bin/
    INCLUDES DESTINATION include # 记录头文件路径（给 EXPORT 用）
)

# 安装头文件
install(
    DIRECTORY include/          # 源目录（结尾的 / 很重要！）
    DESTINATION include         # 安装到 <prefix>/include/
    FILES_MATCHING              # 只安装匹配的文件（而不是整个目录）
    PATTERN "*.h"               # 只安装 .h 文件
    PATTERN "private" EXCLUDE   # 排除 private 目录
)

# 安装单个文件
install(FILES
    cmake/myConfig.cmake
    DESTINATION lib/cmake/mylib
)

# 安装时执行脚本
install(SCRIPT setup_post_install.cmake)

# 安装组件（让用户只安装需要的部分）
install(TARGETS myapp
    RUNTIME DESTINATION bin
    COMPONENT runtime            # 运行时组件
)
install(TARGETS mylib
    ARCHIVE DESTINATION lib
    COMPONENT dev                # 开发组件（头文件+静态库）
)
install(DIRECTORY include/
    DESTINATION include
    COMPONENT dev
)
```

### 7.3 GNUInstallDirs（推荐）

不要硬编码 `bin`、`lib`、`include`——不同系统有不同约定（比如 64 位 Linux 可能用 `lib64`）：

```cmake
include(GNUInstallDirs)  # 引入标准化安装目录变量

install(TARGETS myapp
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}     # 通常是 bin
)
install(TARGETS mylib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}     # 通常是 lib 或 lib64
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}         # 通常是 include
)

# GNUInstallDirs 提供的变量：
# CMAKE_INSTALL_BINDIR        → bin
# CMAKE_INSTALL_LIBDIR        → lib（或 lib64）
# CMAKE_INSTALL_INCLUDEDIR    → include
# CMAKE_INSTALL_DATADIR       → share
# CMAKE_INSTALL_DOCDIR        → share/doc/<project>
# CMAKE_INSTALL_MANDIR        → share/man
```

---

## 八、CMake 模块系统

### 8.1 什么是 CMake 模块

CMake 模块是 `.cmake` 后缀的脚本文件，用 `include()` 加载。CMake 自带了大量内置模块，放在 CMake 安装目录的 `Modules/` 文件夹里。

```cmake
# 加载内置模块
include(GNUInstallDirs)            # 标准安装目录
include(CMakePackageConfigHelpers) # 生成 Config 文件的工具
include(FetchContent)              # 从网络拉取依赖
include(GoogleTest)                # GoogleTest 集成
include(CheckCXXCompilerFlag)      # 检查编译器是否支持某个 flag
include(CheckIncludeFileCXX)       # 检查头文件是否存在
include(CMakeDependentOption)      # 依赖条件的选项

# 加载你自己的模块（搜索路径受 CMAKE_MODULE_PATH 控制）
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(MyCustomModule)
```

### 8.2 常用内置模块示例

**CheckCXXCompilerFlag：检查编译器支持哪些 flag**

```cmake
include(CheckCXXCompilerFlag)

# 检查编译器是否支持 -march=native
check_cxx_compiler_flag("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
if(COMPILER_SUPPORTS_MARCH_NATIVE)
    target_compile_options(mylib PRIVATE -march=native)
endif()
```

**CheckIncludeFileCXX：检查头文件是否存在**

```cmake
include(CheckIncludeFileCXX)

check_include_file_cxx("optional" HAS_STD_OPTIONAL)
if(NOT HAS_STD_OPTIONAL)
    # 降级处理：用第三方实现
    target_compile_definitions(mylib PRIVATE USE_ABSL_OPTIONAL)
endif()
```

**CMakePackageConfigHelpers：发布自己的库**

（详见现代 CMake 实践文章里的第四章，这里不重复了）

---

## 九、调试 CMakeLists.txt

遇到问题时，这些技巧能大幅提升排查效率。

### 9.1 打印变量值

```cmake
# 基本打印
message("变量值: ${MY_VAR}")

# 带级别的消息
message(STATUS  "这是状态信息（-- 前缀）")
message(WARNING "这是警告（不中断）")
message(SEND_ERROR "这是错误（继续执行但配置失败）")
message(FATAL_ERROR "这是致命错误（立即停止）")

# 调试专用（需要 --log-level=DEBUG 才显示）
message(DEBUG "调试信息")

# 打印所有属性（非常有用）
get_target_property(all_inc mylib INCLUDE_DIRECTORIES)
message("mylib include dirs: ${all_inc}")

get_target_property(all_opts mylib COMPILE_OPTIONS)
message("mylib compile opts: ${all_opts}")
```

### 9.2 查看所有变量

```cmake
# 打印所有 CMake 变量（非常多，加个过滤器）
get_cmake_property(_variableNames VARIABLES)
foreach(v IN LISTS _variableNames)
    if(v MATCHES "OPENSSL")   # 只看 OpenSSL 相关的
        message("${v}=${${v}}")
    endif()
endforeach()
```

### 9.3 命令行调试选项

```bash
# 打印 CMake 详细的配置过程（搜索库的过程）
cmake -B build --log-level=DEBUG

# 打印 find_package 的详细搜索路径
cmake -B build --debug-find

# 打印每条 CMake 命令的执行位置
cmake -B build --trace

# 仅追踪你自己的脚本（过滤掉 CMake 内置的）
cmake -B build --trace-source=CMakeLists.txt

# 构建时打印完整的编译命令
cmake --build build --verbose
# 或者等价地
VERBOSE=1 make -C build
```

### 9.4 检查 CMakeCache.txt

配置出问题时，先看 `build/CMakeCache.txt`——找到你关心的变量，看它的实际值是什么。

```bash
# 快速搜索缓存里的某个变量
grep -i "openssl" build/CMakeCache.txt
```

---

## 十、完整项目示例

把前面所有概念串起来，一个完整的真实项目长这样：

### 10.1 目录结构

```
my_project/
├── CMakeLists.txt              ← 顶层入口
├── CMakePresets.json           ← 构建预设
├── cmake/
│   ├── CompilerWarnings.cmake  ← 警告配置模块
│   └── myProjectConfig.cmake.in ← Config 模板
├── include/
│   └── my_project/
│       ├── server.h
│       └── utils.h
├── src/
│   ├── CMakeLists.txt          ← 库目标定义
│   ├── server.cpp
│   └── utils.cpp
├── tests/
│   ├── CMakeLists.txt          ← 测试目标定义
│   └── test_server.cpp
└── examples/
    ├── CMakeLists.txt
    └── simple_server.cpp
```

### 10.2 顶层 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20...3.28)

project(my_project
    VERSION 1.0.0
    LANGUAGES CXX
    DESCRIPTION "A sample server project"
)

# 只有顶层项目才做这些全局设置
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(CMAKE_CXX_EXTENSIONS OFF)    # 禁止 GNU 扩展

    option(MY_PROJECT_BUILD_TESTS    "构建测试" ON)
    option(MY_PROJECT_BUILD_EXAMPLES "构建示例" ON)
    option(MY_PROJECT_WITH_SSL       "启用 SSL" ON)

    # 导出 compile_commands.json 给 clangd/clang-tidy 用
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# 加载自定义 CMake 模块
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# 外部依赖
if(MY_PROJECT_WITH_SSL)
    find_package(OpenSSL REQUIRED)
endif()

include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.13.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# 子目录
add_subdirectory(src)

if(MY_PROJECT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MY_PROJECT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

### 10.3 src/CMakeLists.txt

```cmake
add_library(my_project_lib
    server.cpp
    utils.cpp
)

# 给目标起个别名（带命名空间前缀，风格统一）
add_library(my_project::lib ALIAS my_project_lib)

# C++ 标准
target_compile_features(my_project_lib PUBLIC cxx_std_20)

# 头文件路径
target_include_directories(my_project_lib
    PUBLIC  ${CMAKE_SOURCE_DIR}/include        # 公共头文件
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}        # 实现细节头文件
)

# 编译器警告（从自定义模块加载）
include(CompilerWarnings)
set_project_warnings(my_project_lib)

# 条件依赖
if(MY_PROJECT_WITH_SSL)
    target_link_libraries(my_project_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(my_project_lib PUBLIC MY_PROJECT_WITH_SSL)
endif()

# 日志库（私有依赖，只有实现需要）
target_link_libraries(my_project_lib PRIVATE spdlog::spdlog)

# 安装规则
include(GNUInstallDirs)
install(TARGETS my_project_lib
    EXPORT my_project-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
```

### 10.4 tests/CMakeLists.txt

```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(test_server test_server.cpp)

target_link_libraries(test_server PRIVATE
    my_project::lib    # 用别名（跟外部用法一致）
    GTest::gtest_main  # 包含 main 函数的 gtest
)

include(GoogleTest)
gtest_discover_tests(test_server
    PROPERTIES TIMEOUT 30
)
```

---

## 总结：CMake 原理速查

| 概念            | 核心要点                                                      |
| --------------- | ------------------------------------------------------------- |
| 三阶段          | 配置（读脚本）→ 生成（写 Makefile/ninja）→ 构建（调编译器）   |
| CMakeCache.txt  | 持久化配置，用 `-D` 修改，出问题先看这里                      |
| 生成器          | 决定生成什么格式，单配置（Ninja）和多配置（MSVC）行为不同     |
| Target 与属性   | 一切配置都是目标的属性，用 `target_xxx()` 而不是全局命令      |
| PUBLIC/PRIVATE  | 控制属性传播：PRIVATE 截断，PUBLIC 传递，INTERFACE 只传不用   |
| Imported Target | `find_package` 的结果，封装好属性的"虚拟目标"                 |
| 变量作用域      | 函数作用域 → 目录作用域 → 缓存（全局）                        |
| install()       | 把产物从 `build/` 复制到系统目录，`GNUInstallDirs` 保证跨平台 |

### 常用命令速记

```bash
# 配置
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build --parallel 8

# 安装
cmake --install build --prefix /opt/my_project

# 调试
cmake -B build --log-level=DEBUG --debug-find
cmake --build build --verbose

# 清理重来
rm -rf build/ && cmake -B build && cmake --build build
```

> 理解了三阶段模型和 Target 属性系统，CMake 就不再神秘了。剩下的都是在这个框架上的具体应用。
