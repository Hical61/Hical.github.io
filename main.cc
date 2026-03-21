// drogon-game-server/main.cc
// 极简 Drogon 服务器（先验证云托管平台是否正常，再加游戏逻辑）

#include <drogon/drogon.h>
#include <iostream>

int main()
{
    std::cout << "[drogon-game] Starting server on 0.0.0.0:8080" << std::endl;

    // ── 健康检查路由（验证服务器正常运行）──
    drogon::app().registerHandler(
        "/health",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(R"({"status":"ok","service":"drogon-game"})");
            callback(resp);
        },
        {drogon::Get}
    );

    // ── 根路由 ──
    drogon::app().registerHandler(
        "/",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback)
        {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setBody("Drogon Game Server is running!");
            callback(resp);
        },
        {drogon::Get}
    );

    std::cout << "[drogon-game] Routes registered, starting listener..." << std::endl;

    drogon::app()
        .addListener("0.0.0.0", 8080)
        .setThreadNum(2)
        .setLogLevel(trantor::Logger::kDebug)
        // 日志输出到 stderr（云托管可捕获）
        .setLogPath("")
        .run();

    return 0;
}
