// src/reda/RedaRoomManager.h
// 多人红A（REDA）4人联机房间管理器

#pragma once
#include "common/types.h"
#include <drogon/WebSocketConnection.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <json/json.h>

enum class RedaRoomStatus { WAITING, PLAYING, FINISHED };

// REDA 手牌（只发给本人，其他人看不到）
struct RedaPlayer {
    std::string                      openId;
    std::string                      name;
    bool                             isCreator   = false;
    std::string                      token;
    drogon::WebSocketConnectionPtr   wsConn;
    bool                             connected   = false;
    std::vector<RedaCard>            hand;
    std::string                      camp;       // "redAce" | "civilian"
    int                              handCount   = 0;  // 镜像 hand.size()，用于公开广播
};

// 公开游戏状态（广播给所有人）
struct RedaGameState {
    int         currentPlayerIndex  = 0;
    std::string currentPlayerName;
    std::string message;
    std::string phase;  // "playing" | "gameEnd"

    // 桌面
    std::vector<RedaCard>  tableCards;
    Json::Value            lastPlayedHand;   // {type, cards}
    int                    lastPlayedByIndex = -1;
    bool                   isSpecialHandPlayed = false;
    std::vector<int>       passedPlayers;
    std::vector<RedaCard>  currentTrickScoreCards;
    bool                   allCampsRevealed = false;

    // 公开玩家信息
    std::vector<RedaPublicPlayer> players;
    Json::Value                   gameResult;  // null or result object
};

struct RedaRoom {
    std::string               roomId;
    RedaRoomStatus            status     = RedaRoomStatus::WAITING;
    std::vector<RedaPlayer>   players;   // 最多4人，index 0 = 房主
    RedaGameState             gameState;
    int64_t                   createdAt  = 0;
};

class RedaRoomManager {
public:
    static RedaRoomManager& instance();

    struct CreateResult { bool ok; std::string roomId; std::string token; std::string errMsg; };
    CreateResult createRoom(const std::string& openId, const std::string& name);

    struct JoinResult { bool ok; std::string token; std::string errMsg; Json::Value players; };
    JoinResult joinRoom(const std::string& roomId, const std::string& openId, const std::string& name);

    struct LeaveResult { bool ok; std::string errMsg; };
    LeaveResult leaveRoom(const std::string& roomId, const std::string& openId, const std::string& token);

    struct StartResult { bool ok; std::string errMsg; };
    StartResult startGame(const std::string& roomId, const std::string& openId, const std::string& token);

    Json::Value getRoomJson(const std::string& roomId);

    // WebSocket
    void onWsOpen(const std::string& roomId, const std::string& openId,
                  const std::string& token, drogon::WebSocketConnectionPtr conn);
    void onWsClose(const std::string& roomId, const std::string& openId);

    // 游戏动作
    void onPlay(const std::string& roomId, const std::string& openId,
                const std::vector<RedaCard>& cards);
    void onPass(const std::string& roomId, const std::string& openId);

private:
    RedaRoomManager() = default;
    ~RedaRoomManager() = default;
    RedaRoomManager(const RedaRoomManager&) = delete;

    mutable std::mutex                             _mu;
    std::unordered_map<std::string, RedaRoom>      _rooms;

    std::string genRoomId();
    bool _checkToken(const RedaRoom& room, const std::string& openId, const std::string& token);
    RedaPlayer* _findPlayer(RedaRoom& room, const std::string& openId);
    void _broadcast(RedaRoom& room, const Json::Value& msg);
    void _sendTo(const RedaPlayer& player, const Json::Value& msg);
    void _broadcastGameState(RedaRoom& room);
    void _broadcastRoomUpdate(RedaRoom& room);
    Json::Value _buildPublicGameState(const RedaRoom& room);
    void _cleanupOldRooms();

    // 红A 游戏引擎（嵌入在管理器中）
    static std::vector<RedaCard> buildDeck();
    static RedaCard createCard(int suit, int rank);
    static void sortHand(std::vector<RedaCard>& hand);
    // 出牌验证
    static Json::Value detectHandType(const std::vector<RedaCard>& cards);
    static bool canBeat(const Json::Value& play, const Json::Value& last);
    // 胜负/得分
    void _checkTrickEnd(RedaRoom& room);
    void _checkGameEnd(RedaRoom& room);
};
