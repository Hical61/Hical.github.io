// src/reda/RedaWsController.h
// 多人红A WebSocket 控制器
// 路由：/ws/reda?roomId=XXXX&openId=oXXX&token=YYYY

#pragma once
#include <drogon/WebSocketController.h>

class RedaWsController
    : public drogon::WebSocketController<RedaWsController>
{
public:
    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws/reda");
    WS_PATH_LIST_END

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&&                          message,
                          const drogon::WebSocketMessageType&    type) override;

    void handleNewConnection(const drogon::HttpRequestPtr&        req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;
};
