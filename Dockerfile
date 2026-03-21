# drogon-game-server/Dockerfile
# 单阶段构建（避免多阶段运行时库缺失问题）
# 适用于微信云托管（CloudBase Run）

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# ── 安装所有依赖（编译 + 运行时）──
RUN apt-get update && apt-get install -y \
	cmake g++ git pkg-config \
	libssl-dev libjsoncpp-dev uuid-dev \
	zlib1g-dev libbrotli-dev \
	&& rm -rf /var/lib/apt/lists/*

# ── 编译安装 Drogon ──
RUN git clone --depth=1 https://github.com/drogonframework/drogon /drogon \
	&& cd /drogon \
	&& git submodule update --init --recursive \
	&& cmake -B build \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_EXAMPLES=OFF \
	-DBUILD_TESTING=OFF \
	&& cmake --build build --target install -j$(nproc) \
	&& rm -rf /drogon

# ── 复制源码并编译游戏服务器 ──
WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build -j$(nproc)

# 云托管默认端口 8080
EXPOSE 8080

# 使用绝对路径启动，避免工作目录问题
CMD ["/app/build/drogon-game-server"]
