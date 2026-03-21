// src/poker/PokerWsController.cc
// WebSocket 消息路由：接收客户端 play/pass/use_skill/ping 消息

#include "PokerWsController.h"
#include "PokerRoomManager.h"
#include <drogon/drogon.h>
#include <json/json.h>

// 连接元数据（存储在 connection 的 context 中）
struct PokerWsCtx {
    std::string roomCode;
    std::string openId;
    std::string token;
};

void PokerWsController::handleNewConnection(
    const drogon::HttpRequestPtr&        req,
    const drogon::WebSocketConnectionPtr& conn)
{
    std::string roomCode = req->getParameter("roomCode");
    std::string openId   = req->getParameter("openId");
    std::string token    = req->getParameter("token");

    if (roomCode.empty() || openId.empty() || token.empty()) {
        conn->forceClose();
        return;
    }

    auto ctx = std::make_shared<PokerWsCtx>();
    ctx->roomCode = roomCode;
    ctx->openId   = openId;
    ctx->token    = token;
    conn->setContext(ctx);

    PokerRoomManager::instance().onWsOpen(roomCode, openId, token, conn);
}

void PokerWsController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&&                         message,
    const drogon::WebSocketMessageType&   type)
{
    if (type != drogon::WebSocketMessageType::Text) return;

    auto ctx = conn->getContext<PokerWsCtx>();
    if (!ctx) return;

    // 解析 JSON
    Json::Value  root;
    Json::Reader reader;
    if (!reader.parse(message, root)) return;

    std::string msgType = root["type"].asString();
    Json::Value data    = root.isMember("data") ? root["data"] : Json::Value();

    auto& mgr = PokerRoomManager::instance();

    if (msgType == "play") {
        // 出牌：data.cards + data.skillActivated(bool)
        bool isHrSkill = data.isMember("skillActivated") ? data["skillActivated"].asBool() : false;
        std::vector<PokerCard> cards;
        if (data.isMember("cards") && data["cards"].isArray()) {
            for (auto& cj : data["cards"]) {
                PokerCard c;
                c.val        = cj["val"].asInt();
                c.suit       = cj["suit"].asString();
                c.name       = cj["name"].asString();
                c.rankSymbol = cj["rankSymbol"].asString();
                c.suitSymbol = cj["suitSymbol"].asString();
                c.id         = cj["id"].asString();
                c.isRed      = cj["isRed"].asBool();
                c.isJoker    = cj["isJoker"].asBool();
                cards.push_back(c);
            }
        }
        mgr.onPlayCards(ctx->roomCode, ctx->openId, cards, isHrSkill);
    }
    else if (msgType == "pass") {
        mgr.onPass(ctx->roomCode, ctx->openId);
    }
    else if (msgType == "use_skill") {
        std::string skillType = data["skillId"].asString();
        mgr.onUseSkill(ctx->roomCode, ctx->openId, skillType, data);
    }
    else if (msgType == "ping") {
        // 回 pong
        Json::Value pong;
        pong["type"] = "pong";
        pong["ts"]   = data["ts"];
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        conn->send(Json::writeString(wb, pong));
    }
}

void PokerWsController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<PokerWsCtx>();
    if (!ctx) return;
    PokerRoomManager::instance().onWsClose(ctx->roomCode, ctx->openId);
}
