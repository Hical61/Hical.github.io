+++
date = '2026-03-25'
draft = false
title = 'DeerFlow 2.0 本地部署与排坑实战指南'
+++

# 🦌 DeerFlow 2.0 本地化部署与排坑实战指南

**文档维护：** Hical
**适用环境：** Windows + Docker Desktop (企业内网管控环境)

## 📌 项目简介
DeerFlow 是一个强大的多智能体（Multi-Agent）协作框架，专为长时间运行的复杂自主任务设计（如自动化编码、深度调研、排障分析）。底层基于 LangGraph，支持沙盒（Sandbox）隔离执行。

---

## 🛠️ 部署前置准备

由于公司终端管控策略（如 IP-Guard 等防泄密软件）可能会禁用系统 WSL (Windows Subsystem for Linux) 或拦截 C 盘挂载，建议 Docker Desktop 配置如下：
1. **禁用 WSL**：若启动 Docker 报错，修改 `%APPDATA%\Docker\settings.json`，将 `"wslEngineEnabled"` 改为 `false`，强制使用 Hyper-V 引擎。
2. **准备代码**：
   
```bash
   git clone https://github.com/bytedance/deer-flow.git
   cd deer-flow
   
```

## ⚙️ 核心配置修改 (避坑指南)
为了防止在启动和编译过程中出现各种“水土不服”的报错，在执行启动命令前，请务必完成以下 5 步修改：

1. **配置根目录 .env**：复制 .env.example 重命名为 .env，并在末尾追加以下关键变量：
```env
# 公司内网测试大模型 Token
ANTHROPIC_API_KEY=XXXXXX

# 必须配置 Auth 组件的 Base URL，否则前端 SSR 渲染会报 500 错误！(端口固定为 2026)
BETTER_AUTH_BASE_URL=http://localhost:2026
BETTER_AUTH_SECRET=glacier_network_super_secret_key_2026

```
2. **配置大模型 config.yaml**：复制 config.example.yaml 重命名为 config.yaml，配置内网模型：
models:
  - name: claude-sonnet-4-6
    display_name: Claude Sonnet 4.6 (Claude Code OAuth)
    use: langchain_anthropic:ChatAnthropic
    model: claude-4.5-sonnet
    api_key: $ANTHROPIC_API_KEY
    max_tokens: 8192

# 记得把系统默认的模型指向刚才配置好的这个内网模型
default_model: claude-sonnet-4-6

3. **切除敏感挂载卷 (防 Docker 崩溃)**：打开 docker/docker-compose.yml，搜索 .claude。将这部分挂载代码整块注释掉或删除，防止安全软件拦截对 C 盘敏感目录的访问导致 Docker 引擎闪退：

```yaml
# - type: bind
#   source: ${HOME:?HOME must be set}/.claude
#   target: /root/.claude
#   ...

```

4. **修复前端编译“总闸” (防 Next.js 语法报错)**：开源代码存在部分 TypeScript 类型缺失。为防止构建失败，打开 frontend/next.config.js，注入忽略报错配置：

```javascript
import "./src/env.js";

/** @type {import("next").NextConfig} */
const config = {
  devIndicators: false,
  typescript: { ignoreBuildErrors: true }, // 强制忽略 TS 报错
  eslint: { ignoreDuringBuilds: true },    // 强制忽略格式报错
};

export default config;

```

5. **补齐致命的“空洞文件 (防前端白屏 500 错误)**：这一步极其重要，否则页面渲染会报 Cannot read properties of undefined (reading 'sections')：

# 新建前端环境变量：在 frontend/ 目录下新建 .env 文件，填入：
```env
   NEXT_PUBLIC_API_URL=http://localhost:2026/api
   BETTER_AUTH_URL=http://localhost:2026
   BETTER_AUTH_SECRET=glacier_network_super_secret_key_2026
   
```
# 初始化扩展配置：在项目根目录下新建或修改 extensions_config.json 文件，填入空数组，绝不能留空 {}：
```json

{
  "sections": []
}

```

6. **其他语法错误修复**：
#  frontend/src/core/i18n/locales/types.ts ：在 tokenUsage 的 total: string; 后添加了 }; 闭合括号，让 shortcuts 和 settings 恢复为顶层属性。

#  frontend/src/core/i18n/locales/en-US.ts ：在 tokenUsage 的 total: "Total", 后添加了 }, 并移除了末尾多余的 }。

# frontend/src/core/i18n/locales/zh-CN.ts：在 tokenUsage 的 total: "总计", 后添加了 }, 并将末尾的 }} 改为 }。

