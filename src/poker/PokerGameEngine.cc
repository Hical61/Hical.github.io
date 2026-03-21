// src/poker/PokerGameEngine.cc
// 职场跑得快游戏引擎实现（完整移植自 cloudfunctions/pokerRoom/index.js）
// 所有角色技能均在服务端权威执行

#include "PokerGameEngine.h"
#include "common/utils.h"
#include <algorithm>
#include <set>
#include <map>
#include <cassert>

// ══════════════════════════════════════════════════════════════
//  建牌
// ══════════════════════════════════════════════════════════════

std::vector<PokerCard> PokerGameEngine::buildDeck() {
    static const std::vector<std::string> SUITS  = {"♠","♥","♦","♣"};
    static const std::set<std::string>    RED_S  = {"♥","♦"};
    static const std::map<int,std::string> NAMES = {
        {3,"3"},{4,"4"},{5,"5"},{6,"6"},{7,"7"},{8,"8"},{9,"9"},
        {10,"10"},{11,"J"},{12,"Q"},{13,"K"},{14,"A"},{15,"2"},
        {16,"小"},{17,"大"}
    };

    std::vector<PokerCard> deck;
    deck.reserve(54);

    for (auto& suit : SUITS) {
        for (int val = 3; val <= 15; ++val) {
            PokerCard c;
            c.val        = val;
            c.suit       = suit;
            c.name       = NAMES.at(val);
            c.rankSymbol = c.name;
            c.suitSymbol = suit;
            c.isRed      = RED_S.count(suit) > 0;
            c.isJoker    = false;
            c.id         = suit + std::to_string(val);
            deck.push_back(c);
        }
    }
    // 小王
    {
        PokerCard c;
        c.val=16; c.suit=""; c.name="🤡"; c.rankSymbol="小"; c.suitSymbol="🤡";
        c.isRed=false; c.isJoker=true; c.id="joker_s";
        deck.push_back(c);
    }
    // 大王
    {
        PokerCard c;
        c.val=17; c.suit=""; c.name="🤡"; c.rankSymbol="大"; c.suitSymbol="🤡";
        c.isRed=true; c.isJoker=true; c.id="joker_b";
        deck.push_back(c);
    }
    return deck;
}

// ══════════════════════════════════════════════════════════════
//  角色分配
// ══════════════════════════════════════════════════════════════

std::string PokerGameEngine::pickRandomRole() {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng());
    if (r < 0.01) return "ceo";
    if (r < 0.14) return "intern";
    if (r < 0.27) return "cleaner";
    if (r < 0.40) return "hr";
    if (r < 0.53) return "manager";
    if (r < 0.65) return "pr";
    if (r < 0.77) return "cfo";
    if (r < 0.89) return "hacker";
    if (r < 0.95) return "boss";
    return "intern";
}

std::pair<std::string,std::string> PokerGameEngine::assignRoles() {
    std::string r1 = pickRandomRole();
    std::string r2 = pickRandomRole();
    static const std::vector<std::string> ALL = {
        "intern","boss","hr","manager","cleaner","cfo","hacker","pr"
    };
    if (r2 == r1) {
        std::vector<std::string> others;
        for (auto& r : ALL) if (r != r1) others.push_back(r);
        std::uniform_int_distribution<int> d(0, (int)others.size()-1);
        r2 = others[d(rng())];
    }
    return {r1, r2};
}

// ══════════════════════════════════════════════════════════════
//  发牌
// ══════════════════════════════════════════════════════════════

PokerGameEngine::DealResult PokerGameEngine::deal(
    const std::string& openId0,
    const std::string& openId1)
{
    auto deck = buildDeck();
    shuffleVec(deck);

    DealResult res;
    res.hand0 = std::vector<PokerCard>(deck.begin(),      deck.begin() + 17);
    res.hand1 = std::vector<PokerCard>(deck.begin() + 17, deck.begin() + 34);
    res.waste = std::vector<PokerCard>(deck.begin() + 34, deck.end());

    auto [role0, role1] = assignRoles();
    res.role0 = role0;
    res.role1 = role1;

    // CEO 绝对威权：对手只拿 10 张
    if (role0 == "ceo") {
        for (int i = 10; i < (int)res.hand1.size(); ++i) res.waste.push_back(res.hand1[i]);
        res.hand1.resize(10);
    } else if (role1 == "ceo") {
        for (int i = 10; i < (int)res.hand0.size(); ++i) res.waste.push_back(res.hand0[i]);
        res.hand0.resize(10);
    }

    // 随机先手
    {
        std::uniform_int_distribution<int> coin(0,1);
        res.firstOpenId = (coin(rng()) == 0) ? openId0 : openId1;
    }

    // PR 公关：开局透视对手 2 张
    auto doprVisible = [&](const std::string& prOpenId, const std::vector<PokerCard>& targetHand) {
        if (targetHand.size() < 2) return;
        auto tmp = targetHand;
        shuffleVec(tmp);
        std::vector<std::string> ids = { tmp[0].id, tmp[1].id };
        res.prVisible[prOpenId] = ids;
    };
    if (role0 == "pr") doprVisible(openId0, res.hand1);
    if (role1 == "pr") doprVisible(openId1, res.hand0);

    return res;
}

// ══════════════════════════════════════════════════════════════
//  牌型判断
// ══════════════════════════════════════════════════════════════

PlayType PokerGameEngine::detectPlayType(const std::vector<PokerCard>& cards) {
    if (cards.empty()) return PlayType::INVALID;

    int n = (int)cards.size();

    // 王炸：大+小王
    if (n == 2) {
        bool hasS = false, hasB = false;
        for (auto& c : cards) {
            if (c.id == "joker_s") hasS = true;
            if (c.id == "joker_b") hasB = true;
        }
        if (hasS && hasB) return PlayType::JOKER_BOMB;
    }

    // 收集 val 频率
    std::map<int,int> freq;
    for (auto& c : cards) freq[c.val]++;

    // 炸弹：4张同值
    if (n == 4 && freq.size() == 1) return PlayType::BOMB;

    // 单张
    if (n == 1) return PlayType::SINGLE;

    // 对子
    if (n == 2 && freq.size() == 1) return PlayType::PAIR;

    // 三张
    if (n == 3 && freq.size() == 1) return PlayType::TRIPLE;

    // 三带一
    if (n == 4 && freq.size() == 2) {
        for (auto& [v, cnt] : freq) if (cnt == 3) return PlayType::TRIPLE_1;
    }

    // 三带二
    if (n == 5 && freq.size() == 2) {
        bool has3 = false, has2 = false;
        for (auto& [v, cnt] : freq) { if (cnt == 3) has3 = true; if (cnt == 2) has2 = true; }
        if (has3 && has2) return PlayType::TRIPLE_2;
    }

    // 顺子（5张以上，连续不含2、王）
    if (n >= 5) {
        std::vector<int> vals;
        for (auto& [v, cnt] : freq) {
            if (cnt != 1) goto not_straight;
            if (v >= 15) goto not_straight;  // 2 or Joker
            vals.push_back(v);
        }
        std::sort(vals.begin(), vals.end());
        for (int i = 1; i < (int)vals.size(); ++i)
            if (vals[i] != vals[i-1]+1) goto not_straight;
        return PlayType::STRAIGHT;
        not_straight:;
    }

    // 连对（4张以上，偶数，连续对子，不含2、王）
    if (n >= 4 && n % 2 == 0) {
        bool allPair = true;
        std::vector<int> vals;
        for (auto& [v, cnt] : freq) {
            if (cnt != 2 || v >= 15) { allPair = false; break; }
            vals.push_back(v);
        }
        if (allPair && (int)vals.size() >= 2) {
            std::sort(vals.begin(), vals.end());
            bool consecutive = true;
            for (int i = 1; i < (int)vals.size(); ++i)
                if (vals[i] != vals[i-1]+1) { consecutive = false; break; }
            if (consecutive) return PlayType::CHAIN_PAIR;
        }
    }

    return PlayType::INVALID;
}

// ══════════════════════════════════════════════════════════════
//  比较牌力
// ══════════════════════════════════════════════════════════════

int PokerGameEngine::comparePlay(
    const std::vector<PokerCard>& play,
    PlayType pType,
    const std::vector<PokerCard>& last,
    PlayType lType)
{
    // 王炸 > 一切
    if (pType == PlayType::JOKER_BOMB) return 1;
    if (lType == PlayType::JOKER_BOMB) return -1;

    // 炸弹 > 非炸弹
    if (pType == PlayType::BOMB && lType != PlayType::BOMB) return 1;
    if (lType == PlayType::BOMB && pType != PlayType::BOMB) return -1;

    // 两炸比大小
    if (pType == PlayType::BOMB && lType == PlayType::BOMB) {
        int pMax = 0, lMax = 0;
        for (auto& c : play) pMax = std::max(pMax, c.val);
        for (auto& c : last) lMax = std::max(lMax, c.val);
        return pMax - lMax;
    }

    // 同类型比较
    if (pType != lType) return -1;  // 类型不同不能压
    if (play.size() != last.size()) return -1;  // 张数不同（炸弹除外）

    auto maxVal = [](const std::vector<PokerCard>& v) {
        int m = 0;
        for (auto& c : v) m = std::max(m, c.val);
        return m;
    };

    // 顺子/连对：比最大牌
    if (pType == PlayType::STRAIGHT || pType == PlayType::CHAIN_PAIR) {
        return maxVal(play) - maxVal(last);
    }

    // 三带一/三带二：比三张的值
    if (pType == PlayType::TRIPLE_1 || pType == PlayType::TRIPLE_2) {
        auto tripleVal = [](const std::vector<PokerCard>& v) {
            std::map<int,int> freq;
            for (auto& c : v) freq[c.val]++;
            for (auto& [val,cnt] : freq) if (cnt == 3) return val;
            return 0;
        };
        return tripleVal(play) - tripleVal(last);
    }

    // 其余：比最大 val（单/对/三）
    return maxVal(play) - maxVal(last);
}

// ══════════════════════════════════════════════════════════════
//  是否有合法牌可出
// ══════════════════════════════════════════════════════════════

bool PokerGameEngine::hasValidPlay(
    const std::vector<PokerCard>& hand,
    const std::vector<PokerCard>& lastPlay,
    PlayType lastPlayType,
    const std::string& /*roleId*/)
{
    if (lastPlay.empty()) return true;  // 新回合，任何牌都可出

    int n = (int)hand.size();
    // 枚举所有子集（手牌 <= 17，最多 2^17=131072 种，可接受）
    for (int mask = 1; mask < (1 << n); ++mask) {
        std::vector<PokerCard> subset;
        for (int i = 0; i < n; ++i)
            if (mask & (1 << i)) subset.push_back(hand[i]);

        PlayType t = detectPlayType(subset);
        if (t == PlayType::INVALID) continue;
        if (comparePlay(subset, t, lastPlay, lastPlayType) > 0) return true;
    }
    return false;
}

// ══════════════════════════════════════════════════════════════
//  出牌（含角色被动技能判定）
// ══════════════════════════════════════════════════════════════

PlayResult PokerGameEngine::applyPlay(
    std::vector<PokerCard>&       playerHand,
    const std::vector<PokerCard>& playCards,
    const std::vector<PokerCard>& lastPlay,
    PlayType                      lastPlayType,
    const std::string&            roleId,
    const std::string&            opponentRoleId,
    const std::string&            openId,
    const std::string&            /*opponentOpenId*/,
    int&                          skillUsed,
    int&                          /*opponentSkillUsed*/)
{
    PlayResult res;

    if (playCards.empty()) {
        res.errMsg = "出牌为空";
        return res;
    }

    PlayType pType = detectPlayType(playCards);
    if (pType == PlayType::INVALID) {
        res.errMsg = "无效牌型";
        return res;
    }

    // 如果桌面有牌，需要能压过
    if (!lastPlay.empty()) {
        if (comparePlay(playCards, pType, lastPlay, lastPlayType) <= 0) {
            res.errMsg = "牌力不足";
            return res;
        }
    }

    // 从手牌移除已出牌
    std::set<std::string> playIds;
    for (auto& c : playCards) playIds.insert(c.id);
    std::vector<PokerCard> newHand;
    for (auto& c : playerHand) {
        if (!playIds.count(c.id)) newHand.push_back(c);
    }
    playerHand = newHand;

    res.ok   = true;
    res.type = pType;
    res.isBomb      = (pType == PlayType::BOMB);
    res.isJokerBomb = (pType == PlayType::JOKER_BOMB);

    // 胜负检测
    if (playerHand.empty()) {
        res.gameOver    = true;
        res.winnerOpenId = openId;
        res.winReason    = "手牌出完";
    }

    // ── HR 主动大招使用记录 ──
    if (roleId == "hr") {
        // isHrSkill 由调用方在 event 中标记，此处通过 applyPlay 外层处理
    }

    return res;
}

// ══════════════════════════════════════════════════════════════
//  pass（要不起）——角色被动技能判定
// ══════════════════════════════════════════════════════════════

PokerGameEngine::PassResult PokerGameEngine::applyPass(
    const std::string& /*roleId*/,
    const std::string& opponentRoleId,
    const std::string& openId,
    int&               skillUsed)
{
    (void)skillUsed;  // pass 中不消耗技能次数
    PassResult res;
    // 被动技能触发通知（由房间管理器调用，实际手牌操作也在那里处理）
    // 这里只返回需要触发哪些特效通知

    if (opponentRoleId == "pr") {
        SkillFxEvent fx;
        fx.skillId      = "pr_reveal";
        fx.byOpenId     = "";  // 调用方填入
        fx.targetOpenId = openId;
        fx.desc         = "情绪价值：透视你1张手牌";
        res.skillFxList.push_back(fx);
    }
    if (opponentRoleId == "boss") {
        SkillFxEvent fx;
        fx.skillId      = "boss_penalty";
        fx.byOpenId     = "";
        fx.targetOpenId = openId;
        fx.desc         = "强制加班：你抽了1张惩罚牌";
        res.skillFxList.push_back(fx);
    }
    if (opponentRoleId == "manager") {
        SkillFxEvent fx;
        fx.skillId      = "manager_discard";
        fx.byOpenId     = "";
        fx.targetOpenId = "";  // 主管自己
        fx.desc         = "抢功：主管销毁最小2张牌";
        res.skillFxList.push_back(fx);
    }
    return res;
}
