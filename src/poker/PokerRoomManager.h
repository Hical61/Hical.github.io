// src/poker/PokerRoomManager.h
// 职场跑得快房间管理器：管理房间生命周期 + WebSocket 连接 + 游戏状态

#pragma once
#include "common/types.h"
#include "PokerGameEngine.h"
#include <drogon/WebSocketConnection.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <json/json.h>

// ── 房间状态 ──
enum class PokerRoomStatus {
    WAITING,   // 等待对手
    PLAYING,   // 游戏进行中
    FINISHED   // 游戏结束
};

// ── 连接中的玩家 ──
struct PokerConnPlayer {
    std::string                          openId;
    std::string                          name;
    std::string                          avatar;
    std::string                          roleId;
    std::string                          token;
    bool                                 isCreator    = false;
    bool                                 connected    = false;
    drogon::WebSocketConnectionPtr       wsConn;
    std::vector<PokerCard>               hand;
    int                                  skillUsed    = 0;
    // 黑客木马
    std::string                          hackerTrojanCardId;
    std::string                          hackerTrojanPlantedBy;
    // hacker 被动：boost 值存在手牌 card.hackerBoostVal（交给客户端显示）
};

// ── 完整房间 ──
struct PokerRoom {
    std::string               roomCode;
    PokerRoomStatus           status     = PokerRoomStatus::WAITING;
    std::vector<PokerConnPlayer> players; // 最多2人，index 0=房主
    std::vector<PokerCard>    wastePile;
    std::vector<PokerCard>    lastPlay;
    std::string               lastPlayBy;
    PlayType                  lastPlayType = PlayType::INVALID;
    std::string               lastPlayTypeName;
    std::string               currentTurn;
    int                       turn         = 0;
    int64_t                   turnStartTime = 0;
    std::string               winner;
    bool                      hrSilenceActive  = false;
    std::string               hrSilenceFor;    // HR 的 openId，沉默解除后轮到它
    // 黑客木马（存在房间级别，避免玩家结构重复）
    struct HackerTrojan {
        bool        active       = false;
        std::string plantedBy;
        std::string targetOpenId;
        std::string cardId;
        std::string cardName;
    } hackerTrojan;
    // PR 透视
    std::unordered_map<std::string, std::vector<std::string>> prVisible; // openId → [cardId]
    // 被动日志（供客户端展示特效）
    struct PassiveLogEntry {
        std::string type;
        std::string openId;
        std::string target;
        std::string card;
    };
    std::vector<PassiveLogEntry> passiveLog;
    int64_t createdAt = 0;
};

// ── 单例房间管理器 ──
class PokerRoomManager {
public:
    static PokerRoomManager& instance();

    // ── HTTP 操作（线程安全）──
    struct CreateResult { bool ok; std::string roomCode; std::string token; std::string errMsg; };
    CreateResult createRoom(const std::string& openId, const std::string& name,
                            const std::string& avatar, const std::string& roleId);

    struct JoinResult { bool ok; std::string token; std::string errMsg; };
    JoinResult joinRoom(const std::string& roomCode, const std::string& openId,
                        const std::string& name, const std::string& avatar,
                        const std::string& roleId);

    struct LeaveResult { bool ok; std::string errMsg; };
    LeaveResult leaveRoom(const std::string& roomCode, const std::string& openId,
                          const std::string& token);

    struct StartResult { bool ok; std::string errMsg; };
    StartResult startGame(const std::string& roomCode, const std::string& openId,
                          const std::string& token);

    // 查询房间状态（轮询兜底）
    Json::Value getRoomJson(const std::string& roomCode);

    // ── WebSocket 连接绑定 ──
    void onWsOpen(const std::string& roomCode, const std::string& openId,
                  const std::string& token, drogon::WebSocketConnectionPtr conn);
    void onWsClose(const std::string& roomCode, const std::string& openId);

    // ── 游戏动作（WS 消息触发）──
    void onPlayCards(const std::string& roomCode, const std::string& openId,
                     const std::vector<PokerCard>& cards, bool isHrSkill);
    void onPass(const std::string& roomCode, const std::string& openId);
    void onUseSkill(const std::string& roomCode, const std::string& openId,
                    const std::string& skillType, const Json::Value& extra);

private:
    PokerRoomManager() = default;
    ~PokerRoomManager() = default;
    PokerRoomManager(const PokerRoomManager&) = delete;

    mutable std::mutex                               _mu;
    std::unordered_map<std::string, PokerRoom>       _rooms;  // roomCode → room

    // 工具：向房间所有（或特定）连接广播消息
    void _broadcast(PokerRoom& room, const Json::Value& msg);
    void _sendTo(const PokerConnPlayer& player, const Json::Value& msg);
    Json::Value _buildGameStateMsg(const PokerRoom& room, const std::string& forOpenId);
    void _broadcastGameState(PokerRoom& room);
    void _startCountdownTimer(PokerRoom& room);
    void _cleanupOldRooms();

    // 令牌校验
    bool _checkToken(const PokerRoom& room, const std::string& openId, const std::string& token);
    // 找玩家
    PokerConnPlayer* _findPlayer(PokerRoom& room, const std::string& openId);
    const PokerConnPlayer* _findPlayer(const PokerRoom& room, const std::string& openId) const;
};
