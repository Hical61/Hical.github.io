# drogon-game-server/Dockerfile
# 多阶段构建：
#   Stage 1 (builder)：编译游戏服务器二进制
#   Stage 2 (runtime)：复用同一个 drogon 镜像作为运行时，确保所有动态库完整
#                      只拷贝编译产物，源码和编译中间文件不进入最终镜像
#
# 对比单阶段构建：最终镜像不含源码/中间文件，体积减少约 30~50%

# ── Stage 1: 编译阶段 ────────────────────────────────────────────
FROM drogonframework/drogon:latest AS builder

WORKDIR /app

# 复制源码
COPY . .

# 编译游戏服务器
# 使用 cmake --build --parallel 替代 -j$(nproc)，兼容所有 shell 环境
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build --parallel

# ── Stage 2: 运行阶段 ────────────────────────────────────────────
# 使用与编译阶段相同的基础镜像，确保所有 Drogon 运行时动态库完整匹配
# （避免手动枚举 libcares / libjsoncpp / libssl 等版本不一致问题）
FROM drogonframework/drogon:latest

WORKDIR /app

# 只拷贝编译产物和配置文件（源码、CMake 中间文件等不进入最终镜像）
COPY --from=builder /app/build/drogon-game-server ./build/drogon-game-server
COPY --from=builder /app/config.json ./config.json

# 云托管默认端口 8080（与 config.json 中 port 一致）
EXPOSE 8080

# 使用相对路径启动，确保 CWD 始终是 WORKDIR(/app)
# Drogon 会在 CWD 查找 config.json
CMD ["./build/drogon-game-server"]
