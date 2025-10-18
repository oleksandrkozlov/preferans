#pragma once

#include "common/common.hpp"
#include "proto/pref.pb.h"

#include <emscripten/em_types.h>
#include <emscripten/html5.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace pref {

inline constexpr auto VirtualW = 2560.f;
inline constexpr auto VirtualH = 1440.f;

inline constexpr auto OriginalCardHeight = 726.f;
inline constexpr auto OriginalCardWidth = 500.f;
inline constexpr auto CardAspectRatio = OriginalCardWidth / OriginalCardHeight;
inline constexpr auto CardHeight = VirtualH / 5.f;
inline constexpr auto CardWidth = CardHeight * CardAspectRatio;
inline constexpr auto CardOverlapX = CardWidth * 0.54f;
inline constexpr auto CardOverlapY = CardHeight * 0.26f;
inline constexpr auto CardBorderMargin = VirtualW / 30.f;
inline constexpr auto CardInnerMargin = VirtualW / 100.f;
inline constexpr auto FontSpacing = 1.f;
inline constexpr auto BorderMargin = VirtualW / 52.f;
inline constexpr auto ScrollBarWidth = static_cast<int>(VirtualW / 106.f);
inline constexpr auto MenuX = VirtualW - CardBorderMargin - CardWidth - CardInnerMargin;

inline constexpr auto SettingsIcon = "";
inline constexpr auto ScoreSheetIcon = "";
inline constexpr auto EnterFullScreenIcon = "";
inline constexpr auto ExitFullScreenIcon = "";
inline constexpr auto SpeechBubbleIcon = "";
inline constexpr auto OverallScoreboardIcon = "";

inline constexpr auto PREF_SEVEN_OF_SPADES = 0x1F0A7;
inline constexpr auto PREF_SEVEN_OF_CLUBS = 0x1F0D7;
inline constexpr auto PREF_SEVEN_OF_DIAMONDS = 0x1F0C7;
inline constexpr auto PREF_SEVEN_OF_HEARTS = 0x1F0B7;
inline constexpr auto PREF_EIGHT_OF_SPADES = 0x1F0A8;
inline constexpr auto PREF_EIGHT_OF_CLUBS = 0x1F0D8;
inline constexpr auto PREF_EIGHT_OF_DIAMONDS = 0x01F0C8;
inline constexpr auto PREF_EIGHT_OF_HEARTS = 0x1F0B8;
inline constexpr auto PREF_NINE_OF_SPADES = 0x1F0A9;
inline constexpr auto PREF_NINE_OF_CLUBS = 0x1F0D9;
inline constexpr auto PREF_NINE_OF_DIAMONDS = 0x1F0C9;
inline constexpr auto PREF_NINE_OF_HEARTS = 0x1F0B9;
inline constexpr auto PREF_TEN_OF_SPADES = 0x1F0AA;
inline constexpr auto PREF_TEN_OF_CLUBS = 0x1F0DA;
inline constexpr auto PREF_TEN_OF_DIAMONDS = 0x1F0CA;
inline constexpr auto PREF_TEN_OF_HEARTS = 0x1F0BA;
inline constexpr auto PREF_JACK_OF_SPADES = 0x1F0AB;
inline constexpr auto PREF_JACK_OF_CLUBS = 0x1F0DB;
inline constexpr auto PREF_JACK_OF_DIAMONDS = 0x01F0CB;
inline constexpr auto PREF_JACK_OF_HEARTS = 0x1F0BB;
inline constexpr auto PREF_QUEEN_OF_SPADES = 0x1F0AD;
inline constexpr auto PREF_QUEEN_OF_CLUBS = 0x1F0DD;
inline constexpr auto PREF_QUEEN_OF_DIAMONDS = 0x1F0CD;
inline constexpr auto PREF_QUEEN_OF_HEARTS = 0x1F0BD;
inline constexpr auto PREF_KING_OF_SPADES = 0x1F0AE;
inline constexpr auto PREF_KING_OF_CLUBS = 0x1F0DE;
inline constexpr auto PREF_KING_OF_DIAMONDS = 0x1F0CE;
inline constexpr auto PREF_KING_OF_HEARTS = 0x1F0BE;
inline constexpr auto PREF_ACE_OF_SPADES = 0x1F0A1;
inline constexpr auto PREF_ACE_OF_CLUBS = 0x1F0D1;
inline constexpr auto PREF_ACE_OF_DIAMONDS = 0x1F0C1;
inline constexpr auto PREF_ACE_OF_HEARTS = 0x1F0B1;

[[nodiscard]] inline constexpr auto cardNameCodepoint(const std::string_view cardName) noexcept -> int
{
    if (cardName == SEVEN PREF_OF_ SPADES) { return PREF_SEVEN_OF_SPADES; }
    if (cardName == SEVEN PREF_OF_ CLUBS) { return PREF_SEVEN_OF_CLUBS; }
    if (cardName == SEVEN PREF_OF_ DIAMONDS) { return PREF_SEVEN_OF_DIAMONDS; }
    if (cardName == SEVEN PREF_OF_ HEARTS) { return PREF_SEVEN_OF_HEARTS; }
    if (cardName == EIGHT PREF_OF_ SPADES) { return PREF_EIGHT_OF_SPADES; }
    if (cardName == EIGHT PREF_OF_ CLUBS) { return PREF_EIGHT_OF_CLUBS; }
    if (cardName == EIGHT PREF_OF_ DIAMONDS) { return PREF_EIGHT_OF_DIAMONDS; }
    if (cardName == EIGHT PREF_OF_ HEARTS) { return PREF_EIGHT_OF_HEARTS; }
    if (cardName == NINE PREF_OF_ SPADES) { return PREF_NINE_OF_SPADES; }
    if (cardName == NINE PREF_OF_ CLUBS) { return PREF_NINE_OF_CLUBS; }
    if (cardName == NINE PREF_OF_ DIAMONDS) { return PREF_NINE_OF_DIAMONDS; }
    if (cardName == NINE PREF_OF_ HEARTS) { return PREF_NINE_OF_HEARTS; }
    if (cardName == TEN PREF_OF_ SPADES) { return PREF_TEN_OF_SPADES; }
    if (cardName == TEN PREF_OF_ DIAMONDS) { return PREF_TEN_OF_DIAMONDS; }
    if (cardName == TEN PREF_OF_ HEARTS) { return PREF_TEN_OF_HEARTS; }
    if (cardName == TEN PREF_OF_ CLUBS) { return PREF_TEN_OF_CLUBS; }
    if (cardName == JACK PREF_OF_ SPADES) { return PREF_JACK_OF_SPADES; }
    if (cardName == JACK PREF_OF_ CLUBS) { return PREF_JACK_OF_CLUBS; }
    if (cardName == JACK PREF_OF_ DIAMONDS) { return PREF_JACK_OF_DIAMONDS; }
    if (cardName == JACK PREF_OF_ HEARTS) { return PREF_JACK_OF_HEARTS; }
    if (cardName == QUEEN PREF_OF_ SPADES) { return PREF_QUEEN_OF_SPADES; }
    if (cardName == QUEEN PREF_OF_ CLUBS) { return PREF_QUEEN_OF_CLUBS; }
    if (cardName == QUEEN PREF_OF_ DIAMONDS) { return PREF_QUEEN_OF_DIAMONDS; }
    if (cardName == QUEEN PREF_OF_ HEARTS) { return PREF_QUEEN_OF_HEARTS; }
    if (cardName == KING PREF_OF_ SPADES) { return PREF_KING_OF_SPADES; }
    if (cardName == KING PREF_OF_ CLUBS) { return PREF_KING_OF_CLUBS; }
    if (cardName == KING PREF_OF_ DIAMONDS) { return PREF_KING_OF_DIAMONDS; }
    if (cardName == KING PREF_OF_ HEARTS) { return PREF_KING_OF_HEARTS; }
    if (cardName == ACE PREF_OF_ SPADES) { return PREF_ACE_OF_SPADES; }
    if (cardName == ACE PREF_OF_ CLUBS) { return PREF_ACE_OF_CLUBS; }
    if (cardName == ACE PREF_OF_ DIAMONDS) { return PREF_ACE_OF_DIAMONDS; }
    if (cardName == ACE PREF_OF_ HEARTS) { return PREF_ACE_OF_HEARTS; }
    std::unreachable();
}

#define PREF_TRICKS_01 "❶"
#define PREF_TRICKS_02 "❷"
#define PREF_TRICKS_03 "❸"
#define PREF_TRICKS_04 "❹"
#define PREF_TRICKS_05 "❺"
#define PREF_TRICKS_06 "❻"
#define PREF_TRICKS_07 "❼"
#define PREF_TRICKS_08 "❽"
#define PREF_TRICKS_09 "❾"
#define PREF_TRICKS_10 "❿"

[[nodiscard]] inline constexpr auto prettifyTricksTaken(const int tricksTaken) noexcept -> std::string_view
{
    switch (tricksTaken) {
    case 1: return PREF_TRICKS_01;
    case 2: return PREF_TRICKS_02;
    case 3: return PREF_TRICKS_03;
    case 4: return PREF_TRICKS_04;
    case 5: return PREF_TRICKS_05;
    case 6: return PREF_TRICKS_06;
    case 7: return PREF_TRICKS_07;
    case 8: return PREF_TRICKS_08;
    case 9: return PREF_TRICKS_09;
    case 10: return PREF_TRICKS_10;
    }
    std::unreachable();
}

inline constexpr const auto LocalStoragePrefix = "preferans_";

// ♠ - Spades | ♣ - Clubs | ♦ - Diamonds | ♥ - Hearts
// clang-format off
inline constexpr auto BidsRank = std::array{
     SIX SPADE,   SIX CLUB,   SIX DIAMOND,   SIX HEART,    SIX,
   SEVEN SPADE, SEVEN CLUB, SEVEN DIAMOND, SEVEN HEART,  SEVEN,
   EIGHT SPADE, EIGHT CLUB, EIGHT DIAMOND, EIGHT HEART,  EIGHT,    PREF_MISER,
    NINE SPADE,  NINE CLUB,  NINE DIAMOND,  NINE HEART,   NINE, PREF_MISER_WT, NINE_WT,
     TEN SPADE,   TEN CLUB,   TEN DIAMOND,   TEN HEART,    TEN,        TEN_WT, PREF_PASS};

inline constexpr auto BidTable = std::array<std::array<std::string_view, 6>, 6>{
{{   SIX SPADE,   SIX CLUB,   SIX DIAMOND,   SIX HEART,           SIX,        "" },
 { SEVEN SPADE, SEVEN CLUB, SEVEN DIAMOND, SEVEN HEART,         SEVEN,        "" },
 { EIGHT SPADE, EIGHT CLUB, EIGHT DIAMOND, EIGHT HEART,         EIGHT,        "" },
 {  NINE SPADE,  NINE CLUB,  NINE DIAMOND,  NINE HEART,          NINE,   NINE_WT },
 {   TEN SPADE,   TEN CLUB,   TEN DIAMOND,   TEN HEART,           TEN,    TEN_WT },
 {          "",         "",            "",  PREF_MISER, PREF_MISER_WT, PREF_PASS }}};
// clang-format on

inline constexpr auto AllRanks = std::size(BidsRank);

inline constexpr auto BidCellW = VirtualW / 13.f;
inline constexpr auto BidCellH = BidCellW / 2.f;
inline constexpr auto BidGap = BidCellH / 10.f;
inline constexpr auto BidRows = static_cast<int>(std::size(BidTable));
inline constexpr auto BidCols = static_cast<int>(std::size(BidTable[0]));
inline constexpr auto BidMenuW = BidCols * BidCellW + (BidCols - 1) * BidGap;
inline constexpr auto BidMenuH = BidRows * BidCellH + (BidRows - 1) * BidGap;
inline constexpr auto BidOriginX = (VirtualW - BidMenuW) / 2.f;
// TODO: properly align bid menu vertically
inline constexpr auto BidOriginY = (VirtualH - BidMenuH) / 2.f - VirtualH / 10.8f;

enum class GameLang : std::size_t {
    English,
    Ukrainian,
    Alternative,
    Count,
};

enum class GameText : std::size_t {
    None,
    Preferans,
    CurrentPlayers,
    Login,
    Enter,
    Whist,
    HalfWhist,
    Pass,
    Miser,
    Catch,
    Trust,
    YourTurn,
    YourTurnFor,
    Settings,
    ColorScheme,
    Language,
    English,
    Ukrainian,
    Alternative,
    Light,
    Bluish,
    Dark,
    Amber,
    Genesis,
    Cyber,
    Jungle,
    Lavanda,
    Phrases,
    Send,
    Openly,
    Closed,
    Other,
    ShowFps,
    Date,
    Time,
    WinRate,
    MMR,
    PDW,
    Duration,
    Games,
    Type,
    Ranked,
    Normal,
    Total,
    Result,
    Win,
    Lost,
    Draw,
    OverallScoreboard,
    Count
};

inline constexpr auto localization = std::
    array<std::array<std::string_view, std::to_underlying(GameText::Count)>, std::to_underlying(GameLang::Count)>{{
        {
            // English
            "",
            "PREFERANS",
            "Current players:",
            "Log In",
            "Enter",
            PREF_WHIST, //
            PREF_HALF_WHIST,
            PREF_PASS,
            PREF_MISER,
            PREF_CATCH,
            PREF_TRUST,
            "Your turn",
            "Your turn for",
            "Settings",
            "Color scheme",
            "Language",
            "English",
            "Ukrainian",
            "Alternative",
            "Light",
            "Bluish",
            "Dark",
            "Amber",
            "Genesis",
            "Cyber",
            "Jungle",
            "Lavanda",
            "Phrases",
            "Send",
            "Openly",
            "Closed",
            "Other",
            "Show Ping & FPS",
            "DATE",
            "TIME",
            "WIN RATE",
            "MMR",
            "P/D/W",
            "DURATION",
            "GAMES",
            "TYPE",
            "Ranked",
            "Normal",
            "TOTAL",
            "RESULT",
            "Win",
            "Lost",
            "Draw",
            "OVERALL SCOREBOARD",
        },
        {
            // Ukrainian
            "",
            "ПРЕФЕРАНС",
            "Поточні гравці:",
            "Логін",
            "Увійти",
            "Віст",
            "Піввіста",
            "Пас",
            "Мізер",
            "Ловлю",
            "Довіряю",
            "Ваш хід",
            "Ваш хід за",
            "Налаштування",
            "Кольорова схема",
            "Мова",
            "Англійська",
            "Українська",
            "Альтернативна",
            "Світлий",
            "Блакитний",
            "Темний",
            "Бурштин",
            "Генезис",
            "Кібер",
            "Джунглі",
            "Лаванда",
            "Фрази",
            "Надіслати",
            "У світлу",
            "У темну",
            "Інше",
            "Показувати пінг та част. кадрів",
            "ДАТА",
            "ЧАС",
            "ПЕРЕМОГ",
            "РЕЙТИНГ",
            "П/Г/В",
            "ТРИВАЛІСТЬ",
            "К-ТЬ ІГОР",
            "ТИП",
            "Рейтингова",
            "Звичайна",
            "УСЬОГО",
            "РЕЗУЛЬТАТ",
            "Перемога",
            "Поразка",
            "Нічия",
            "ЗАГАЛЬНА ТАБЛИЦЯ РЕЗУЛЬТАТІВ",
        },
        {
            // Alternative
            "",
            "ПРЕФЕРАНС",
            "Текущие игроки:",
            "Логин",
            "Войти",
            "Вист",
            "Полвиста",
            "Пас",
            "Мизер",
            "Ловлю",
            "Доверяю",
            "Ваш ход",
            "Ваш ход за",
            "Настройки",
            "Цветовая схема",
            "Язык",
            "Английский",
            "Украинский",
            "Альтернативный",
            "Светлый",
            "Голубоватый",
            "Тёмный",
            "Янтарь",
            "Генезис",
            "Кибер",
            "Джунгли",
            "Лаванда",
            "Фразы",
            "Отправить",
            "В светлую",
            "Втёмную",
            "Другое",
            "Показывать пинг и част. кадров",
            "ДАТА",
            "ВРЕМЯ",
            "ПОБЕД",
            "РЕЙТИНГ",
            "П/Г/В",
            "ДЛИТЕЛЬНОСТЬ",
            "К-ВО ИГР",
            "ТИП",
            "Рейтинговая",
            "Обычная",
            "ВСЕГО",
            "РЕЗУЛЬТАТ",
            "Победа",
            "Поражение",
            "Ничья",
            "ОБЩАЯ ТАБЛИЦА РЕЗУЛЬТАТОВ",
        },
    }};

inline constexpr auto WhistingButtons = std::array{GameText::Whist, GameText::HalfWhist, GameText::Pass};
inline constexpr auto MiserButtons = std::array{GameText::Catch, GameText::Trust};
inline constexpr auto HowToPlayButtons = std::array{GameText::Openly, GameText::Closed};

[[nodiscard]] constexpr auto phrases() noexcept -> std::string_view
{
    static constexpr char result[] = {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc23-extensions"
#embed "../client/resources/text/phrases.txt"
#pragma GCC diagnostic pop
    };
    return {result, sizeof(result)};
}

[[nodiscard]] constexpr auto getCloseReason(const std::uint16_t code) noexcept -> std::string_view
{
    switch (code) {
    case 1000: return "Normal closure";
    case 1001: return "Going away";
    case 1002: return "Protocol error";
    case 1003: return "Unsupported data";
    case 1005: return "No status heceived";
    case 1006: return "Abnormal closure";
    case 1007: return "Invalid payload";
    case 1008: return "Policy violation";
    case 1009: return "Message too big";
    case 1010: return "Mandatory extension";
    case 1011: return "Internal error";
    case 1012: return "Service sestart";
    case 1013: return "Try again later";
    case 1014: return "Bad gateway";
    case 1015: return "TLS handshake failed";
    }
    return "Unknown";
}

// clang-format off
#define PREF_WS_RESULTS \
    X(SUCCESS) X(DEFERRED) X(NOT_SUPPORTED) X(FAILED_NOT_DEFERRED) X(INVALID_TARGET) \
    X(UNKNOWN_TARGET) X(INVALID_PARAM) X(FAILED) X(NO_DATA) X(TIMED_OUT)

#define PREF_HTML_EVENTS \
    X(KEYPRESS) X(KEYDOWN) X(KEYUP) X(CLICK) X(MOUSEDOWN) X(MOUSEUP) X(DBLCLICK) X(MOUSEMOVE) X(WHEEL) \
    X(RESIZE) X(SCROLL) X(BLUR) X(FOCUS) X(FOCUSIN) X(FOCUSOUT) X(DEVICEORIENTATION) X(DEVICEMOTION)   \
    X(ORIENTATIONCHANGE) X(FULLSCREENCHANGE) X(POINTERLOCKCHANGE) X(VISIBILITYCHANGE) X(TOUCHSTART)    \
    X(TOUCHEND) X(TOUCHMOVE) X(TOUCHCANCEL) X(GAMEPADCONNECTED) X(GAMEPADDISCONNECTED) X(BEFOREUNLOAD) \
    X(BATTERYCHARGINGCHANGE) X(BATTERYLEVELCHANGE) X(WEBGLCONTEXTLOST) X(WEBGLCONTEXTRESTORED)         \
    X(MOUSEENTER) X(MOUSELEAVE) X(MOUSEOVER) X(MOUSEOUT) X(CANVASRESIZED) X(POINTERLOCKERROR)
// clang-format on

[[nodiscard]] constexpr auto emResult(const EMSCRIPTEN_RESULT value) noexcept -> std::string_view
{
    switch (value) {
#define X(name)                                                                                                        \
    case EMSCRIPTEN_RESULT_##name: return #name;
        PREF_WS_RESULTS
#undef X
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr auto htmlEvent(const int value) noexcept -> std::string_view
{
    switch (value) {
#define X(name)                                                                                                        \
    case EMSCRIPTEN_EVENT_##name: return #name;
        PREF_HTML_EVENTS
#undef X
    }
    return "UNKNOWN";
}

} // namespace pref
