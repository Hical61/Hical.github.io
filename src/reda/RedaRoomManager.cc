// src/reda/RedaRoomManager.cc
// 多人红A 4人联机房间管理器实现
// 游戏引擎移植自 cloudfunctions/startMultiGame/index.js + subpkg-games/utils/reda/game-engine.js

#include "RedaRoomManager.h"
#include "common/utils.h"
#include <drogon/drogon.h>
#include <algorithm>
#include <cassert>

// ══════════════════════════════════════════════════════════════
//  牌符号表
// ══════════════════════════════════════════════════════════════

static const std::string SUIT_SYMBOLS[4] = {"♠","♥","♦","♣"};
static const std::string RANK_SYMBOLS[13] = {
    "3","4","5","6","7","8","9","10","J","Q","K","A","2"
};

// ══════════════════════════════════════════════════════════════
//  单例
// ══════════════════════════════════════════════════════════════

RedaRoomManager& RedaRoomManager::instance() {
    static RedaRoomManager inst;
    return inst;
}

// ══════════════════════════════════════════════════════════════
//  工具
// ══════════════════════════════════════════════════════════════

std::string RedaRoomManager::genRoomId() {
    return genToken().substr(0, 8);
}

bool RedaRoomManager::_checkToken(const RedaRoom& room, const std::string& openId, const std::string& token) {
    for (auto& p : room.players) if (p.openId == openId) return p.token == token;
    return false;
}

RedaPlayer* RedaRoomManager::_findPlayer(RedaRoom& room, const std::string& openId) {
    for (auto& p : room.players) if (p.openId == openId) return &p;
    return nullptr;
}

void RedaRoomManager::_sendTo(const RedaPlayer& player, const Json::Value& msg) {
    if (player.wsConn && player.wsConn->connected()) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        player.wsConn->send(Json::writeString(wb, msg));
    }
}

void RedaRoomManager::_broadcast(RedaRoom& room, const Json::Value& msg) {
    for (auto& p : room.players) _sendTo(p, msg);
}

void RedaRoomManager::_broadcastRoomUpdate(RedaRoom& room) {
    Json::Value msg;
    msg["type"] = "room_update";
    Json::Value d;
    d["roomId"] = room.roomId;
    d["status"] = room.status == RedaRoomStatus::WAITING ? "waiting" :
                  room.status == RedaRoomStatus::PLAYING  ? "playing" : "finished";
    Json::Value ps(Json::arrayValue);
    for (auto& p : room.players) {
        Json::Value pj;
        pj["openId"]    = p.openId;
        pj["name"]      = p.name;
        pj["isCreator"] = p.isCreator;
        pj["connected"] = p.connected;
        ps.append(pj);
    }
    d["players"]    = ps;
    d["canStart"]   = (int)room.players.size() == 4;
    msg["data"]     = d;
    _broadcast(room, msg);
}

Json::Value RedaRoomManager::_buildPublicGameState(const RedaRoom& room) {
    const auto& gs = room.gameState;
    Json::Value d;
    d["currentPlayerIndex"] = gs.currentPlayerIndex;
    d["currentPlayerName"]  = gs.currentPlayerName;
    d["message"]            = gs.message;
    d["phase"]              = gs.phase;
    d["lastPlayedByIndex"]  = gs.lastPlayedByIndex;
    d["isSpecialHandPlayed"]= gs.isSpecialHandPlayed;
    d["allCampsRevealed"]   = gs.allCampsRevealed;
    d["lastPlayedHand"]     = gs.lastPlayedHand;

    // 桌面牌
    Json::Value tc(Json::arrayValue);
    for (auto& c : gs.tableCards) tc.append(c.toJson());
    d["tableCards"] = tc;

    // 当前轮积分牌
    Json::Value sc(Json::arrayValue);
    for (auto& c : gs.currentTrickScoreCards) sc.append(c.toJson());
    d["currentTrickScoreCards"] = sc;

    // passed players
    Json::Value pp(Json::arrayValue);
    for (int idx : gs.passedPlayers) pp.append(idx);
    d["passedPlayers"] = pp;

    // 公开玩家信息
    Json::Value players(Json::arrayValue);
    for (auto& pub : gs.players) players.append(pub.toJson());
    d["players"] = players;

    d["gameResult"] = gs.gameResult;
    return d;
}

void RedaRoomManager::_broadcastGameState(RedaRoom& room) {
    Json::Value msg;
    msg["type"] = "game_state";
    msg["ts"]   = nowMs();
    msg["data"] = _buildPublicGameState(room);
    _broadcast(room, msg);
}

// ══════════════════════════════════════════════════════════════
//  建牌（52张，与 startMultiGame 完全一致）
// ══════════════════════════════════════════════════════════════

RedaCard RedaRoomManager::createCard(int suit, int rank) {
    RedaCard c;
    c.suit        = suit;
    c.rank        = rank;
    c.rankValue   = rank;
    c.isRed       = (suit == 1 || suit == 2);
    c.isRedAce    = c.isRed && (rank == 11);  // ACE = rank 11
    c.isHeartAce  = (suit == 1) && (rank == 11);
    c.isDiamondAce= (suit == 2) && (rank == 11);
    c.isSpadeThree= (suit == 0) && (rank == 0);   // THREE = rank 0
    c.isScoreCard = (rank == 2 || rank == 7 || rank == 10);  // 5,10,K
    c.rankSymbol  = RANK_SYMBOLS[rank];
    c.suitSymbol  = SUIT_SYMBOLS[suit];
    return c;
}

std::vector<RedaCard> RedaRoomManager::buildDeck() {
    std::vector<RedaCard> deck;
    deck.reserve(52);
    for (int s = 0; s < 4; ++s)
        for (int r = 0; r < 13; ++r)
            deck.push_back(createCard(s, r));
    return deck;
}

void RedaRoomManager::sortHand(std::vector<RedaCard>& hand) {
    std::sort(hand.begin(), hand.end(), [](const RedaCard& a, const RedaCard& b){
        return a.rankValue != b.rankValue ? a.rankValue < b.rankValue : a.suit < b.suit;
    });
}

// ══════════════════════════════════════════════════════════════
//  简化版出牌类型检测（5种基本牌型）
//  完整版在客户端 hand-type.js 中，服务端只需验证合法性
// ══════════════════════════════════════════════════════════════

Json::Value RedaRoomManager::detectHandType(const std::vector<RedaCard>& cards) {
    Json::Value h;
    h["cards"] = Json::Value(Json::arrayValue);
    for (auto& c : cards) h["cards"].append(c.toJson());

    int n = (int)cards.size();
    if (n == 0) { h["type"] = 0; return h; }

    // 频率统计
    std::map<int,int> freq;
    for (auto& c : cards) freq[c.rankValue]++;

    // 单张
    if (n == 1) { h["type"] = 1; h["rank"] = cards[0].rankValue; return h; }

    // 对子
    if (n == 2 && freq.size() == 1) { h["type"] = 2; h["rank"] = cards[0].rankValue; return h; }

    // 顺子（5张，连续，不含2）
    if (n == 5) {
        bool allSingle = true;
        std::vector<int> vals;
        for (auto& [v, cnt] : freq) {
            if (cnt != 1 || v == 12) { allSingle = false; break; }  // 2 = rank 12
            vals.push_back(v);
        }
        if (allSingle) {
            std::sort(vals.begin(), vals.end());
            bool consec = true;
            for (int i = 1; i < (int)vals.size(); ++i) if (vals[i] != vals[i-1]+1) { consec=false; break; }
            if (consec) { h["type"] = 3; h["rank"] = vals.back(); return h; }
        }
    }

    // 三张
    if (n == 3 && freq.size() == 1) { h["type"] = 4; h["rank"] = cards[0].rankValue; return h; }

    // 炸弹（4张同值）
    if (n == 4 && freq.size() == 1) { h["type"] = 5; h["rank"] = cards[0].rankValue; return h; }

    h["type"] = 0;  // 无效
    return h;
}

bool RedaRoomManager::canBeat(const Json::Value& play, const Json::Value& last) {
    if (last.isNull() || !last.isMember("type") || last["type"].asInt() == 0) return true;

    int pType = play["type"].asInt();
    int lType = last["type"].asInt();
    int pRank = play["rank"].asInt();
    int lRank = last["rank"].asInt();

    // 炸弹 > 一切
    if (pType == 5 && lType != 5) return true;
    if (lType == 5 && pType != 5) return false;

    if (pType != lType) return false;
    // 同型比 rank
    return pRank > lRank;
}

// ══════════════════════════════════════════════════════════════
//  HTTP 操作
// ══════════════════════════════════════════════════════════════

RedaRoomManager::CreateResult RedaRoomManager::createRoom(
    const std::string& openId, const std::string& name)
{
    std::lock_guard<std::mutex> lk(_mu);
    _cleanupOldRooms();

    std::string roomId = genRoomId();
    std::string token  = genToken();

    RedaRoom room;
    room.roomId    = roomId;
    room.status    = RedaRoomStatus::WAITING;
    room.createdAt = nowMs();

    RedaPlayer p;
    p.openId    = openId;
    p.name      = name.empty() ? "玩家" : name;
    p.isCreator = true;
    p.token     = token;
    room.players.push_back(p);

    _rooms[roomId] = std::move(room);
    return { true, roomId, token, "" };
}

RedaRoomManager::JoinResult RedaRoomManager::joinRoom(
    const std::string& roomId, const std::string& openId, const std::string& name)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return { false, "", "房间不存在", {} };

    RedaRoom& room = it->second;
    if (room.status != RedaRoomStatus::WAITING) return { false, "", "游戏已开始", {} };
    if ((int)room.players.size() >= 4) return { false, "", "房间已满（4/4）", {} };
    for (auto& p : room.players) if (p.openId == openId) return { false, "", "你已在房间中", {} };

    std::string token = genToken();
    RedaPlayer p;
    p.openId    = openId;
    p.name      = name.empty() ? "玩家" : name;
    p.isCreator = false;
    p.token     = token;
    room.players.push_back(p);

    // 构建 players JSON 返回
    Json::Value ps(Json::arrayValue);
    for (auto& pl : room.players) {
        Json::Value pj;
        pj["openId"]    = pl.openId;
        pj["name"]      = pl.name;
        pj["isCreator"] = pl.isCreator;
        ps.append(pj);
    }

    // 广播 room_update（WS 未连接时 _broadcastRoomUpdate 无效，连接后重新推送）
    // 注意：joinRoom 时 WS 可能还没建立，广播由 onWsOpen 时补推
    _broadcastRoomUpdate(room);

    return { true, token, "", ps };
}

RedaRoomManager::LeaveResult RedaRoomManager::leaveRoom(
    const std::string& roomId, const std::string& openId, const std::string& token)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return { true, "" };

    RedaRoom& room = it->second;
    if (!_checkToken(room, openId, token)) return { false, "token 无效" };

    // 通知中止
    if (room.status == RedaRoomStatus::PLAYING) {
        auto* p = _findPlayer(room, openId);
        std::string leaverName = p ? p->name : "某玩家";
        Json::Value abortMsg;
        abortMsg["type"] = "game_aborted";
        Json::Value d;
        d["abortedByName"] = leaverName;
        abortMsg["data"]   = d;
        _broadcast(room, abortMsg);
    }

    _rooms.erase(it);
    return { true, "" };
}

RedaRoomManager::StartResult RedaRoomManager::startGame(
    const std::string& roomId, const std::string& openId, const std::string& token)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return { false, "房间不存在" };

    RedaRoom& room = it->second;
    if (!_checkToken(room, openId, token)) return { false, "token 无效" };
    if (room.status != RedaRoomStatus::WAITING) return { false, "游戏已开始" };
    if ((int)room.players.size() != 4) return { false, "需要恰好4名玩家" };

    // 只有房主可以开始
    RedaPlayer* creator = _findPlayer(room, openId);
    if (!creator || !creator->isCreator) return { false, "只有房主可以开始游戏" };

    // ── 发牌 ──
    auto deck = buildDeck();
    shuffleVec(deck);

    std::vector<std::vector<RedaCard>> hands(4);
    for (int i = 0; i < 52; ++i) hands[i % 4].push_back(deck[i]);
    for (auto& h : hands) sortHand(h);

    // 确定先手（持有♠3）
    int firstIdx = 0;
    for (int i = 0; i < 4; ++i) {
        for (auto& c : hands[i]) { if (c.isSpadeThree) { firstIdx = i; goto found; } }
    }
    found:

    // 确定阵营（有红A的是 redAce 阵营）
    std::vector<std::string> camps(4, "civilian");
    for (int i = 0; i < 4; ++i)
        for (auto& c : hands[i]) if (c.isRedAce) { camps[i] = "redAce"; break; }

    // 更新玩家手牌和阵营
    for (int i = 0; i < 4; ++i) {
        room.players[i].hand      = hands[i];
        room.players[i].camp      = camps[i];
        room.players[i].handCount = (int)hands[i].size();
    }

    // 初始化公开游戏状态
    room.gameState = RedaGameState();
    room.gameState.phase                = "playing";
    room.gameState.currentPlayerIndex   = firstIdx;
    room.gameState.currentPlayerName    = room.players[firstIdx].name;
    room.gameState.message = room.players[firstIdx].name + " 持有♠3，率先出牌！";
    room.gameState.lastPlayedByIndex    = -1;
    room.gameState.lastPlayedHand       = Json::Value(Json::nullValue);

    for (int i = 0; i < 4; ++i) {
        RedaPublicPlayer pub;
        pub.id          = i;
        pub.name        = room.players[i].name;
        pub.isCreator   = room.players[i].isCreator;
        pub.camp        = camps[i];
        pub.handCount   = 13;
        pub.lastPlayedCards = Json::Value(Json::arrayValue);
        pub.lastPlayedHand  = Json::Value(Json::nullValue);
        pub.scoreHistory    = Json::Value(Json::arrayValue);
        room.gameState.players.push_back(pub);
    }

    room.status = RedaRoomStatus::PLAYING;

    // 向每位玩家分别推送 game_start（含私有手牌）
    for (int i = 0; i < 4; ++i) {
        Json::Value msg;
        msg["type"] = "game_start";
        msg["ts"]   = nowMs();
        Json::Value d;

        Json::Value hand(Json::arrayValue);
        for (auto& c : room.players[i].hand) hand.append(c.toJson());
        d["myHand"]           = hand;
        d["myPlayerIndex"]    = i;
        d["firstPlayerIndex"] = firstIdx;
        d["myCamp"]           = camps[i];

        // 公开玩家信息
        Json::Value pubPlayers(Json::arrayValue);
        for (auto& pub : room.gameState.players) pubPlayers.append(pub.toJson());
        d["publicPlayers"] = pubPlayers;

        msg["data"] = d;
        _sendTo(room.players[i], msg);
    }

    return { true, "" };
}

Json::Value RedaRoomManager::getRoomJson(const std::string& roomId) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return Json::Value(Json::nullValue);

    const RedaRoom& room = it->second;
    Json::Value r;
    r["roomId"] = room.roomId;
    r["status"] = room.status == RedaRoomStatus::WAITING ? "waiting" :
                  room.status == RedaRoomStatus::PLAYING  ? "playing" : "finished";
    Json::Value ps(Json::arrayValue);
    for (auto& p : room.players) {
        Json::Value pj;
        pj["openId"]    = p.openId;
        pj["name"]      = p.name;
        pj["isCreator"] = p.isCreator;
        pj["connected"] = p.connected;
        ps.append(pj);
    }
    r["players"]  = ps;
    r["canStart"] = (int)room.players.size() == 4;
    return r;
}

// ══════════════════════════════════════════════════════════════
//  WebSocket
// ══════════════════════════════════════════════════════════════

void RedaRoomManager::onWsOpen(const std::string& roomId, const std::string& openId,
                                const std::string& token, drogon::WebSocketConnectionPtr conn)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) { conn->forceClose(); return; }

    RedaRoom& room = it->second;
    if (!_checkToken(room, openId, token)) { conn->forceClose(); return; }

    auto* p = _findPlayer(room, openId);
    if (!p) { conn->forceClose(); return; }

    p->wsConn    = conn;
    p->connected = true;

    // 推送房间状态
    _broadcastRoomUpdate(room);

    // 若游戏进行中，补推游戏状态 + 私有手牌
    if (room.status == RedaRoomStatus::PLAYING) {
        Json::Value gsMsg;
        gsMsg["type"] = "game_state";
        gsMsg["ts"]   = nowMs();
        gsMsg["data"] = _buildPublicGameState(room);
        _sendTo(*p, gsMsg);

        // 补推私有手牌（重连用）
        Json::Value handMsg;
        handMsg["type"] = "my_hand";
        Json::Value d;
        Json::Value hand(Json::arrayValue);
        for (auto& c : p->hand) hand.append(c.toJson());
        d["myHand"]  = hand;
        handMsg["data"] = d;
        _sendTo(*p, handMsg);
    }
}

void RedaRoomManager::onWsClose(const std::string& roomId, const std::string& openId) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return;

    RedaRoom& room = it->second;
    auto* p = _findPlayer(room, openId);
    if (p) { p->connected = false; p->wsConn = nullptr; }

    Json::Value msg;
    msg["type"] = "player_disconnect";
    Json::Value d;
    d["openId"]              = openId;
    d["reconnectDeadlineSec"] = 30;
    msg["data"] = d;
    for (auto& pl : room.players) if (pl.openId != openId) _sendTo(pl, msg);
}

// ══════════════════════════════════════════════════════════════
//  游戏动作
// ══════════════════════════════════════════════════════════════

void RedaRoomManager::onPlay(const std::string& roomId, const std::string& openId,
                              const std::vector<RedaCard>& cards)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return;

    RedaRoom& room = it->second;
    if (room.status != RedaRoomStatus::PLAYING) return;

    auto& gs = room.gameState;
    auto* me = _findPlayer(room, openId);
    if (!me) return;

    // 验证是否轮到自己
    int myIdx = -1;
    for (int i = 0; i < (int)room.players.size(); ++i)
        if (room.players[i].openId == openId) { myIdx = i; break; }
    if (myIdx != gs.currentPlayerIndex) return;

    // 验证牌型
    Json::Value playHand = detectHandType(cards);
    if (playHand["type"].asInt() == 0) {
        Json::Value errMsg;
        errMsg["type"] = "error";
        Json::Value d; d["code"] = "invalid_hand"; d["msg"] = "无效牌型";
        errMsg["data"] = d;
        _sendTo(*me, errMsg);
        return;
    }

    // 验证能否压过上家
    if (!canBeat(playHand, gs.lastPlayedHand)) {
        Json::Value errMsg;
        errMsg["type"] = "error";
        Json::Value d; d["code"] = "cannot_beat"; d["msg"] = "牌力不足，无法压过";
        errMsg["data"] = d;
        _sendTo(*me, errMsg);
        return;
    }

    // 从手牌移除已出牌
    std::set<std::string> playKeys;
    for (auto& c : cards) playKeys.insert(RANK_SYMBOLS[c.rank] + SUIT_SYMBOLS[c.suit]);
    me->hand.erase(
        std::remove_if(me->hand.begin(), me->hand.end(), [&](const RedaCard& c){
            return playKeys.count(RANK_SYMBOLS[c.rank] + SUIT_SYMBOLS[c.suit]) > 0;
        }),
        me->hand.end()
    );
    me->handCount = (int)me->hand.size();
    gs.players[myIdx].handCount = me->handCount;
    gs.players[myIdx].lastPlayedCards = playHand["cards"];
    gs.players[myIdx].lastPlayedHand  = playHand;

    // 记录已出红A
    for (auto& c : cards) {
        if (c.isRedAce) {
            gs.players[myIdx].hasPlayedRedAce = true;
            if (!gs.allCampsRevealed) gs.allCampsRevealed = true;
        }
    }

    // 积分牌统计
    for (auto& c : cards) if (c.isScoreCard) gs.currentTrickScoreCards.push_back(c);

    // 更新桌面
    gs.tableCards = cards;
    gs.lastPlayedHand     = playHand;
    gs.lastPlayedByIndex  = myIdx;
    gs.passedPlayers.clear();

    // 下一位玩家
    int nextIdx = (myIdx + 1) % 4;
    gs.currentPlayerIndex = nextIdx;
    gs.currentPlayerName  = room.players[nextIdx].name;
    gs.message            = room.players[nextIdx].name + " 出牌...";

    _checkTrickEnd(room);
    if (room.status == RedaRoomStatus::FINISHED) return;

    _checkGameEnd(room);
    if (room.status == RedaRoomStatus::FINISHED) return;

    _broadcastGameState(room);
}

void RedaRoomManager::onPass(const std::string& roomId, const std::string& openId) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomId);
    if (it == _rooms.end()) return;

    RedaRoom& room = it->second;
    if (room.status != RedaRoomStatus::PLAYING) return;

    auto& gs = room.gameState;
    int myIdx = -1;
    for (int i = 0; i < (int)room.players.size(); ++i)
        if (room.players[i].openId == openId) { myIdx = i; break; }
    if (myIdx != gs.currentPlayerIndex) return;

    // 记录 pass
    gs.passedPlayers.push_back(myIdx);

    // 下一位
    int nextIdx = (myIdx + 1) % 4;
    gs.currentPlayerIndex = nextIdx;
    gs.currentPlayerName  = room.players[nextIdx].name;
    gs.message            = room.players[nextIdx].name + " 出牌...";

    _broadcastGameState(room);
}

// 检测一轮（trick）是否结束：其他3人均 pass
void RedaRoomManager::_checkTrickEnd(RedaRoom& room) {
    auto& gs = room.gameState;
    // 桌面有牌，且除上家外其余3人均 pass
    if ((int)gs.passedPlayers.size() >= 3) {
        // 本轮结束，分配积分，清桌面
        int winnerIdx = gs.lastPlayedByIndex;
        // 统计本轮积分牌
        int score = 0;
        for (auto& c : gs.currentTrickScoreCards) {
            if (c.rank == 2)  score += 5;   // 5分
            if (c.rank == 7)  score += 10;  // 10分
            if (c.rank == 10) score += 10;  // K=10分
        }
        if (winnerIdx >= 0 && winnerIdx < (int)gs.players.size()) {
            gs.players[winnerIdx].score += score;
        }
        gs.currentTrickScoreCards.clear();
        gs.tableCards.clear();
        gs.lastPlayedHand  = Json::Value(Json::nullValue);
        gs.lastPlayedByIndex = -1;
        gs.passedPlayers.clear();
        gs.isSpecialHandPlayed = false;
        gs.currentPlayerIndex = winnerIdx >= 0 ? winnerIdx : 0;
        gs.currentPlayerName  = room.players[gs.currentPlayerIndex].name;
        gs.message            = gs.currentPlayerName + " 赢得本轮，继续出牌";
    }
}

// 检测游戏是否结束：任一玩家手牌为空
void RedaRoomManager::_checkGameEnd(RedaRoom& room) {
    auto& gs = room.gameState;
    bool gameEnd = false;
    int  firstEmpty = -1;
    for (int i = 0; i < (int)room.players.size(); ++i) {
        if (room.players[i].hand.empty()) {
            gameEnd = true;
            if (firstEmpty < 0) firstEmpty = i;
            // 设置排名
            if (gs.players[i].rank_result == "none") {
                // 按完成顺序赋予 1st/2nd/3rd/4th
                int finishedCount = 0;
                for (auto& pp : gs.players) if (pp.rank_result != "none") finishedCount++;
                static const std::string RANKS[] = {"1st","2nd","3rd","4th"};
                gs.players[i].rank_result = RANKS[std::min(finishedCount, 3)];
            }
        }
    }

    // 只有一人手牌未空时游戏结束
    int remaining = 0;
    for (auto& p : room.players) if (!p.hand.empty()) remaining++;
    if (remaining <= 1) {
        // 最后一人设为4th
        for (int i = 0; i < (int)room.players.size(); ++i) {
            if (!room.players[i].hand.empty() && gs.players[i].rank_result == "none") {
                gs.players[i].rank_result = "4th";
            }
        }
        gs.phase = "gameEnd";
        gs.message = "游戏结束！";

        // 构建结果
        Json::Value result;
        Json::Value rankList(Json::arrayValue);
        for (auto& pub : gs.players) {
            Json::Value rj;
            rj["name"]  = pub.name;
            rj["rank"]  = pub.rank_result;
            rj["score"] = pub.score;
            rj["camp"]  = pub.camp;
            rankList.append(rj);
        }
        result["players"] = rankList;
        gs.gameResult = result;

        room.status = RedaRoomStatus::FINISHED;

        Json::Value overMsg;
        overMsg["type"] = "game_over";
        overMsg["ts"]   = nowMs();
        overMsg["data"] = _buildPublicGameState(room);
        _broadcast(room, overMsg);
    }
}

// ══════════════════════════════════════════════════════════════
//  过期清理
// ══════════════════════════════════════════════════════════════

void RedaRoomManager::_cleanupOldRooms() {
    int64_t now      = nowMs();
    const int64_t EX = 2LL * 3600 * 1000;
    for (auto it = _rooms.begin(); it != _rooms.end(); ) {
        if (now - it->second.createdAt > EX) it = _rooms.erase(it);
        else ++it;
    }
}
