// src/reda/RedaWsController.cc
#include "RedaWsController.h"
#include "RedaRoomManager.h"
#include <drogon/drogon.h>
#include <json/json.h>

struct RedaWsCtx {
    std::string roomId;
    std::string openId;
    std::string token;
};

void RedaWsController::handleNewConnection(
    const drogon::HttpRequestPtr&        req,
    const drogon::WebSocketConnectionPtr& conn)
{
    std::string roomId = req->getParameter("roomId");
    std::string openId = req->getParameter("openId");
    std::string token  = req->getParameter("token");

    if (roomId.empty() || openId.empty() || token.empty()) {
        conn->forceClose();
        return;
    }

    auto ctx    = std::make_shared<RedaWsCtx>();
    ctx->roomId = roomId;
    ctx->openId = openId;
    ctx->token  = token;
    conn->setContext(ctx);

    RedaRoomManager::instance().onWsOpen(roomId, openId, token, conn);
}

void RedaWsController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&&                         message,
    const drogon::WebSocketMessageType&   type)
{
    if (type != drogon::WebSocketMessageType::Text) return;

    auto ctx = conn->getContext<RedaWsCtx>();
    if (!ctx) return;

    Json::Value  root;
    Json::Reader reader;
    if (!reader.parse(message, root)) return;

    std::string msgType = root["type"].asString();
    Json::Value data    = root.isMember("data") ? root["data"] : Json::Value();

    auto& mgr = RedaRoomManager::instance();

    if (msgType == "play") {
        std::vector<RedaCard> cards;
        if (data.isMember("cards") && data["cards"].isArray()) {
            for (auto& cj : data["cards"]) {
                RedaCard c;
                c.suit        = cj["suit"].asInt();
                c.rank        = cj["rank"].asInt();
                c.rankValue   = cj["rankValue"].asInt();
                c.isRed       = cj["isRed"].asBool();
                c.isRedAce    = cj["isRedAce"].asBool();
                c.isHeartAce  = cj["isHeartAce"].asBool();
                c.isDiamondAce= cj["isDiamondAce"].asBool();
                c.isSpadeThree= cj["isSpadeThree"].asBool();
                c.isScoreCard = cj["isScoreCard"].asBool();
                c.rankSymbol  = cj["rankSymbol"].asString();
                c.suitSymbol  = cj["suitSymbol"].asString();
                cards.push_back(c);
            }
        }
        mgr.onPlay(ctx->roomId, ctx->openId, cards);
    }
    else if (msgType == "pass") {
        mgr.onPass(ctx->roomId, ctx->openId);
    }
    else if (msgType == "ping") {
        Json::Value pong;
        pong["type"] = "pong";
        pong["ts"]   = data["ts"];
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        conn->send(Json::writeString(wb, pong));
    }
}

void RedaWsController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<RedaWsCtx>();
    if (!ctx) return;
    RedaRoomManager::instance().onWsClose(ctx->roomId, ctx->openId);
}
