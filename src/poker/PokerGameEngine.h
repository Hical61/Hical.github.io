// src/poker/PokerGameEngine.h
// 职场跑得快游戏逻辑引擎（移植自 cloudfunctions/pokerRoom/index.js）
// 职责：建牌、发牌、角色分配、出牌验证、技能判定、胜负检测

#pragma once
#include "common/types.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <json/json.h>

// ── 出牌类型枚举 ──
enum class PlayType {
    INVALID = 0,
    SINGLE  = 1,   // 单张
    PAIR    = 2,   // 对子
    TRIPLE  = 3,   // 三张
    STRAIGHT= 4,   // 顺子（5张以上连牌）
    CHAIN_PAIR=5,  // 连对
    TRIPLE_1= 6,   // 三带一
    TRIPLE_2= 7,   // 三带二
    BOMB    = 8,   // 炸弹
    JOKER_BOMB=9,  // 王炸
};

// ── 技能特效通知 ──
struct SkillFxEvent {
    std::string skillId;
    std::string byOpenId;
    std::string targetOpenId;
    std::string desc;
};

// ── 出牌结果 ──
struct PlayResult {
    bool        ok      = false;
    std::string errMsg;
    PlayType    type    = PlayType::INVALID;
    std::vector<SkillFxEvent> skillFxList;  // 技能触发特效列表
    // 是否触发游戏结束
    bool        gameOver    = false;
    std::string winnerOpenId;
    std::string winReason;
    // 是否是炸弹/王炸（用于客户端特效）
    bool        isBomb      = false;
    bool        isJokerBomb = false;
};

class PokerGameEngine {
public:
    // ── 建牌（54张，含大小王）──
    static std::vector<PokerCard> buildDeck();

    // ── 发牌 + 角色分配（服务端调用一次）──
    struct DealResult {
        std::vector<PokerCard>  hand0;
        std::vector<PokerCard>  hand1;
        std::vector<PokerCard>  waste;
        std::string             role0;
        std::string             role1;
        std::string             firstOpenId;  // 先手（随机）
        // pr 透视（公关角色开局透视对手2张）
        std::unordered_map<std::string,std::vector<std::string>> prVisible;
    };
    static DealResult deal(const std::string& openId0, const std::string& openId1);

    // ── 出牌验证 ──
    // hands       : 当前玩家手牌（按引用修改）
    // playCards   : 本次出的牌
    // lastPlay    : 上一手牌（为空表示新回合首出）
    // lastPlayType: 上一手牌型
    // roleId      : 出牌者角色
    // opponentRoleId : 对手角色
    // openId      : 出牌者
    // opponentOpenId : 对手
    // skillUsed   : 此局中该玩家已用技能次数（可更新）
    static PlayResult applyPlay(
        std::vector<PokerCard>&       playerHand,
        const std::vector<PokerCard>& playCards,
        const std::vector<PokerCard>& lastPlay,
        PlayType                      lastPlayType,
        const std::string&            roleId,
        const std::string&            opponentRoleId,
        const std::string&            openId,
        const std::string&            opponentOpenId,
        int&                          skillUsed,
        int&                          opponentSkillUsed
    );

    // ── pass 动作（验证 + 技能处理）──
    struct PassResult {
        bool ok = true;
        std::string errMsg;
        std::vector<SkillFxEvent> skillFxList;
    };
    static PassResult applyPass(
        const std::string& roleId,
        const std::string& opponentRoleId,
        const std::string& openId,
        int&               skillUsed
    );

    // ── 判断当前玩家手牌中是否有合法牌可出 ──
    static bool hasValidPlay(
        const std::vector<PokerCard>& hand,
        const std::vector<PokerCard>& lastPlay,
        PlayType                      lastPlayType,
        const std::string&            roleId
    );

    // ── 判断牌型 ──
    static PlayType detectPlayType(const std::vector<PokerCard>& cards);

    // ── 比较牌力（同类型返回 >0 则出牌方更大）──
    static int comparePlay(
        const std::vector<PokerCard>& play,
        PlayType                      pType,
        const std::vector<PokerCard>& last,
        PlayType                      lType
    );

private:
    // ── 角色分配（1% CEO 隐藏角色）──
    static std::string pickRandomRole();
    static std::pair<std::string,std::string> assignRoles();
};
