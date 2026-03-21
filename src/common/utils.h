// src/common/utils.h
// 公共工具函数

#pragma once
#include <string>
#include <vector>
#include <random>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

// ── 随机引擎（线程安全：每个调用栈用局部引擎即可，游戏逻辑不在热路径上） ──
inline std::mt19937& rng() {
    static thread_local std::mt19937 engine(
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())
    );
    return engine;
}

// ── 生成 6 字符房间码 ──
inline std::string genRoomCode() {
    static const char CHARS[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static const int  LEN     = sizeof(CHARS) - 1;
    std::uniform_int_distribution<int> dist(0, LEN - 1);
    std::string code;
    code.reserve(6);
    for (int i = 0; i < 6; ++i) code += CHARS[dist(rng())];
    return code;
}

// ── 生成 UUID-like token ──
inline std::string genToken() {
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << dist(rng())
        << std::setw(16) << dist(rng());
    return oss.str();
}

// ── Fisher-Yates shuffle ──
template<typename T>
inline void shuffleVec(std::vector<T>& v) {
    for (int i = static_cast<int>(v.size()) - 1; i > 0; --i) {
        std::uniform_int_distribution<int> d(0, i);
        std::swap(v[i], v[d(rng())]);
    }
}

// ── 当前毫秒时间戳 ──
inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}
