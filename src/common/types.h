// src/common/types.h
// 公共类型定义：扑克牌、玩家、角色等

#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <json/json.h>

// ══════════════════════════════════════════════════════════════
//  扑克牌（职场跑得快）
// ══════════════════════════════════════════════════════════════

struct PokerCard {
    int         val;         // 3~17（17=大王, 16=小王）
    std::string suit;        // "♠" "♥" "♦" "♣" ""（王牌无花色）
    std::string name;        // "3".."A","2","小","大"
    std::string rankSymbol;
    std::string suitSymbol;
    std::string id;          // 唯一 ID，如 "♠3", "joker_s"
    bool        isRed   = false;
    bool        isJoker = false;

    Json::Value toJson() const {
        Json::Value j;
        j["val"]        = val;
        j["suit"]       = suit;
        j["name"]       = name;
        j["rankSymbol"] = rankSymbol;
        j["suitSymbol"] = suitSymbol;
        j["id"]         = id;
        j["isRed"]      = isRed;
        j["isJoker"]    = isJoker;
        return j;
    }
};

// ── 职场跑得快游戏状态 ────────────────────────────────────────

struct PokerPlayer {
    std::string              openId;
    std::string              name;
    std::string              avatar;
    std::string              roleId;
    std::vector<PokerCard>   hand;
    int                      skillUsed = 0;
    bool                     isCreator = false;
    bool                     connected = false;
};

struct PokerGameState {
    // roles.p0 / p1
    struct RoleEntry {
        std::string openId;
        std::string roleId;
    } p0, p1;

    // 废牌堆
    std::vector<PokerCard>               waste;
    // 上一手出牌
    std::vector<PokerCard>               lastPlay;
    std::string                          lastPlayByOpenId;
    int                                  lastPlayType = 0;  // 1=单 2=对 3=顺 ...
    // 当前轮到谁
    std::string                          currentTurnOpenId;
    int                                  turn = 0;
    bool                                 gameOver = false;
    std::string                          winnerOpenId;
    std::string                          winReason;
    // pr 透视
    std::unordered_map<std::string, std::vector<std::string>> prVisible; // openId → []cardId
};

// ══════════════════════════════════════════════════════════════
//  红A 扑克牌
// ══════════════════════════════════════════════════════════════

enum class RedaSuit { SPADE=0, HEART=1, DIAMOND=2, CLUB=3 };
enum class RedaRank {
    THREE=0, FOUR, FIVE, SIX, SEVEN, EIGHT, NINE, TEN,
    JACK, QUEEN, KING, ACE, TWO
};

struct RedaCard {
    int  suit;        // 0~3
    int  rank;        // 0~12
    int  rankValue;   // = rank
    bool isRed        = false;
    bool isRedAce     = false;
    bool isHeartAce   = false;
    bool isDiamondAce = false;
    bool isSpadeThree = false;
    bool isScoreCard  = false;
    std::string rankSymbol;
    std::string suitSymbol;

    Json::Value toJson() const {
        Json::Value j;
        j["suit"]        = suit;
        j["rank"]        = rank;
        j["rankValue"]   = rankValue;
        j["isRed"]       = isRed;
        j["isRedAce"]    = isRedAce;
        j["isHeartAce"]  = isHeartAce;
        j["isDiamondAce"]= isDiamondAce;
        j["isSpadeThree"]= isSpadeThree;
        j["isScoreCard"] = isScoreCard;
        j["rankSymbol"]  = rankSymbol;
        j["suitSymbol"]  = suitSymbol;
        return j;
    }
};

struct RedaPublicPlayer {
    int         id;
    std::string name;
    bool        isCreator    = false;
    std::string camp;        // "redAce" | "civilian"
    int         score        = 0;
    std::string rank_result  = "none"; // 结算排名
    int         handCount    = 13;
    bool        hasPlayedRedAce = false;
    Json::Value lastPlayedCards;
    Json::Value lastPlayedHand;
    Json::Value scoreHistory;

    Json::Value toJson() const {
        Json::Value j;
        j["id"]              = id;
        j["name"]            = name;
        j["isCreator"]       = isCreator;
        j["camp"]            = camp;
        j["score"]           = score;
        j["rank"]            = rank_result;
        j["handCount"]       = handCount;
        j["hasPlayedRedAce"] = hasPlayedRedAce;
        j["lastPlayedCards"] = lastPlayedCards;
        j["lastPlayedHand"]  = lastPlayedHand;
        j["scoreHistory"]    = scoreHistory;
        return j;
    }
};
