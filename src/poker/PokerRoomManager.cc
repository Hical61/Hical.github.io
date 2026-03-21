// src/poker/PokerRoomManager.cc
// 职场跑得快房间管理器实现

#include "PokerRoomManager.h"
#include "common/utils.h"
#include <drogon/drogon.h>
#include <algorithm>
#include <sstream>

// ══════════════════════════════════════════════════════════════
//  单例
// ══════════════════════════════════════════════════════════════

PokerRoomManager& PokerRoomManager::instance() {
    static PokerRoomManager inst;
    return inst;
}

// ══════════════════════════════════════════════════════════════
//  工具
// ══════════════════════════════════════════════════════════════

PokerConnPlayer* PokerRoomManager::_findPlayer(PokerRoom& room, const std::string& openId) {
    for (auto& p : room.players) if (p.openId == openId) return &p;
    return nullptr;
}
const PokerConnPlayer* PokerRoomManager::_findPlayer(const PokerRoom& room, const std::string& openId) const {
    for (auto& p : room.players) if (p.openId == openId) return &p;
    return nullptr;
}

bool PokerRoomManager::_checkToken(const PokerRoom& room, const std::string& openId, const std::string& token) {
    auto* p = _findPlayer(room, openId);
    return p && p->token == token;
}

void PokerRoomManager::_sendTo(const PokerConnPlayer& player, const Json::Value& msg) {
    if (player.wsConn && player.wsConn->connected()) {
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        player.wsConn->send(Json::writeString(wb, msg));
    }
}

void PokerRoomManager::_broadcast(PokerRoom& room, const Json::Value& msg) {
    for (auto& p : room.players) _sendTo(p, msg);
}

// 构建 game_state 消息（为指定玩家：只返回自己的手牌，对手只返回数量）
Json::Value PokerRoomManager::_buildGameStateMsg(const PokerRoom& room, const std::string& forOpenId) {
    Json::Value msg;
    msg["type"] = "game_state";
    msg["ts"]   = nowMs();

    Json::Value d;
    d["turn"]          = room.turn;
    d["currentTurn"]   = room.currentTurn;
    d["isMyTurn"]      = (room.currentTurn == forOpenId);
    d["turnStartTime"] = room.turnStartTime;
    d["lastPlayBy"]    = room.lastPlayBy;
    d["lastPlayTypeName"] = room.lastPlayTypeName;
    d["wastePileCount"]= (int)room.wastePile.size();
    d["hrSilenceActive"] = room.hrSilenceActive;

    // lastPlay
    Json::Value lp(Json::arrayValue);
    for (auto& c : room.lastPlay) lp.append(c.toJson());
    d["lastPlay"] = lp;

    // 我的手牌
    auto* me = _findPlayer(room, forOpenId);
    if (me) {
        Json::Value hand(Json::arrayValue);
        for (auto& c : me->hand) hand.append(c.toJson());
        d["myHand"]    = hand;
        d["myHandCount"] = (int)me->hand.size();
    }

    // 对手手牌数量
    for (auto& p : room.players) {
        if (p.openId != forOpenId) {
            d["opponentHandCount"] = (int)p.hand.size();
            d["opponentOpenId"]    = p.openId;
            d["opponentConnected"] = p.connected;
            break;
        }
    }

    // pr 透视（只发给对应 openId 的 prVisible）
    if (room.prVisible.count(forOpenId)) {
        Json::Value pv(Json::arrayValue);
        for (auto& id : room.prVisible.at(forOpenId)) pv.append(id);
        d["prVisible"] = pv;
    }

    // 被动日志（最新一条）
    if (!room.passiveLog.empty()) {
        auto& last = room.passiveLog.back();
        Json::Value log;
        log["type"]   = last.type;
        log["openId"] = last.openId;
        log["target"] = last.target;
        log["card"]   = last.card;
        d["passiveLog"] = log;
    }

    // 木马状态（只告诉受害者有木马，不透露是哪张）
    if (room.hackerTrojan.active && room.hackerTrojan.targetOpenId == forOpenId) {
        d["hackerTrojanActive"] = true;
    }

    msg["data"] = d;
    return msg;
}

void PokerRoomManager::_broadcastGameState(PokerRoom& room) {
    for (auto& p : room.players) {
        _sendTo(p, _buildGameStateMsg(room, p.openId));
    }
}

// ══════════════════════════════════════════════════════════════
//  HTTP 操作
// ══════════════════════════════════════════════════════════════

PokerRoomManager::CreateResult PokerRoomManager::createRoom(
    const std::string& openId, const std::string& name,
    const std::string& avatar, const std::string& roleId)
{
    std::lock_guard<std::mutex> lk(_mu);
    _cleanupOldRooms();

    std::string code  = genRoomCode();
    std::string token = genToken();

    PokerRoom room;
    room.roomCode   = code;
    room.status     = PokerRoomStatus::WAITING;
    room.createdAt  = nowMs();

    PokerConnPlayer p;
    p.openId    = openId;
    p.name      = name.empty() ? "玩家" : name;
    p.avatar    = avatar;
    p.roleId    = roleId.empty() ? "intern" : roleId;
    p.token     = token;
    p.isCreator = true;
    room.players.push_back(p);

    _rooms[code] = std::move(room);
    return { true, code, token, "" };
}

PokerRoomManager::JoinResult PokerRoomManager::joinRoom(
    const std::string& roomCode, const std::string& openId,
    const std::string& name, const std::string& avatar,
    const std::string& roleId)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return { false, "", "房间不存在" };

    PokerRoom& room = it->second;
    if (room.status != PokerRoomStatus::WAITING) return { false, "", "游戏已开始" };
    if ((int)room.players.size() >= 2) return { false, "", "房间已满" };
    for (auto& p : room.players) if (p.openId == openId) return { false, "", "你已在房间中" };

    std::string token = genToken();
    PokerConnPlayer p;
    p.openId    = openId;
    p.name      = name.empty() ? "玩家" : name;
    p.avatar    = avatar;
    p.roleId    = roleId.empty() ? "intern" : roleId;
    p.token     = token;
    p.isCreator = false;
    room.players.push_back(p);

    // 广播 room_update
    Json::Value msg;
    msg["type"] = "room_update";
    Json::Value d;
    d["status"] = "waiting";
    Json::Value ps(Json::arrayValue);
    for (auto& pl : room.players) {
        Json::Value pj;
        pj["openId"]    = pl.openId;
        pj["name"]      = pl.name;
        pj["avatar"]    = pl.avatar;
        pj["roleId"]    = pl.roleId;
        pj["isCreator"] = pl.isCreator;
        ps.append(pj);
    }
    d["players"] = ps;
    msg["data"]  = d;
    _broadcast(room, msg);

    return { true, token, "" };
}

PokerRoomManager::LeaveResult PokerRoomManager::leaveRoom(
    const std::string& roomCode, const std::string& openId,
    const std::string& token)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return { true, "" };  // 已不存在，幂等成功

    PokerRoom& room = it->second;
    if (!_checkToken(room, openId, token)) return { false, "token 无效" };

    // 通知对手
    Json::Value msg;
    msg["type"] = "player_disconnect";
    Json::Value d;
    d["openId"]              = openId;
    d["reconnectDeadlineSec"] = 0;  // 0 = 直接结束
    msg["data"] = d;
    for (auto& p : room.players) {
        if (p.openId != openId) _sendTo(p, msg);
    }

    // 删除房间
    _rooms.erase(it);
    return { true, "" };
}

PokerRoomManager::StartResult PokerRoomManager::startGame(
    const std::string& roomCode, const std::string& openId,
    const std::string& token)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return { false, "房间不存在" };

    PokerRoom& room = it->second;
    if (!_checkToken(room, openId, token)) return { false, "token 无效" };
    if (room.status != PokerRoomStatus::WAITING) return { false, "游戏已开始" };
    if ((int)room.players.size() < 2) return { false, "需要2名玩家" };

    // 只有房主可以开始
    auto* creator = _findPlayer(room, openId);
    if (!creator || !creator->isCreator) return { false, "只有房主可以开始游戏" };

    // 发牌
    auto dealRes = PokerGameEngine::deal(room.players[0].openId, room.players[1].openId);
    room.players[0].hand = dealRes.hand0;
    room.players[1].hand = dealRes.hand1;
    room.players[0].roleId = dealRes.role0;  // 覆盖为服务端随机角色
    room.players[1].roleId = dealRes.role1;
    room.wastePile   = dealRes.waste;
    room.currentTurn = dealRes.firstOpenId;
    room.prVisible   = dealRes.prVisible;
    room.status      = PokerRoomStatus::PLAYING;
    room.turn        = 0;
    room.turnStartTime = nowMs();

    // 向每位玩家分别推送 game_start（含各自私有手牌）
    for (auto& p : room.players) {
        Json::Value msg;
        msg["type"] = "game_start";
        msg["ts"]   = nowMs();
        Json::Value d;

        // 私有手牌
        Json::Value hand(Json::arrayValue);
        for (auto& c : p.hand) hand.append(c.toJson());
        d["myHand"] = hand;

        // 对手手牌数量
        for (auto& op : room.players) {
            if (op.openId != p.openId) {
                d["opponentHandCount"] = (int)op.hand.size();
                d["opponentOpenId"]    = op.openId;
                d["opponentName"]      = op.name;
                d["opponentAvatar"]    = op.avatar;
                d["opponentRoleId"]    = op.roleId;
            }
        }
        d["myRoleId"]        = p.roleId;
        d["firstOpenId"]     = room.currentTurn;
        d["isMyTurn"]        = (room.currentTurn == p.openId);
        d["wastePileCount"]  = (int)room.wastePile.size();
        d["turnStartTime"]   = room.turnStartTime;

        // pr 透视
        if (room.prVisible.count(p.openId)) {
            Json::Value pv(Json::arrayValue);
            for (auto& id : room.prVisible.at(p.openId)) pv.append(id);
            d["prVisible"] = pv;
        }

        msg["data"] = d;
        _sendTo(p, msg);
    }

    return { true, "" };
}

Json::Value PokerRoomManager::getRoomJson(const std::string& roomCode) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return Json::Value(Json::nullValue);

    const PokerRoom& room = it->second;
    Json::Value r;
    r["roomCode"] = room.roomCode;
    r["status"]   = room.status == PokerRoomStatus::WAITING ? "waiting" :
                    room.status == PokerRoomStatus::PLAYING  ? "playing" : "finished";
    Json::Value ps(Json::arrayValue);
    for (auto& p : room.players) {
        Json::Value pj;
        pj["openId"]    = p.openId;
        pj["name"]      = p.name;
        pj["avatar"]    = p.avatar;
        pj["roleId"]    = p.roleId;
        pj["isCreator"] = p.isCreator;
        pj["connected"] = p.connected;
        ps.append(pj);
    }
    r["players"] = ps;
    return r;
}

// ══════════════════════════════════════════════════════════════
//  WebSocket 连接管理
// ══════════════════════════════════════════════════════════════

void PokerRoomManager::onWsOpen(const std::string& roomCode, const std::string& openId,
                                 const std::string& token, drogon::WebSocketConnectionPtr conn)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) { conn->forceClose(); return; }

    PokerRoom& room = it->second;
    if (!_checkToken(room, openId, token)) { conn->forceClose(); return; }

    auto* p = _findPlayer(room, openId);
    if (!p) { conn->forceClose(); return; }

    p->wsConn    = conn;
    p->connected = true;

    // 推送当前房间状态
    Json::Value msg;
    msg["type"] = "room_update";
    Json::Value d;
    d["status"] = room.status == PokerRoomStatus::WAITING ? "waiting" :
                  room.status == PokerRoomStatus::PLAYING  ? "playing" : "finished";
    Json::Value ps(Json::arrayValue);
    for (auto& pl : room.players) {
        Json::Value pj;
        pj["openId"]    = pl.openId;
        pj["name"]      = pl.name;
        pj["avatar"]    = pl.avatar;
        pj["roleId"]    = pl.roleId;
        pj["isCreator"] = pl.isCreator;
        pj["connected"] = pl.connected;
        ps.append(pj);
    }
    d["players"] = ps;
    msg["data"]  = d;
    // 只发给自己（重连用）
    _sendTo(*p, msg);

    // 若游戏进行中，额外推送当前状态
    if (room.status == PokerRoomStatus::PLAYING) {
        _sendTo(*p, _buildGameStateMsg(room, openId));
    }
}

void PokerRoomManager::onWsClose(const std::string& roomCode, const std::string& openId) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return;

    PokerRoom& room = it->second;
    auto* p = _findPlayer(room, openId);
    if (p) {
        p->connected = false;
        p->wsConn    = nullptr;
    }

    // 通知对手断线
    Json::Value msg;
    msg["type"] = "player_disconnect";
    Json::Value d;
    d["openId"]              = openId;
    d["reconnectDeadlineSec"] = 30;
    msg["data"] = d;
    for (auto& pl : room.players) {
        if (pl.openId != openId) _sendTo(pl, msg);
    }
}

// ══════════════════════════════════════════════════════════════
//  游戏动作
// ══════════════════════════════════════════════════════════════

void PokerRoomManager::onPlayCards(const std::string& roomCode, const std::string& openId,
                                    const std::vector<PokerCard>& cards, bool isHrSkill)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return;

    PokerRoom& room = it->second;
    if (room.status != PokerRoomStatus::PLAYING) return;
    if (room.currentTurn != openId) {
        // 不是当前玩家回合，静默忽略
        return;
    }

    auto* me = _findPlayer(room, openId);
    if (!me) return;

    // 找对手
    PokerConnPlayer* opp = nullptr;
    for (auto& p : room.players) if (p.openId != openId) { opp = &p; break; }
    if (!opp) return;

    // ── 黑客木马检测（服务端权威）──
    if (room.hackerTrojan.active && room.hackerTrojan.targetOpenId == openId) {
        bool trojanHit = false;
        for (auto& c : cards) {
            if (c.id == room.hackerTrojan.cardId) { trojanHit = true; break; }
        }
        if (trojanHit) {
            // 木马触发：出牌失败，归还手牌（被感染牌降级）
            for (auto& c : cards) {
                PokerCard rc = c;
                if (c.id == room.hackerTrojan.cardId) {
                    rc.val  = 3;
                    // 客户端扩展字段（JSON 级别）
                }
                me->hand.push_back(rc);
            }
            std::string hackerOpenId = room.hackerTrojan.plantedBy;
            room.hackerTrojan = {};  // 清除木马

            room.lastPlay.clear();
            room.lastPlayBy.clear();
            room.lastPlayTypeName.clear();
            room.currentTurn = hackerOpenId.empty() ? opp->openId : hackerOpenId;
            room.turnStartTime = nowMs();

            // 发特效通知
            Json::Value fxMsg;
            fxMsg["type"] = "skill_fx";
            Json::Value fd;
            fd["skillId"]      = "hacker_trojan_trigger";
            fd["byOpenId"]     = hackerOpenId;
            fd["targetOpenId"] = openId;
            fd["desc"]         = "木马触发！出牌失败，牌点降为3";
            fxMsg["data"] = fd;
            _broadcast(room, fxMsg);

            _broadcastGameState(room);
            return;
        }
    }

    // ── 正常出牌 ──
    int& mySkill  = me->skillUsed;
    int& oppSkill = opp->skillUsed;
    PlayResult res = PokerGameEngine::applyPlay(
        me->hand, cards, room.lastPlay, room.lastPlayType,
        me->roleId, opp->roleId,
        openId, opp->openId,
        mySkill, oppSkill
    );

    if (!res.ok) {
        // 发错误给本玩家
        Json::Value errMsg;
        errMsg["type"] = "error";
        Json::Value d;
        d["code"] = "invalid_play";
        d["msg"]  = res.errMsg;
        errMsg["data"] = d;
        _sendTo(*me, errMsg);
        return;
    }

    // 更新废牌堆
    for (auto& c : cards) room.wastePile.push_back(c);

    // 更新 lastPlay（黑客被动：boost 等效值记录到 lastPlay）
    room.lastPlay         = cards;
    room.lastPlayBy       = openId;
    room.lastPlayType     = res.type;

    // ── CFO 税务抽成（对手是 CFO，出 3 张以上被征税）──
    if ((int)cards.size() >= 3 && opp->roleId == "cfo" && !room.wastePile.empty()) {
        std::uniform_int_distribution<int> d2(0, (int)room.wastePile.size()-1);
        int idx = d2(rng());
        PokerCard taxCard = room.wastePile[idx];
        room.wastePile.erase(room.wastePile.begin() + idx);
        me->hand.push_back(taxCard);
        PokerRoom::PassiveLogEntry log;
        log.type   = "cfo_tax";
        log.openId = openId;
        log.card   = taxCard.rankSymbol + taxCard.suit;
        room.passiveLog.push_back(log);
    }

    // HR 主动大招：对手被沉默
    if (isHrSkill && me->roleId == "hr") {
        room.hrSilenceActive = true;
        room.hrSilenceFor    = openId;  // HR 的 openId
        room.currentTurn     = opp->openId;
        me->skillUsed++;

        Json::Value fxMsg;
        fxMsg["type"] = "skill_fx";
        Json::Value fd;
        fd["skillId"]      = "hr_silence";
        fd["byOpenId"]     = openId;
        fd["targetOpenId"] = opp->openId;
        fd["desc"]         = "单向面谈：对手本回合被沉默";
        fxMsg["data"]      = fd;
        _broadcast(room, fxMsg);
    } else {
        room.currentTurn = opp->openId;
    }
    room.turn++;
    room.turnStartTime = nowMs();

    // 炸弹特效
    if (res.isBomb || res.isJokerBomb) {
        Json::Value fxMsg;
        fxMsg["type"] = "skill_fx";
        Json::Value fd;
        fd["skillId"]      = res.isJokerBomb ? "joker_bomb" : "bomb";
        fd["byOpenId"]     = openId;
        fd["targetOpenId"] = opp->openId;
        fd["desc"]         = res.isJokerBomb ? "王炸！" : "炸弹！";
        fxMsg["data"]      = fd;
        _broadcast(room, fxMsg);
    }

    // 胜负检测
    if (res.gameOver) {
        room.status = PokerRoomStatus::FINISHED;
        room.winner = res.winnerOpenId;
        Json::Value overMsg;
        overMsg["type"] = "game_over";
        Json::Value od;
        od["winnerOpenId"] = res.winnerOpenId;
        od["reason"]       = res.winReason;
        overMsg["data"]    = od;
        _broadcast(room, overMsg);
        return;
    }

    _broadcastGameState(room);
}

void PokerRoomManager::onPass(const std::string& roomCode, const std::string& openId) {
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return;

    PokerRoom& room = it->second;
    if (room.status != PokerRoomStatus::PLAYING) return;
    if (room.currentTurn != openId) return;  // 不是我的回合

    auto* me = _findPlayer(room, openId);
    if (!me) return;
    PokerConnPlayer* opp = nullptr;
    for (auto& p : room.players) if (p.openId != openId) { opp = &p; break; }
    if (!opp) return;

    // 上家角色（出牌方）
    std::string lastPlayerOpenId = room.lastPlayBy.empty() ? opp->openId : room.lastPlayBy;
    PokerConnPlayer* lastPlayer = _findPlayer(room, lastPlayerOpenId);
    std::string lastPlayerRole  = lastPlayer ? lastPlayer->roleId : "";

    // ── 被动技能：公关情绪价值（透视要不起方1张牌）──
    if (lastPlayerRole == "pr" && !me->hand.empty()) {
        std::vector<std::string>& alreadyVisible = room.prVisible[lastPlayerOpenId];
        std::vector<PokerCard> unknown;
        for (auto& c : me->hand) {
            bool seen = false;
            for (auto& id : alreadyVisible) if (id == c.id) { seen = true; break; }
            if (!seen) unknown.push_back(c);
        }
        if (!unknown.empty()) {
            std::uniform_int_distribution<int> d(0, (int)unknown.size()-1);
            std::string revealId = unknown[d(rng())].id;
            alreadyVisible.push_back(revealId);
            // 发特效
            Json::Value fxMsg;
            fxMsg["type"] = "skill_fx";
            Json::Value fd;
            fd["skillId"]      = "pr_reveal";
            fd["byOpenId"]     = lastPlayerOpenId;
            fd["targetOpenId"] = openId;
            fd["revealCardId"] = revealId;
            fd["desc"]         = "情绪价值：透视你1张手牌";
            fxMsg["data"]      = fd;
            _sendTo(*lastPlayer, fxMsg);
        }
    }

    // ── 被动技能：画饼老板（对手强制加班，要不起者抽废牌1张）──
    if (lastPlayerRole == "boss" && !room.wastePile.empty()) {
        std::uniform_int_distribution<int> d(0, (int)room.wastePile.size()-1);
        int idx = d(rng());
        PokerCard penalty = room.wastePile[idx];
        room.wastePile.erase(room.wastePile.begin() + idx);
        me->hand.push_back(penalty);
        PokerRoom::PassiveLogEntry log;
        log.type   = "boss_penalty";
        log.target = openId;
        log.card   = penalty.rankSymbol + penalty.suit;
        room.passiveLog.push_back(log);
        // 特效
        Json::Value fxMsg;
        fxMsg["type"] = "skill_fx";
        Json::Value fd;
        fd["skillId"]      = "boss_penalty";
        fd["byOpenId"]     = lastPlayerOpenId;
        fd["targetOpenId"] = openId;
        fd["desc"]         = "强制加班：你抽了1张惩罚牌";
        fxMsg["data"]      = fd;
        _broadcast(room, fxMsg);
    }

    // ── 被动技能：妩媚主管（要不起后，主管销毁自己最小2张牌）──
    if (lastPlayerRole == "manager" && lastPlayer && !lastPlayer->hand.empty()) {
        auto& lHand = lastPlayer->hand;
        std::sort(lHand.begin(), lHand.end(), [](const PokerCard& a, const PokerCard& b){ return a.val < b.val; });
        int destroyCount = std::min(2, (int)lHand.size());
        for (int i = 0; i < destroyCount; ++i) room.wastePile.push_back(lHand[i]);
        lHand.erase(lHand.begin(), lHand.begin() + destroyCount);

        // 特效
        Json::Value fxMsg;
        fxMsg["type"] = "skill_fx";
        Json::Value fd;
        fd["skillId"]      = "manager_discard";
        fd["byOpenId"]     = lastPlayerOpenId;
        fd["targetOpenId"] = lastPlayerOpenId;  // 主管自己
        fd["desc"]         = "抢功：主管销毁最小2张牌";
        fxMsg["data"]      = fd;
        _broadcast(room, fxMsg);

        // 检查主管是否因此手牌清零
        if (lHand.empty()) {
            room.status = PokerRoomStatus::FINISHED;
            room.winner = lastPlayerOpenId;
            Json::Value overMsg;
            overMsg["type"] = "game_over";
            Json::Value od;
            od["winnerOpenId"] = lastPlayerOpenId;
            od["reason"]       = "抢功技能手牌清零";
            overMsg["data"]    = od;
            _broadcast(room, overMsg);
            return;
        }
    }

    // ── 被动技能：天才黑客碎片整理（被迫要不起时，最小牌 hackerBoostVal +1）──
    if (me->roleId == "hacker" && !me->hand.empty()) {
        auto& hand = me->hand;
        int  minIdx  = 0;
        for (int i = 1; i < (int)hand.size(); ++i)
            if (hand[i].val < hand[minIdx].val) minIdx = i;
        // 使用 isJoker 字段避免王牌升值（王牌已是最大）
        if (!hand[minIdx].isJoker && hand[minIdx].val < 14) {
            // hackerBoostVal 存储为 JSON 扩展字段，这里在 PokerCard 中暂没对应字段
            // 实际发送给客户端时，在 toJson() 中追加 hackerBoostVal
            // 简化：直接修改 val（服务端权威），rankSymbol 保留原始
            hand[minIdx].val++;  // 升值1点（视觉上由客户端显示原牌面+角标）
        }
    }

    // 清桌面，HR 沉默处理
    room.lastPlay.clear();
    room.lastPlayBy.clear();
    room.lastPlayTypeName.clear();
    room.turn++;

    if (room.hrSilenceActive && !room.hrSilenceFor.empty()) {
        room.currentTurn     = room.hrSilenceFor;
        room.hrSilenceActive = false;
        room.hrSilenceFor.clear();
    } else {
        room.currentTurn = opp->openId;
    }
    room.turnStartTime = nowMs();

    _broadcastGameState(room);
}

void PokerRoomManager::onUseSkill(const std::string& roomCode, const std::string& openId,
                                   const std::string& skillType, const Json::Value& extra)
{
    std::lock_guard<std::mutex> lk(_mu);
    auto it = _rooms.find(roomCode);
    if (it == _rooms.end()) return;

    PokerRoom& room = it->second;
    if (room.status != PokerRoomStatus::PLAYING) return;

    auto* me = _findPlayer(room, openId);
    if (!me) return;
    PokerConnPlayer* opp = nullptr;
    for (auto& p : room.players) if (p.openId != openId) { opp = &p; break; }
    if (!opp) return;

    // 需要我方回合的技能
    static const std::vector<std::string> MY_TURN_SKILLS = {
        "intern_swipe","manager_swap","boss_downgrade",
        "hacker_trojan","pr_deal","ceo_verdict"
    };
    bool needsMyTurn = std::find(MY_TURN_SKILLS.begin(), MY_TURN_SKILLS.end(), skillType)
                       != MY_TURN_SKILLS.end();
    if (needsMyTurn && room.currentTurn != openId) {
        Json::Value errMsg;
        errMsg["type"] = "error";
        Json::Value d;
        d["code"] = "not_your_turn";
        d["msg"]  = "该技能只能在自己回合使用";
        errMsg["data"] = d;
        _sendTo(*me, errMsg);
        return;
    }

    auto sendFx = [&](const std::string& sid, const std::string& by,
                      const std::string& tgt, const std::string& desc) {
        Json::Value fxMsg;
        fxMsg["type"] = "skill_fx";
        Json::Value fd;
        fd["skillId"]      = sid;
        fd["byOpenId"]     = by;
        fd["targetOpenId"] = tgt;
        fd["desc"]         = desc;
        fxMsg["data"]      = fd;
        _broadcast(room, fxMsg);
    };

    if (skillType == "intern_swipe") {
        // 实习生【摸鱼甩锅】：把最小牌塞给对手
        if (!me->hand.empty()) {
            auto& hand = me->hand;
            int  minIdx = 0;
            for (int i = 1; i < (int)hand.size(); ++i)
                if (hand[i].val < hand[minIdx].val) minIdx = i;
            opp->hand.push_back(hand[minIdx]);
            hand.erase(hand.begin() + minIdx);
            sendFx("intern_swipe", openId, opp->openId, "摸鱼甩锅：把最小牌甩给你");
            me->skillUsed++;
        }
    }
    else if (skillType == "boss_downgrade") {
        // 画饼老板【毕业输送】：对手最大牌降为 val=3
        if (!opp->hand.empty()) {
            int maxIdx = 0;
            for (int i = 1; i < (int)opp->hand.size(); ++i)
                if (opp->hand[i].val > opp->hand[maxIdx].val) maxIdx = i;
            opp->hand[maxIdx].val  = 3;
            // isDowngraded 由 JSON 扩展字段表示，此处 PokerCard 没有对应字段，
            // 通过 toJson() 已有结构能告知客户端（客户端检测 val=3 但 rankSymbol 不是 "3"）
            sendFx("boss_downgrade", openId, opp->openId, "毕业输送：你最大牌被降级");
            me->skillUsed++;
        }
    }
    else if (skillType == "manager_swap") {
        // 妩媚主管【权谋博弈】：盲抽对手1张，把自己最小2张塞给对手
        if (!opp->hand.empty()) {
            std::uniform_int_distribution<int> d(0, (int)opp->hand.size()-1);
            int idx = d(rng());
            PokerCard stolen = opp->hand[idx];
            opp->hand.erase(opp->hand.begin() + idx);

            // 合并+排序
            me->hand.push_back(stolen);
            std::sort(me->hand.begin(), me->hand.end(), [](const PokerCard& a, const PokerCard& b){ return a.val < b.val; });
            int give = std::min(2, (int)me->hand.size());
            for (int i = 0; i < give; ++i) opp->hand.push_back(me->hand[i]);
            me->hand.erase(me->hand.begin(), me->hand.begin() + give);

            sendFx("manager_swap", openId, opp->openId, "权谋博弈：盲抽对手1张，甩出最小2张");
            me->skillUsed++;
        }
    }
    else if (skillType == "cleaner_sweep") {
        // 环卫阿姨【打扫战场】：对手强制丢最大牌，阿姨获得出牌权
        if (!opp->hand.empty()) {
            int maxIdx = 0;
            for (int i = 1; i < (int)opp->hand.size(); ++i)
                if (opp->hand[i].val > opp->hand[maxIdx].val) maxIdx = i;
            room.wastePile.push_back(opp->hand[maxIdx]);
            opp->hand.erase(opp->hand.begin() + maxIdx);
        }
        room.lastPlay.clear();
        room.lastPlayBy.clear();
        room.lastPlayTypeName.clear();
        room.currentTurn = openId;
        sendFx("cleaner_sweep", openId, opp->openId, "打扫战场：你最大牌被清除，阿姨获得出牌权");
        me->skillUsed++;
    }
    else if (skillType == "cfo_reorganize") {
        // 精明财务【资产重组】：对手手牌全洗回废牌堆，再随机抽等量
        if (!opp->hand.empty()) {
            int count = (int)opp->hand.size();
            for (auto& c : opp->hand) room.wastePile.push_back(c);
            opp->hand.clear();
            shuffleVec(room.wastePile);
            int draw = std::min(count, (int)room.wastePile.size());
            for (int i = 0; i < draw; ++i) opp->hand.push_back(room.wastePile[i]);
            room.wastePile.erase(room.wastePile.begin(), room.wastePile.begin() + draw);
            sendFx("cfo_reorganize", openId, opp->openId, "资产重组：你的手牌被洗牌重抽");
            me->skillUsed++;
        }
    }
    else if (skillType == "hacker_trojan") {
        // 天才黑客【木马植入】：对手某张牌被植入木马
        if (!opp->hand.empty()) {
            std::uniform_int_distribution<int> d(0, (int)opp->hand.size()-1);
            int idx = d(rng());
            room.hackerTrojan.active       = true;
            room.hackerTrojan.plantedBy    = openId;
            room.hackerTrojan.targetOpenId = opp->openId;
            room.hackerTrojan.cardId       = opp->hand[idx].id;
            room.hackerTrojan.cardName     = opp->hand[idx].rankSymbol + opp->hand[idx].suit;
            sendFx("hacker_trojan", openId, opp->openId, "木马植入：你的某张牌被感染");
            me->skillUsed++;
        }
    }
    else if (skillType == "pr_deal") {
        // 销冠公关【敬酒应酬】：送出giveCardId，换对手takeCardId（takeCardId为空则盲抽）
        std::string giveId = extra.isMember("giveCardId") ? extra["giveCardId"].asString() : "";
        std::string takeId = extra.isMember("takeCardId") ? extra["takeCardId"].asString() : "";

        PokerCard giveCard, takeCard;
        bool foundGive = false, foundTake = false;
        int giveIdx = -1, takeIdx = -1;

        for (int i = 0; i < (int)me->hand.size(); ++i) {
            if (me->hand[i].id == giveId) { giveCard = me->hand[i]; giveIdx = i; foundGive = true; break; }
        }
        if (!foundGive || opp->hand.empty()) goto pr_fail;

        if (!takeId.empty()) {
            for (int i = 0; i < (int)opp->hand.size(); ++i) {
                if (opp->hand[i].id == takeId) { takeCard = opp->hand[i]; takeIdx = i; foundTake = true; break; }
            }
        }
        if (!foundTake) {
            // 盲抽
            std::uniform_int_distribution<int> d(0, (int)opp->hand.size()-1);
            takeIdx = d(rng());
            takeCard = opp->hand[takeIdx];
        }
        // 执行交换
        me->hand.erase(me->hand.begin() + giveIdx);
        opp->hand.erase(opp->hand.begin() + takeIdx);
        me->hand.push_back(takeCard);
        opp->hand.push_back(giveCard);

        sendFx("pr_deal", openId, opp->openId, "敬酒应酬：换牌成功");
        me->skillUsed++;
        goto pr_end;
        pr_fail: ;
        pr_end: ;
    }
    else if (skillType == "ceo_verdict") {
        // CEO【神来之笔】：对手手牌全部销毁（弃牌），CEO直接获胜
        room.wastePile.insert(room.wastePile.end(), opp->hand.begin(), opp->hand.end());
        opp->hand.clear();
        sendFx("ceo_verdict", openId, opp->openId, "神来之笔：裁决执行，你的手牌全被销毁");
        me->skillUsed++;

        room.status = PokerRoomStatus::FINISHED;
        room.winner = openId;
        Json::Value overMsg;
        overMsg["type"] = "game_over";
        Json::Value od;
        od["winnerOpenId"] = openId;
        od["reason"]       = "CEO神来之笔";
        overMsg["data"]    = od;
        _broadcast(room, overMsg);
        return;
    }

    _broadcastGameState(room);
}

// ══════════════════════════════════════════════════════════════
//  过期房间清理（创建超过 2 小时的房间）
// ══════════════════════════════════════════════════════════════

void PokerRoomManager::_cleanupOldRooms() {
    // 调用时已持有锁
    int64_t now = nowMs();
    const int64_t EXPIRE_MS = 2LL * 3600 * 1000;
    for (auto it = _rooms.begin(); it != _rooms.end(); ) {
        if (now - it->second.createdAt > EXPIRE_MS) {
            it = _rooms.erase(it);
        } else {
            ++it;
        }
    }
}
