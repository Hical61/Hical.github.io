+++
title = 'Docker 新手入门：从零开始容器化你的应用'
date = '2025-10-01'
draft = false
tags = ["Docker", "容器", "DevOps", "学习笔记", "运维"]
categories = ["DevOps"]
description = "面向完全零基础的 Docker 入门教程：核心概念、安装、镜像与容器操作、Dockerfile 编写、Docker Compose 多容器编排，以及实战部署一个 Web 应用。"
+++

# Docker 新手入门：从零开始容器化你的应用

> 如果你的程序在你电脑上能跑，那就把你的电脑也一起发给客户吧。——Docker 之前的世界

---

## 写在前面

### 这篇文章适合谁？

- 听说过 Docker 但从未用过的开发者
- 被「在我电脑上明明能跑」折磨过的人
- 想了解容器化部署但不知道从哪开始的人

### 读完你将获得什么？

- 理解 Docker 核心概念（镜像、容器、仓库）
- 能独立编写 Dockerfile 并构建镜像
- 能用 Docker Compose 编排多容器应用
- 能将一个 Web 应用容器化部署

---

## 一、Docker 是什么？

### 1.1 一句话解释

Docker 是一个**应用打包、分发、运行**的平台。它把你的应用和所有依赖（库、配置、系统工具）打包成一个**镜像**，然后在任何安装了 Docker 的机器上以**容器**的形式运行。

### 1.2 虚拟机 vs 容器

| 对比项   | 虚拟机 (VM)               | Docker 容器        |
| -------- | ------------------------- | ------------------ |
| 隔离级别 | 硬件级（Hypervisor）      | 进程级（内核共享） |
| 启动速度 | 分钟级                    | 秒级               |
| 体积     | GB 级                     | MB 级              |
| 性能损耗 | 10-20%                    | 接近原生           |
| 资源占用 | 高（每个 VM 一个完整 OS） | 低（共享宿主内核） |

```
┌─────────────────────────────────┐    ┌─────────────────────────────────┐
│         虚拟机架构               │    │         Docker 架构              │
├─────────────────────────────────┤    ├─────────────────────────────────┤
│  App A │  App B │  App C        │    │  App A │  App B │  App C        │
│  Libs  │  Libs  │  Libs         │    │  Libs  │  Libs  │  Libs         │
│  OS    │  OS    │  OS           │    ├─────────────────────────────────┤
├─────────────────────────────────┤    │         Docker Engine           │
│       Hypervisor                │    ├─────────────────────────────────┤
├─────────────────────────────────┤    │         Host OS                 │
│       Host OS                   │    ├─────────────────────────────────┤
├─────────────────────────────────┤    │         Hardware                │
│       Hardware                  │    └─────────────────────────────────┘
└─────────────────────────────────┘
```

### 1.3 核心三概念

- **镜像（Image）**：只读模板，包含运行应用所需的一切。类比：安装光盘
- **容器（Container）**：镜像的运行实例。类比：用光盘装好的一台电脑
- **仓库（Registry）**：存放镜像的地方。类比：应用商店（Docker Hub）

三者关系：

```
Registry（仓库）──pull──▶ Image（镜像）──run──▶ Container（容器）
                         ▲                        │
                         └────────commit───────────┘
```

---

## 二、安装 Docker

### 2.1 Windows

1. 下载 [Docker Desktop for Windows](https://www.docker.com/products/docker-desktop/)
2. 安装时勾选 **Use WSL 2 instead of Hyper-V**（推荐）
3. 安装完成后重启电脑
4. 打开终端验证：

```bash
docker --version
# Docker version 27.x.x, build xxxxxx

docker run hello-world
# 看到 "Hello from Docker!" 说明安装成功
```

### 2.2 Linux (Ubuntu/Debian)

```bash
# 卸载旧版本
sudo apt-get remove docker docker-engine docker.io containerd runc

# 安装依赖
sudo apt-get update
sudo apt-get install ca-certificates curl gnupg

# 添加 Docker 官方 GPG key
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
sudo chmod a+r /etc/apt/keyrings/docker.gpg

# 添加仓库
echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

# 安装 Docker Engine
sudo apt-get update
sudo apt-get install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# 将当前用户加入 docker 组（免 sudo）
sudo usermod -aG docker $USER
newgrp docker

# 验证
docker run hello-world
```

### 2.3 macOS

1. 下载 [Docker Desktop for Mac](https://www.docker.com/products/docker-desktop/)（区分 Intel / Apple Silicon）
2. 拖入 Applications 安装
3. 启动 Docker Desktop，等待状态栏鲸鱼图标稳定
4. 终端验证：`docker run hello-world`

### 2.4 配置国内镜像加速（可选）

如果拉取镜像很慢，可以配置镜像加速器。编辑 `/etc/docker/daemon.json`（Linux）或 Docker Desktop 设置中的 Docker Engine：

```json
{
  "registry-mirrors": [
    "https://mirror.ccs.tencentyun.com",
    "https://docker.mirrors.ustc.edu.cn"
  ]
}
```

配置后重启 Docker：

```bash
sudo systemctl restart docker   # Linux
# Windows/macOS: 重启 Docker Desktop
```

---

## 三、镜像操作

### 3.1 搜索镜像

```bash
docker search nginx
```

### 3.2 拉取镜像

```bash
# 拉取最新版
docker pull nginx

# 拉取指定版本
docker pull nginx:1.25

# 拉取指定平台
docker pull --platform linux/amd64 nginx
```

### 3.3 查看本地镜像

```bash
docker images
# 或
docker image ls

# 输出示例：
# REPOSITORY   TAG       IMAGE ID       CREATED        SIZE
# nginx        latest    a8758716bb6a   2 weeks ago    187MB
# hello-world  latest    d2c94e258dcb   12 months ago  13.3kB
```

### 3.4 删除镜像

```bash
# 按名称删除
docker rmi nginx:1.25

# 按 ID 删除
docker rmi a8758716bb6a

# 删除所有未被使用的镜像
docker image prune -a
```

### 3.5 镜像的分层结构

Docker 镜像由多个**只读层**叠加而成，每一层代表 Dockerfile 中的一条指令：

```
┌───────────────────────┐
│  Layer 4: COPY app    │  ← 你的应用代码
├───────────────────────┤
│  Layer 3: RUN npm i   │  ← 安装依赖
├───────────────────────┤
│  Layer 2: WORKDIR     │  ← 设置工作目录
├───────────────────────┤
│  Layer 1: node:20     │  ← 基础镜像
└───────────────────────┘
```

分层的好处：**层可以共享和缓存**。如果两个镜像都基于 `node:20`，这一层在磁盘上只需存一份。

---

## 四、容器操作

### 4.1 运行容器

```bash
# 最基本的运行
docker run nginx

# 后台运行（-d = detach）
docker run -d nginx

# 指定名称
docker run -d --name my-nginx nginx

# 端口映射（宿主机端口:容器端口）
docker run -d -p 8080:80 --name my-nginx nginx

# 此时浏览器访问 http://localhost:8080 即可看到 Nginx 默认页面
```

### 4.2 常用运行参数

| 参数        | 说明               | 示例                            |
| ----------- | ------------------ | ------------------------------- |
| `-d`        | 后台运行           | `docker run -d nginx`           |
| `-p`        | 端口映射           | `-p 8080:80`                    |
| `--name`    | 指定容器名         | `--name web`                    |
| `-e`        | 设置环境变量       | `-e MYSQL_ROOT_PASSWORD=123456` |
| `-v`        | 挂载数据卷         | `-v /host/path:/container/path` |
| `--rm`      | 退出后自动删除容器 | `docker run --rm nginx`         |
| `-it`       | 交互模式+伪终端    | `docker run -it ubuntu bash`    |
| `--network` | 指定网络           | `--network my-net`              |
| `--restart` | 重启策略           | `--restart unless-stopped`      |

### 4.3 查看容器

```bash
# 查看运行中的容器
docker ps

# 查看所有容器（含已停止的）
docker ps -a

# 输出示例：
# CONTAINER ID   IMAGE   COMMAND                  STATUS         PORTS                  NAMES
# a1b2c3d4e5f6   nginx   "/docker-entrypoint.…"   Up 2 minutes   0.0.0.0:8080->80/tcp   my-nginx
```

### 4.4 容器生命周期管理

```bash
# 停止容器
docker stop my-nginx

# 启动已停止的容器
docker start my-nginx

# 重启容器
docker restart my-nginx

# 删除已停止的容器
docker rm my-nginx

# 强制删除运行中的容器
docker rm -f my-nginx

# 删除所有已停止的容器
docker container prune
```

### 4.5 进入容器内部

```bash
# 进入正在运行的容器
docker exec -it my-nginx bash

# 在容器内执行命令（不进入）
docker exec my-nginx cat /etc/nginx/nginx.conf
```

### 4.6 查看容器日志

```bash
# 查看全部日志
docker logs my-nginx

# 实时跟踪日志（类似 tail -f）
docker logs -f my-nginx

# 查看最近 100 行
docker logs --tail 100 my-nginx

# 带时间戳
docker logs -t my-nginx
```

### 4.7 查看容器信息

```bash
# 详细信息（网络、挂载、配置等）
docker inspect my-nginx

# 资源使用统计
docker stats my-nginx

# 查看端口映射
docker port my-nginx
```

---

## 五、数据持久化

容器默认是**无状态**的——容器删除后，内部的数据也会丢失。要持久化数据，需要使用 Volume 或 Bind Mount。

### 5.1 Bind Mount（绑定挂载）

将宿主机的目录直接映射到容器内：

```bash
# 把宿主机的 ./html 目录挂载到容器的 /usr/share/nginx/html
docker run -d \
  -p 8080:80 \
  -v $(pwd)/html:/usr/share/nginx/html \
  --name web \
  nginx
```

修改宿主机 `./html/index.html`，容器内实时生效。

### 5.2 Volume（卷）

由 Docker 管理的持久化存储，与宿主机目录解耦：

```bash
# 创建卷
docker volume create my-data

# 使用卷
docker run -d \
  -v my-data:/var/lib/mysql \
  -e MYSQL_ROOT_PASSWORD=123456 \
  --name db \
  mysql:8.0

# 查看卷列表
docker volume ls

# 查看卷详情
docker volume inspect my-data

# 删除卷
docker volume rm my-data
```

### 5.3 何时用 Bind Mount vs Volume？

| 场景             | 推荐方式   |
| ---------------- | ---------- |
| 开发环境同步代码 | Bind Mount |
| 数据库持久化存储 | Volume     |
| 配置文件挂载     | Bind Mount |
| 容器间共享数据   | Volume     |

---

## 六、Dockerfile：构建自己的镜像

### 6.1 一个最小的 Dockerfile

```dockerfile
# 基础镜像
FROM node:20-alpine

# 设置工作目录
WORKDIR /app

# 复制 package.json 并安装依赖（利用缓存）
COPY package*.json ./
RUN npm install --production

# 复制应用代码
COPY . .

# 暴露端口（文档作用）
EXPOSE 3000

# 启动命令
CMD ["node", "server.js"]
```

### 6.2 常用指令说明

| 指令         | 说明                            | 示例                                 |
| ------------ | ------------------------------- | ------------------------------------ |
| `FROM`       | 指定基础镜像                    | `FROM ubuntu:22.04`                  |
| `WORKDIR`    | 设置工作目录                    | `WORKDIR /app`                       |
| `COPY`       | 复制文件到镜像                  | `COPY . .`                           |
| `ADD`        | 复制文件（支持 URL 和自动解压） | `ADD app.tar.gz /app`                |
| `RUN`        | 构建时执行命令                  | `RUN apt-get update`                 |
| `CMD`        | 容器启动默认命令                | `CMD ["nginx", "-g", "daemon off;"]` |
| `ENTRYPOINT` | 容器入口点（不会被覆盖）        | `ENTRYPOINT ["python"]`              |
| `ENV`        | 设置环境变量                    | `ENV NODE_ENV=production`            |
| `EXPOSE`     | 声明端口                        | `EXPOSE 8080`                        |
| `VOLUME`     | 声明匿名卷                      | `VOLUME /data`                       |
| `ARG`        | 构建参数                        | `ARG VERSION=1.0`                    |

### 6.3 构建镜像

```bash
# 在 Dockerfile 所在目录执行
docker build -t my-app:1.0 .

# 指定 Dockerfile 路径
docker build -f deploy/Dockerfile -t my-app:1.0 .

# 传递构建参数
docker build --build-arg VERSION=2.0 -t my-app:2.0 .
```

### 6.4 Dockerfile 最佳实践

#### 利用构建缓存

Docker 按层缓存，**变化频率低的指令放前面**：

```dockerfile
# ✅ 好：先 COPY package.json，再 COPY 代码
COPY package*.json ./
RUN npm install
COPY . .

# ❌ 差：每次代码变动都会重新 npm install
COPY . .
RUN npm install
```

#### 使用多阶段构建

减小最终镜像体积：

```dockerfile
# === 构建阶段 ===
FROM node:20 AS builder
WORKDIR /app
COPY package*.json ./
RUN npm install
COPY . .
RUN npm run build

# === 运行阶段 ===
FROM node:20-alpine
WORKDIR /app
COPY --from=builder /app/dist ./dist
COPY --from=builder /app/node_modules ./node_modules
EXPOSE 3000
CMD ["node", "dist/server.js"]
```

#### 使用 .dockerignore

在项目根目录创建 `.dockerignore` 文件，排除不需要的文件：

```
node_modules
.git
.env
*.log
dist
.DS_Store
```

#### 选择合适的基础镜像

```bash
# 按体积从小到大：
# scratch       → 空镜像（用于静态编译的 Go/Rust 程序）
# alpine        → ~5MB（musl libc，某些场景兼容性问题）
# slim          → ~80MB（Debian 精简版）
# 完整版         → ~300MB+（Debian/Ubuntu）
```

---

## 七、Docker Compose：多容器编排

### 7.1 为什么需要 Compose？

一个真实的应用通常不止一个容器。比如一个 Web 应用可能需要：
- 一个 Nginx 做反向代理
- 一个 Node.js 应用
- 一个 MySQL 数据库
- 一个 Redis 缓存

用 `docker run` 一个个启动太麻烦，Docker Compose 用一个 YAML 文件描述所有服务，一键启动。

### 7.2 docker-compose.yml 示例

```yaml
services:
  # Web 应用
  app:
    build: .
    ports:
      - "3000:3000"
    environment:
      - DB_HOST=db
      - REDIS_HOST=redis
    depends_on:
      - db
      - redis
    restart: unless-stopped

  # 数据库
  db:
    image: mysql:8.0
    environment:
      MYSQL_ROOT_PASSWORD: secret
      MYSQL_DATABASE: myapp
    volumes:
      - db-data:/var/lib/mysql
    ports:
      - "3306:3306"
    restart: unless-stopped

  # 缓存
  redis:
    image: redis:7-alpine
    ports:
      - "6379:6379"
    restart: unless-stopped

  # 反向代理
  nginx:
    image: nginx:alpine
    ports:
      - "80:80"
    volumes:
      - ./nginx.conf:/etc/nginx/nginx.conf:ro
    depends_on:
      - app
    restart: unless-stopped

volumes:
  db-data:
```

### 7.3 Compose 常用命令

```bash
# 启动所有服务（后台）
docker compose up -d

# 查看服务状态
docker compose ps

# 查看日志
docker compose logs -f

# 查看某个服务的日志
docker compose logs -f app

# 停止所有服务
docker compose down

# 停止并删除数据卷（慎用！会丢数据）
docker compose down -v

# 重新构建并启动
docker compose up -d --build

# 扩缩容（启动 3 个 app 实例）
docker compose up -d --scale app=3

# 进入某个服务的容器
docker compose exec app bash
```

### 7.4 Compose 中的网络

Docker Compose 会自动创建一个网络，同一个 `docker-compose.yml` 中的服务可以通过**服务名**互相访问：

```javascript
// app 中连接数据库，直接用服务名 "db" 作为 host
const connection = mysql.createConnection({
  host: 'db',       // ← 不是 localhost，是服务名
  user: 'root',
  password: 'secret',
  database: 'myapp'
});
```

---

## 八、实战：容器化一个 Express 应用

### 8.1 项目结构

```
my-express-app/
├── server.js
├── package.json
├── Dockerfile
├── .dockerignore
└── docker-compose.yml
```

### 8.2 server.js

```javascript
const express = require('express');
const app = express();
const PORT = process.env.PORT || 3000;

app.get('/', (req, res) => {
  res.json({
    message: 'Hello Docker!',
    timestamp: new Date().toISOString(),
    hostname: require('os').hostname()
  });
});

app.get('/health', (req, res) => {
  res.json({ status: 'ok' });
});

app.listen(PORT, () => {
  console.log(`Server running on port ${PORT}`);
});
```

### 8.3 package.json

```json
{
  "name": "my-express-app",
  "version": "1.0.0",
  "main": "server.js",
  "scripts": {
    "start": "node server.js"
  },
  "dependencies": {
    "express": "^4.18.0"
  }
}
```

### 8.4 Dockerfile

```dockerfile
FROM node:20-alpine

WORKDIR /app

COPY package*.json ./
RUN npm install --production

COPY . .

EXPOSE 3000

# 健康检查
HEALTHCHECK --interval=30s --timeout=3s \
  CMD wget --no-verbose --tries=1 --spider http://localhost:3000/health || exit 1

CMD ["node", "server.js"]
```

### 8.5 .dockerignore

```
node_modules
.git
.env
*.log
```

### 8.6 构建并运行

```bash
# 构建镜像
docker build -t my-express-app:1.0 .

# 运行容器
docker run -d -p 3000:3000 --name app my-express-app:1.0

# 测试
curl http://localhost:3000
# {"message":"Hello Docker!","timestamp":"2026-05-14T...","hostname":"a1b2c3d4e5f6"}

# 查看健康状态
docker ps
# STATUS 列会显示 (healthy)
```

---

## 九、常用 Docker 命令速查表

### 镜像相关

```bash
docker pull <image>            # 拉取镜像
docker images                  # 列出本地镜像
docker rmi <image>             # 删除镜像
docker build -t <tag> .        # 构建镜像
docker tag <src> <dst>         # 给镜像打标签
docker push <image>            # 推送镜像到仓库
docker image prune             # 清理悬空镜像
```

### 容器相关

```bash
docker run <image>             # 运行容器
docker ps                      # 查看运行中容器
docker ps -a                   # 查看所有容器
docker stop <container>        # 停止容器
docker start <container>       # 启动容器
docker rm <container>          # 删除容器
docker exec -it <c> bash       # 进入容器
docker logs <container>        # 查看日志
docker inspect <container>     # 查看详情
docker cp <src> <dst>          # 容器与宿主机之间复制文件
```

### 系统清理

```bash
# 一键清理所有未使用的资源（镜像、容器、网络、缓存）
docker system prune -a

# 查看 Docker 磁盘占用
docker system df
```

---

## 十、常见问题 FAQ

### Q1：容器里的数据重启后还在吗？

**停止再启动（stop → start）**：数据还在。
**删除容器（rm）**：数据丢失（除非使用了 Volume）。

### Q2：`EXPOSE` 和 `-p` 的区别？

- `EXPOSE`：Dockerfile 中的声明，仅文档作用，不会实际开放端口
- `-p`：运行时参数，实际将容器端口映射到宿主机

### Q3：`CMD` 和 `ENTRYPOINT` 的区别？

- `CMD`：容器启动默认命令，可被 `docker run` 后面的参数覆盖
- `ENTRYPOINT`：入口点，不会被覆盖，`docker run` 的参数会追加到后面

```dockerfile
# 示例：ENTRYPOINT + CMD 组合
ENTRYPOINT ["python"]
CMD ["app.py"]

# docker run my-image          → 执行 python app.py
# docker run my-image test.py  → 执行 python test.py（CMD 被覆盖）
```

### Q4：如何调试容器启动失败？

```bash
# 查看退出的容器
docker ps -a

# 查看日志
docker logs <container_id>

# 用交互模式进入排查
docker run -it <image> sh
```

### Q5：Windows 上 volume 挂载路径怎么写？

```bash
# PowerShell
docker run -v ${PWD}/data:/app/data my-app

# CMD
docker run -v %cd%/data:/app/data my-app

# WSL2 里
docker run -v $(pwd)/data:/app/data my-app
```

---

## 十一、学习路径建议

```
第一阶段：基础操作
├── 理解镜像/容器/仓库概念
├── 熟练 pull/run/stop/rm 操作
└── 掌握端口映射、环境变量、数据卷

第二阶段：镜像构建
├── 编写 Dockerfile
├── 理解分层缓存机制
├── 多阶段构建优化体积
└── .dockerignore 使用

第三阶段：多容器编排
├── Docker Compose 基础
├── 服务间网络通信
├── 健康检查与依赖管理
└── 生产环境配置

第四阶段：进阶主题
├── Docker 网络模式（bridge/host/none/overlay）
├── 资源限制（--memory/--cpus）
├── 日志驱动与监控
├── CI/CD 集成
└── 容器安全最佳实践
```

---

## 十二、推荐资源

- [Docker 官方文档](https://docs.docker.com/) — 最权威的参考
- [Docker Hub](https://hub.docker.com/) — 官方镜像仓库
- [Play with Docker](https://labs.play-with-docker.com/) — 在线实验环境，无需安装
- `docker --help` / `docker <command> --help` — 随时查看帮助

---

## 总结

Docker 的核心价值很简单：**环境一致性**。开发环境、测试环境、生产环境跑的是同一个镜像，从此告别「在我电脑上能跑」。

入门 Docker 的关键是动手：

1. 先跑几个官方镜像感受一下（nginx、mysql、redis）
2. 把自己的项目写个 Dockerfile 打包
3. 用 Docker Compose 把前端+后端+数据库串起来
4. 部署到服务器上验证

不要纠结于底层原理（namespace、cgroups、overlay fs），这些等你用熟了再深入也不迟。先用起来，用出问题了再去理解为什么——这是最高效的学习路径。
