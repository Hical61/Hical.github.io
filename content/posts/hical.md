+++
date = '2026-03-25'
draft = false
title = '极客专属博客搭建全记录'
categories = ["工具与效率"]
tags = ["Hugo", "GitHub Actions", "博客搭建", "PaperMod", "CI/CD"]
description = "从零搭建 Hugo + PaperMod 博客，配置 GitHub Actions 自动部署，绑定自定义域名全过程记录。"
+++

# 🚀 极客专属博客搭建全记录 (Hugo + GitHub Actions)

- **技术栈**：Hugo (Extended版) + GitHub Pages + GitHub Actions CI/CD
- **主题**：PaperMod (极简名片风 / 深色模式)
- **作者**：Hical

---

## 阶段一：基础设施搭建 (域名与云端仓库)

### 1. 配置 DNS 域名解析
在域名控制台（如腾讯云 DNSPod）添加以下记录，将域名指向 GitHub 全球 CDN 节点：
- **A 记录**：主机记录 `@`，记录值分别填入（官方建议填2-4个做负载均衡）：
  - `185.199.108.153`
  - `185.199.109.153`
- **CNAME 记录**：主机记录 `www`，记录值填入你的 GitHub 默认分配域名（如 `hical61.github.io`）。

### 2. 创建空白 GitHub 仓库
1. 登录 GitHub，新建一个 Public 仓库，命名为 `你的用户名.github.io`。
2. **注意避坑**：创建时务必保持完全空白，**绝对不要**勾选生成 `README` 或 `.gitignore`，以免与本地推送产生冲突。

---

## 阶段二：本地引擎组装 (Hugo 初始化)

### 1. 安装 Hugo 引擎（windows环境下）
1. 去 GitHub Releases 下载带 **`extended`** 和 **`windows-amd64`** 的最新版 Hugo 压缩包。
2. 解压出 `hugo.exe`，放入固定目录（如 `D:\hugo\bin`）。
3. 将该路径加入 Windows 系统的 **环境变量 PATH** 中。
4. 在终端执行 `hugo version` 验证安装成功。

### 2. 初始化博客目录与防掉线机制
在本地新建一个空文件夹作为博客源码库，打开 Git 终端执行：

```bash
# 1. 初始化 Hugo 架构
hugo new site . --force

# 2. 【核心避坑】创建 CNAME 文件并移入 static 目录，防止每次部署域名掉线
echo "hicalio.cn" > CNAME
mv CNAME static/

```

### 3. 安装 PaperMod 主题与精装配置

```bash
# 通过 Git 子模块拉取主题
git submodule add https://github.com/adityatelange/hugo-PaperMod.git themes/PaperMod

```
清空根目录下的 hugo.toml，替换为以下极简大名片配置：
```toml
baseURL = 'https://hicalio.cn/'
languageCode = 'zh-cn'
title = 'Hical 的填坑日记'
theme = 'PaperMod'

[params]
  defaultTheme = "dark" # 默认深色极客模式

# 开启主页大名片模式
[params.profileMode]
  enabled = true
  title = "Hical"
  subtitle = "🧊 冰川网络 | C++ 开发"
  imageUrl = "/avatar.png" # 请将头像图片放入 static/ 目录
  imageWidth = 150
  imageHeight = 150

  [[params.profileMode.buttons]]
    name = "📚 翻阅我的填坑日记"
    url = "/posts/"

# 社交图标配置
[[params.socialIcons]]
  name = "github"
  url = "https://github.com/Hical61"
[[params.socialIcons]]
  name = "email"
  url = "mailto:wuchaohua@hicalio.cn"

```
---

## 阶段三：自动化产线 (CI/CD 部署流水线)

### 1. 编写 GitHub Actions 脚本
在博客根目录依次创建文件夹 .github/workflows/，并在其中新建 deploy.yml 文件，填入以下打包脚本：

```yaml
name: Deploy Hugo site to Pages
on:
  push:
    branches: ["master"]
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: "pages"
  cancel-in-progress: false

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - name: 检出代码
        uses: actions/checkout@v4
        with:
          submodules: recursive # 必须递归拉取主题子模块
          fetch-depth: 0
      - name: 安装 Hugo
        uses: peaceiris/actions-hugo@v3
        with:
          hugo-version: 'latest'
          extended: true        # 必须使用 extended 版本编译
      - name: 编译生成网页
        run: hugo --minify
      - name: 上传打包文件
        uses: actions/upload-pages-artifact@v3
        with:
          path: ./public
  deploy:
    environment:
      name: github-pages
      url: ${{ steps.deploy.outputs.page_url }}
    runs-on: ubuntu-latest
    needs: build
    steps:
      - name: 部署到 GitHub Pages
        id: deploy
        uses: actions/deploy-pages@v4

```

### 2.  首次推送与云端激活
将所有代码推送到 GitHub 仓库：
```bash
git add .
git commit -m "init: 初始化 Hugo 博客并配置自动化流水线"
git push origin master

```
云端激活操作（仅需一次）：
1、打开 GitHub 仓库页面 -> Settings -> 左侧 Pages。
2、在 Build and deployment 下的 Source 下拉菜单中，选择 GitHub Actions。
3、等待 Actions 绿灯亮起，检查 Custom domain 框内是否填有域名，如有空缺补填保存。


## 阶段四：日常发版工作流 (极简四步)
以后每次发布新文章，只需执行以下闭环：

### 1. 新建文章：
```bash
hugo new content/posts/my-new-post.md

```
### 2. 编写内容：使用 Markdown 编写，务必将文件顶部的 draft = true 改为 draft = false。
### 3. 本地预览 (可选)：
```bash
hugo server -D

```
打开 localhost:1313 实时预览效果。

### 4. 一键发射：
```bash
git add .
git commit -m "docs: 新增一篇填坑日记"
git push origin master

```
推送完成后，静候 30 秒，GitHub Actions 自动完成打包与 CDN 刷新，外网 hicalio.cn 无缝更新！