// src/reda/RedaHttpController.cc
#include "RedaHttpController.h"
#include "RedaRoomManager.h"
#include <drogon/drogon.h>

static drogon::HttpResponsePtr jsonResp(const Json::Value& j, drogon::HttpStatusCode code = drogon::k200OK) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
    resp->setStatusCode(code);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    return resp;
}

void RedaHttpController::createRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value e; e["success"]=false; e["errMsg"]="请求体必须是JSON";
        cb(jsonResp(e, drogon::k400BadRequest)); return;
    }
    std::string openId = (*body)["openId"].asString();
    std::string name   = (*body)["playerName"].asString();
    if (openId.empty()) {
        Json::Value e; e["success"]=false; e["errMsg"]="openId不能为空";
        cb(jsonResp(e, drogon::k400BadRequest)); return;
    }
    auto res = RedaRoomManager::instance().createRoom(openId, name);
    Json::Value r;
    r["success"] = res.ok;
    r["roomId"]  = res.roomId;
    r["token"]   = res.token;
    if (!res.ok) r["errMsg"] = res.errMsg;
    cb(jsonResp(r));
}

void RedaHttpController::joinRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value e; e["success"]=false; e["errMsg"]="请求体必须是JSON";
        cb(jsonResp(e, drogon::k400BadRequest)); return;
    }
    std::string roomId = (*body)["roomId"].asString();
    std::string openId = (*body)["openId"].asString();
    std::string name   = (*body)["playerName"].asString();
    if (roomId.empty() || openId.empty()) {
        Json::Value e; e["success"]=false; e["errMsg"]="roomId/openId不能为空";
        cb(jsonResp(e, drogon::k400BadRequest)); return;
    }
    auto res = RedaRoomManager::instance().joinRoom(roomId, openId, name);
    Json::Value r;
    r["success"] = res.ok;
    r["token"]   = res.token;
    r["roomId"]  = roomId;
    r["players"] = res.players;
    if (!res.ok) r["errMsg"] = res.errMsg;
    cb(jsonResp(r));
}

void RedaHttpController::leaveRoom(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value e; e["success"]=false; e["errMsg"]="请求体必须是JSON";
        cb(jsonResp(e, drogon::k400BadRequest)); return;
    }
    std::string roomId = (*body)["roomId"].asString();
    std::string openId = (*body)["openId"].asString();
    std::string token  = (*body)["token"].asString();
    auto res = RedaRoomManager::instance().leaveRoom(roomId, openId, token);
    Json::Value r;
    r["success"] = res.ok;
    if (!res.ok) r["errMsg"] = res.errMsg;
    cb(jsonResp(r));
}

void RedaHttpController::startGame(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    auto body = req->getJsonObject();
    if (!body) {
        Json::Value e; e["success"]=false; e["errMsg"]="请求体必须是JSON";
        cb(jsonResp(e, drogon::k400BadRequest)); return;
    }
    std::string roomId = (*body)["roomId"].asString();
    std::string openId = (*body)["openId"].asString();
    std::string token  = (*body)["token"].asString();
    auto res = RedaRoomManager::instance().startGame(roomId, openId, token);
    Json::Value r;
    r["success"] = res.ok;
    if (!res.ok) r["errMsg"] = res.errMsg;
    cb(jsonResp(r));
}

void RedaHttpController::getRoom(
    const drogon::HttpRequestPtr& /*req*/,
    std::function<void(const drogon::HttpResponsePtr&)>&& cb,
    const std::string& roomId)
{
    auto room = RedaRoomManager::instance().getRoomJson(roomId);
    if (room.isNull()) {
        Json::Value e; e["success"]=false; e["errMsg"]="房间不存在";
        cb(jsonResp(e, drogon::k404NotFound)); return;
    }
    Json::Value r;
    r["success"] = true;
    r["room"]    = room;
    cb(jsonResp(r));
}
