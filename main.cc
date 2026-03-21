// drogon-game-server/main.cc
// 游戏服务器入口：职场跑得快 1v1 PVP + 多人红A 4人联机
// 使用代码配置（不依赖 config.json），日志输出到 stderr（云托管可捕获）

#include <drogon/drogon.h>
#include <iostream>

int main()
{
    std::cout << "[drogon-game] Starting server on 0.0.0.0:8080" << std::endl;

    // ── 健康检查路由（云托管/负载均衡存活探测）──
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

    std::cout << "[drogon-game] Starting listener..." << std::endl;

    // HttpController 和 WebSocketController 通过静态初始化自动注册路由
    // 无需手动注册，只要对应 .cc 文件被链接进来即可（CMakeLists.txt 的 GLOB_RECURSE 保证）

    drogon::app()
        .addListener("0.0.0.0", 8080)
        .setThreadNum(4)
        .setLogLevel(trantor::Logger::kInfo)
        .setLogPath("")   // 输出到 stderr，方便云托管日志查看
        .run();

    return 0;
}
