// drogon-game-server/main.cc
// 游戏服务器入口
// - HTTP 路由：lambda 直接注册（最可靠）
// - WebSocket 路由：显式调用 registerWebSocketController 注册

#include <drogon/drogon.h>
#include <json/json.h>
#include <iostream>

// 包含 WS 控制器头文件，触发 WebSocketController<T> 的静态初始化
// （注册控制器工厂，使 drogon 能创建控制器实例）
#include "poker/PokerWsController.h"
#include "reda/RedaWsController.h"

// 包含 RoomManager（HTTP 路由处理器需要）
#include "poker/PokerRoomManager.h"
#include "reda/RedaRoomManager.h"

// ── JSON 响应工具 ──
static drogon::HttpResponsePtr jsonResp(const Json::Value& j, drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(code);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    return resp;
}

static bool parseBody(const drogon::HttpRequestPtr& req, Json::Value& out) {
    auto body = req->getJsonObject();
    if (!body) return false;
    out = *body;
    return true;
}

int main()
{
    std::cout << "[drogon-game] Starting server on 0.0.0.0:8080" << std::endl;

    // ══════════════════════════════════════════════════════════════════
    //  WebSocket 路由（显式注册，绕过静态初始化的链接器优化问题）
    // ══════════════════════════════════════════════════════════════════

    // 注意：registerWebSocketController 第二参数是控制器类名（字符串）
    // 类名必须与 PokerWsController / RedaWsController 完全匹配
    drogon::app().registerWebSocketController("/ws/poker", "PokerWsController");
    drogon::app().registerWebSocketController("/ws/reda",  "RedaWsController");

    std::cout << "[drogon-game] WebSocket routes registered" << std::endl;

    // ══════════════════════════════════════════════════════════════════
    //  健康检查
    // ══════════════════════════════════════════════════════════════════

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
    //  Poker 1v1 HTTP 路由（lambda 方式，最可靠）
    // ══════════════════════════════════════════════════════════════════

    drogon::app().registerHandler("/api/poker/room/create",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().createRoom(
                body["openId"].asString(), body["playerName"].asString(),
                body["playerAvatar"].asString(), body["roleId"].asString());
            Json::Value j;
            j["success"]  = res.ok;
            j["roomCode"] = res.roomCode;
            j["token"]    = res.token;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    drogon::app().registerHandler("/api/poker/room/join",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().joinRoom(
                body["roomCode"].asString(), body["openId"].asString(),
                body["playerName"].asString(), body["playerAvatar"].asString(),
                body["roleId"].asString());
            Json::Value j;
            j["success"]  = res.ok;
            j["token"]    = res.token;
            j["roomCode"] = body["roomCode"];
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    drogon::app().registerHandler("/api/poker/room/leave",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().leaveRoom(
                body["roomCode"].asString(), body["openId"].asString(),
                body["token"].asString());
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    drogon::app().registerHandler("/api/poker/room/start",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = PokerRoomManager::instance().startGame(
                body["roomCode"].asString(), body["openId"].asString(),
                body["token"].asString());
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

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

    drogon::app().registerHandler("/api/reda/room/create",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().createRoom(
                body["openId"].asString(), body["playerName"].asString());
            Json::Value j;
            j["success"] = res.ok;
            j["roomId"]  = res.roomId;
            j["token"]   = res.token;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    drogon::app().registerHandler("/api/reda/room/join",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().joinRoom(
                body["roomId"].asString(), body["openId"].asString(),
                body["playerName"].asString());
            Json::Value j;
            j["success"] = res.ok;
            j["token"]   = res.token;
            j["roomId"]  = body["roomId"];
            j["players"] = res.players;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    drogon::app().registerHandler("/api/reda/room/leave",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().leaveRoom(
                body["roomId"].asString(), body["openId"].asString(),
                body["token"].asString());
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

    drogon::app().registerHandler("/api/reda/room/start",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value body; parseBody(req, body);
            auto res = RedaRoomManager::instance().startGame(
                body["roomId"].asString(), body["openId"].asString(),
                body["token"].asString());
            Json::Value j;
            j["success"] = res.ok;
            if (!res.ok) j["errMsg"] = res.errMsg;
            cb(jsonResp(j));
        }, {drogon::Post});

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

    std::cout << "[drogon-game] All HTTP routes registered, starting listener..." << std::endl;

    drogon::app()
        .addListener("0.0.0.0", 8080)
        .setThreadNum(4)
        .setLogLevel(trantor::Logger::kInfo)
        .setLogPath("")
        .run();

    return 0;
}
