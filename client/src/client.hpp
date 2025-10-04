#pragma once

#include "common/common.hpp"

#include <emscripten/em_types.h>
#include <emscripten/html5.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace pref {

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

enum class GameLang : std::size_t {
    English,
    Ukrainian,
    Alternative,
    Count,
};

enum class GameText : std::size_t {
    Preferans,
    CurrentPlayers,
    EnterYourName,
    Enter,
    Whist,
    HalfWhist,
    Pass,
    Miser,
    Catch,
    Trust,
    YourTurn,
    Settings,
    ColorScheme,
    Language,
    English,
    Ukrainian,
    Alternative,
    Light,
    Bluish,
    Ashes,
    Dark,
    Amber,
    Genesis,
    Cyber,
    Jungle,
    Lavanda,
    Count
};

inline constexpr auto localization = std::
    array<std::array<std::string_view, std::to_underlying(GameText::Count)>, std::to_underlying(GameLang::Count)>{{
        {
            // English
            "PREFERANS",
            "Current players:",
            "Enter your name:",
            "Enter",
            PREF_WHIST, //
            PREF_HALF_WHIST,
            PREF_PASS,
            PREF_MISER,
            PREF_CATCH,
            PREF_TRUST,
            "Your turn",
            "Settings",
            "Color scheme",
            "Language",
            "English",
            "Ukrainian",
            "Alternative",
            "Light",
            "Bluish",
            "Ashes",
            "Dark",
            "Amber",
            "Genesis",
            "Cyber",
            "Jungle",
            "Lavanda",
        },
        {
            // Ukrainian
            "ПРЕФЕРАНС",
            "Поточні гравці:",
            "Введіть своє ім’я:",
            "Увійти",
            "Віст",
            "Піввіста",
            "Пас",
            "Мізер",
            "Ловлю",
            "Довіряю",
            "Ваш хід",
            "Налаштування",
            "Кольорова схема",
            "Мова",
            "Англійська",
            "Українська",
            "Альтернативна",
            "Світлий",
            "Блакитний",
            "Попіл",
            "Темний",
            "Бурштин",
            "Генезис",
            "Кібер",
            "Джунглі",
            "Лаванда",
        },
        {
            // Alternative
            "ПРЕФЕРАНС",
            "Текущие игроки:",
            "Введите своё имя:",
            "Войти",
            "Вист",
            "Полвиста",
            "Пас",
            "Мизер",
            "Ловлю",
            "Доверяю",
            "Ваш ход",
            "Настройки",
            "Цветовая схема",
            "Язык",
            "Английский",
            "Украинский",
            "Альтернативный",
            "Светлый",
            "Голубоватый",
            "Пепел",
            "Тёмный",
            "Янтарь",
            "Генезис",
            "Кибер",
            "Джунгли",
            "Лаванда",
        },
    }};

inline constexpr auto virtualWidth = 2560;
inline constexpr auto virtualHeight = 1440;
// constexpr auto virtualWidth = 1920;
// constexpr auto virtualHeight = 1080;
inline constexpr auto virtualW = static_cast<float>(virtualWidth);
inline constexpr auto virtualH = static_cast<float>(virtualHeight);

inline constexpr auto originalCardHeight = 726.0f;
inline constexpr auto originalCardWidth = 500.0f;
inline constexpr auto cardAspectRatio = originalCardWidth / originalCardHeight;
inline constexpr auto cardHeight = virtualHeight / 5.0f;
inline constexpr auto cardWidth = cardHeight * cardAspectRatio;
inline constexpr auto cardOverlapX = cardWidth * 0.6f; // 60% overlap
inline constexpr auto cardOverlapY = cardHeight * 0.2f;
inline constexpr auto fontSpacing = 1.0f;
inline constexpr auto borderMargin = virtualWidth / 52.f;

inline constexpr auto settingsIcon = "";
inline constexpr auto scoreSheetIcon = "";
inline constexpr auto enterFullScreenIcon = "";
inline constexpr auto exitFullScreenIcon = "";

// ♠ - Spades | ♣ - Clubs | ♦ - Diamonds | ♥ - Hearts
// clang-format off
inline constexpr auto bidsRank = std::array{
     SIX SPADE,   SIX CLUB,   SIX DIAMOND,   SIX HEART,    SIX,
   SEVEN SPADE, SEVEN CLUB, SEVEN DIAMOND, SEVEN HEART,  SEVEN,
   EIGHT SPADE, EIGHT CLUB, EIGHT DIAMOND, EIGHT HEART,  EIGHT,    PREF_MISER,
    NINE SPADE,  NINE CLUB,  NINE DIAMOND,  NINE HEART,   NINE, PREF_MISER_WT, NINE_WT,
     TEN SPADE,   TEN CLUB,   TEN DIAMOND,   TEN HEART,    TEN,        TEN_WT, PREF_PASS};

inline constexpr auto bidTable = std::array<std::array<std::string_view, 6>, 6>{
{{   SIX SPADE,   SIX CLUB,   SIX DIAMOND,   SIX HEART,           SIX,        "" },
 { SEVEN SPADE, SEVEN CLUB, SEVEN DIAMOND, SEVEN HEART,         SEVEN,        "" },
 { EIGHT SPADE, EIGHT CLUB, EIGHT DIAMOND, EIGHT HEART,         EIGHT,        "" },
 {  NINE SPADE,  NINE CLUB,  NINE DIAMOND,  NINE HEART,          NINE,   NINE_WT },
 {   TEN SPADE,   TEN CLUB,   TEN DIAMOND,   TEN HEART,           TEN,    TEN_WT },
 {          "",         "",            "",  PREF_MISER, PREF_MISER_WT, PREF_PASS }}};
// clang-format on

inline constexpr auto allRanks = std::size(bidsRank);
inline constexpr auto whistingButtons = std::array{GameText::Whist, GameText::HalfWhist, GameText::Pass};
inline constexpr auto miserButtons = std::array{GameText::Catch, GameText::Trust};

inline constexpr auto bidCellW = virtualWidth / 13;
inline constexpr auto bidCellH = bidCellW / 2;
inline constexpr auto bidGap = bidCellH / 10;
inline constexpr auto bidRows = static_cast<int>(std::size(bidTable));
inline constexpr auto bidCols = static_cast<int>(std::size(bidTable[0]));
inline constexpr auto bidMenuW = bidCols * bidCellW + (bidCols - 1) * bidGap;
inline constexpr auto bidMenuH = bidRows * bidCellH + (bidRows - 1) * bidGap;
inline constexpr auto bidOriginX = (virtualWidth - bidMenuW) / 2.0f;
// TODO: properly align bid menu vertically
inline constexpr auto bidOriginY = (virtualHeight - bidMenuH) / 2.0f - virtualH / 10.8f;

} // namespace pref
