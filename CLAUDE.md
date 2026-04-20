# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**Hical Geek Blog** — 个人极客博客，基于 Hugo + PaperMod 主题，部署在 GitHub Pages，自定义域名 [hicalio.cn](https://hicalio.cn/)。

## 常用命令

### 构建注意事项
- CI 使用 `hugo --minify`（不含 `--buildFuture`），**日期在未来的文章不会出现在线上**
- 本地 `hugo server` 默认也不构建未来文章，需加 `--buildFuture` 预览
- 文章 `date` 字段建议使用当天或过去日期以确保正常发布

```bash
# 本地开发服务器（热重载，访问 http://localhost:1313）
hugo server

# 构建静态文件到 public/（CI/CD 自动执行，本地一般不需要）
hugo --minify

# 新建文章（会使用 archetypes/default.md 模板）
hugo new posts/文章名.md
```

部署：直接推送到 `master` 分支，GitHub Actions 自动触发构建和部署（见 [.github/workflows/deploy.yml](.github/workflows/deploy.yml)）。`public/` 目录不提交到 Git，由 CI 生成。

## 技术架构

```
本地 Markdown 编写 → git push → GitHub Actions → Hugo Extended 编译 → GitHub Pages CDN → hicalio.cn
```

- **静态生成器**：Hugo Extended（必须 Extended 版，PaperMod 主题依赖 SCSS 编译）
- **主题**：PaperMod v7.0+，以 Git 子模块形式挂载在 `themes/PaperMod/`
- **配置文件**：根目录 [hugo.toml](hugo.toml)（TOML 格式）

## 目录结构关键点

- [content/posts/](content/posts/) — 博客文章（Markdown），这是主要的编辑区域

### 文章系列
- **Trantor 网络库课程**（第01课~第18课）— 网络库源码逐模块讲解
- **Hical 框架笔记**（Hical-00 ~ Hical-10, 课程表, 应用场景）— 自研框架学习
- **从零构建 C++ Web 服务器**（一~五）— 实战系列
- **Boost 库**（Asio/Beast/JSON/System + 学习路径）— Boost 子库教程
- **C++ 特性实战**（C++17/20/26 + PMR/协程/反射深入）— 现代 C++ 专题

- [layouts/index.html](layouts/index.html) — 自定义首页（赛博朋克风格，含 Three.js 3D 地球、粒子系统、Glitch 效果），**覆盖**了 PaperMod 主题的首页
- [static/](static/) — 静态资源，直接复制到输出根目录（含 `CNAME`、`avatar.jpg`、`game.html`）
- [archetypes/default.md](archetypes/default.md) — `hugo new` 命令使用的文章 Front Matter 模板
- [assets/css/extended/markdown-colors.css](assets/css/extended/markdown-colors.css) — 自定义 Markdown 配色（One Dark 风格，覆盖 PaperMod 默认样式）

## 文章 Front Matter 格式

```toml
+++
title = '文章标题'
date = 2026-03-27T00:00:00+08:00
draft = false
tags = ["标签1", "标签2"]
categories = ["分类"]
description = "文章摘要"
+++
```

> **注意**：`hugo new` 生成的模板仅包含 `title`/`date`/`draft`，`tags`/`categories`/`description` 需手动添加。

## 主题子模块

初次克隆或子模块未拉取时：

```bash
git submodule update --init --recursive
```

## 首页特殊说明

[layouts/index.html](layouts/index.html) 是高度定制的页面（~1300 行），使用原生 HTML/CSS/JS，引入了 Three.js CDN。修改时注意：
- 赛博朋克色彩变量定义在文件顶部 CSS 变量中
- 粒子系统和 3D 地球逻辑在文件底部 `<script>` 部分
- 该文件完全独立于 PaperMod 主题，不继承主题布局
