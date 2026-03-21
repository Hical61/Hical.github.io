// drogon-game-server/main.cc
// 游戏服务器入口：职场跑得快 1v1 PVP + 多人红A 4人联机

#include "poker/PokerHttpController.h"
#include "poker/PokerRoomManager.h"
#include "poker/PokerWsController.h"
#include "reda/RedaHttpController.h"
#include "reda/RedaRoomManager.h"
#include "reda/RedaWsController.h"
#include <drogon/drogon.h>


int main() {
  // ── 初始化单例管理器 ──
  PokerRoomManager::instance();
  RedaRoomManager::instance();

  // ── 加载配置并启动 Drogon ──
  drogon::app().loadConfigFile("config.json").run();

  return 0;
}
