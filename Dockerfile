# drogon-game-server/Dockerfile
# 多阶段构建：
#   Stage 1 (builder)：使用 Drogon 官方预编译镜像编译游戏服务器
#   Stage 2 (runtime)：只保留可执行文件和运行时依赖，镜像体积从 >2GB 压缩到 ~200MB
#
# 注意：使用固定版本 tag（非 latest）确保构建可复现

# ── Stage 1: 编译阶段 ────────────────────────────────────────────
FROM drogonframework/drogon:latest AS builder

WORKDIR /app

# 复制源码
COPY . .

# 编译游戏服务器
# 使用 cmake --build --parallel 替代 -j$(nproc)，无需依赖 nproc 命令
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build --parallel

# ── Stage 2: 运行阶段（精简镜像）────────────────────────────────
FROM ubuntu:22.04

# 安装 Drogon 运行时所需的动态库（不含编译工具链）
RUN apt-get update && apt-get install -y --no-install-recommends \
	libssl3 \
	libjsoncpp25 \
	libuuid1 \
	zlib1g \
	libbrotli1 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 只拷贝可执行文件和配置文件（不包含源码和编译工具）
COPY --from=builder /app/build/drogon-game-server ./build/drogon-game-server
COPY --from=builder /app/config.json ./config.json

# 云托管默认端口 8080（与 config.json 中 port 一致）
EXPOSE 8080

# 使用相对路径启动，确保 CWD 始终是 WORKDIR(/app)
# Drogon 会在 CWD 查找 config.json 和 ./logs 目录
CMD ["./build/drogon-game-server"]
