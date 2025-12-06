// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

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
inline constexpr auto CardOverlapX = CardWidth * 0.51f;
inline constexpr auto CardOverlapY = CardHeight * 0.26f;
inline constexpr auto CardBorderMargin = VirtualW / 30.f;
inline constexpr auto MyCardBorderMarginY = CardHeight / 10.8f;
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
inline constexpr auto LogoutIcon = "";
inline constexpr auto HandshakeIcon = "";
inline constexpr auto MicOnIcon = "";
inline constexpr auto MicOffIcon = "";
inline constexpr auto VoiceOnIcon = "";
inline constexpr auto VoiceOffIcon = "";
inline constexpr int LeftArrowIcon = 0xF112;
inline constexpr int RightArrowIcon = 0xF064;

inline constexpr auto PREF_SEVEN_OF_SPADES_CARD = 0x1F0A7;
inline constexpr auto PREF_SEVEN_OF_CLUBS_CARD = 0x1F0D7;
inline constexpr auto PREF_SEVEN_OF_DIAMONDS_CARD = 0x1F0C7;
inline constexpr auto PREF_SEVEN_OF_HEARTS_CARD = 0x1F0B7;
inline constexpr auto PREF_EIGHT_OF_SPADES_CARD = 0x1F0A8;
inline constexpr auto PREF_EIGHT_OF_CLUBS_CARD = 0x1F0D8;
inline constexpr auto PREF_EIGHT_OF_DIAMONDS_CARD = 0x01F0C8;
inline constexpr auto PREF_EIGHT_OF_HEARTS_CARD = 0x1F0B8;
inline constexpr auto PREF_NINE_OF_SPADES_CARD = 0x1F0A9;
inline constexpr auto PREF_NINE_OF_CLUBS_CARD = 0x1F0D9;
inline constexpr auto PREF_NINE_OF_DIAMONDS_CARD = 0x1F0C9;
inline constexpr auto PREF_NINE_OF_HEARTS_CARD = 0x1F0B9;
inline constexpr auto PREF_TEN_OF_SPADES_CARD = 0x1F0AA;
inline constexpr auto PREF_TEN_OF_CLUBS_CARD = 0x1F0DA;
inline constexpr auto PREF_TEN_OF_DIAMONDS_CARD = 0x1F0CA;
inline constexpr auto PREF_TEN_OF_HEARTS_CARD = 0x1F0BA;
inline constexpr auto PREF_JACK_OF_SPADES_CARD = 0x1F0AB;
inline constexpr auto PREF_JACK_OF_CLUBS_CARD = 0x1F0DB;
inline constexpr auto PREF_JACK_OF_DIAMONDS_CARD = 0x01F0CB;
inline constexpr auto PREF_JACK_OF_HEARTS_CARD = 0x1F0BB;
inline constexpr auto PREF_QUEEN_OF_SPADES_CARD = 0x1F0AD;
inline constexpr auto PREF_QUEEN_OF_CLUBS_CARD = 0x1F0DD;
inline constexpr auto PREF_QUEEN_OF_DIAMONDS_CARD = 0x1F0CD;
inline constexpr auto PREF_QUEEN_OF_HEARTS_CARD = 0x1F0BD;
inline constexpr auto PREF_KING_OF_SPADES_CARD = 0x1F0AE;
inline constexpr auto PREF_KING_OF_CLUBS_CARD = 0x1F0DE;
inline constexpr auto PREF_KING_OF_DIAMONDS_CARD = 0x1F0CE;
inline constexpr auto PREF_KING_OF_HEARTS_CARD = 0x1F0BE;
inline constexpr auto PREF_ACE_OF_SPADES_CARD = 0x1F0A1;
inline constexpr auto PREF_ACE_OF_CLUBS_CARD = 0x1F0D1;
inline constexpr auto PREF_ACE_OF_DIAMONDS_CARD = 0x1F0C1;
inline constexpr auto PREF_ACE_OF_HEARTS_CARD = 0x1F0B1;

#define PREF_SEVEN_OF_SPADES PREF_SEVEN PREF_OF_ PREF_SPADES
#define PREF_SEVEN_OF_CLUBS PREF_SEVEN PREF_OF_ PREF_CLUBS
#define PREF_SEVEN_OF_DIAMONDS PREF_SEVEN PREF_OF_ PREF_DIAMONDS
#define PREF_SEVEN_OF_HEARTS PREF_SEVEN PREF_OF_ PREF_HEARTS
#define PREF_EIGHT_OF_SPADES PREF_EIGHT PREF_OF_ PREF_SPADES
#define PREF_EIGHT_OF_CLUBS PREF_EIGHT PREF_OF_ PREF_CLUBS
#define PREF_EIGHT_OF_DIAMONDS PREF_EIGHT PREF_OF_ PREF_DIAMONDS
#define PREF_EIGHT_OF_HEARTS PREF_EIGHT PREF_OF_ PREF_HEARTS
#define PREF_NINE_OF_SPADES PREF_NINE PREF_OF_ PREF_SPADES
#define PREF_NINE_OF_CLUBS PREF_NINE PREF_OF_ PREF_CLUBS
#define PREF_NINE_OF_DIAMONDS PREF_NINE PREF_OF_ PREF_DIAMONDS
#define PREF_NINE_OF_HEARTS PREF_NINE PREF_OF_ PREF_HEARTS
#define PREF_TEN_OF_SPADES PREF_TEN PREF_OF_ PREF_SPADES
#define PREF_TEN_OF_DIAMONDS PREF_TEN PREF_OF_ PREF_DIAMONDS
#define PREF_TEN_OF_HEARTS PREF_TEN PREF_OF_ PREF_HEARTS
#define PREF_TEN_OF_CLUBS PREF_TEN PREF_OF_ PREF_CLUBS
#define PREF_JACK_OF_SPADES PREF_JACK PREF_OF_ PREF_SPADES
#define PREF_JACK_OF_CLUBS PREF_JACK PREF_OF_ PREF_CLUBS
#define PREF_JACK_OF_DIAMONDS PREF_JACK PREF_OF_ PREF_DIAMONDS
#define PREF_JACK_OF_HEARTS PREF_JACK PREF_OF_ PREF_HEARTS
#define PREF_QUEEN_OF_SPADES PREF_QUEEN PREF_OF_ PREF_SPADES
#define PREF_QUEEN_OF_CLUBS PREF_QUEEN PREF_OF_ PREF_CLUBS
#define PREF_QUEEN_OF_DIAMONDS PREF_QUEEN PREF_OF_ PREF_DIAMONDS
#define PREF_QUEEN_OF_HEARTS PREF_QUEEN PREF_OF_ PREF_HEARTS
#define PREF_KING_OF_SPADES PREF_KING PREF_OF_ PREF_SPADES
#define PREF_KING_OF_CLUBS PREF_KING PREF_OF_ PREF_CLUBS
#define PREF_KING_OF_DIAMONDS PREF_KING PREF_OF_ PREF_DIAMONDS
#define PREF_KING_OF_HEARTS PREF_KING PREF_OF_ PREF_HEARTS
#define PREF_ACE_OF_SPADES PREF_ACE PREF_OF_ PREF_SPADES
#define PREF_ACE_OF_CLUBS PREF_ACE PREF_OF_ PREF_CLUBS
#define PREF_ACE_OF_DIAMONDS PREF_ACE PREF_OF_ PREF_DIAMONDS
#define PREF_ACE_OF_HEARTS PREF_ACE PREF_OF_ PREF_HEARTS

#define PREF_OK_CLICK 1
#define PREF_NO_CLICK -1

// clang-format off
#define PREF_CARDS \
    PREF_X(PREF_SEVEN_OF_SPADES) PREF_X(PREF_SEVEN_OF_CLUBS) PREF_X(PREF_SEVEN_OF_DIAMONDS) PREF_X(PREF_SEVEN_OF_HEARTS) \
    PREF_X(PREF_EIGHT_OF_SPADES) PREF_X(PREF_EIGHT_OF_CLUBS) PREF_X(PREF_EIGHT_OF_DIAMONDS) PREF_X(PREF_EIGHT_OF_HEARTS) \
    PREF_X(PREF_NINE_OF_SPADES) PREF_X(PREF_NINE_OF_CLUBS) PREF_X(PREF_NINE_OF_DIAMONDS) PREF_X(PREF_NINE_OF_HEARTS) \
    PREF_X(PREF_TEN_OF_SPADES) PREF_X(PREF_TEN_OF_CLUBS) PREF_X(PREF_TEN_OF_DIAMONDS) PREF_X(PREF_TEN_OF_HEARTS) \
    PREF_X(PREF_JACK_OF_SPADES) PREF_X(PREF_JACK_OF_CLUBS) PREF_X(PREF_JACK_OF_DIAMONDS) PREF_X(PREF_JACK_OF_HEARTS) \
    PREF_X(PREF_QUEEN_OF_SPADES) PREF_X(PREF_QUEEN_OF_CLUBS) PREF_X(PREF_QUEEN_OF_DIAMONDS) PREF_X(PREF_QUEEN_OF_HEARTS) \
    PREF_X(PREF_KING_OF_SPADES) PREF_X(PREF_KING_OF_CLUBS) PREF_X(PREF_KING_OF_DIAMONDS) PREF_X(PREF_KING_OF_HEARTS) \
    PREF_X(PREF_ACE_OF_SPADES) PREF_X(PREF_ACE_OF_CLUBS) PREF_X(PREF_ACE_OF_DIAMONDS) PREF_X(PREF_ACE_OF_HEARTS)
// clang-format on

[[nodiscard]] constexpr auto cardNameCodepoint(const CardNameView cardName) noexcept -> int
{
#define PREF_X(PREF_CARD_NAME)                                                                                         \
    if (cardName == (PREF_CARD_NAME)) { return PREF_CARD_NAME##_CARD; }
    PREF_CARDS
#undef PREF_X
    PREF_DE(cardName);
    std::unreachable();
}

#define PREF_NUMBER_01 "❶"
#define PREF_NUMBER_02 "❷"
#define PREF_NUMBER_03 "❸"
#define PREF_NUMBER_04 "❹"
#define PREF_NUMBER_05 "❺"
#define PREF_NUMBER_06 "❻"
#define PREF_NUMBER_07 "❼"
#define PREF_NUMBER_08 "❽"
#define PREF_NUMBER_09 "❾"
#define PREF_NUMBER_10 "❿"

#define PREF_MOUSE_FACE "\xF0\x9F\x90\xAD"
#define PREF_COW_FACE "\xF0\x9F\x90\xAE"
#define PREF_CAT_FACE "\xF0\x9F\x90\xB1"
#define PREF_MONKEY_FACE "\xF0\x9F\x90\xB5"

[[nodiscard]] constexpr auto prettifyNumber(const int tricksTaken) noexcept -> std::string_view
{
    switch (tricksTaken) {
    case 1: return PREF_NUMBER_01;
    case 2: return PREF_NUMBER_02;
    case 3: return PREF_NUMBER_03;
    case 4: return PREF_NUMBER_04;
    case 5: return PREF_NUMBER_05;
    case 6: return PREF_NUMBER_06;
    case 7: return PREF_NUMBER_07;
    case 8: return PREF_NUMBER_08;
    case 9: return PREF_NUMBER_09;
    case 10: return PREF_NUMBER_10;
    }
    std::unreachable();
}

inline constexpr const auto LocalStoragePrefix = "preferans_";

// ♠ - Spades | ♣ - Clubs | ♦ - Diamonds | ♥ - Hearts
// clang-format off
inline constexpr auto BidsRank = std::array{
     PREF_SIX PREF_SPADE,   PREF_SIX PREF_CLUB,   PREF_SIX PREF_DIAMOND,   PREF_SIX PREF_HEART,            PREF_SIX,
   PREF_SEVEN PREF_SPADE, PREF_SEVEN PREF_CLUB, PREF_SEVEN PREF_DIAMOND, PREF_SEVEN PREF_HEART,          PREF_SEVEN,
   PREF_EIGHT PREF_SPADE, PREF_EIGHT PREF_CLUB, PREF_EIGHT PREF_DIAMOND, PREF_EIGHT PREF_HEART,          PREF_EIGHT, PREF_MISER,
    PREF_NINE PREF_SPADE,  PREF_NINE PREF_CLUB,  PREF_NINE PREF_DIAMOND,  PREF_NINE PREF_HEART,           PREF_NINE, PREF_MISER_WT,
            PREF_NINE_WT,  PREF_TEN PREF_SPADE,      PREF_TEN PREF_CLUB, PREF_TEN PREF_DIAMOND, PREF_TEN PREF_HEART, PREF_TEN, PREF_PASS};

inline constexpr auto BidTable = std::array<std::array<std::string_view, 7>, 6>{
{{           "",   PREF_SIX PREF_SPADE,   PREF_SIX PREF_CLUB,   PREF_SIX PREF_DIAMOND,   PREF_SIX PREF_HEART,   PREF_SIX, ""},
 {           "", PREF_SEVEN PREF_SPADE, PREF_SEVEN PREF_CLUB, PREF_SEVEN PREF_DIAMOND, PREF_SEVEN PREF_HEART, PREF_SEVEN, ""},
 {           "", PREF_EIGHT PREF_SPADE, PREF_EIGHT PREF_CLUB, PREF_EIGHT PREF_DIAMOND, PREF_EIGHT PREF_HEART, PREF_EIGHT, ""},
 {   PREF_MISER,  PREF_NINE PREF_SPADE,  PREF_NINE PREF_CLUB,  PREF_NINE PREF_DIAMOND,  PREF_NINE PREF_HEART,  PREF_NINE, PREF_MISER_WT},
 { PREF_NINE_WT,   PREF_TEN PREF_SPADE,   PREF_TEN PREF_CLUB,   PREF_TEN PREF_DIAMOND,   PREF_TEN PREF_HEART,   PREF_TEN, PREF_PASS}}};
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
inline constexpr auto BidOriginY = (VirtualH - BidMenuH) / 2.f - VirtualH / 10.8f;

enum class GameLang : std::size_t {
    English,
    Ukrainian,
    Alternative,
    Count,
};

// clang-format off
#define PREF_GAME_TEXT                                                                                                 \
    PREF_X(ACCEPT, "ACCEPT", "ПРИЙНЯТИ", "ПРИНЯТЬ")                                                                    \
    PREF_X(Alternative, "Alternative", "Альтернативна", "Альтернативный")                                              \
    PREF_X(Amber, "Amber", "Бурштин", "Янтарь")                                                                        \
    PREF_X(Bluish, "Bluish", "Блакитний", "Голубоватый")                                                               \
    PREF_X(Catch, PREF_CATCH, "Ловлю", "Ловлю")                                                                        \
    PREF_X(Closed, "Closed", "У темну", "Втёмную")                                                                     \
    PREF_X(ColorScheme, "Color scheme", "Кольорова схема", "Цветовая схема")                                           \
    PREF_X(CurrentPlayers, "Current players:", "Поточні гравці:", "Текущие игроки:")                                   \
    PREF_X(Cyber, "Cyber", "Кібер", "Кибер")                                                                           \
    PREF_X(DECLINE, "DECLINE", "ВІДХИЛИТИ", "ОТКЛОНИТЬ")                                                               \
    PREF_X(Dark, "Dark", "Темний", "Тёмный")                                                                           \
    PREF_X(Date, "DATE", "ДАТА", "ДАТА")                                                                               \
    PREF_X(Dracula, "Dracula", "Дракула", "Дракула")                                                                   \
    PREF_X(Draw, "Draw", "Нічия", "Ничья")                                                                             \
    PREF_X(Duration, "DURATION", "ТРИВАЛІСТЬ", "ДЛИТЕЛЬНОСТЬ")                                                         \
    PREF_X(English, "English", "Англійська", "Английский")                                                             \
    PREF_X(Enter, "Enter", "Увійти", "Войти")                                                                          \
    PREF_X(Games, "GAMES", "К-ТЬ ІГОР", "К-ВО ИГР")                                                                    \
    PREF_X(Genesis, "Genesis", "Генезис", "Генезис")                                                                   \
    PREF_X(HalfWhist, PREF_HALF_WHIST, "Піввіста", "Полвиста")                                                         \
    PREF_X(Jungle, "Jungle", "Джунглі", "Джунгли")                                                                     \
    PREF_X(Language, "Language", "Мова", "Язык")                                                                       \
    PREF_X(Lavanda, "Lavanda", "Лаванда", "Лаванда")                                                                   \
    PREF_X(LogOut, "LOG OUT", "ВИХІД", "ВЫХОД")                                                                        \
    PREF_X(LogOutOfTheAccount, "Log out of the account?", "Вийти з облікового запису?", "Выйти из учётной записи?")    \
    PREF_X(Login, "LOG IN", "ЛОГІН", "ЛОГИН")                                                                          \
    PREF_X(Lost, "Lost", "Поразка", "Поражение")                                                                       \
    PREF_X(MMR, "MMR", "РЕЙТИНГ", "РЕЙТИНГ")                                                                           \
    PREF_X(Miser, PREF_MISER, "Мізéр", "Мизéр")                                                                        \
    PREF_X(No, "No", "Ні", "Нет")                                                                                      \
    PREF_X(NoMoreForMe, "No more for me", "Більше не беру", "Моих больше нет")                                         \
    PREF_X(None, "", "", "")                                                                                           \
    PREF_X(Normal, "Normal", "Звичайна", "Обычная")                                                                    \
    PREF_X(Openly, PREF_OPENLY, "У світлу", "В светлую")                                                               \
    PREF_X(Offer, "Offer", "Запропонувати", "Предложить")                                                              \
    PREF_X(Other, "Other", "Інше", "Другое")                                                                           \
    PREF_X(OverallScoreboard, "SCOREBOARD", "ТАБЛИЦЯ РЕЗУЛЬТАТІВ", "ТАБЛИЦА РЕЗУЛЬТАТОВ")                              \
    PREF_X(PDW, "P/D/W", "П/Г/В", "П/Г/В")                                                                             \
    PREF_X(PLAY, "PLAY", "ГРАТИ", "ИГРАТЬ")                                                                            \
    PREF_X(Pass, PREF_PASS, "Пас", "Пас")                                                                              \
    PREF_X(Passing, "Passing", "Розпаси", "Распасы")                                                                   \
    PREF_X(Phrases, "PHRASES", "ФРАЗИ", "ФРАЗЫ")                                                                       \
    PREF_X(Preferans, "PREFERANS", "ПРЕФЕРАНС", "ПРЕФЕРАНС")                                                           \
    PREF_X(Ranked, "Ranked", "Рейтингова", "Рейтинговая")                                                              \
    PREF_X(RemainingMine, "Remaining — mine", "Решта — мої", "Остальные — мои")                                        \
    PREF_X(Result, "RESULT", "РЕЗУЛЬТАТ", "РЕЗУЛЬТАТ")                                                                 \
    PREF_X(RevealCardsAndOffer, "Reveal cards and offer", "Відкрити карти та запропонувати", "Открыть карты и предложить") \
    PREF_X(Send, "Send", "Надіслати", "Отправить")                                                                     \
    PREF_X(Settings, "SETTINGS", "НАЛАШТУВАННЯ", "НАСТРОЙКИ")                                                          \
    PREF_X(ShowFps, "Show Ping & FPS", "Показувати пінг та част. кадрів", "Показывать пинг и част. кадров")            \
    PREF_X(Time, "TIME", "ЧАС", "ВРЕМЯ")                                                                               \
    PREF_X(Total, "TOTAL", "УСЬОГО", "ВСЕГО")                                                                          \
    PREF_X(Trust, PREF_TRUST, "Довіряю", "Доверяю")                                                                    \
    PREF_X(Type, "TYPE", "ТИП", "ТИП")                                                                                 \
    PREF_X(Ukrainian, "Ukrainian", "Українська", "Украинский")                                                         \
    PREF_X(Whist, PREF_WHIST, "Віст", "Вист")                                                                          \
    PREF_X(Win, "Win", "Перемога", "Победа")                                                                           \
    PREF_X(WinRate, "WIN RATE", "ПЕРЕМОГ", "ПОБЕД")                                                                    \
    PREF_X(SoundEffects, "Sound effects", "Звукові ефекти", "Звуковые эффекты")                                        \
    PREF_X(Yes, "Yes", "Так", "Да")                                                                                    \
    PREF_X(YourTurn, "Your turn", "Ваш хід", "Ваш ход")                                                                \
    PREF_X(YourTurnFor, "Your turn for", "Ваш хід за", "Ваш ход за")
// clang-format on

enum class GameText : std::size_t {
#define PREF_X(PREF_TEXT_ID, PREF_LANG_EN, PREF_LANG_UA, PREF_LANG_ALT) PREF_TEXT_ID,
    PREF_GAME_TEXT
#undef PREF_X
        Count
};

struct Localication {
    GameText id = GameText::None;
    std::string_view en;
    std::string_view ua;
    std::string_view alt;
};

inline constexpr auto localization = std::array{
#define PREF_X(PREF_TEXT_ID, PREF_LANG_EN, PREF_LANG_UA, PREF_LANG_ALT)                                                \
    Localication{GameText::PREF_TEXT_ID, PREF_LANG_EN, PREF_LANG_UA, PREF_LANG_ALT},
    PREF_GAME_TEXT
#undef PREF_X
};
static_assert(std::size(localization) == std::to_underlying(GameText::Count));

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
    PREF_X(SUCCESS) PREF_X(DEFERRED) PREF_X(NOT_SUPPORTED) PREF_X(FAILED_NOT_DEFERRED) PREF_X(INVALID_TARGET) \
    PREF_X(UNKNOWN_TARGET) PREF_X(INVALID_PARAM) PREF_X(FAILED) PREF_X(NO_DATA) PREF_X(TIMED_OUT)

#define PREF_HTML_EVENTS \
    PREF_X(KEYPRESS) PREF_X(KEYDOWN) PREF_X(KEYUP) PREF_X(CLICK) PREF_X(MOUSEDOWN) PREF_X(MOUSEUP) PREF_X(DBLCLICK) PREF_X(MOUSEMOVE) PREF_X(WHEEL) \
    PREF_X(RESIZE) PREF_X(SCROLL) PREF_X(BLUR) PREF_X(FOCUS) PREF_X(FOCUSIN) PREF_X(FOCUSOUT) PREF_X(DEVICEORIENTATION) PREF_X(DEVICEMOTION)   \
    PREF_X(ORIENTATIONCHANGE) PREF_X(FULLSCREENCHANGE) PREF_X(POINTERLOCKCHANGE) PREF_X(VISIBILITYCHANGE) PREF_X(TOUCHSTART)    \
    PREF_X(TOUCHEND) PREF_X(TOUCHMOVE) PREF_X(TOUCHCANCEL) PREF_X(GAMEPADCONNECTED) PREF_X(GAMEPADDISCONNECTED) PREF_X(BEFOREUNLOAD) \
    PREF_X(BATTERYCHARGINGCHANGE) PREF_X(BATTERYLEVELCHANGE) PREF_X(WEBGLCONTEXTLOST) PREF_X(WEBGLCONTEXTRESTORED)         \
    PREF_X(MOUSEENTER) PREF_X(MOUSELEAVE) PREF_X(MOUSEOVER) PREF_X(MOUSEOUT) PREF_X(CANVASRESIZED) PREF_X(POINTERLOCKERROR)
// clang-format on

[[nodiscard]] constexpr auto emResult(const EMSCRIPTEN_RESULT value) noexcept -> std::string_view
{
    switch (value) {
#define PREF_X(PREF_RESULT)                                                                                            \
    case EMSCRIPTEN_RESULT_##PREF_RESULT: return #PREF_RESULT;
        PREF_WS_RESULTS
#undef PREF_X
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr auto htmlEvent(const int value) noexcept -> std::string_view
{
    switch (value) {
#define PREF_X(PREF_EVENT)                                                                                             \
    case EMSCRIPTEN_EVENT_##PREF_EVENT: return #PREF_EVENT;
        PREF_HTML_EVENTS
#undef PREF_X
    }
    return "UNKNOWN";
}

} // namespace pref
