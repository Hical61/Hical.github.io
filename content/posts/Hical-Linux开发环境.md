+++
title = 'Hical Linux 开发环境搭建'
date = '2026-05-08'
draft = false
tags = ["Linux", "开发环境", "VirtualBox", "VS Code", "Ubuntu", "Hical"]
categories = ["开发环境"]
description = "将 Hical C++20 Web 框架开发环境从 Windows 迁移到 Ubuntu VM 的全流程记录：VirtualBox 配置、GCC 14 编译链、VS Code Remote SSH、共享目录挂载等。"
+++

# Hical Linux 开发环境（实操记录）

## 概述

将 Hical C++20 Web 框架的日常开发环境从 Windows 本地迁移到 Linux 虚拟机，通过 **Oracle VM VirtualBox** 运行 Ubuntu 24.04.3 LTS Server 虚拟机，使用 **Tabby Terminal** + **VS Code Remote SSH** 进行开发。

### 为什么迁移

| 痛点                                  | Linux 解决                            |
| ------------------------------------- | ------------------------------------- |
| MSYS2/vcpkg 路径混乱、ABI 不兼容      | 原生 apt 包管理，toolchain 统一       |
| liburing/io_uring 仅 Linux 可用       | 原生支持，解锁异步文件 I/O 最优路径   |
| CI 环境是 Ubuntu，本地是 Windows      | 开发即 CI，减少"本地能跑 CI 挂"的问题 |
| clang-format/clang-tidy 版本对齐困难  | 直接装 LLVM APT 源的 clang-20         |
| Sanitizer（ASan/UBSan）Windows 支持差 | Linux 原生完美支持                    |

### 宿主机配置

| 项目        | 值                                          |
| ----------- | ------------------------------------------- |
| 处理器      | Intel Core i7-11700K @ 3.60GHz（8核16线程） |
| 内存        | 32 GB                                       |
| 操作系统    | Windows 10 Enterprise LTSC 2021             |
| VM 存放磁盘 | D:\ （900 GB，SSD）                         |

---

## 一、VirtualBox 虚拟机创建

### 1.1 下载

| 软件       | 版本                                              | 下载地址                                                    |
| ---------- | ------------------------------------------------- | ----------------------------------------------------------- |
| VirtualBox | 7.1+                                              | https://www.virtualbox.org/wiki/Downloads                   |
| Ubuntu ISO | `ubuntu-24.04.3-live-server-amd64.iso`（3.1 GiB） | https://mirrors.tuna.tsinghua.edu.cn/ubuntu-releases/24.04/ |

> 选择 **Server** 版（无桌面 GUI），减少资源占用。国内推荐清华镜像下载更快。

### 1.2 新建虚拟机向导

打开 VirtualBox → 新建（Ctrl+N），按以下填写：

**第一步：虚拟电脑名称与操作系统**

| 字段         | 值                                                |
| ------------ | ------------------------------------------------- |
| 名称         | hical                                             |
| 文件夹       | D:\VirtualBox VMs                                 |
| 虚拟光盘     | 选择下载的 `ubuntu-24.04.3-live-server-amd64.iso` |
| 类型         | Linux                                             |
| 版本         | **Ubuntu (64-bit)**（不要选 Oracle Linux）        |
| 跳过自动安装 | **勾选**（自动安装可能导致"完成"按钮无法点击）    |

> **踩坑记录**：不勾选"跳过自动安装"时，摘要页的"完成"按钮可能灰色无法点击。解决方法是返回第一步勾选"跳过自动安装"。

**第二步：自动安装（已跳过）**

勾选了"跳过自动安装"后此步骤不显示。

**第三步：硬件**

| 字段     | 值                    | 说明                           |
| -------- | --------------------- | ------------------------------ |
| 内存     | **16384 MB**（16 GB） | 宿主机 32G，各半               |
| 处理器   | **8 CPU**             | 宿主机 8核16线程，给一半线程数 |
| 启用 EFI | **勾选**              | Ubuntu 24.04 支持 UEFI 启动    |

**第四步：虚拟硬盘**

| 字段             | 值                                   |
| ---------------- | ------------------------------------ |
| 选项             | 现在创建虚拟硬盘                     |
| 磁盘空间         | **102 GB**                           |
| 预先分配全部空间 | **不勾选**（动态分配，用多少占多少） |

点击"完成"创建虚拟机。

### 1.3 网络配置：NAT + 端口转发

默认创建的 VM 使用 NAT 网络，VM 内部 IP 为 `10.0.2.15`，宿主机无法直接 SSH 访问，需要配置端口转发。

**配置步骤**：

```
VirtualBox → 选中 hical VM → 设置 → 网络
  连接方式: NAT → 高级 → 端口转发 → 添加规则:
```

| 名称 | 协议 | 主机 IP   | 主机端口 | 子系统 IP | 子系统端口 |
| ---- | ---- | --------- | -------- | --------- | ---------- |
| SSH  | TCP  | 127.0.0.1 | 2222     | 10.0.2.15 | 22         |

> **备选方案**：也可以改为桥接模式，VM 直接获取局域网 IP（192.168.x.x），无需端口转发。

---

## 二、Ubuntu 24.04 手动安装

启动 VM 后进入安装界面，按以下步骤操作：

### 2.1 启动选择

选择 **\*Try or Install Ubuntu Server** → 按 Enter。

### 2.2 安装器更新

选择 **Continue without updating** → 按 Enter。

### 2.3 安装类型

保持 **(X) Ubuntu Server**（默认），Done。

### 2.4 网络配置

默认 DHCP 自动获取，Done。

### 2.5 代理配置

留空，Done。

### 2.6 Ubuntu 镜像源

默认即可，Done。

### 2.7 磁盘分区

选择 **(X) Use an entire disk**，勾选 **[X] Set up this disk as an LVM group**，加密不勾选，Done。

在 Storage configuration 页面确认分区方案后 Done。

> **注意**：默认 LVM 只分配了约一半空间给根分区（49.5G / 99G）。可以尝试编辑 `ubuntu-lv` 扩大，如果安装界面不方便操作，装完系统后用命令扩容（见下文）。

确认 destructive action 对话框，Continue。

### 2.8 用户配置

| 字段               | 值           |
| ------------------ | ------------ |
| Your name          | hical        |
| Your server's name | hical        |
| Pick a username    | hical        |
| Choose a password  | （你的密码） |

> 名字、服务器名、用户名可以相同，没有问题。

### 2.9 Ubuntu Pro

选择 **(X) Skip for now**，Continue。

### 2.10 SSH 配置（重要）

**必须勾选 `[X] Install OpenSSH server`**，否则装完无法远程连接。

用方向键移到该行，按空格键勾选，Done。

### 2.11 Featured Server Snaps

不勾选任何 snap，Done。

### 2.12 等待安装完成

安装过程约 5-10 分钟。完成后选择 **Reboot Now**。

### 2.13 重启

重启时可能出现：
```
[FAILED] Failed unmounting cdrom.mount - /cdrom.
Please remove the installation medium, then press ENTER:
```

这是正常现象，直接按 **Enter**。VirtualBox 会自动弹出 ISO，系统从硬盘启动。

### 2.14 首次启动

看到 `Ubuntu 24.04.3 LTS hical tty1` 后，按 Enter，输入用户名和密码登录。

---

## 三、系统初始化与 LVM 扩容

### 3.1 查看 IP 地址

```bash
ip addr show | grep "inet "
```

NAT 模式下输出为 `10.0.2.15`（内网 IP），通过端口转发访问。

### 3.2 LVM 根分区扩容

默认安装只用了 LVM 一半空间，扩容到 100%：

```bash
sudo lvextend -l +100%FREE /dev/ubuntu-vg/ubuntu-lv
sudo resize2fs /dev/ubuntu-vg/ubuntu-lv
```

验证：
```bash
df -h /
# 应显示约 96G 可用
```

> 实测输出：`Size of logical volume ubuntu-vg/ubuntu-lv changed from 49.57 GiB to 99.14 GiB`

### 3.3 更新系统并安装基础工具

```bash
sudo apt update && sudo apt upgrade -y

sudo apt install -y \
    build-essential git curl wget unzip \
    cmake ninja-build ccache \
    htop tmux vim \
    software-properties-common
```

---

## 四、Tabby Terminal SSH 连接

### 4.1 安装 Tabby

从 https://tabby.sh/ 下载 Windows 安装包，安装。

### 4.2 创建 SSH Profile

打开 Tabby → Settings → Profiles & connections → New profile → SSH connection：

| 字段                  | 值           |
| --------------------- | ------------ |
| Name                  | Hical-Dev    |
| Host                  | 127.0.0.1    |
| Port                  | 2222         |
| Username              | hical        |
| Authentication method | Password     |
| Password              | （你的密码） |

Save 保存。

### 4.3 首次连接

点击 Hical-Dev 连接，弹出 **Host key verification** 对话框，点击 **Accept and remember key**。

连接成功后即可在 Tabby 终端操作 VM。

### 4.4 推荐 Tabby 设置

| 设置项         | 推荐值                     | 说明         |
| -------------- | -------------------------- | ------------ |
| 终端字体       | JetBrains Mono / Fira Code | 等宽 + 连字  |
| 字体大小       | 14-16                      | 舒适阅读     |
| 配色主题       | Dracula / One Dark         | 护眼         |
| 滚动缓冲       | 10000 行                   | 编译输出长   |
| 快捷键: 新标签 | Ctrl+T                     | 多标签并行   |
| 快捷键: 分屏   | Ctrl+Shift+D               | 左编辑右编译 |

---

## 五、Hical 编译工具链安装

### 5.1 GCC 14（主力编译器）

Ubuntu 24.04 默认 GCC 13.3，需要额外安装 GCC 14：

```bash
sudo apt install -y gcc-14 g++-14
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100
```

验证：
```bash
gcc --version
# gcc (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0
```

### 5.2 Clang 20（Sanitizer / clang-tidy / clang-format）

```bash
# 添加 LLVM APT 源
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/llvm.asc
echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" | \
    sudo tee /etc/apt/sources.list.d/llvm-20.list
sudo apt update

# 安装 Clang 20（注意：clang++-20 不是独立包，包含在 clang-20 中）
sudo apt install -y clang-20 clang-tidy-20 clang-format-20 lld-20

# 设置 alternatives
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 100
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 100
sudo update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-20 100
sudo update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-20 100
```

> **踩坑记录**：安装命令中不要写 `clang++-20`，会报 `Unable to locate package`。`clang++-20` 二进制已包含在 `clang-20` 包中。

验证：
```bash
clang --version
# Ubuntu clang version 20.1.8

clang++-20 --version
# Ubuntu clang version 20.1.8
```

### 5.3 项目核心依赖

```bash
sudo apt install -y \
    libboost-all-dev \
    libssl-dev \
    libgtest-dev \
    liburing-dev
```

实际安装版本：

| 依赖    | 版本   | 要求       |
| ------- | ------ | ---------- |
| Boost   | 1.83.0 | >= 1.82 ✓  |
| OpenSSL | 3.0.13 | Required ✓ |
| GTest   | 1.14.0 | Required ✓ |
| CMake   | 3.28.3 | >= 3.20 ✓  |
| Ninja   | 1.11.1 | — ✓        |

### 5.4 ccache 加速编译

```bash
echo 'export PATH="/usr/lib/ccache:$PATH"' >> ~/.bashrc
echo 'export CCACHE_MAXSIZE=5G' >> ~/.bashrc
source ~/.bashrc
```

---

## 六、Git 配置与 SSH Key

### 6.1 Git 全局配置

```bash
git config --global user.name "hical"
git config --global user.email "150020540409@163.com"
git config --global core.autocrlf input   # 防止 CRLF 问题
```

### 6.2 生成 SSH Key 并添加到 GitHub

```bash
ssh-keygen -t ed25519 -C "hical-vm"
# 一路 Enter（默认路径，空密码）

cat ~/.ssh/id_ed25519.pub
# 输出类似：ssh-ed25519 AAAAC3Nza... hical-vm
```

将输出的公钥添加到 GitHub：
1. 打开 https://github.com/settings/keys
2. 点 **New SSH key**
3. Title: `hical-vm`
4. Key: 粘贴公钥全文（`ssh-ed25519 ... hical-vm`）
5. 点 **Add SSH key**

### 6.3 验证 SSH 连接

```bash
ssh -T git@github.com
# 首次连接输入 yes 信任 GitHub 主机密钥
# 输出: Hi Hical61! You've successfully authenticated, but GitHub does not provide shell access.
```

---

## 七、项目克隆与首次构建

### 7.1 克隆项目

```bash
mkdir -p ~/projects && cd ~/projects
git clone git@github.com:Hical61/Hical.git
cd Hical
```

### 7.2 CMake 配置

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-14 \
    -DCMAKE_CXX_COMPILER=g++-14
```

输出确认：
```
-- The CXX compiler identification is GNU 14.2.0
-- Found Boost: .../Boost-1.83.0/BoostConfig.cmake (found suitable version "1.83.0", minimum required is "1.82") found components: json
-- Found OpenSSL: .../libcrypto.so (found version "3.0.13")
-- Found GTest: .../GTestConfig.cmake (found version "1.14.0")
```

> `CMAKE_C_COMPILER` 未使用的警告可忽略（项目是纯 C++）。

### 7.3 编译

```bash
cmake --build build -j$(nproc)
```

编译 123 个目标全部通过（有少量警告，不影响功能）。

**编译警告说明**：
- `HICAL_LOG_TRACE_F redefined` — Log.h 中 NDEBUG 宏分支重定义，不影响运行
- `-Wformat-truncation` — LogFormatter.cpp 中 snprintf 缓冲区大小警告
- `-Wunused-variable` — AsyncFileSink.cpp 中 `hasData` 未使用
- `-Wmissing-field-initializers` — test_openapi.cpp 中 OpenApiConfig 缺少 servers 初始化

### 7.4 运行测试

```bash
ctest --test-dir build --output-on-failure --timeout 60 -j4
```

结果：**467/470 通过**，3 个失败：

| 失败测试                      | 原因            |
| ----------------------------- | --------------- |
| AsioTimerTest.RunOnce         | VM 时钟精度不足 |
| AsioTimerTest.RunRepeatedly   | VM 时钟精度不足 |
| AsioTimerTest.CancelRepeating | VM 时钟精度不足 |

> 这 3 个都是定时器精度测试，VirtualBox 虚拟化环境下时钟抖动是已知现象，不影响框架功能。CI 在原生 Linux 上测试通过。

### 7.5 Clang Debug + Sanitizer 构建（可选）

```bash
cmake -B build-asan -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=clang-20 \
    -DCMAKE_CXX_COMPILER=clang++-20 \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

cmake --build build-asan -j$(nproc)

# 运行测试（超时放宽到 120 秒）
ctest --test-dir build-asan --output-on-failure --timeout 120 -j4
```

### 7.6 格式检查

```bash
find src tests examples -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror
```

---

## 八、VS Code Remote SSH 配置

### 8.1 Windows 上安装扩展

打开 VS Code → 扩展（Ctrl+Shift+X）→ 搜索安装 **Remote - SSH**（微软官方）。

### 8.2 添加 SSH Host

`Ctrl+Shift+P` → 输入 `Remote-SSH: Connect to Host` → 选 `Add New SSH Host` → 输入：

```
ssh hical@127.0.0.1 -p 2222
```

选择保存到 `C:\Users\wuchaohua\.ssh\config`，然后点击连接，输入密码。

### 8.3 VM 上安装 clangd

```bash
sudo apt install -y clangd-20
sudo update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-20 100
```

### 8.4 VS Code 远程环境安装扩展

连接成功后，在 VS Code 扩展面板搜索并安装（安装到远程 VM 上）：

| 扩展            | 用途                 |
| --------------- | -------------------- |
| **clangd**      | 代码补全、跳转、诊断 |
| **CMake Tools** | CMake 集成           |

### 8.5 打开项目

`File → Open Folder` → 输入 `/home/hical/projects/Hical` → 打开。

clangd 会自动读取 `build/compile_commands.json`（CMakeLists.txt 已启用 `CMAKE_EXPORT_COMPILE_COMMANDS`），代码补全和跳转即可使用。

---

## 九、开发工作流总结

### 架构图

```
┌─────────────────────────────────────────────────────────┐
│  Windows 宿主机（i7-11700K / 32GB）                       │
│                                                          │
│  ┌──────────┐  SSH :2222  ┌───────────────────────────┐ │
│  │  Tabby   │ ──────────→ │  Ubuntu 24.04.3 VM        │ │
│  │ Terminal  │             │  16GB RAM / 8 CPU         │ │
│  └──────────┘             │                            │ │
│                           │  GCC 14.2 / Clang 20.1    │ │
│  ┌──────────┐  SSH :2222  │  Boost 1.83 / OpenSSL 3.0 │ │
│  │ VS Code  │ ──────────→ │  liburing / GTest 1.14    │ │
│  │Remote-SSH│             │                            │ │
│  └──────────┘             │  ~/projects/Hical/         │ │
│                           │    build/     (GCC Release) │ │
│                           │    build-asan/ (Clang ASan) │ │
│                           └───────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

### 日常开发循环

```
1. 打开 Tabby / VS Code Remote → 连接 127.0.0.1:2222
2. cd ~/projects/Hical
3. git pull / git checkout -b feature/xxx
4. 编辑代码（VS Code Remote SSH）
5. cmake --build build -j$(nproc)         → 编译
6. ctest --test-dir build -R test_xxx     → 跑目标测试
7. clang-format 格式化
8. git commit & push
9. （可选）cmake --build build-asan 跑 Sanitizer
```

### 多标签工作流（Tabby）

| 标签  | 用途                        |
| ----- | --------------------------- |
| Tab 1 | git 操作                    |
| Tab 2 | 编译 & 测试                 |
| Tab 3 | 运行服务器示例              |
| Tab 4 | 日志/监控（htop / tail -f） |

---

## 十、性能调优

### 10.1 编译加速组合拳

| 手段         | 效果                                       | 状态     |
| ------------ | ------------------------------------------ | -------- |
| ccache       | 增量编译缓存，重复编译近乎瞬间             | 已配置 ✓ |
| Ninja        | 比 Make 快 10-20%（更好的并行调度）        | 已使用 ✓ |
| `-j$(nproc)` | 满核并行（8核）                            | 已使用 ✓ |
| unity build  | `CMAKE_UNITY_BUILD=ON`（可选，首次编译快） | 未启用   |
| tmpfs 编译   | 内存充足时编译到内存盘                     | 按需使用 |

### 10.2 VirtualBox 优化

```
系统 → 处理器 → 启用 PAE/NX
系统 → 加速 → 半虚拟化接口: KVM
存储 → 控制器: SATA → 使用主机 I/O 缓存: 勾选
存储 → 虚拟硬盘 → 固态驱动器: 勾选（宿主是 SSD）
```

**各项收益说明**：

| 设置              | 作用                                                           | 实际收益                                                                     |
| ----------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| 启用 PAE/NX       | 允许 VM 访问超过 4GB 物理地址 + NX 位防止数据段执行            | VM 分配了 16GB 内存必须开；NX 是安全加固，Ubuntu 内核依赖它                  |
| 半虚拟化接口: KVM | 让 Guest OS 知道自己跑在虚拟机里，直接用宿主机时钟源和中断注入 | 时钟精度提升（可能改善 Timer 测试失败）、系统调用开销降低，零成本            |
| 主机 I/O 缓存     | SATA 控制器层启用宿主机文件系统缓存（Windows NTFS 缓存）       | **编译速度提升明显**——大量小文件随机读写（头文件、.o 文件）命中宿主缓存      |
| 固态驱动器        | 告诉 Guest 内核底层是 SSD，启用 TRIM/discard 和 I/O 调度优化   | 内核选用 `none`/`mq-deadline` 调度器而非旋转盘的 `bfq`，减少无意义的寻道排序 |

> 后两项对编译（123 个目标）和 Docker build（大量层写入）等 I/O 密集操作体感最明显。

**操作步骤**：

1. **关闭 VM**（VM 终端内 `sudo shutdown now`，或 VirtualBox 右键 → 关闭 → 正常关机）
2. VirtualBox 管理器 → 选中 `hical` VM → **设置**（Ctrl+S）
3. 按以下路径配置：

| 路径                                         | 操作                                                    |
| -------------------------------------------- | ------------------------------------------------------- |
| 系统 → 处理器                                | 勾选 `启用 PAE/NX`                                      |
| 系统 → 加速                                  | 半虚拟化接口下拉选 `KVM`（默认可能是"默认"或"Hyper-V"） |
| 存储 → `控制器: SATA`（点控制器文字本身）    | 右侧勾选 `使用主机 I/O 缓存`                            |
| 存储 → `hical.vdi`（点 SATA 下面的磁盘图标） | 右侧勾选 `固态驱动器`                                   |

4. 点击 **确定** 保存，重新启动 VM

> **注意**：存储面板中**控制器**和**磁盘**是两个不同的可点击项——I/O 缓存在控制器上，固态驱动器在磁盘上，别搞混。

### 10.3 Headless 启动（节省资源）

```powershell
# Windows 上无窗口启动 VM
VBoxManage startvm "hical" --type headless
# 关闭
VBoxManage controlvm "hical" acpipowerbutton
```

---

## 十一、常见问题排查

### Q1: VirtualBox "完成"按钮灰色无法点击

返回第一步，勾选 **"跳过自动安装"**，然后一路下一步到最后即可。

### Q2: 重启时 `Failed unmounting cdrom.mount`

正常现象，直接按 Enter。VirtualBox 会自动弹出 ISO。

### Q3: SSH 连接不上 VM

```bash
# 确认 VirtualBox 端口转发已配置（主机 2222 → VM 22）
# VM 内检查 SSH 服务
sudo systemctl status ssh
# 确认防火墙
sudo ufw allow 22
```

### Q4: `Unable to locate package clang++-20`

`clang++-20` 包含在 `clang-20` 中，不需要单独安装。安装命令：
```bash
sudo apt install -y clang-20 clang-tidy-20 clang-format-20 lld-20
```

### Q5: GCC 版本是 13.3 不是 14

Ubuntu 24.04 默认 GCC 13.3，需要额外安装：
```bash
sudo apt install -y gcc-14 g++-14
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100
```

### Q6: LVM 根分区只用了一半

默认安装只分配约 50% 给根分区，扩容命令：
```bash
sudo lvextend -l +100%FREE /dev/ubuntu-vg/ubuntu-lv
sudo resize2fs /dev/ubuntu-vg/ubuntu-lv
```

### Q7: AsioTimerTest 定时器测试失败

VirtualBox 虚拟化环境下时钟精度抖动是已知现象（3 个 Timer 测试失败），不影响框架功能。CI 在原生 Linux 上测试全部通过。

### Q8: Boost 版本不够（DB 中间件需要 1.85+）

Ubuntu 24.04 自带 Boost 1.83，满足基本要求。如需 1.85+：
```bash
# 方案1: 用 Conan
pip install conan
conan install . --build=missing
# 方案2: 源码编译 Boost
```

### Q9: clang-format 结果与 CI 不一致

```bash
clang-format --version  # 确认版本
# CI 用 pip install clang-format==22.1.1
pip install clang-format==22.1.1
```

### Q10: VirtualBox 增强功能（Guest Additions）

安装时已勾选增强功能。如需手动安装：
```bash
sudo apt install -y virtualbox-guest-utils virtualbox-guest-dkms
sudo usermod -aG vboxsf $USER
# 重启 VM
```

---

## 十二、可选进阶

### 12.1 Docker-in-VM

见下方 **第十三章** 完整安装步骤。

### 12.2 自动快照

定期给 VM 打快照（VirtualBox 快照功能），方便回滚到干净状态。

### 12.3 共享文件夹（可选）

将宿主机 `D:\hical` 目录挂载到 VM 内 `/mnt/hical_host`，方便偶尔传文件。

**第一步：VM 内安装 Guest Additions**

```bash
sudo apt install -y virtualbox-guest-utils
sudo reboot
```

**第二步：关闭 VM，在 VirtualBox 管理器配置**

`sudo shutdown now` 关闭 VM → VirtualBox 管理器 → 选中 `hical` VM → **设置** → 左侧 **共享文件夹** → 右侧点击 **+**（添加共享文件夹）图标：

| 字段       | 值                                     |
| ---------- | -------------------------------------- |
| 文件夹路径 | `D:\hical`                             |
| 文件夹名称 | `hical_host`（自动填充，可改）         |
| 只读分配   | **不勾选**                             |
| 自动挂载   | **勾选**                               |
| 挂载点     | `/mnt/hical_host`                      |
| 固定分配   | **勾选**（永久生效，不勾则仅本次有效） |

点击 **确定** → 再点 **确定** 保存。

**第三步：启动 VM，授权用户访问**

```bash
# 将用户加入 vboxsf 组（共享文件夹专用权限组）
sudo usermod -aG vboxsf $USER

# 重新登录生效
logout
# 重新 SSH 连接后验证
ls /mnt/hical_host
# 应能看到 D:\hical 下的文件，若是看不到，说明需要 vboxsf 组权限。

# 看看当前用户的组有没有生效：
groups
# 如果里面没有 vboxsf，说明需要完全重新登录（newgrp 或重启 SSH 连接）。试试：
newgrp vboxsf
ls /mnt/hical_host
```

> **注意**：共享文件夹走 VirtualBox 虚拟文件系统，I/O 性能比 VM 本地磁盘差很多。**不要在上面编译或跑 Docker build**，仅用于偶尔在宿主机和 VM 之间传文件。日常开发用 `~/projects/Hical`（VM 本地 clone）。

### 12.4 SSH 密钥登录（替代密码）

```powershell
# Windows PowerShell 中执行
ssh-keygen -t ed25519 -C "hical-dev"
type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh -p 2222 hical@127.0.0.1 "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys"
```

配置后 Tabby 和 VS Code 可以免密登录。


## 十三、Docker 安装（Benchmark 前置条件）

> 基于 2026-05-09 实操，Docker 官方 APT 源安装方式。

### 13.1 添加 Docker 官方 APT 源

```bash
sudo apt update
sudo apt install -y ca-certificates curl

# 导入 Docker GPG 密钥
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

# 添加仓库
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
  https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

sudo apt update
```

### 13.2 安装 Docker Engine + Compose 插件

```bash
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
```

实际安装版本：

| 组件           | 版本                  |
| -------------- | --------------------- |
| Docker Engine  | 29.4.3                |
| Docker Compose | v5.1.3（plugin 模式） |
| containerd     | 2.2.3                 |
| docker-buildx  | 0.33.0                |

### 13.3 免 sudo 运行 Docker

```bash
sudo usermod -aG docker $USER
newgrp docker   # 立即生效（或重新登录）
```

### 13.4 验证安装

```bash
docker --version
# Docker version 29.4.3, build 055a478

docker compose version
# Docker Compose version v5.1.3

docker run --rm hello-world
# 应输出 "Hello from Docker!" 确认引擎正常
```

> **注意**：使用的是 `docker compose`（Compose V2 插件模式），不是老式的 `docker-compose`（独立二进制）。

---

## 十四、Benchmark 压测运行

### 14.1 概述

`benchmark/` 目录包含 8 个 Web 框架的 Docker 化压测环境，通过 `wrk` 进行 HTTP 负载测试。

| 框架            | 语言    | 容器端口 | 依赖方式     |
| --------------- | ------- | -------- | ------------ |
| **Hical**       | C++20   | 8080     | Conan 2      |
| **Drogon**      | C++20   | 8083     | 源码编译     |
| **Crow**        | C++20   | 8084     | 单头文件     |
| **Oat++**       | C++20   | 8085     | FetchContent |
| **cpp-httplib** | C++17   | 8086     | 单头文件     |
| **Cinatra**     | C++20   | 8087     | FetchContent |
| **Gin**         | Go 1.22 | 8081     | Go Module    |
| **Actix**       | Rust    | 8082     | Cargo        |

两种运行模式：

| Profile      | 框架                                                  | 场景数 |
| ------------ | ----------------------------------------------------- | ------ |
| `cpp`        | Hical / Drogon / Crow / Oat++ / cpp-httplib / Cinatra | 10+    |
| `cross-lang` | Hical / Gin / Actix                                   | 4      |

每个容器限制 **4 CPU + 512 MB 内存**，保证公平。

### 14.2 C++ 框架对比（cpp profile）

#### 首次运行（含构建）

```bash
cd ~/projects/Hical/benchmark

# ── 步骤 1: 构建所有镜像 ──────────────────────────────────
# 首次约 15-30 分钟（Drogon 源码编译最慢）
# 构建完成后镜像缓存在本地，重启电脑不会丢失
docker compose --profile cpp build

# ── 步骤 2: 确认镜像已构建成功 ────────────────────────────
# 应看到 benchmark-hical / benchmark-drogon / benchmark-crow /
# benchmark-oatpp / benchmark-cpphttplib / benchmark-cinatra / benchmark-wrk
docker images | grep benchmark

# ── 步骤 3: 启动所有服务（后台） ──────────────────────────
docker compose --profile cpp up -d

# ── 步骤 4: 确认容器全部 running ──────────────────────────
docker compose --profile cpp ps
# 应有 7 个容器: hical / drogon / crow / oatpp / cpphttplib / cinatra / wrk

# ── 步骤 5: 验证服务响应正常 ──────────────────────────────
curl http://localhost:8080/    # Hical
curl http://localhost:8083/    # Drogon
curl http://localhost:8084/    # Crow
curl http://localhost:8085/    # Oat++
curl http://localhost:8086/    # cpp-httplib
curl http://localhost:8087/    # Cinatra

# ── 步骤 6: 运行压测（在 wrk 容器内执行，约 10-15 分钟） ─
docker compose --profile cpp exec wrk bash -c "BENCH_MODE=cpp bash /bench/run_bench.sh"

# ── 步骤 7: 采集资源统计（宿主机 bash 执行） ─────────────
# 采集内存/CPU/二进制大小/镜像大小/代码行数
BENCH_MODE=cpp bash collect_stats.sh

# ── 步骤 8: 查看结果 ─────────────────────────────────────
cat results.md    # QPS 对比表（wrk 输出）
cat stats.md      # 资源统计表（内存/二进制大小等）

# ── 步骤 9: 清理容器（不删镜像，下次可直接启动） ─────────
docker compose --profile cpp down
```

#### 后续复现（镜像已存在）

重启电脑 / `docker compose down` 后再次跑压测，跳过构建直接启动：

```bash
cd ~/projects/Hical/benchmark

# 1. 确认镜像还在（可选，看到 7 个 benchmark-* 即可）
docker images | grep benchmark

# 2. 直接启动（秒级，不需要 --build）
docker compose --profile cpp up -d

# 3. 确认容器就绪
docker compose --profile cpp ps

# 4. 运行压测
docker compose --profile cpp exec wrk bash -c "BENCH_MODE=cpp bash /bench/run_bench.sh"

# 5. 采集统计
BENCH_MODE=cpp bash collect_stats.sh

# 6. 查看结果
cat results.md
cat stats.md

# 7. 清理
docker compose --profile cpp down
```

> **什么时候需要重新 build？** 只有当你修改了 `benchmark/` 下的源码（`main.cpp`、`Dockerfile`、`CMakeLists.txt` 等）才需要重新 `docker compose --profile cpp build`。代码没改就不用重建。

#### 需要重建单个框架时

```bash
# 只重建 hical 镜像（其他框架不受影响）
docker compose --profile cpp build hical

# 重建后重启
docker compose --profile cpp up -d
```

**压测场景（10 种）**：

| 场景              | 路径                    | 说明                 |
| ----------------- | ----------------------- | -------------------- |
| hello             | GET /                   | 纯文本 "Hello World" |
| json              | GET /api/status         | JSON 序列化响应      |
| echo              | POST /api/echo          | JSON 反序列化+序列化 |
| param             | GET /users/42           | 路径参数提取         |
| mw0               | GET /middleware/0       | 无中间件基线         |
| mw3               | GET /middleware/3       | 3 层原生中间件       |
| mw10              | GET /middleware/10      | 10 层原生中间件      |
| smw3              | GET /sync-middleware/3  | 3 层 SyncMiddleware  |
| smw10             | GET /sync-middleware/10 | 10 层 SyncMiddleware |
| c100/c1000/c10000 | GET /                   | 高并发连接测试       |

#### 压测指标说明

| 指标                   | 含义                                                                                                          | 越高/低越好 |
| ---------------------- | ------------------------------------------------------------------------------------------------------------- | ----------- |
| **Requests/sec (QPS)** | 每秒成功完成的请求数，衡量框架吞吐能力上限                                                                    | 越高越好    |
| **Avg Latency**        | 平均响应延迟：从发出请求到收到完整响应的平均时间，反映用户体感等待                                            | 越低越好    |
| **Stdev Latency**      | 延迟标准差：衡量延迟波动幅度，Stdev 大说明部分请求会卡很久（不稳定）                                          | 越低越好    |
| **Max Latency**        | 最大延迟：最慢那个请求的响应时间（尾延迟），对支付/竞价等场景是硬指标                                         | 越低越好    |
| **Socket errors**      | 连接错误：connect（连接失败）、read（读取失败）、write（写入失败）、timeout（超时），反映框架在压力下的稳定性 | 0 最好      |
| **Transfer/sec**       | 每秒传输的数据量（含 HTTP 头 + Body），衡量网络吞吐带宽。QPS 相近时 Transfer 大说明响应体更大                 | 视场景而定  |

### 14.3 跨语言对比（cross-lang profile）

```bash
cd ~/projects/Hical/benchmark

# 构建 + 启动
docker compose --profile cross-lang up -d --build

# 压测
docker compose --profile cross-lang exec wrk bash /bench/run_bench.sh

# 采集统计
BENCH_MODE=cross-lang bash collect_stats.sh

# 清理
docker compose --profile cross-lang down
```

跨语言模式只跑 4 个基础场景（hello / json / echo / param）。

### 14.4 自定义压测参数

```bash
# 增大并发连接数和持续时间
CONNECTIONS=500 DURATION=60s docker compose --profile cpp exec wrk bash /bench/run_bench.sh

# 只跑 all 模式（8 框架全跑）
docker compose --profile cpp --profile cross-lang up -d --build
BENCH_MODE=all docker compose exec wrk bash /bench/run_bench.sh
```

默认参数：

| 参数        | 默认值 | 说明           |
| ----------- | ------ | -------------- |
| DURATION    | 30s    | 每场景持续时间 |
| THREADS     | 4      | wrk 线程数     |
| CONNECTIONS | 100    | 并发连接数     |

### 14.5 结果文件说明

| 文件         | 内容                                       | 生成命令           |
| ------------ | ------------------------------------------ | ------------------ |
| `results.md` | QPS 对比表（各框架各场景的吞吐量）         | `run_bench.sh`     |
| `stats.md`   | 资源统计（空载/满载内存、CPU、二进制大小） | `collect_stats.sh` |

### 14.6 常见问题

**Q: 构建 Drogon 镜像很慢**

Drogon 需要从源码编译框架本身（git clone + cmake + make），首次约 10-15 分钟。后续有 Docker 缓存会快很多。

**Q: wrk 容器内找不到脚本**

确认 `docker-compose.yml` 中 wrk 服务挂载了 benchmark 目录：
```bash
docker compose --profile cpp exec wrk ls /bench/
# 应看到 run_bench.sh、collect_stats.sh、post_echo.lua 等
```

**Q: 某个框架容器启动失败**

```bash
# 查看日志定位问题
docker compose --profile cpp logs <服务名>
# 例如
docker compose --profile cpp logs drogon
```

**Q: 压测结果不稳定**

- 确保 VM 空闲（关闭无关进程）
- 增大 `DURATION` 到 60s 减少波动
- 多跑几轮取平均值

---

## 十五、Profiling 性能剖析（perf + 火焰图）

> 目标：用 `perf record` 采样 + 火焰图可视化，精确定位 Hical benchmark server 的 CPU 热点函数。

### 15.1 原理

| 工具                    | 作用                                           |
| ----------------------- | ---------------------------------------------- |
| `perf record -g -F 99`  | 以 99Hz 频率采样 CPU 调用栈，`-g` 记录完整栈帧 |
| `perf script`           | 将二进制 `perf.data` 转为文本调用栈            |
| `stackcollapse-perf.pl` | 折叠调用栈为火焰图输入格式                     |
| `flamegraph.pl`         | 生成交互式 SVG 火焰图                          |

**前置条件**：需要 **未 strip** 的二进制（保留符号表），否则火焰图中只有 `[unknown]`。

### 15.2 Profiling 专用 Dockerfile

已创建 `benchmark/hical/Dockerfile.profiling`，与生产 Dockerfile 的区别：

| 项目           | 生产 Dockerfile | Profiling Dockerfile                     |
| -------------- | --------------- | ---------------------------------------- |
| 优化级别       | `-O2`           | `-O2 -g`（保留 DWARF 符号）              |
| strip          | 是              | 否                                       |
| 运行阶段额外包 | 无              | `linux-tools-generic linux-tools-common` |
| 二进制大小     | 7.8M            | ~12M                                     |

### 15.3 docker-compose.yml 配置

`hical-profiling` 服务配置了 perf 所需的内核权限：

```yaml
hical-profiling:
  build:
    context: ./hical
    dockerfile: Dockerfile.profiling
  ports:
    - "8080:8080"
  cpus: 4
  mem_limit: 512m
  profiles: [profiling]
  cap_add:
    - SYS_ADMIN       # 访问 perf_event
    - SYS_PTRACE       # 跟踪进程
  security_opt:
    - seccomp=unconfined  # 放宽 seccomp 限制
  volumes:
    - ./profiling-output:/profiling  # 挂载输出目录
  networks:
    - bench
```

> **重要**：`cap_add` 和 `seccomp=unconfined` 是 perf 在容器内运行的必要权限，缺少会报 `permission denied`。

### 15.4 完整执行步骤

```bash
cd ~/projects/Hical/benchmark

# ── 步骤 1: 确保 cpp profile 已停止（释放 8080 端口） ──
docker compose --profile cpp down --timeout 5

# ── 步骤 2: 构建 profiling 镜像 ──────────────────────
docker compose --profile profiling build

# ── 步骤 3: 启动 profiling 容器 + wrk ────────────────
docker compose --profile profiling up -d

# ── 步骤 4: 验证 perf 可用 + 符号完整 ───────────────
docker compose --profile profiling exec hical-profiling perf --version
# 应输出 perf version 6.x

docker compose --profile profiling exec hical-profiling sh -c "ls -lh /server && ldd /server"
# 应看到 ~12M（未 strip），ldd 只有 libc/libstdc++

# ── 步骤 5: 后台启动 perf 录制（30 秒采样） ─────────
docker compose --profile profiling exec -d hical-profiling \
  perf record -g -F 99 -p 1 -o /profiling/perf.data -- sleep 30

# ── 步骤 6: 立即用 wrk 压测（与 perf 同步 30 秒） ───
docker compose --profile profiling exec wrk \
  wrk -t4 -c100 -d30s http://hical-profiling:8080/

# ── 步骤 7: 等待 5 秒让 perf 写完，生成调用栈文本 ───
sleep 5
docker compose --profile profiling exec hical-profiling \
  perf script -i /profiling/perf.data > profiling-output/perf.script

# ── 步骤 8: 安装 FlameGraph 工具并生成 SVG ──────────
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git /tmp/FlameGraph 2>/dev/null || true

/tmp/FlameGraph/stackcollapse-perf.pl profiling-output/perf.script | \
  /tmp/FlameGraph/flamegraph.pl --title "Hical Hello World (wrk 4t/100c/30s)" \
  > profiling-output/flamegraph.svg

# ── 步骤 9: 查看结果 ─────────────────────────────────
ls -lh profiling-output/
# 应看到 perf.data、perf.script、flamegraph.svg

# ── 步骤 10: 拷贝到宿主机用浏览器查看 ───────────────
cp profiling-output/flamegraph.svg /mnt/hical_host/
# 在 Windows 浏览器中打开 flamegraph.svg，鼠标悬停查看函数占比

# ── 步骤 11: 清理 ────────────────────────────────────
docker compose --profile profiling down
```

### 15.5 分场景 Profiling

替换步骤 6 的 wrk 端点即可针对不同场景录制：

| 场景         | wrk 命令                                                                           | 预期热点                          |
| ------------ | ---------------------------------------------------------------------------------- | --------------------------------- |
| Hello World  | `wrk -t4 -c100 -d30s http://hical-profiling:8080/`                                 | 内核 sendmsg、epoll_ctl、协程调度 |
| JSON Echo    | `wrk -t4 -c100 -d30s -s /bench/post_echo.lua http://hical-profiling:8080/api/echo` | Boost.JSON 解析/序列化            |
| 中间件 10 层 | `wrk -t4 -c100 -d30s http://hical-profiling:8080/middleware/10`                    | 协程链 co_await 状态机切换        |

### 15.6 火焰图读图指南

- **X 轴**：不代表时间，代表采样占比（越宽 = CPU 时间越多）
- **Y 轴**：调用栈深度（底部 = 入口函数，顶部 = 叶子函数）
- **颜色**：随机分配，无特殊含义
- **操作**：鼠标悬停显示函数名和占比，点击可 zoom in 某个子栈

**关注重点**：
1. 最宽的顶层函数（"火焰尖端"）= 真正消耗 CPU 的叶子函数
2. `[unknown]` 层 = 内核态函数（容器内无内核符号导致）
3. `malloc` / `cfree` / `operator new` = 堆内存分配热点
4. `pthread_mutex_lock` / `pthread_cond_signal` = 锁竞争

### 15.7 已知限制

1. **容器内无内核符号**：`sendmsg` 以上的内核调用栈显示为 `[unknown]`。如需完整内核栈，需在宿主机直接运行 `perf record -a -g` 并指定容器进程 PID
2. **perf 与 Docker Desktop (Hyper-V)**：Windows Docker Desktop 不支持 `perf`，必须在 Linux VM 或物理机上操作
3. **采样频率**：`-F 99` 是安全默认值。提高到 `-F 999` 可获得更精确数据，但会增加开销
