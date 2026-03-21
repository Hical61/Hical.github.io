# drogon-game-server/Dockerfile
# 用于微信云托管（CloudBase Run）Docker 容器部署
# 构建镜像后推送到云托管，可享受与云开发同 Region 的极低延迟

FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 安装编译依赖
RUN apt-get update && apt-get install -y \
	cmake g++ git pkg-config \
	libssl-dev libjsoncpp-dev uuid-dev \
	zlib1g-dev libbrotli-dev \
	&& rm -rf /var/lib/apt/lists/*

# 编译安装 Drogon（从源码，确保版本可控）
RUN git clone --depth=1 https://github.com/drogonframework/drogon /drogon \
	&& cd /drogon \
	&& git submodule update --init --recursive \
	&& cmake -B build \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_TESTING=OFF \
	&& cmake --build build --target install -j$(nproc) \
	&& rm -rf /drogon

# 编译游戏服务器
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build -j$(nproc)

# ── 运行阶段（精简镜像）──
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
	libssl3 libjsoncpp25 libuuid1 zlib1g libbrotli1 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/drogon-game-server .
COPY --from=builder /src/config.json .

# 云托管默认使用 8080 端口
EXPOSE 8080

CMD ["./drogon-game-server"]
