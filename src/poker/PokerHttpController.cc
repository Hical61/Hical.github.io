// src/poker/PokerHttpController.cc

#include "PokerHttpController.h"
#include "PokerRoomManager.h"
#include <drogon/drogon.h>

// ── 工具：构建 JSON 响应 ──
static drogon::HttpResponsePtr jsonResp(const Json::Value& j, drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(code);
    // CORS（微信小程序不需要，但开发调试时方便）
    resp->addHeader("Access-Control-Allow-Origin", "*");
    return resp;
}

// ── POST /api/poker/room/create ──
void PokerHttpController::createRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err;
        err["success"] = false;
        err["errMsg"]  = "请求体必须是 JSON";
        cb(jsonResp(err, drogon::k400BadRequest));
        return;
    }

    std::string openId    = (*body)["openId"].asString();
    std::string name      = (*body)["playerName"].asString();
    std::string avatar    = (*body)["playerAvatar"].asString();
    std::string roleId    = (*body)["roleId"].asString();

    if (openId.empty()) {
        Json::Value err; err["success"] = false; err["errMsg"] = "openId 不能为空";
        cb(jsonResp(err, drogon::k400BadRequest)); return;
    }

    auto res = PokerRoomManager::instance().createRoom(openId, name, avatar, roleId);
    Json::Value resp;
    resp["success"]  = res.ok;
    resp["roomCode"] = res.roomCode;
    resp["token"]    = res.token;
    if (!res.ok) resp["errMsg"] = res.errMsg;
    cb(jsonResp(resp));
}

// ── POST /api/poker/room/join ──
void PokerHttpController::joinRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err; err["success"] = false; err["errMsg"] = "请求体必须是 JSON";
        cb(jsonResp(err, drogon::k400BadRequest)); return;
    }

    std::string roomCode = (*body)["roomCode"].asString();
    std::string openId   = (*body)["openId"].asString();
    std::string name     = (*body)["playerName"].asString();
    std::string avatar   = (*body)["playerAvatar"].asString();
    std::string roleId   = (*body)["roleId"].asString();

    if (roomCode.empty() || openId.empty()) {
        Json::Value err; err["success"] = false; err["errMsg"] = "roomCode/openId 不能为空";
        cb(jsonResp(err, drogon::k400BadRequest)); return;
    }

    auto res = PokerRoomManager::instance().joinRoom(roomCode, openId, name, avatar, roleId);
    Json::Value resp;
    resp["success"]  = res.ok;
    resp["token"]    = res.token;
    resp["roomCode"] = roomCode;
    if (!res.ok) {
        resp["errMsg"] = res.errMsg;
    } else {
        // BUG FIX: 将加入后的完整 players 列表写入 HTTP 响应，
        // 客户端收到后可立即渲染双侧玩家槽，无需等待 WS room_update 的异步推送
        resp["players"] = res.players;   // [房主, 新加入者]
    }
    cb(jsonResp(resp));
}

// ── POST /api/poker/room/leave ──
void PokerHttpController::leaveRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err; err["success"] = false; err["errMsg"] = "请求体必须是 JSON";
        cb(jsonResp(err, drogon::k400BadRequest)); return;
    }
    std::string roomCode = (*body)["roomCode"].asString();
    std::string openId   = (*body)["openId"].asString();
    std::string token    = (*body)["token"].asString();

    auto res = PokerRoomManager::instance().leaveRoom(roomCode, openId, token);
    Json::Value resp;
    resp["success"] = res.ok;
    if (!res.ok) resp["errMsg"] = res.errMsg;
    cb(jsonResp(resp));
}

// ── POST /api/poker/room/start ──
void PokerHttpController::startGame(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value err; err["success"] = false; err["errMsg"] = "请求体必须是 JSON";
        cb(jsonResp(err, drogon::k400BadRequest)); return;
    }
    std::string roomCode = (*body)["roomCode"].asString();
    std::string openId   = (*body)["openId"].asString();
    std::string token    = (*body)["token"].asString();

    auto res = PokerRoomManager::instance().startGame(roomCode, openId, token);
    Json::Value resp;
    resp["success"] = res.ok;
    if (!res.ok) resp["errMsg"] = res.errMsg;
    cb(jsonResp(resp));
}

// ── GET /api/poker/room/{roomCode} ──
void PokerHttpController::getRoom(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const std::string& roomCode)
{
    auto room = PokerRoomManager::instance().getRoomJson(roomCode);
    if (room.isNull()) {
        Json::Value err; err["success"] = false; err["errMsg"] = "房间不存在";
        cb(jsonResp(err, drogon::k404NotFound)); return;
    }
    Json::Value resp;
    resp["success"] = true;
    resp["room"]    = room;
    cb(jsonResp(resp));
}
