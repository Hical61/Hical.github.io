// drogon-game-server/main.cc
// 游戏服务器入口
// 所有路由直接在 main 中注册（避免控制器自动注册机制的版本差异问题）

#include <drogon/drogon.h>
#include <json/json.h>
#include <iostream>
#include "poker/PokerRoomManager.h"
#include "reda/RedaRoomManager.h"

// ── JSON 响应工具 ──
static drogon::HttpResponsePtr jsonResp(const Json::Value& j, drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(code);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    return resp;
}

// ── 解析请求体 JSON ──
static bool parseBody(const drogon::HttpRequestPtr& req, Json::Value& out) {
    auto body = req->getJsonObject();
    if (!body) return false;
    out = *body;
    return true;
}

int main()
{
    std::cout << "[drogon-game] Starting server on 0.0.0.0:8080" << std::endl;

    // ── 健康检查 ──────────────────────────────────────────────────────
    drogon::app().registerHandler("/health",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value j;
            j["status"]  = "ok";
            j["service"] = "drogon-game";
            cb(jsonResp(j));
        }, {drogon::Get});

    drogon::app().registerHandler("/",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setBody("Drogon Game Server is running!");
            cb(resp);
        }, {drogon::Get});

    // ══════════════════════════════════════════════════════════════════
    //  Poker 1v1 HTTP 路由
    // ══════════════════════════════════════════════════════════════════

    // POST /api/poker/room/create
    drogon::app().registerHandler("/api/poker/room/create",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().createRoom(
                body["openId"].asString(),
                body["playerName"].asString(),
                body["playerAvatar"].asString(),
                body["roleId"].asString()
            );
            Json::Value j;
            j["success"]  = res.ok;
            j["roomCode"] = res.roomCode;
            j["token"]    = res.token;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // POST /api/poker/room/join
    drogon::app().registerHandler("/api/poker/room/join",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().joinRoom(
                body["roomCode"].asString(),
                body["openId"].asString(),
                body["playerName"].asString(),
                body["playerAvatar"].asString(),
                body["roleId"].asString()
            );
            Json::Value j;
            j["success"]  = res.ok;
            j["token"]    = res.token;
            j["roomCode"] = body["roomCode"];
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // POST /api/poker/room/leave
    drogon::app().registerHandler("/api/poker/room/leave",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().leaveRoom(
                body["roomCode"].asString(),
                body["openId"].asString(),
                body["token"].asString()
            );
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // POST /api/poker/room/start
    drogon::app().registerHandler("/api/poker/room/start",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().startGame(
                body["roomCode"].asString(),
                body["openId"].asString(),
                body["token"].asString()
            );
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // GET /api/poker/room/{roomCode}
    drogon::app().registerHandler("/api/poker/room/{1}",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb,
           const std::string& roomCode) {
            auto room = PokerRoomManager::instance().getRoomJson(roomCode);
            if (room.isNull()) {
                Json::Value j; j["success"] = false; j["errMsg"] = "房间不存在";
                cb(jsonResp(j, drogon::k404NotFound)); return;
            }
            Json::Value j; j["success"] = true; j["room"] = room;
            cb(jsonResp(j));
        }, {drogon::Get});

    // ══════════════════════════════════════════════════════════════════
    //  REDA 多人红A HTTP 路由
    // ══════════════════════════════════════════════════════════════════

    // POST /api/reda/room/create
    drogon::app().registerHandler("/api/reda/room/create",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().createRoom(
                body["openId"].asString(),
                body["playerName"].asString()
            );
            Json::Value j;
            j["success"] = res.ok;
            j["roomId"]  = res.roomId;
            j["token"]   = res.token;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // POST /api/reda/room/join
    drogon::app().registerHandler("/api/reda/room/join",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().joinRoom(
                body["roomId"].asString(),
                body["openId"].asString(),
                body["playerName"].asString()
            );
            Json::Value j;
            j["success"] = res.ok;
            j["token"]   = res.token;
            j["roomId"]  = body["roomId"];
            j["players"] = res.players;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // POST /api/reda/room/leave
    drogon::app().registerHandler("/api/reda/room/leave",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().leaveRoom(
                body["roomId"].asString(),
                body["openId"].asString(),
                body["token"].asString()
            );
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // POST /api/reda/room/start
    drogon::app().registerHandler("/api/reda/room/start",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().startGame(
                body["roomId"].asString(),
                body["openId"].asString(),
                body["token"].asString()
            );
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    // GET /api/reda/room/{roomId}
    drogon::app().registerHandler("/api/reda/room/{1}",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb,
           const std::string& roomId) {
            auto room = RedaRoomManager::instance().getRoomJson(roomId);
            if (room.isNull()) {
                Json::Value j; j["success"] = false; j["errMsg"] = "房间不存在";
                cb(jsonResp(j, drogon::k404NotFound)); return;
            }
            Json::Value j; j["success"] = true; j["room"] = room;
            cb(jsonResp(j));
        }, {drogon::Get});

    // ── WebSocket 控制器由 Drogon 自动注册（PokerWsController / RedaWsController）──
    std::cout << "[drogon-game] All HTTP routes registered, starting listener..." << std::endl;

    drogon::app()
        .addListener("0.0.0.0", 8080)
        .setThreadNum(4)
        .setLogLevel(trantor::Logger::kInfo)
        .setLogPath("")
        .run();

    return 0;
}
