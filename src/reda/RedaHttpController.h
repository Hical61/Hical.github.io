// src/reda/RedaHttpController.h
// 多人红A HTTP 控制器
// 路由：/api/reda/room/*

#pragma once
#include <drogon/HttpController.h>

class RedaHttpController
    : public drogon::HttpController<RedaHttpController>
{
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(RedaHttpController::createRoom, "/api/reda/room/create", drogon::Post);
        ADD_METHOD_TO(RedaHttpController::joinRoom,   "/api/reda/room/join",   drogon::Post);
        ADD_METHOD_TO(RedaHttpController::leaveRoom,  "/api/reda/room/leave",  drogon::Post);
        ADD_METHOD_TO(RedaHttpController::startGame,  "/api/reda/room/start",  drogon::Post);
        ADD_METHOD_TO(RedaHttpController::getRoom,    "/api/reda/room/{roomId}", drogon::Get);
    METHOD_LIST_END

    void createRoom(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void joinRoom(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void leaveRoom(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void startGame(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& cb);

    void getRoom(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                 const std::string& roomId);
};
