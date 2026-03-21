// src/poker/PokerWsController.h
// 职场跑得快 WebSocket 控制器
// 路由：/ws/poker?roomCode=XXXX&openId=oXXX&token=YYYY

#pragma once
#include <drogon/WebSocketController.h>

class PokerWsController
    : public drogon::WebSocketController<PokerWsController>
{
public:
    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws/poker");
    WS_PATH_LIST_END

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&&                          message,
                          const drogon::WebSocketMessageType&    type) override;

    void handleNewConnection(const drogon::HttpRequestPtr&        req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;
};
