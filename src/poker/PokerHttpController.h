// src/poker/PokerHttpController.h
// 职场跑得快 HTTP 控制器
// 路由：/api/poker/room/*

#pragma once
#include <drogon/HttpController.h>

class PokerHttpController
    : public drogon::HttpController<PokerHttpController>
{
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(PokerHttpController::createRoom, "/api/poker/room/create", drogon::Post);
        ADD_METHOD_TO(PokerHttpController::joinRoom,   "/api/poker/room/join",   drogon::Post);
        ADD_METHOD_TO(PokerHttpController::leaveRoom,  "/api/poker/room/leave",  drogon::Post);
        ADD_METHOD_TO(PokerHttpController::startGame,  "/api/poker/room/start",  drogon::Post);
        ADD_METHOD_TO(PokerHttpController::getRoom,    "/api/poker/room/{roomCode}", drogon::Get);
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
                 const std::string& roomCode);
};
