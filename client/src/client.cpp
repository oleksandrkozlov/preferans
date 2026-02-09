// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#include "client.hpp"

#include "common/common.hpp"
#include "common/logger.hpp"
#include "common/time.hpp"
#include "proto/pref.pb.h"

#include <docopt.h>
#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <emscripten/websocket.h>
#include <range/v3/all.hpp>

#include <utility>
#define RAYGUI_TEXTSPLIT_MAX_ITEMS 128
#define RAYGUI_TEXTSPLIT_MAX_TEXT_SIZE 8192
#define RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT 36
#define RAYGUI_WINDOWBOX_CLOSEBUTTON_HEIGHT 27
#define RAYGUI_TEXTINPUTBOX_BUTTON_HEIGHT 80
#define RAYGUI_TEXTINPUTBOX_HEIGHT 80
#define RAYGUI_TEXTINPUTBOX_BUTTON_PADDING 20
#define RAYGUI_MESSAGEBOX_BUTTON_HEIGHT 80
#define RAYGUI_MESSAGEBOX_BUTTON_PADDING 40
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>
#include <raylib-cpp.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstring>
#include <functional>
#include <gsl/gsl>
#include <list>
#include <mdspan>
#include <numbers>
#include <vector>

namespace pref {
namespace {

namespace r = raylib;

constexpr auto PlayerIdStorageKey = "player_id";
constexpr auto PlayerNameStorageKey = "player_name";
constexpr auto AuthTokenStorageKey = "auth_token";

[[maybe_unused]] [[nodiscard]] auto operator+(const r::Vector2& lhs, const float rhs) -> r::Vector2
{
    return Vector2AddValue(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] auto operator-(const r::Vector2& lhs, const float rhs) -> r::Vector2
{
    return Vector2SubtractValue(lhs, rhs);
}

[[maybe_unused]] [[nodiscard]] constexpr auto operator+(r::Rectangle lhs, const float value) noexcept -> r::Rectangle
{
    lhs.x -= value * 0.5f;
    lhs.y -= value * 0.5f;
    lhs.width += value;
    lhs.height += value;
    return lhs;
}

[[maybe_unused]] [[nodiscard]] constexpr auto operator-(r::Rectangle lhs, const float value) noexcept -> r::Rectangle
{
    lhs.x += value * 0.5f;
    lhs.y += value * 0.5f;
    lhs.width -= value;
    lhs.height -= value;
    return lhs;
}

enum class Shift : std::uint8_t {
    None = 1 << 0,
    Horizont = 1 << 1,
    Vertical = 1 << 2,
    Positive = 1 << 3,
    Negative = 1 << 4,
};

[[nodiscard]] constexpr auto operator&(const Shift lhs, const Shift rhs) noexcept -> Shift
{
    return static_cast<Shift>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

[[nodiscard]] constexpr auto operator|(const Shift lhs, const Shift rhs) noexcept -> Shift
{
    return static_cast<Shift>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

[[nodiscard]] constexpr auto hasShift(const Shift value, const Shift flag) noexcept -> bool
{
    return (value & flag) == flag;
}

enum class DrawPosition { Left, Right };

[[nodiscard]] constexpr auto isRight(const DrawPosition drawPosition) noexcept -> bool
{
    return drawPosition == DrawPosition::Right;
}

[[maybe_unused]] [[nodiscard]] constexpr auto isLeft(const DrawPosition drawPosition) noexcept -> bool
{
    return not isRight(drawPosition);
}

template<typename... Args>
[[nodiscard]] auto resources(Args&&... args) -> std::string
{
    return (fs::path("resources") / ... / std::forward<Args>(args)).string();
}

[[nodiscard]] auto sounds(const std::string_view name) -> std::string
{
    return resources("sounds", name);
}

[[nodiscard]] auto fonts(const std::string_view name) -> std::string
{
    return resources("fonts", name);
}

struct Card {
    Card(CardNameView n)
        : name{n}
        , image{r::Image{resources("cards", fmt::format("{}.png", name))}}
    {
        image.Resize(static_cast<int>(CardWidth), static_cast<int>(CardHeight));
        texture = image.LoadTexture();
    }

    CardNameView name;
    r::Image image;
    r::Texture texture{};
};

[[nodiscard]] auto suitValue(const std::string_view suit) -> int
{
    static const auto map
        = std::map<std::string_view, int>{{PREF_SPADES, 1}, {PREF_DIAMONDS, 2}, {PREF_CLUBS, 3}, {PREF_HEARTS, 4}};
    return map.at(suit);
}

template<typename Projection = std::identity>
[[nodiscard]] auto cardLess(Projection&& proj = {})
{
    return [proj = std::forward<Projection>(proj)](auto&& lhs, auto&& rhs) {
        const auto lhsSuit = suitValue(cardSuit(std::invoke(proj, lhs)));
        const auto rhsSuit = suitValue(cardSuit(std::invoke(proj, rhs)));
        const auto lhsRank = rankValue(cardRank(std::invoke(proj, lhs)));
        const auto rhsRank = rankValue(cardRank(std::invoke(proj, rhs)));
        return std::tie(lhsSuit, lhsRank) < std::tie(rhsSuit, rhsRank);
    };
}

[[nodiscard]] auto areAllUnique(const auto& r) -> bool
{
    return std::ssize(r | rng::to<std::set<std::string_view>>()) == rng::distance(r);
}

struct Player {
    Player() = default;
    Player(PlayerId i, PlayerName n)
        : id{std::move(i)}
        , name{std::move(n)}
    {
    }

    auto sortCards() -> void
    {
        assert(areAllUnique(hand | rv::transform(&Card::name)));
        hand.sort(cardLess(&Card::name));
    }

    auto clear() -> void
    {
        hand.clear();
        whistingChoice.clear();
        howToPlayChoice.clear();
        bid.clear();
        tricksTaken = 0;
        playsOnBehalfOf.clear();
    }

    PlayerId id;
    PlayerName name;
    std::list<const Card*> hand;
    std::string bid;
    std::string whistingChoice;
    std::string howToPlayChoice;
    PlayerId playsOnBehalfOf;
    int tricksTaken{};
    ReadyCheckState readyCheckState = ReadyCheckState::NOT_REQUESTED;
    Offer offer = Offer::NO_OFFER;
};

struct BiddingMenu {
    bool isVisible{};
    std::string bid;
    int passRound{};
    std::size_t rank = AllRanks;

    auto clear() -> void
    {
        isVisible = false;
        bid.clear();
        passRound = 0;
        rank = AllRanks;
    }
};

struct WhistingMenu {
    bool isVisible{};
    bool canHalfWhist{};
    std::string choice;

    auto clear() -> void
    {
        isVisible = false;
        canHalfWhist = false;
        choice.clear();
    }
};

struct HowToPlayMenu {
    bool isVisible{};
    std::string choice;

    auto clear() -> void
    {
        isVisible = false;
        choice.clear();
    }
};

[[nodiscard]] constexpr auto bidRank(const std::string_view bid) noexcept -> std::size_t
{
    for (auto i = 0uz; i < std::size(BidsRank); ++i) {
        if (BidsRank[i] == bid) { return i; }
    }
    return AllRanks;
}

[[nodiscard]] constexpr auto isRedSuit(const std::string_view suit) noexcept -> bool
{
    return suit.ends_with(PREF_DIAMOND) or suit.ends_with(PREF_HEART);
}

[[nodiscard]] auto getStyle(const int control, const int property) noexcept -> float
{
    return static_cast<float>(GuiGetStyle(control, property));
}

[[nodiscard]] auto getGuiColor(const int control, const int property) -> r::Color
{
    return {gsl::narrow_cast<unsigned int>(GuiGetStyle(control, property))};
}

[[nodiscard]] auto getGuiColor(const int property) -> r::Color
{
    return getGuiColor(DEFAULT, property);
}

[[nodiscard]] auto getGuiButtonBorderWidth() noexcept -> float
{
    return getStyle(BUTTON, BORDER_WIDTH);
}

[[nodiscard]] auto getGuiTextSize() noexcept -> float
{
    return getStyle(DEFAULT, TEXT_SIZE);
}

[[nodiscard]] auto measureGuiText(const std::string& text) -> r::Vector2
{
    return MeasureTextEx(GuiGetFont(), text.c_str(), getGuiTextSize(), FontSpacing);
}

template<
    std::invocable Get,
    typename Value = std::invoke_result_t<Get>,
    std::invocable<Value> Set,
    std::invocable ModifyAndDraw>
auto withTemporary(Get&& get, Set&& set, ModifyAndDraw&& modifyAndDraw)
{
    const Value prev = get();
    const auto _ = gsl::finally([&] {
        if (get() != prev) { std::forward<Set>(set)(prev); };
    });
    return std::forward<ModifyAndDraw>(modifyAndDraw)();
}

template<std::invocable Draw>
auto withGuiStyle(const int control, const int property, const int newValue, Draw&& draw)
{
    return withTemporary(
        [&] { return GuiGetStyle(control, property); },
        [&](const int prevValue) { GuiSetStyle(control, property, prevValue); },
        [&] {
            GuiSetStyle(control, property, newValue);
            return std::forward<Draw>(draw)();
        });
}

template<std::invocable Draw>
[[maybe_unused]] auto withGuiStyles(
    const int control, const std::initializer_list<std::pair<int, int>> propertiesAndNewValues, Draw&& draw)
{
    const auto propertiesAndPrevValues = propertiesAndNewValues
        | rv::transform(unpair([&](const int property, const int newValue) {
                                             const auto prevValue = GuiGetStyle(control, property);
                                             GuiSetStyle(control, property, newValue);
                                             return std::pair{property, prevValue};
                                         }))
        | rng::to_vector;
    const auto _ = gsl::finally([&] {
        for (auto [property, prevValue] : propertiesAndPrevValues) { GuiSetStyle(control, property, prevValue); }
    });
    return std::forward<Draw>(draw)();
}

template<std::invocable Draw>
auto withGuiStyle(const int control, const int prevProperty, const int newProperty, const bool condition, Draw&& draw)
{
    return withTemporary(
        [&] { return GuiGetStyle(control, prevProperty); },
        [&](const int prevValue) { GuiSetStyle(control, prevProperty, prevValue); },
        [&] {
            if (condition) { GuiSetStyle(control, prevProperty, GuiGetStyle(control, newProperty)); }
            return std::forward<Draw>(draw)();
        });
}

template<std::invocable Draw>
auto withGuiState(const int newState, const bool condition, Draw&& draw)
{
    return withTemporary(
        [] { return GuiGetState(); },
        [](const auto prevState) { GuiSetState(prevState); },
        [&] {
            if (condition) { GuiSetState(newState); }
            return std::forward<Draw>(draw)();
        });
}

[[nodiscard]] constexpr auto localizeText(const GameText text, const GameLang lang) noexcept -> std::string_view
{
    static constexpr auto langs = std::array{&Localication::en, &Localication::ua, &Localication::alt};
    return localization[std::to_underlying(text)].*langs[std::to_underlying(lang)];
}

[[nodiscard]] constexpr auto whistingChoiceToGameText(const std::string_view choice) noexcept -> GameText
{
    using enum GameText;
    if (choice == localizeText(Whist, GameLang::English)) { return Whist; }
    if (choice == localizeText(HalfWhist, GameLang::English)) { return HalfWhist; }
    if (choice == localizeText(Catch, GameLang::English)) { return Catch; }
    if (choice == localizeText(Trust, GameLang::English)) { return Trust; }
    if (choice == localizeText(Pass, GameLang::English)) { return Pass; }
    return None;
}

[[nodiscard]] auto fontSize(const r::Font& font) noexcept -> float
{
    return static_cast<float>(font.baseSize);
}

struct SettingsMenu {
    static constexpr auto windowBoxW = VirtualW / 5.f;
    static constexpr auto ButtomY = VirtualH / 1.9f; // approximately
    int colorSchemeIdScroll = -1;
    int colorSchemeIdSelect = -1;
    int langIdScroll = -1;
    int langIdSelect = -1;
    bool isVisible{};
    std::string loadedColorScheme;
    std::string loadedLang;
    bool showPingAndFps{};
    bool soundEffects = true;
    r::Vector2 grabOffset{};
    bool moving{};
    r::Vector2 windowBoxPos{MenuX - windowBoxW, BorderMargin};
};

struct ScoreSheetMenu {
    ScoreSheet score;
    bool isVisible{};

    auto clear() -> void
    {
        score.clear();
    }
};

struct SpeechBubbleMenu {
    static constexpr auto windowBoxW = VirtualW / 5.f;
    static constexpr auto Margin = VirtualH / 60.f;
    static constexpr auto ListViewW = windowBoxW - (Margin * 2.f);
    static constexpr auto SendCooldownInSec = 10;
    bool isVisible{};
    bool isSendButtonActive = true;
    int cooldown = SendCooldownInSec;
    std::map<PlayerId, std::string> text;
    r::Vector2 grabOffset{};
    bool moving{};
    r::Vector2 windowBoxPos{MenuX - windowBoxW, SettingsMenu::ButtomY + Margin};
};

struct OverallScoreboard {
    using Table = std::vector<std::vector<std::string>>;
    float windowBoxW{};
    bool isVisible{};
    r::Vector2 grabOffset{};
    bool moving{};
    r::Vector2 windowBoxPos{};
    UserGames userGames;
    Table table;
};

struct MovingCard {
    PlayerId playerId;
    const Card* card{};
    r::Vector2 from{};
    r::Vector2 to{};
    double startedAt{};
    double durationMs{};
};

struct LogoutMessage {
    bool isVisible{};
};

struct MiserCardsPanel {
    bool isVisible{};
    std::vector<CardName> remaining;
    std::vector<CardName> played;

    auto clear() -> void
    {
        isVisible = false;
        remaining.clear();
        played.clear();
    }
};

struct Ping {
    static constexpr auto Interval = 15s; // 15s;
    static constexpr auto InvalidRtt = -1;
    std::unordered_map<std::int32_t, double> sentAt;
    double rtt = static_cast<double>(InvalidRtt);
};

[[nodiscard]] auto getCard(const CardNameView name) -> const Card&
{
    static auto allCards = std::invoke([&] {
        auto result = std::map<CardNameView, Card>{};
        const auto add = [&](const CardNameView name) { result.emplace(name, Card{name}); };
#define PREF_X(PREF_CARD_NAME) add(PREF_CARD_NAME);
        PREF_CARDS
#undef PREF_X
        return result;
    });
    return allCards.at(name);
}

auto loadCards() -> void
{
    auto&& _ = getCard(PREF_SEVEN_OF_SPADES);
}

struct PassGameTalon {
    const Card* first = nullptr;
    const Card* second = nullptr;

    auto push(const Card& card) noexcept -> void
    {
        if (first == nullptr) {
            first = &card;
        } else {
            assert(second == nullptr);
            second = &card;
        }
    }

    [[nodiscard]] auto get() noexcept -> const Card&
    {
        assert(exists());
        return *first;
    }

    [[nodiscard]] auto pop() noexcept -> const Card*
    {
        return std::exchange(first, std::exchange(second, nullptr));
    }

    [[nodiscard]] auto exists() const noexcept -> bool
    {
        return first != nullptr;
    }

    auto clear() noexcept -> void
    {
        first = nullptr;
        second = nullptr;
    }
};

struct StartGameButton {
    bool isVisible{};
};

struct ReadyCheckPopUp {
    bool isVisible{};
};

struct TalonDiscardPopUp {
    bool isVisible{};
};

struct OfferButton {
    bool isPossible{};
    bool isVisible{};
    bool beenClicked{};

    auto clear() -> void
    {
        isPossible = {};
        isVisible = {};
    }
};

struct OfferPopUp {
    bool isVisible{};
};

struct Mic {
    std::string status;
    bool isError{};
    bool isMuted = false;
};

[[nodiscard]] auto makeSound(const std::string_view filename) -> std::optional<r::Sound>
{
    if (const auto path = sounds(filename); std::filesystem::exists(path)) { return {path}; }
    return {};
}

auto stopSound(std::optional<r::Sound>& sound) -> void;

struct Sound {
    std::optional<r::Sound> gameAboutToStarted = makeSound("game_about_to_start.mp3");
    std::optional<r::Sound> gameStarted = makeSound("game_started.mp3");
    std::optional<r::Sound> readyCheckAccepted = makeSound("ready_check_accepted.mp3");
    std::optional<r::Sound> readyCheckDeclined = makeSound("ready_check_declined.mp3");
    std::optional<r::Sound> readyCheckReceived = makeSound("ready_check_received.mp3");
    std::optional<r::Sound> readyCheckRequested = makeSound("ready_check_requested.mp3");
    std::optional<r::Sound> readyCheckSucceeded = makeSound("ready_check_succeeded.mp3");
    std::optional<r::Sound> dealCards = makeSound("deal_cards.wav");
    std::optional<r::Sound> placeCard = makeSound("place_card.mp3");

    auto withoutSoundEffects() -> void
    {
        // TODO: add sounds to a container and replace with a loop
        stopSound(gameAboutToStarted);
        stopSound(gameStarted);
        stopSound(readyCheckAccepted);
        stopSound(readyCheckDeclined);
        stopSound(readyCheckReceived);
        stopSound(readyCheckRequested);
        stopSound(readyCheckSucceeded);
        stopSound(dealCards);
        stopSound(placeCard);
    }
};

struct Context {
    r::Font fontS;
    r::Font fontM;
    r::Font fontL;
    r::Font initialFont;
    r::Font fontAwesome;
    r::Font fontAwesomeXL;
    float scale = 1.f;
    float offsetX = 0.f;
    float offsetY = 0.f;
    int windowWidth = static_cast<int>(VirtualW);
    int windowHeight = static_cast<int>(VirtualH);
    r::Window window{windowWidth, windowHeight, "Preferans"};
    r::RenderTexture target{static_cast<int>(VirtualW), static_cast<int>(VirtualH)};
    r::AudioDevice audio;
    Sound sound;
    Mic microphone;
    PlayerId myPlayerId;
    PlayerName myPlayerName;
    std::string password;
    std::string authToken;
    bool isLoggedIn{};
    bool isLoginInProgress{};
    mutable std::map<PlayerId, Player> players;
    EMSCRIPTEN_WEBSOCKET_T ws{};
    int leftCardCount = 10;
    int rightCardCount = 10;
    std::map<PlayerId, const Card*> cardsOnTable;
    std::vector<CardNameView> lastTrickOrTalon;
    std::vector<CardNameView> pendingTalonReveal;
    double pendingTalonRevealUntil = 0.0;
    PlayerId forehandId;
    PlayerId turnPlayerId;
    BiddingMenu bidding;
    WhistingMenu whisting;
    HowToPlayMenu howToPlay;
    GameStage stage = GameStage::UNKNOWN;
    std::vector<CardNameView> discardedTalon;
    std::string leadSuit;
    PassGameTalon passGameTalon;
    GameLang lang{};
    SettingsMenu settingsMenu;
    ScoreSheetMenu scoreSheet;
    SpeechBubbleMenu speechBubbleMenu;
    OverallScoreboard overallScoreboard;
    MiserCardsPanel miserCardsPanel;
    LogoutMessage logoutMessage;
    StartGameButton startGameButton;
    ReadyCheckPopUp readyCheckPopUp;
    TalonDiscardPopUp talonDiscardPopUp;
    OfferButton offerButton;
    OfferPopUp offerPopUp;
    Ping ping;
    std::string url;
    std::map<CardNameView, r::Vector2> cardPositions;
    std::vector<MovingCard> movingCards;
    bool isGameFreezed{};
    bool needsDraw = true;
    double tick = 0.0;
    bool isGameStarted{};

    auto clear() -> void
    {
        leftCardCount = 10;
        rightCardCount = 10;
        cardsOnTable.clear();
        lastTrickOrTalon.clear();
        pendingTalonReveal.clear();
        pendingTalonRevealUntil = 0.0;
        forehandId.clear();
        turnPlayerId.clear();
        bidding.clear();
        whisting.clear();
        howToPlay.clear();
        stage = GameStage::UNKNOWN;
        discardedTalon.clear();
        leadSuit.clear();
        passGameTalon.clear();
        scoreSheet.isVisible = false;
        miserCardsPanel.clear();
        cardPositions.clear();
        movingCards.clear();
        isGameFreezed = false;
        offerButton.clear();
        offerPopUp.isVisible = false;
        for (auto& p : players | rv::values) { p.clear(); }
    }

    [[nodiscard]] auto localizeText(const GameText text) const -> std::string
    {
        return std::string{::pref::localizeText(text, lang)};
    }

    [[nodiscard]] constexpr auto localizeBid(const std::string_view bid) const noexcept -> std::string_view
    {
        if (lang == GameLang::Alternative) {
            if (bid == PREF_NINE_WT) { return PREF_NINE " БП"; }
            if (bid == PREF_MISER) { return pref::localizeText(GameText::Miser, lang); }
            if (bid == PREF_MISER_WT) { return "Миз.БП"; }
            if (bid == PREF_PASS) { return pref::localizeText(GameText::Pass, lang); }
        } else if (lang == GameLang::Ukrainian) {
            if (bid == PREF_NINE_WT) { return PREF_NINE " БП"; }
            if (bid == PREF_MISER) { return pref::localizeText(GameText::Miser, lang); }
            if (bid == PREF_MISER_WT) { return "Міз.БП"; }
            if (bid == PREF_PASS) { return pref::localizeText(GameText::Pass, lang); }
        }
        return bid;
    }

    [[nodiscard]] auto areAllPlayersJoined() const noexcept -> bool
    {
        return std::size(players) == NumberOfPlayers;
    }

    [[nodiscard]] auto fontSizeS() const noexcept -> float
    {
        return fontSize(fontS);
    }

    [[nodiscard]] auto fontSizeM() const noexcept -> float
    {
        return fontSize(fontM);
    }

    [[nodiscard]] auto fontSizeL() const noexcept -> float
    {
        return fontSize(fontL);
    }

    [[nodiscard]] auto fontSizeXL() const noexcept -> float
    {
        return fontSize(fontAwesomeXL);
    }

    [[nodiscard]] auto player(const PlayerId& playerId) const -> Player&
    {
        assert(players.contains(playerId) and "player exists");
        return players[playerId];
    }

    [[nodiscard]] auto myPlayer() const -> Player&
    {
        return player(myPlayerId);
    }
};

[[nodiscard]] auto ctx() -> Context&
{
    static auto ctx = Context{};
    return ctx;
}

auto initAudioEngine() -> void;
auto syncAudioPeers() -> void;
auto teardownAudioEngine() -> void;
[[nodiscard]] auto isTalonDiscardSelected(CardNameView name) -> bool;
auto removeCardsFromHand(Player& player, const std::vector<CardNameView>& cardNames) -> void;
auto handleTalonCardClick(std::list<const Card*>& hand) -> void;
auto applyPendingTalonReveal() -> void;

auto playSound(std::optional<r::Sound>& sound) -> void
{
    if (not sound or not ctx().settingsMenu.soundEffects) { return; }
    sound->Play();
}

auto stopSound(std::optional<r::Sound>& sound) -> void
{
    if (not sound) { return; }
    sound->Stop();
}

[[nodiscard]] auto players() -> decltype(auto)
{
    return ctx().players | rv::values;
}

[[nodiscard]] auto loadFromLocalStorage(const std::string_view key) -> std::string
{
    char buffer[128] = {};
    const auto keyWithPrefix = fmt::format("{}{}", LocalStoragePrefix, key);

    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
    EM_ASM(
        {
            const key = UTF8ToString($2);
            const value = localStorage.getItem(key) || "";
            stringToUTF8(value, $0, $1);
        },
        buffer,
        sizeof(buffer),
        keyWithPrefix.c_str());
#pragma GCC diagnostic pop
    // clang-format on
    return {buffer};
}

auto saveToLocalStorage(const std::string_view key, const std::string_view value) -> void
{
    const auto js = fmt::format("localStorage.setItem('{}{}', '{}');", LocalStoragePrefix, key, value);
    PREF_DI(key);
    emscripten_run_script(js.c_str());
}

auto removeFromLocalStorage(const std::string_view key) -> void
{
    const auto js = fmt::format("localStorage.removeItem('{}{}');", LocalStoragePrefix, key);
    emscripten_run_script(js.c_str());
}

auto loadFromLocalStoragePlayerId() -> void
{
    ctx().myPlayerId = loadFromLocalStorage(PlayerIdStorageKey);
}

auto saveToLocalStoragePlayerId() -> void
{
    saveToLocalStorage(PlayerIdStorageKey, ctx().myPlayerId);
}

auto removeFromLocalStoragePlayerId() -> void
{
    removeFromLocalStorage(PlayerIdStorageKey);
}

auto loadFromLocalStoragePlayerName() -> void
{
    ctx().myPlayerName = loadFromLocalStorage(PlayerNameStorageKey);
}

auto saveToLocalStoragePlayerName() -> void
{
    saveToLocalStorage(PlayerNameStorageKey, ctx().myPlayerName);
}

auto loadFromLocalStorageAuthToken() -> void
{
    ctx().authToken = loadFromLocalStorage(AuthTokenStorageKey);
}

auto initMic() -> void
{
    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
    EM_ASM({
        (function () {
            Module.ccall('pref_on_microphone_status', null, ['string', 'number'], ['Requesting access', 0]);
            const constraints = (window.constraints = { audio: true, video: false });
            if (window.stream && window.stream.getTracks) {
                window.stream.getTracks().forEach(track => {
                    try { track.stop(); } catch (_) {}
                });
                window.stream = null;
            }
            const audio = new Audio();
            audio.autoplay = true;
            audio.controls = false;
            audio.style.display = 'none';
            audio.muted = true; // avoid local loopback playback
            window.prefDesiredMute = false;

            function handleSuccess(stream) {
                const audioTracks = stream.getAudioTracks();
                const device = audioTracks[0]?.label || 'unknown device';
                const mute = !!window.prefDesiredMute;
                if (audioTracks) {
                    audioTracks.forEach(track => { track.enabled = !mute; });
                }
                Module.ccall('pref_on_microphone_status', null, ['string', 'number'], [ mute ? 'OFF' : 'ON', 0]);
                stream.oninactive = function () {
                    Module.ccall('pref_on_microphone_status', null, ['string', 'number'], ['Stream ended', 1]);
                };
                window.stream = stream;
                audio.srcObject = stream;
                audio.play().catch(() => {});
            }

            function handleError(error) {
                const errorMessage =
                    'navigator.MediaDevices.getUserMedia error: ' + error.message + ' ' + error.name;
                    Module.ccall('pref_on_microphone_status', null, ['string', 'number'], [errorMessage, 1]);
            }

            if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
                handleError(new Error('getUserMedia not supported'));
                return;
            }

            navigator.mediaDevices.getUserMedia(constraints).then(handleSuccess).catch(handleError);
        })();
    });
#pragma GCC diagnostic pop
    // clang-format on
}

auto saveToLocalStorageAuthToken() -> void
{
    saveToLocalStorage(AuthTokenStorageKey, ctx().authToken);
}

auto removeFromLocalStorageAuthToken() -> void
{
    removeFromLocalStorage(AuthTokenStorageKey);
}

[[nodiscard]] auto isPlayerTurn(const PlayerId& playerId) -> bool
{
    return not std::empty(ctx().turnPlayerId) and ctx().turnPlayerId == playerId;
}

[[nodiscard]] auto isMyTurn() -> bool
{
    return isPlayerTurn(ctx().myPlayerId);
}

[[nodiscard]] auto isSomeonePlayingOnBehalfOf(const PlayerId& playerId) -> bool
{
    return not std::empty(playerId) and rng::any_of(players(), equalTo(playerId), &Player::playsOnBehalfOf);
}

[[nodiscard]] auto isSomeonePlayingOnMyBehalf() -> bool
{
    return isSomeonePlayingOnBehalfOf(ctx().myPlayerId);
}

[[nodiscard]] auto isPlayerTurnOnBehalfOfSomeone(const PlayerId& playerId) -> bool
{
    return not std::empty(ctx().turnPlayerId)
        and not std::empty(playerId)
        and ctx().turnPlayerId == ctx().player(playerId).playsOnBehalfOf;
}

[[nodiscard]] auto isMyTurnOnBehalfOfSomeone() -> bool
{
    return isPlayerTurnOnBehalfOfSomeone(ctx().myPlayerId);
}

[[nodiscard]] auto getOpponentIds() -> std::pair<PlayerId, PlayerId>
{
    assert(ctx().areAllPlayersJoined());
    auto order = std::vector<PlayerId>{};
    for (const auto& id : ctx().players | rv::keys) { order.push_back(id); }
    const auto it = rng::find(order, ctx().myPlayerId);
    assert(it != order.end());
    const auto selfIndex = std::distance(order.begin(), it);
    const auto leftIndex = (selfIndex + 1) % 3;
    const auto rightIndex = (selfIndex + 2) % 3;
    return {order[static_cast<std::size_t>(leftIndex)], order[static_cast<std::size_t>(rightIndex)]};
}

struct PlaySlots {
    r::Vector2 top;
    r::Vector2 left;
    r::Vector2 right;
    r::Vector2 bottom;
};

[[nodiscard]] auto playSlots() -> const PlaySlots&
{
    static const PlaySlots slots = [] {
        static constexpr auto cardSpacing = CardWidth * 0.1f;
        static const auto centerPos = r::Vector2{VirtualW * 0.5f, (VirtualH - MyCardBorderMarginY - CardHeight) * 0.5f};
        static const auto topBottomX = centerPos.x - CardWidth * 0.5f;
        static const auto leftRightY = centerPos.y - CardHeight * 0.5f;
        static const auto topPlayPos = r::Vector2{topBottomX, centerPos.y - cardSpacing - CardHeight};
        static const auto bottomPlayPos = r::Vector2{topBottomX, centerPos.y + cardSpacing};
        static const auto leftPlayPos
            = r::Vector2{centerPos.x - CardWidth * 0.5f - cardSpacing - CardWidth, leftRightY};
        static const auto rightPlayPos = r::Vector2{centerPos.x + cardSpacing + CardWidth * 0.5f, leftRightY};
        return PlaySlots{topPlayPos, leftPlayPos, rightPlayPos, bottomPlayPos};
    }();
    return slots;
}

[[nodiscard]] auto playedCardPosition(const PlayerId& playerId) -> std::optional<r::Vector2>
{
    if (std::empty(playerId) or not ctx().areAllPlayersJoined()) { return std::nullopt; }
    const auto [leftOpponentId, rightOpponentId] = getOpponentIds();
    const auto& slots = playSlots();
    if (playerId == leftOpponentId) { return slots.left; }
    if (playerId == rightOpponentId) { return slots.right; }
    if (playerId == ctx().myPlayerId) { return slots.bottom; }
    return std::nullopt;
}

[[nodiscard]] auto handAnchor(const PlayerId& playerId) -> std::optional<r::Vector2>
{
    if (std::empty(playerId) or not ctx().areAllPlayersJoined()) { return std::nullopt; }
    const auto [leftOpponentId, rightOpponentId] = getOpponentIds();
    if (playerId == ctx().myPlayerId) {
        const auto cardCount = std::max<std::size_t>(1, std::size(ctx().myPlayer().hand));
        const auto totalWidth = (static_cast<float>(cardCount) - 1.f) * CardOverlapX + CardWidth;
        return r::Vector2{(VirtualW - totalWidth) * 0.5f, VirtualH - CardHeight - MyCardBorderMarginY};
    }
    const auto isRight = playerId == rightOpponentId;
    const auto isLeft = playerId == leftOpponentId;
    if (not isLeft and not isRight) { return std::nullopt; }
    const auto cardCount = std::max(1, isRight ? ctx().rightCardCount : ctx().leftCardCount);
    const auto totalHeight = (static_cast<float>(cardCount) - 1.f) * CardOverlapY + CardHeight;
    const auto x = isRight ? VirtualW - CardWidth - CardBorderMargin : CardBorderMargin;
    const auto y = (VirtualH - totalHeight) * 0.5f;
    return r::Vector2{x, y};
}

[[nodiscard]] auto isCardMoving(const PlayerId& playerId) -> bool
{
    return rng::any_of(ctx().movingCards, equalTo(playerId), &MovingCard::playerId);
}

auto startCardMove(const PlayerId& playerId, const Card& card, std::optional<r::Vector2> fromHint = {}) -> void
{
    const auto to = playedCardPosition(playerId);
    if (not to) { return; }
    const auto from = fromHint.value_or(handAnchor(playerId).value_or(*to));
    constexpr auto durationMs = 240.0;
    std::erase_if(ctx().movingCards, [&](const MovingCard& movingCard) { return movingCard.playerId == playerId; });
    ctx().movingCards.push_back(MovingCard{playerId, &card, from, *to, emscripten_get_now(), durationMs});
    ctx().needsDraw = true;
}

auto drawMovingCards() -> void
{
    if (std::empty(ctx().movingCards)) { return; }
    const auto now = emscripten_get_now();
    std::erase_if(ctx().movingCards, [&](const MovingCard& movingCard) {
        const auto progress = std::clamp((now - movingCard.startedAt) / movingCard.durationMs, 0.0, 1.0);
        const auto pos = Vector2Lerp(movingCard.from, movingCard.to, static_cast<float>(progress));
        movingCard.card->texture.Draw(pos);
        return progress >= 1.0;
    });
    ctx().needsDraw = true;
}

auto sendMessage(const EMSCRIPTEN_WEBSOCKET_T ws, const Message& msg) -> bool
{
    if (ws == 0) {
        PREF_W("error: ws is not open");
        return false;
    }
    ctx().needsDraw = true;
    auto data = msg.SerializeAsString();
    if (const auto result = emscripten_websocket_send_binary(ws, data.data(), std::size(data));
        result != EMSCRIPTEN_RESULT_SUCCESS) {
        PREF_W("error: {}, method: {}", emResult(result), msg.method());
        return false;
    }
    return true;
}

[[nodiscard]] auto makeMessage(const EmscriptenWebSocketMessageEvent& event) -> std::optional<Message>
{
    if (event.isText) {
        PREF_W("error: expect binary data");
        return {};
    }
    if (auto result = Message{}; result.ParseFromArray(event.data, static_cast<int>(event.numBytes))) { return result; }
    PREF_W("error: failed to make Message from array");
    return {};
}

[[nodiscard]] auto makeLoginRequest(const PlayerName& playerName, const std::string& password) -> LoginRequest
{
    auto result = LoginRequest{};
    result.set_player_name(playerName);
    result.set_password(password);
    return result;
}

[[nodiscard]] auto makeAuthRequest(const PlayerId& playerId, const std::string& authToken) -> AuthRequest
{
    auto result = AuthRequest{};
    result.set_player_id(playerId);
    result.set_auth_token(authToken);
    return result;
}

[[nodiscard]] auto makeLogout(const PlayerId& playerId, const std::string& authToken) -> Logout
{
    auto result = Logout{};
    result.set_player_id(playerId);
    result.set_auth_token(authToken);
    return result;
}

[[nodiscard]] auto makeReadyCheck(const PlayerId& playerId, const ReadyCheckState& state) -> ReadyCheck
{
    auto result = ReadyCheck{};
    result.set_player_id(playerId);
    result.set_state(state);
    return result;
}

[[nodiscard]] auto makeBidding(const std::string& playerId, const std::string& bid) -> Bidding
{
    auto result = Bidding{};
    result.set_player_id(playerId);
    result.set_bid(bid);
    return result;
}

[[nodiscard]] auto makeDiscardTalon(
    const std::string& playerId, const std::string& bid, const std::vector<CardNameView>& discardedTalon)
    -> DiscardTalon
{
    auto result = DiscardTalon{};
    result.set_player_id(playerId);
    result.set_bid(bid);
    for (const auto card : discardedTalon) { result.add_cards(std::string{card}); }
    return result;
}

[[nodiscard]] auto makeWhisting(const std::string& playerId, const std::string& choice) -> Whisting
{
    auto result = Whisting{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return result;
}

[[nodiscard]] auto makeHowToPlay(const std::string& playerId, const std::string& choice) -> HowToPlay
{
    auto result = HowToPlay{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return result;
}

[[nodiscard]] auto makeOffer(const std::string& playerId, const Offer offer) -> MakeOffer
{
    auto result = MakeOffer{};
    result.set_player_id(playerId);
    result.set_offer(offer);
    return result;
}

[[nodiscard]] auto makePlayCard(const std::string& playerId, const CardNameView cardName) -> PlayCard
{
    auto result = PlayCard{};
    result.set_player_id(playerId);
    result.set_card(std::string{cardName});
    return result;
}

[[nodiscard]] auto makeLog(const std::string& playerId, const std::string& text) -> Log
{
    auto result = Log{};
    result.set_player_id(playerId);
    result.set_text(text);
    return result;
}

[[nodiscard]] auto makeSpeechBubble(const std::string& playerId, const std::string& text) -> SpeechBubble
{
    auto result = SpeechBubble{};
    result.set_player_id(playerId);
    result.set_text(text);
    return result;
}

[[nodiscard]] auto makeAudioSignal(
    const std::string& fromPlayerId, const std::string& toPlayerId, const std::string& kind, const std::string& data)
    -> AudioSignal
{
    auto result = AudioSignal{};
    PREF_DI(fromPlayerId, toPlayerId, kind, data);
    result.set_from_player_id(fromPlayerId);
    result.set_to_player_id(toPlayerId);
    result.set_kind(kind);
    result.set_data(data);
    return result;
}

[[nodiscard]] auto makePingPong(const int id) -> PingPong
{
    auto result = PingPong{};
    result.set_id(id);
    return result;
}

auto sendLoginRequest() -> void
{
    auto _ = gsl::finally([] { ctx().password.clear(); });
    // The password is sent in plain text; that's why SSL (wss://) is required in prod.
    if (not sendMessage(ctx().ws, makeMessage(makeLoginRequest(ctx().myPlayerName, ctx().password)))) {
        ctx().isLoginInProgress = false;
    }
}

auto sendAuthRequest() -> void
{
    if (not sendMessage(ctx().ws, makeMessage(makeAuthRequest(ctx().myPlayerId, ctx().authToken)))) {
        ctx().isLoginInProgress = false;
    }
}

auto sendLogout() -> void
{
    sendMessage(ctx().ws, makeMessage(makeLogout(ctx().myPlayerId, ctx().authToken)));
}

auto sendReadyCheck(const ReadyCheckState state) -> void
{
    sendMessage(ctx().ws, makeMessage(makeReadyCheck(ctx().myPlayerId, state)));
}

auto sendBidding(const std::string& bid) -> void
{
    sendMessage(ctx().ws, makeMessage(makeBidding(ctx().myPlayerId, bid)));
}

auto sendDiscardTalon(const std::string& bid) -> void
{
    sendMessage(ctx().ws, makeMessage(makeDiscardTalon(ctx().myPlayerId, bid, ctx().discardedTalon)));
}

auto sendWhisting(const std::string& choice) -> void
{
    sendMessage(ctx().ws, makeMessage(makeWhisting(ctx().myPlayerId, choice)));
}

auto sendHowToPlay(const std::string& choice) -> void
{
    sendMessage(ctx().ws, makeMessage(makeHowToPlay(ctx().myPlayerId, choice)));
}

auto sendMakeOffer(const Offer offer) -> void
{
    sendMessage(ctx().ws, makeMessage(makeOffer(ctx().myPlayerId, offer)));
}

auto sendPlayCard(const PlayerId& playerId, const CardNameView cardName) -> void
{
    sendMessage(ctx().ws, makeMessage(makePlayCard(playerId, cardName)));
}

[[maybe_unused]] auto sendLog(const std::string& text) -> void
{
    sendMessage(ctx().ws, makeMessage(makeLog(ctx().myPlayerId, text)));
}

auto sendSpeechBubble(const std::string& text) -> void
{
    sendMessage(ctx().ws, makeMessage(makeSpeechBubble(ctx().myPlayerId, text)));
}

auto sendAudioSignal(const std::string& toPlayerId, const std::string& kind, const std::string& data) -> void
{
    PREF_DI(toPlayerId, kind, data);
    if (toPlayerId == ctx().myPlayerId) { return; }
    sendMessage(ctx().ws, makeMessage(makeAudioSignal(ctx().myPlayerId, toPlayerId, kind, data)));
}

[[nodiscard]] auto sendPingPong(const std::int32_t id) -> bool
{
    return sendMessage(ctx().ws, makeMessage(makePingPong(id)));
}

template<typename Rep, typename Period, typename Func>
auto waitFor(const std::chrono::duration<Rep, Period> duration, Func func, void* ud) -> void
{
    emscripten_async_call(
        func, ud, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()));
}

template<typename Rep, typename Period, typename Func>
auto waitFor(const std::chrono::duration<Rep, Period> duration, Func func) -> void
{
    waitFor(
        duration,
        [](void* ud) {
            auto* f = static_cast<std::function<void()>*>(ud);
            const auto _ = gsl::finally([&] { delete f; });
            (*f)();
        },
        new std::function<void()>{std::move(func)});
}

auto repeatPingPong() -> void;

auto shedulePingPong() -> void
{
    waitFor(Ping::Interval, [] { repeatPingPong(); });
}

auto repeatPingPong() -> void
{
    static auto id = 0;
    const auto now = emscripten_get_now();
    ctx().ping.sentAt[++id] = now;
    if (not sendPingPong(id)) {
        ctx().ping.rtt = static_cast<double>(Ping::InvalidRtt);
        return;
    }
    shedulePingPong();
}

auto finishLogin(auto& response) -> void
{
    ctx().players.clear();
    for (const auto& p : response.players()) {
        auto playerId = std::string{p.player_id()};
        auto player = Player{playerId, std::string{p.player_name()}};
        ctx().players.insert_or_assign(std::move(playerId), std::move(player));
    }
    if (ctx().areAllPlayersJoined()) { ctx().startGameButton.isVisible = true; }
    repeatPingPong();
    ctx().isLoggedIn = true;
    ctx().isLoginInProgress = false;
    initMic();
    initAudioEngine();
}

auto handleLoginResponse(const Message& msg) -> void
{
    const auto loginResponse = makeMethod<LoginResponse>(msg);
    if (not loginResponse) { return; }
    if (const auto& error = loginResponse->error(); not std::empty(error)) {
        PREF_DW(error);
        ctx().isLoginInProgress = false;
        return;
    }
    ctx().stage = loginResponse->stage();
    if (ctx().stage != GameStage::UNKNOWN) { ctx().isGameStarted = true; }
    PREF_I("playerId: {}, stage: {}", loginResponse->player_id(), GameStage_Name(ctx().stage));
    ctx().myPlayerId = std::string{loginResponse->player_id()};
    ctx().authToken = std::string{loginResponse->auth_token()};
    saveToLocalStoragePlayerId();
    saveToLocalStorageAuthToken();
    finishLogin(*loginResponse);
}

[[nodiscard]] auto isReadyCheckSucceeded() -> bool
{
    return rng::all_of(players(), equalTo(ReadyCheckState::ACCEPTED), &Player::readyCheckState);
}

auto resetReadyCheck() -> void
{
    for (auto& player : players()) { player.readyCheckState = ReadyCheckState::NOT_REQUESTED; }
}

auto startReadyCheck(const PlayerId& playerId) -> void
{
    ctx().startGameButton.isVisible = false;
    resetReadyCheck();
    ctx().player(playerId).readyCheckState = ReadyCheckState::ACCEPTED;
    if (playerId == ctx().myPlayerId) {
        playSound(ctx().sound.readyCheckRequested);
    } else {
        playSound(ctx().sound.readyCheckReceived);
    }
}

auto whenReadyCheckDeclined(const PlayerId& playerId) -> void
{
    if (ctx().player(playerId).readyCheckState != ReadyCheckState::DECLINED) { return; }
    ctx().startGameButton.isVisible = true;
    ctx().readyCheckPopUp.isVisible = false;
    stopSound(ctx().sound.readyCheckAccepted);
    stopSound(ctx().sound.readyCheckReceived);
    stopSound(ctx().sound.readyCheckRequested);
    playSound(ctx().sound.readyCheckDeclined);
}

auto readyCheckSucceeded() -> void
{
    assert(not ctx().startGameButton.isVisible);
    assert(not ctx().readyCheckPopUp.isVisible);
    stopSound(ctx().sound.readyCheckAccepted);
    stopSound(ctx().sound.readyCheckReceived);
    stopSound(ctx().sound.readyCheckRequested);
    playSound(ctx().sound.readyCheckSucceeded);
}

[[nodiscard]] auto isReadyCheckAccepted(const PlayerId& playerId) -> bool
{
    return ctx().player(playerId).readyCheckState == ReadyCheckState::ACCEPTED;
}

auto evaluateReadyCheck(const PlayerId& playerId) -> void
{
    if (isReadyCheckSucceeded()) {
        readyCheckSucceeded();
    } else if (
        isReadyCheckAccepted(playerId)
        and (ctx().sound.readyCheckReceived and not ctx().sound.readyCheckReceived->IsPlaying())) {
        playSound(ctx().sound.readyCheckAccepted);
    } else {
        whenReadyCheckDeclined(playerId);
    }
}

auto resetOffer() -> void
{
    for (auto& player : players()) { player.offer = Offer::NO_OFFER; }
}

auto evaluateOffer(const PlayerId& playerId) -> void
{
    ctx().offerButton.isPossible = false;
    ctx().offerButton.isVisible = false;
    resetOffer();
    ctx().player(playerId).offer = Offer::OFFER_ACCEPTED;
}

auto whenOfferDeclined(const PlayerId& playerId) -> void
{
    if (ctx().player(playerId).offer != Offer::OFFER_DECLINED) { return; }
    ctx().offerPopUp.isVisible = false;
}

auto handleReadyCheck(const Message& msg) -> void
{
    const auto readyCheck = makeMethod<ReadyCheck>(msg);
    if (not readyCheck) { return; }
    const auto playerId = std::string{readyCheck->player_id()};
    ctx().player(playerId).readyCheckState = readyCheck->state();
    PREF_I("{}, state: {}", PREF_V(playerId), ReadyCheckState_Name(ctx().player(playerId).readyCheckState));
    if (ctx().player(playerId).readyCheckState == ReadyCheckState::REQUESTED) {
        ctx().readyCheckPopUp.isVisible = true;
        startReadyCheck(playerId);
        return;
    }
    evaluateReadyCheck(playerId);
}

auto handleAuthResponse(const Message& msg) -> void
{
    const auto authResponse = makeMethod<AuthResponse>(msg);
    if (not authResponse) { return; }
    if (const auto& error = authResponse->error(); not std::empty(error)) {
        PREF_DW(error);
        ctx().isLoginInProgress = false;
        ctx().myPlayerId.clear();
        ctx().authToken.clear();
        removeFromLocalStoragePlayerId();
        removeFromLocalStorageAuthToken();
        return;
    }
    ctx().stage = authResponse->stage();
    if (ctx().stage != GameStage::UNKNOWN) { ctx().isGameStarted = true; }
    ctx().myPlayerName = std::string{authResponse->player_name()};
    PREF_I("playerName: {}, stage: {}", ctx().myPlayerName, GameStage_Name(ctx().stage));
    saveToLocalStoragePlayerName();
    finishLogin(*authResponse);
}

auto handlePlayerJoined(const Message& msg) -> void
{
    auto playerJoined = makeMethod<PlayerJoined>(msg);
    if (not playerJoined) { return; }
    auto playerId = std::string{playerJoined->player_id()};
    auto playerName = std::string{playerJoined->player_name()};
    PREF_DI(playerId, playerName);
    auto player = Player{playerId, std::move(playerName)};
    ctx().players.insert_or_assign(std::move(playerId), std::move(player));
    if (ctx().areAllPlayersJoined()) { ctx().startGameButton.isVisible = true; }
    syncAudioPeers();
}

auto handlePlayerLeft(const Message& msg) -> void
{
    const auto playerLeft = makeMethod<PlayerLeft>(msg);
    if (not playerLeft) { return; }
    PREF_I("playerId: {}", playerLeft->player_id());
    ctx().players.erase(std::string{playerLeft->player_id()});
    syncAudioPeers();
}

[[nodiscard]] auto isMiser() -> bool
{
    return rng::any_of(players(), [](const std::string_view bid) { return bid.contains(PREF_MIS); }, &Player::bid);
}

auto handleForehand(const Message& msg) -> void
{
    auto forehand = makeMethod<Forehand>(msg);
    if (not forehand) { return; }
    auto playerId = std::string{forehand->player_id()};
    PREF_DI(playerId);
    ctx().forehandId = std::move(playerId);
}

auto handleDealCards(const Message& msg) -> void
{
    if (not ctx().isGameStarted) {
        waitFor(1s, [] { playSound(ctx().sound.gameAboutToStarted); });
        waitFor(3s, [] {
            ctx().isGameStarted = true;
            resetReadyCheck();
            playSound(ctx().sound.gameStarted);
            playSound(ctx().sound.dealCards);
        });
    }
    auto dealCards = makeMethod<DealCards>(msg);
    if (not dealCards) { return; }
    const auto playerId = std::string{dealCards->player_id()};
    auto& player = std::invoke([&] -> Player& {
        if (playerId == ctx().myPlayerId) {
            if (ctx().isGameStarted) { playSound(ctx().sound.dealCards); }
            ctx().clear();
        }
        return ctx().player(playerId);
    });
    for (const auto& cardName : dealCards->cards()) { player.hand.emplace_back(&getCard(cardName)); }
    PREF_I("{}, hand: {}", PREF_V(player.id), player.hand | rv::transform(&Card::name));
    player.sortCards();
}

auto handlePlayerTurn(const Message& msg) -> void
{
    using enum GameStage;
    auto playerTurn = makeMethod<PlayerTurn>(msg);
    if (not playerTurn) { return; }
    ctx().turnPlayerId = playerTurn->player_id();
    ctx().stage = playerTurn->stage();
    const auto minBid = playerTurn->min_bid();
    const auto passRound = playerTurn->pass_round();
    PREF_I(
        "turnPlayerId: {}, stage: {}, {}{}",
        ctx().turnPlayerId,
        GameStage_Name(ctx().stage),
        PREF_V(minBid),
        PREF_B(passRound));
    assert(not std::empty(minBid));
    const auto minRank = std::invoke([&] {
        if (minBid == PREF_SIX) { return AllRanks; }
        if (minBid == PREF_SEVEN) { return bidRank(PREF_SIX); }
        return bidRank(PREF_SEVEN);
    });
    if (minRank != AllRanks) {
        auto& rank = ctx().bidding.rank;
        rank = (rank == AllRanks) ? minRank : std::max(minRank, rank);
    }
    ctx().bidding.passRound = passRound;
    const auto isMyTurn = pref::isMyTurn();
    // FIXME: On reconnection during talon picking, previously discarded cards are incorrectly restored to the hand
    if (ctx().stage == TALON_PICKING) {
        const auto talonCards = playerTurn->talon()
            | rv::transform([](const auto& cardName) { return getCard(cardName).name; })
            | rng::to_vector;
        if (not std::empty(talonCards)) {
            const auto allCardsAlreadyApplied = rng::all_of(talonCards, [&](const auto cardName) {
                const auto& card = getCard(cardName);
                return rng::contains(ctx().lastTrickOrTalon, card.name)
                    and (not isMyTurn or rng::contains(ctx().myPlayer().hand, &card));
            });
            if (not allCardsAlreadyApplied and not rng::equal(ctx().pendingTalonReveal, talonCards)) {
                ctx().pendingTalonReveal = std::move(talonCards);
                ctx().pendingTalonRevealUntil = ctx().window.GetTime() + 2.0;
                ctx().needsDraw = true;
            }
        }
        applyPendingTalonReveal();
        for (const auto& cardName : playerTurn->talon()) {
            const auto& card = getCard(cardName);
            // Note: On reconnection, the cards are already sent by DealCards, so they must not be inserted twice
            if (rng::contains(ctx().pendingTalonReveal, card.name)) { continue; }
            if (not rng::contains(ctx().lastTrickOrTalon, card.name)) { ctx().lastTrickOrTalon.push_back(card.name); }
            if (isMyTurn and not rng::contains(ctx().myPlayer().hand, &card)) { ctx().myPlayer().hand.emplace_back(&card); }
        }
        // TODO: Sorting should be skipped if no cards were added above
        if (isMyTurn) { ctx().myPlayer().sortCards(); }
        return;
    }
    ctx().offerButton.isPossible = //
        isMyTurn and ctx().stage == GameStage::PLAYING and ctx().myPlayer().bid != PREF_PASS;
    if (not ctx().offerButton.isPossible) { ctx().offerButton.isVisible = false; }
    if (not isMyTurn) { return; }
    if (ctx().stage == WITHOUT_TALON) {
        ctx().bidding.rank -= 7;
        ctx().bidding.isVisible = true;
        return;
    }
    if (ctx().stage == BIDDING) {
        ctx().bidding.isVisible = true;
        return;
    }
    if (ctx().stage == WHISTING) {
        ctx().whisting.canHalfWhist = playerTurn->can_half_whist();
        ctx().whisting.isVisible = true;
        return;
    }
    if (ctx().stage == HOW_TO_PLAY) {
        ctx().howToPlay.isVisible = true;
        return;
    }
}

auto handleBidding(const Message& msg) -> void
{
    auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) { return; }
    const auto playerId = std::string{bidding->player_id()};
    auto bid = std::string{bidding->bid()};
    auto newRank = bidRank(bid);
    auto& curRank = ctx().bidding.rank;
    if (ctx().myPlayerId == ctx().forehandId and newRank != 0) { --newRank; }
    if (newRank >= curRank or curRank == AllRanks) { ctx().bidding.bid = bid; }
    if (bid != PREF_PASS and (newRank > curRank or curRank == AllRanks)) { curRank = newRank; }
    PREF_DI(playerId, bid, curRank, newRank);
    ctx().player(playerId).bid = std::move(bid);
}

auto handleWhisting(const Message& msg) -> void
{
    auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { return; }
    const auto playerId = std::string{whisting->player_id()};
    auto choice = std::string{whisting->choice()};
    PREF_DI(playerId, choice);
    ctx().player(playerId).whistingChoice = std::move(choice);
}

auto handleOpenWhistPlay(const Message& msg) -> void
{
    auto openWhistPlay = makeMethod<OpenWhistPlay>(msg);
    if (not openWhistPlay) { return; }
    const auto activeWhisterId = std::string{openWhistPlay->active_whister_id()};
    auto passiveWhisterId = std::string{openWhistPlay->passive_whister_id()};
    PREF_DI(activeWhisterId, passiveWhisterId);
    auto& activeWhister = ctx().player(activeWhisterId);
    activeWhister.playsOnBehalfOf = std::move(passiveWhisterId);
}

auto handleHowToPlay(const Message& msg) -> void
{
    const auto howToPlay = makeMethod<HowToPlay>(msg);
    if (not howToPlay) { return; }
    const auto playerId = std::string{howToPlay->player_id()};
    const auto choice = howToPlay->choice();
    ctx().player(playerId).howToPlayChoice = choice;
    PREF_DI(playerId, choice);
}

auto handleMakeOffer(const Message& msg) -> void
{
    const auto makeOffer = makeMethod<MakeOffer>(msg);
    if (not makeOffer) { return; }
    const auto playerId = std::string{makeOffer->player_id()};
    ctx().player(playerId).offer = makeOffer->offer();
    PREF_I("{}, state: {}", PREF_V(playerId), Offer_Name(ctx().player(playerId).offer));
    if (ctx().player(playerId).offer == Offer::OFFER_REQUESTED) {
        ctx().offerPopUp.isVisible = true;
        evaluateOffer(playerId);
        return;
    }
    whenOfferDeclined(playerId);
}

auto handlePlayCard(const Message& msg) -> void
{
    auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) { return; }
    auto [leftOpponentId, rightOpponentId] = getOpponentIds();
    auto playerId = std::string{playCard->player_id()};
    const auto cardName = playCard->card();
    PREF_DI(playerId, cardName);

    const auto& card = getCard(cardName);
    if (playerId == leftOpponentId) {
        if (ctx().leftCardCount > 0) { --ctx().leftCardCount; }
    } else if (playerId == rightOpponentId) {
        if (ctx().rightCardCount > 0) { --ctx().rightCardCount; }
    }

    ctx().player(playerId).hand |= rng::actions::remove_if(equalTo(cardName), &Card::name);
    if (std::empty(ctx().cardsOnTable) and not ctx().passGameTalon.exists()) { ctx().leadSuit = cardSuit(cardName); }
    ctx().cardsOnTable.insert_or_assign(playerId, &card);
    if (not isCardMoving(playerId)) { startCardMove(playerId, card); }
    playSound(ctx().sound.placeCard);
}

auto handleGameState(const Message& msg) -> void
{
    auto gameState = makeMethod<GameState>(msg);
    if (not gameState) { return; }
    ctx().lastTrickOrTalon.clear();
    for (auto&& cardName : gameState->last_trick()) {
        PREF_DI(cardName);
        const auto& card = getCard(cardName);
        ctx().lastTrickOrTalon.push_back(card.name);
    }
    for (auto&& tricks : gameState->taken_tricks()) {
        const auto playerId = std::string{tricks.player_id()};
        auto&& tricksTaken = tricks.taken();
        PREF_DI(playerId, tricksTaken);
        ctx().player(playerId).tricksTaken = tricksTaken;
    }
    const auto [leftOpponentId, rightOpponentId] = getOpponentIds();
    for (auto&& cardsLeft : gameState->cards_left()) {
        const auto playerId = std::string{cardsLeft.player_id()};
        auto&& cardsLeftCount = cardsLeft.count();
        PREF_DI(playerId, cardsLeftCount);
        if (leftOpponentId == playerId) {
            ctx().leftCardCount = std::min(cardsLeftCount, 10);
            continue;
        }
        if (rightOpponentId == playerId) {
            ctx().rightCardCount = std::min(cardsLeftCount, 10);
            continue;
        }
    }
}

auto handleTrickFinished(const Message& msg) -> void
{
    const auto trickFinished = makeMethod<TrickFinished>(msg);
    if (not trickFinished) { return; }
    PREF_I();
    ctx().isGameFreezed = true;
    waitFor(
        1s,
        [](void* ud) {
            assert(ud != nullptr);
            auto trickFinished = static_cast<TrickFinished*>(ud);
            auto _ = gsl::finally([&] { delete trickFinished; });
            if (not ctx().isGameFreezed) { return; }
            if (std::empty(ctx().myPlayer().hand)) {
                ctx().scoreSheet.isVisible = true;
            } else {
                ctx().isGameFreezed = false;
            }
            for (auto&& tricks : trickFinished->tricks()) {
                const auto playerId = std::string{tricks.player_id()};
                auto&& tricksTaken = tricks.taken();
                PREF_DI(playerId, tricksTaken);
                ctx().player(playerId).tricksTaken = tricksTaken;
            }
            assert(std::size(ctx().cardsOnTable) == 3);
            ctx().lastTrickOrTalon = ctx().cardsOnTable | rv::values | rv::transform(&Card::name) | rng::to_vector;
            if (const auto card = ctx().passGameTalon.pop(); card != nullptr) {
                ctx().lastTrickOrTalon.push_back(card->name);
            }
            rng::sort(ctx().lastTrickOrTalon, cardLess());
            ctx().cardsOnTable.clear();
        },
        new TrickFinished{*trickFinished});
}

auto handleDealFinished(const Message& msg) -> void
{
    const auto dealFinished = makeMethod<DealFinished>(msg);
    if (not dealFinished) { return; }
    ctx().offerButton.beenClicked = false;
    const auto isGameOver = dealFinished->is_game_over();
    PREF_DI(isGameOver);
    ctx().scoreSheet.score = dealFinished->score_sheet() // clang-format off
        | rv::transform(unpair([](const auto& playerId, const auto& score) {
        return std::pair{playerId, Score{
            .dump = score.dump().values() | rng::to_vector,
            .pool = score.pool().values() | rng::to_vector,
            .whists = score.whists() | rv::transform(unpair([](const auto& id, const auto& whist) {
                return std::pair{id, whist.values() | rng::to_vector};
        })) | rng::to<std::map>}};
    })) | rng::to<ScoreSheet>; // clang-format on
    if (not std::empty(ctx().myPlayer().hand)) { ctx().scoreSheet.isVisible = true; }
    if (isGameOver) {
        waitFor(10s, [] {
            ctx().clear();
            ctx().scoreSheet.clear();
            ctx().isGameStarted = false;
            if (ctx().areAllPlayersJoined()) { ctx().startGameButton.isVisible = true; }
        });
    }
}

auto handleSpeechBubble(const Message& msg) -> void
{
    const auto speechBubble = makeMethod<SpeechBubble>(msg);
    if (not speechBubble) { return; }
    PREF_I("playerId: {}, text: {}", speechBubble->player_id(), speechBubble->text());
    ctx().speechBubbleMenu.text.insert_or_assign(
        std::string{speechBubble->player_id()}, std::string{speechBubble->text()});
}

auto handleAudioSignal(const Message& msg) -> void
{
    const auto signal = makeMethod<AudioSignal>(msg);
    if (not signal) { return; }
    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
    EM_ASM(
        {
            if (!Module.prefAudio) { return; }
            Module.prefAudio.handleSignal(
                UTF8ToString($0), // from
                UTF8ToString($1), // kind
                UTF8ToString($2)); // data
        },
        std::string{signal->from_player_id()}.c_str(),
        std::string{signal->kind()}.c_str(),
        std::string{signal->data()}.c_str());
#pragma GCC diagnostic pop
    // clang-format on
}

auto handlePingPong(const Message& msg) -> void
{
    const auto pingPong = makeMethod<PingPong>(msg);
    if (not pingPong) { return; }
    const auto id = pingPong->id();
    if (not ctx().ping.sentAt.contains(id)) { return; }
    ctx().ping.rtt = emscripten_get_now() - ctx().ping.sentAt[id];
    ctx().ping.sentAt.erase(id);
}

auto updateOverallScoreboardTable() -> void
{
    const auto& games = ctx().overallScoreboard.userGames.games();
    const auto totalGames = std::ssize(games);
    const auto totalWins = rng::count_if(games, std::bind_front(std::less{}, 0), &UserGame::mmr);
    const auto totalLosses = rng::count_if(games, std::bind_front(std::greater{}, 0), &UserGame::mmr);
    const auto winRate
        = static_cast<int>((static_cast<float>(totalWins) / static_cast<float>(totalWins + totalLosses)) * 100.f);
    const auto total = [&](auto proj) { return rng::accumulate(games, 0, std::plus{}, proj); };
    auto mmrDiff = games | rv::transform(&UserGame::mmr) | rv::partial_sum(std::plus{}) | rng::to_vector;
    rng::actions::reverse(mmrDiff);
    ctx().overallScoreboard.table = OverallScoreboard::Table{
        {ctx().localizeText(GameText::Date),
         ctx().localizeText(GameText::Time),
         ctx().localizeText(GameText::Duration),
         ctx().localizeText(GameText::Result),
         ctx().localizeText(GameText::MMR),
         ctx().localizeText(GameText::PDW),
         fmt::format("{} / {}", ctx().localizeText(GameText::Games), ctx().localizeText(GameText::Type))},
        {ctx().localizeText(GameText::Total),
         std::string{"-"},
         formatDuration(total(&UserGame::duration)),
         fmt::format("{}% {}", winRate, ctx().localizeText(GameText::WinRate)),
         fmt::format("{}", rng::accumulate(games, 0, std::plus{}, &UserGame::mmr)),
         fmt::format("{}/{}/{}", total(&UserGame::pool), total(&UserGame::dump), total(&UserGame::whists)),
         std::format("{}", totalGames)}};
    const auto view
        = games | rv::reverse | rv::enumerate | rv::transform([&](auto&& index_game) {
              auto&& [index, game] = index_game;
              const auto result = (game.mmr() == 0)
                  ? ctx().localizeText(GameText::Draw)
                  : (game.mmr() > 0 ? ctx().localizeText(GameText::Win) : ctx().localizeText(GameText::Lost));
              return std::vector{
                  formatDate(game.timestamp()),
                  formatTime(game.timestamp()),
                  formatDuration(game.duration()),
                  fmt::format("{} ({}{})", result, result == ctx().localizeText(GameText::Win) ? "+" : "", game.mmr()),
                  fmt::format("{}{}", mmrDiff.at(index) <= 0 ? "" : "+", mmrDiff.at(index)),
                  fmt::format("{}/{}/{}", game.pool(), game.dump(), game.whists()),
                  game.game_type() == GameType::RANKED ? ctx().localizeText(GameText::Ranked)
                                                       : ctx().localizeText(GameText::Normal)};
          });
    std::ranges::copy(view, std::back_inserter(ctx().overallScoreboard.table));
}

auto handleUserGames(const Message& msg) -> void
{
    auto userGames = makeMethod<UserGames>(msg);
    if (not userGames) { return; }
    PREF_I();
    ctx().overallScoreboard.userGames = std::move(*userGames);
    updateOverallScoreboardTable();
}

auto handleOpenTalon(const Message& msg) -> void
{
    const auto openTalon = makeMethod<OpenTalon>(msg);
    if (not openTalon) { return; }
    const auto cardName = openTalon->card();
    PREF_DI(cardName);
    ctx().leadSuit = cardSuit(cardName);
    ctx().passGameTalon.push(getCard(cardName));
}

auto handleMiserCards(const Message& msg) -> void
{
    auto miserCards = makeMethod<MiserCards>(msg);
    if (not miserCards) { return; }
    ctx().miserCardsPanel.played = miserCards->played_cards() | rng::to_vector;
    ctx().miserCardsPanel.remaining = miserCards->remaining_cards() | rng::to_vector;
    PREF_DI(ctx().miserCardsPanel.played, ctx().miserCardsPanel.remaining);
    ctx().miserCardsPanel.isVisible = true;
}

auto applyPendingTalonReveal() -> void
{
    if (std::empty(ctx().pendingTalonReveal)) { return; }
    if (ctx().window.GetTime() < ctx().pendingTalonRevealUntil) { return; }
    const auto isMyTurn = pref::isMyTurn();
    for (const auto cardName : ctx().pendingTalonReveal) {
        const auto& card = getCard(cardName);
        if (not rng::contains(ctx().lastTrickOrTalon, card.name)) { ctx().lastTrickOrTalon.push_back(card.name); }
        if (isMyTurn and not rng::contains(ctx().myPlayer().hand, &card)) { ctx().myPlayer().hand.emplace_back(&card); }
    }
    if (isMyTurn) { ctx().myPlayer().sortCards(); }
    ctx().pendingTalonReveal.clear();
    ctx().pendingTalonRevealUntil = 0.0;
    ctx().needsDraw = true;
}

auto updateWindowSize() -> void
{
    EmscriptenFullscreenChangeEvent f;
    emscripten_get_fullscreen_status(&f);
    auto canvasCssW = 0.0;
    auto canvasCssH = 0.0;
    if (emscripten_get_element_css_size("#canvas", &canvasCssW, &canvasCssH) != EMSCRIPTEN_RESULT_SUCCESS) { return; }
    const auto dpr = emscripten_get_device_pixel_ratio();
    ctx().windowWidth = static_cast<int>(canvasCssW * dpr);
    ctx().windowHeight = static_cast<int>(canvasCssH * dpr);
    emscripten_set_canvas_element_size("#canvas", ctx().windowWidth, ctx().windowHeight);
    ctx().window.SetSize(ctx().windowWidth, ctx().windowHeight);
    const auto windowW = static_cast<float>(ctx().windowWidth);
    const auto windowH = static_cast<float>(ctx().windowHeight);
    ctx().scale = std::fminf(windowW / VirtualW, windowH / VirtualH);
    ctx().offsetX = (windowW - VirtualW * ctx().scale) * 0.5f;
    ctx().offsetY = (windowH - VirtualH * ctx().scale) * 0.5f;
    r::Mouse::SetOffset(static_cast<int>(-ctx().offsetX), static_cast<int>(-ctx().offsetY));
    r::Mouse::SetScale(1.f / ctx().scale, 1.f / ctx().scale);
}

// clang-format off
#define PREF_METHODS \
    PREF_X(AuthResponse) \
    PREF_X(AudioSignal) \
    PREF_X(Bidding) \
    PREF_X(DealCards) \
    PREF_X(DealFinished) \
    PREF_X(Forehand) \
    PREF_X(GameState) \
    PREF_X(HowToPlay) \
    PREF_X(LoginResponse) \
    PREF_X(MakeOffer) \
    PREF_X(MiserCards) \
    PREF_X(OpenTalon) \
    PREF_X(OpenWhistPlay) \
    PREF_X(PingPong) \
    PREF_X(PlayCard) \
    PREF_X(PlayerJoined) \
    PREF_X(PlayerLeft) \
    PREF_X(PlayerTurn) \
    PREF_X(ReadyCheck) \
    PREF_X(SpeechBubble) \
    PREF_X(TrickFinished) \
    PREF_X(UserGames) \
    PREF_X(Whisting)
// clang-format on

auto dispatchMessage(const std::optional<Message>& msg) -> void
{
    if (not msg) { return; }
    const auto& method = msg->method();
#define PREF_X(PREF_MSG_NAME)                                                                                          \
    if (method == std::string_view{#PREF_MSG_NAME}) {                                                                  \
        ctx().needsDraw = true;                                                                                        \
        return handle##PREF_MSG_NAME(*msg);                                                                            \
    }
    PREF_METHODS;
#undef PREF_X
    PREF_W("error: unknown {}", PREF_V(method));
}

auto setupWebsocket() -> void;

auto onWsOpen(
    [[maybe_unused]] const int eventType,
    [[maybe_unused]] const EmscriptenWebSocketOpenEvent* event,
    [[maybe_unused]] void* ud) -> EM_BOOL
{
    if (not std::empty(ctx().myPlayerId) and not std::empty(ctx().authToken)) {
        // TODO: draw Enter Screen (no password) instead of Login Screen when auth token is in place and
        // replace `ctx().password.clear()` with `assert(std::empty(ctx().password))`
        ctx().password.clear();
        sendAuthRequest();
        return EM_TRUE;
    }
    assert(not std::empty(ctx().myPlayerName));
    assert(not std::empty(ctx().password));
    sendLoginRequest();
    return EM_TRUE;
}

auto onWsMessage(
    [[maybe_unused]] const int eventType, const EmscriptenWebSocketMessageEvent* event, [[maybe_unused]] void* ud)
    -> EM_BOOL
{
    assert(event);
    dispatchMessage(makeMessage(*event));
    return EM_TRUE;
}

auto onWsError(
    [[maybe_unused]] const int eventType,
    [[maybe_unused]] const EmscriptenWebSocketErrorEvent* event,
    [[maybe_unused]] void* ud) -> EM_BOOL
{
    assert(event);
    return EM_TRUE;
}

auto onWsClosed([[maybe_unused]] const int eventType, const EmscriptenWebSocketCloseEvent* e, [[maybe_unused]] void* ud)
    -> EM_BOOL
{
    PREF_I("wasClean: {}, code: {}, reason: {}", e->wasClean, getCloseReason(e->code), e->reason);
    emscripten_websocket_delete(ctx().ws);
    ctx().ws = 0;
    if (e->wasClean
        and getCloseReason(e->code) == "Policy violation"
        and std::strcmp(e->reason, "Another tab connected") == 0) {
        ctx().isLoggedIn = false;
        return EM_TRUE;
    }
    // TODO: draw error message: e.g., server is down, etc.
    if (not e->wasClean and ctx().isLoggedIn) {
        waitFor(2s, [] { setupWebsocket(); });
        // TODO: stop reconnection attempts at some point
    }
    return EM_TRUE;
}

auto setupWebsocket() -> void
{
    PREF_I("url: {}", ctx().url);
    if (ctx().ws != 0) {
        PREF_I("ws is already connected");
        onWsOpen(0, nullptr, nullptr);
        return;
    }
    if (not emscripten_websocket_is_supported()) {
        PREF_W("ws is not supported");
        return;
    }
    EmscriptenWebSocketCreateAttributes attr = {};
    attr.url = ctx().url.c_str();
    attr.protocols = nullptr;
    attr.createOnMainThread = EM_TRUE;
    ctx().ws = emscripten_websocket_new(&attr);
    if (ctx().ws <= 0) {
        PREF_W("Failed to create WebSocket");
        return;
    }
    emscripten_websocket_set_onopen_callback(ctx().ws, nullptr, onWsOpen);
    emscripten_websocket_set_onmessage_callback(ctx().ws, nullptr, onWsMessage);
    emscripten_websocket_set_onerror_callback(ctx().ws, nullptr, onWsError);
    emscripten_websocket_set_onclose_callback(ctx().ws, nullptr, onWsClosed);
}

auto setFont(const r::Font& font) -> void
{
    GuiSetFont(font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, font.baseSize);
}

auto setDefaultFont() -> void
{
    setFont(ctx().fontM);
}

auto withGuiFont(const r::Font& font, std::invocable auto draw) -> auto
{
    setFont(font);
    const auto _ = gsl::finally([&] { setDefaultFont(); });
    return draw();
}

[[nodiscard]] auto redColor() -> r::Color
{
    return ctx().settingsMenu.loadedColorScheme == "dracula" ? r::Color{255, 85, 85} : r::Color::Red();
}

[[nodiscard]] auto greenColor() -> r::Color
{
    return ctx().settingsMenu.loadedColorScheme == "dracula" ? r::Color{80, 250, 123} : r::Color::DarkGreen();
}

[[maybe_unused]] auto drawDebugDot(const r::Vector2& pos, const std::string_view text) -> void
{
    pos.DrawCircle(5, redColor());
    ctx().fontS.DrawText(std::string{text}, pos, ctx().fontSizeS(), FontSpacing, r::Color::Black());
}

[[maybe_unused]] auto drawDebugVertLine(const float x, const std::string_view text) -> void
{
    r::Vector2{x, 0.f}.DrawLine({x, VirtualH}, 1, redColor());
    ctx().fontS.DrawText(std::string{text}, {x, VirtualH * 0.5f}, ctx().fontSizeS(), FontSpacing, r::Color::Black());
}

[[maybe_unused]] auto drawDebugHorzLine(const float y, const std::string_view text) -> void
{
    r::Vector2{0.f, y}.DrawLine({VirtualW, y}, 1, redColor());
    ctx().fontS.DrawText(std::string{text}, {VirtualW * 0.5f, y}, ctx().fontSizeS(), FontSpacing, r::Color::Black());
}

auto drawGuiLabelCentered(const std::string& text, const r::Vector2& anchor) -> void
{
    const auto size = measureGuiText(text);
    const auto shift = VirtualH / 135.f;
    const auto bounds = r::Rectangle{
        anchor.x - size.x * 0.5f - (shift * 0.5f), // shift left to center and add left padding
        anchor.y - size.y * 0.5f - (shift * 0.5f), // shift up to center and add top padding
        size.x + shift, // width = text + left + right padding
        size.y + shift // height = text + top + bottom padding
    };
    GuiLabel(bounds, text.c_str());
}

auto drawRectangleWithBorder(const r::Rectangle& rect, const r::Color& fillColor, const r::Color& borderColor) -> void
{
    rect.Draw(fillColor);
    rect.DrawLines(borderColor, getGuiButtonBorderWidth());
}

auto drawRectangleRoundedWithBorder(
    const r::Rectangle& rect,
    const float roundness,
    const float thick = getGuiButtonBorderWidth(),
    const r::Color& fillColor = getGuiColor(BASE_COLOR_NORMAL),
    const r::Color& borderColor = getGuiColor(BORDER_COLOR_NORMAL)) -> void
{
    static constexpr int segments = 64;
    const auto fillRect = rect + thick;
    fillRect.DrawRounded(roundness, segments, fillColor);
    rect.DrawRoundedLines(roundness, segments, thick, borderColor);
}

auto drawSpeechBubbleText(r::Vector2 p3, const std::string& text, const DrawPosition drawPosition) -> void
{
    using enum DrawPosition;
    static constexpr auto roundness = 0.7f;
    const auto fontSize = ctx().fontSizeM();
    const auto textSize = ctx().fontM.MeasureText(text.c_str(), fontSize, FontSpacing);
    const auto padding = textSize.y * 0.5f;
    const auto bubbleWidth = textSize.x + padding * 2.f;
    const auto bubbleHeight = textSize.y + padding * 2.f;
    static constexpr auto shift = VirtualH / 28.8f;
    p3 = isRight(drawPosition) ? r::Vector2{p3.x - shift, p3.y} : r::Vector2{p3.x + shift, p3.y};
    const auto pos = isRight(drawPosition) ? r::Vector2{p3.x - textSize.y - bubbleWidth, p3.y - bubbleHeight * 0.5f}
                                           : r::Vector2{p3.x + textSize.y, p3.y - bubbleHeight * 0.5f};
    const auto rect = r::Rectangle{pos.x, pos.y, bubbleWidth, bubbleHeight};

    const auto p1 = isRight(drawPosition) ? r::Vector2{rect.x + rect.width, rect.y + bubbleHeight * 0.66f}
                                          : r::Vector2{rect.x, rect.y + bubbleHeight * 0.66f};
    const auto p2 = isRight(drawPosition) ? r::Vector2{rect.x + rect.width, rect.y + bubbleHeight * 0.33f}
                                          : r::Vector2{rect.x, rect.y + bubbleHeight * 0.33f};
    const auto colorBorder = getGuiColor(BORDER_COLOR_NORMAL);
    const auto colorBackground = getGuiColor(BASE_COLOR_NORMAL);
    const auto colorText = getGuiColor(TEXT_COLOR_NORMAL);
    const auto thick = getGuiButtonBorderWidth();
    drawRectangleRoundedWithBorder(rect, roundness);
    isRight(drawPosition) ? DrawTriangle(p3, p2, p1, colorBackground) : DrawTriangle(p1, p2, p3, colorBackground);
    p3.DrawLine(p1, thick, colorBorder);
    p3.DrawLine(p2, thick, colorBorder);
    ctx().fontM.DrawText(text.c_str(), {rect.x + padding, rect.y + padding}, fontSize, FontSpacing, colorText);
}

auto drawWelcomeScreen() -> void
{
    if (ctx().isGameStarted) { return; }
    const auto title = ctx().localizeText(GameText::Preferans);
    const auto textSize = ctx().fontL.MeasureText(title, ctx().fontSizeL(), FontSpacing);
    const auto x = (VirtualW - textSize.x) * 0.5f;
    const auto y = VirtualH / 54.f;
    ctx().fontL.DrawText(title, {x, y}, ctx().fontSizeL(), FontSpacing, getGuiColor(LABEL, TEXT_COLOR_NORMAL));
}

// TODO: support text input for mobile devices
auto drawLoginScreen() -> void
{
    if (ctx().isLoggedIn or ctx().isLoginInProgress) { return; }
    static constexpr auto boxWidth = VirtualW / 5.f;
    static constexpr auto boxHeight = RAYGUI_TEXTINPUTBOX_HEIGHT;
    static constexpr auto textInputBoxWidth = boxWidth + RAYGUI_TEXTINPUTBOX_BUTTON_PADDING * 2.f;
    static constexpr auto textInputBoxHeight = boxHeight * 4.5f;
    static constexpr auto maxLengthPassword = 27;
    static constexpr auto maxLengthName = 11;
    static const auto screenCenter = r::Vector2{VirtualW, VirtualH} * 0.5f;
    static const auto textInputBox = r::Rectangle{
        screenCenter.x - textInputBoxWidth * 0.5f,
        screenCenter.y - textInputBoxHeight * 0.5f,
        textInputBoxWidth,
        textInputBoxHeight};

    static auto passwordBuffer = std::string{};
    passwordBuffer.resize(maxLengthPassword);
    static bool passwordViewActive = false;

    const auto clicked = withGuiFont(ctx().fontS, [&] {
        const auto loginText = ctx().localizeText(GameText::Login);
        const auto enterText = ctx().localizeText(GameText::Enter);
        return GuiTextInputBox(
            textInputBox,
            loginText.c_str(),
            "",
            enterText.c_str(),
            std::data(passwordBuffer),
            maxLengthPassword,
            &passwordViewActive);
    });

    static const auto loginBoxPos = r::Vector2{screenCenter.x - boxWidth * 0.5f, screenCenter.y - boxHeight * 1.5f};
    static const auto loginInputBox = r::Rectangle{loginBoxPos, {boxWidth, boxHeight}};
    static auto loginEditMode = true;
    static auto loginBuffer = ctx().myPlayerName;
    loginBuffer.resize(maxLengthName);
    if (withGuiFont(ctx().fontS, [&] {
            return GuiTextBox(loginInputBox, std::data(loginBuffer), maxLengthName, loginEditMode);
        })) {
        loginEditMode = false;
    }

    // hide x button
    r::Rectangle{
        textInputBox.x + textInputBoxWidth - RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT - 2,
        textInputBox.y + 2,
        RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT - 4,
        RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT - 4}
        .Draw(getGuiColor(BUTTON, BASE_COLOR_NORMAL));

    if (r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON) and r::Mouse::GetPosition().CheckCollision(loginInputBox)) {
        loginEditMode = true;
    }
    if ((1 == clicked or r::Keyboard::IsKeyPressed(KEY_ENTER))
                    and ((std::strlen(loginBuffer.c_str()) != 0 and std::strlen(passwordBuffer.c_str()) != 0)
                        // TODO: remove allowing to login without password after implementing Enter Screen
                or (std::strlen(loginBuffer.c_str()) != 0
                    and not std::empty(ctx().authToken)
                    and not std::empty(ctx().myPlayerId)))) {
        loginEditMode = false;
        ctx().myPlayerName = loginBuffer.c_str();
        ctx().password = passwordBuffer.c_str();
        passwordBuffer.clear();
        saveToLocalStoragePlayerName();
        setupWebsocket();
    }
}

[[nodiscard]] auto isVisible(auto& widget) -> bool
{
    if (r::Keyboard::IsKeyPressed(KEY_ESCAPE)) { widget.isVisible = false; }
    return widget.isVisible;
}

[[maybe_unused]] auto drawMessageBox(
    const r::Vector2& pos, const std::string& title, const std::string& message, const std::string& buttons) -> int;

auto drawLogoutMessage() -> void
{
    if (not isVisible(ctx().logoutMessage)) { return; }
    assert(ctx().isLoggedIn);
    static constexpr auto boxWidth = VirtualW / 5.f;
    static constexpr auto boxHeight = RAYGUI_TEXTINPUTBOX_HEIGHT;
    static constexpr auto messageBoxWidth = boxWidth + RAYGUI_TEXTINPUTBOX_BUTTON_PADDING * 2.f;
    static constexpr auto messageBoxHeight = boxHeight * 4.5f;
    static const auto messageBox = r::Rectangle{
        VirtualW - CardBorderMargin - CardWidth - messageBoxWidth - 20,
        BorderMargin + 170,
        messageBoxWidth,
        messageBoxHeight};
    const auto clicked = withGuiFont(ctx().fontS, [&] {
        const auto logoutText = ctx().localizeText(GameText::LogOut);
        const auto areYouSureText = ctx().localizeText(GameText::LogOutOfTheAccount);
        const auto yesNoText = std::format(
            "{};{}", //
            ctx().localizeText(GameText::Yes),
            ctx().localizeText(GameText::No));
        return GuiMessageBox(messageBox, logoutText.c_str(), areYouSureText.c_str(), yesNoText.c_str());
    });
    if (clicked == PREF_NO_CLICK) { return; }
    if (clicked == PREF_OK_CLICK) {
        sendLogout();
        ctx().authToken.clear();
        removeFromLocalStoragePlayerId();
        removeFromLocalStorageAuthToken();
        ctx().isLoggedIn = false;
        teardownAudioEngine();
        emscripten_websocket_close(ctx().ws, 1000, "Logout");
    }
    ctx().logoutMessage.isVisible = false;
}

auto drawTalonDiscardPopUp() -> void
{
    if (not ctx().talonDiscardPopUp.isVisible) { return; }
    const auto title = ctx().localizeText(GameText::ConfirmTitle);
    const auto message = ctx().localizeText(GameText::DiscardSelectedCards);
    const auto buttons = std::format(
        "{};{}", //
        ctx().localizeText(GameText::Yes),
        ctx().localizeText(GameText::No));
    const auto clicked = drawMessageBox({VirtualW * 0.5f, VirtualH * 0.45f}, title, message, buttons);
    if (clicked == PREF_NO_CLICK) { return; }
    if (clicked == PREF_OK_CLICK) {
        removeCardsFromHand(ctx().myPlayer(), ctx().discardedTalon);
        if (const auto isSixSpade = ctx().bidding.rank == 0; isSixSpade) {
            ctx().bidding.rank = AllRanks;
        } else if (const auto isNotAllPassed = ctx().bidding.rank != AllRanks; isNotAllPassed) {
            --ctx().bidding.rank;
        }
        ctx().bidding.isVisible = true;
    } else {
        ctx().discardedTalon.clear();
    }
    ctx().talonDiscardPopUp.isVisible = false;
    ctx().needsDraw = true;
}

[[maybe_unused]] auto drawMessageBox(
    const r::Vector2& pos, const std::string& title, const std::string& message, const std::string& buttons) -> int
{
    static constexpr auto boxWidth = VirtualW / 5.f;
    static constexpr auto boxHeight = RAYGUI_TEXTINPUTBOX_HEIGHT;
    static constexpr auto messageBoxWidth = boxWidth + RAYGUI_TEXTINPUTBOX_BUTTON_PADDING * 2.f;
    static constexpr auto messageBoxHeight = boxHeight * 4.5f;
    const auto rect = r::Rectangle{
        pos.x - messageBoxWidth * 0.5f, pos.y - messageBoxHeight * 0.5f, messageBoxWidth, messageBoxHeight};
    return withGuiFont(
        ctx().fontS, [&] { return GuiMessageBox(rect, title.c_str(), message.c_str(), buttons.c_str()); });
}

auto drawConnectedPlayers() -> void
{
    if (not ctx().isLoggedIn or ctx().isGameStarted) { return; }
    static constexpr auto pad = VirtualH * 0.007f;
    static constexpr auto fontSpacingX = VirtualH * 0.02f;
    static const auto& fontName = ctx().fontM;
    static const auto& fontAvatar = ctx().fontL;
    static const auto fontNameSize = ctx().fontSizeM();
    static const auto fontAvatarSize = ctx().fontSizeL() * 1.2f;
    const auto [rectColorDefault, textColorDefault] = std::invoke([&] -> std::pair<r::Color, r::Color> {
        const auto& scheme = ctx().settingsMenu.loadedColorScheme;
        if (scheme == "genesis") { return {getGuiColor(BORDER_COLOR_DISABLED), getGuiColor(TEXT_COLOR_FOCUSED)}; }
        if (scheme == "amber") { return {getGuiColor(TEXT_COLOR_NORMAL), getGuiColor(TEXT_COLOR_PRESSED)}; }
        if (scheme == "dark") { return {getGuiColor(BASE_COLOR_FOCUSED), getGuiColor(BASE_COLOR_NORMAL)}; }
        if (scheme == "cyber") { return {getGuiColor(TEXT_COLOR_NORMAL), getGuiColor(BASE_COLOR_DISABLED)}; }
        if (scheme == "jungle") { return {getGuiColor(TEXT_COLOR_NORMAL), getGuiColor(BASE_COLOR_NORMAL)}; }
        if (scheme == "lavanda") { return {getGuiColor(TEXT_COLOR_NORMAL), getGuiColor(BASE_COLOR_NORMAL)}; }
        if (scheme == "bluish") { return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(BORDER_COLOR_NORMAL)}; }
        return {getGuiColor(TEXT_COLOR_NORMAL), getGuiColor(BASE_COLOR_NORMAL)};
    });
    static constexpr auto avatars = std::array{PREF_MOUSE_FACE, PREF_MONKEY_FACE, PREF_COW_FACE, PREF_CAT_FACE};
    const auto players = rv::zip(
        pref::players() | rv::transform([](const auto& player) { return std::pair{player.id, player.name}; }), avatars);
    auto contentSizes = std::vector<r::Vector2>(std::size(players));
    auto maxSide = 0.0f;
    for (auto&& [i, playerWithAvatar] : players | rv::enumerate) {
        const auto& [player, avatar] = playerWithAvatar;
        const auto& [id, name] = player;
        const auto nameSize = fontName.MeasureText(name, fontNameSize, FontSpacing);
        const auto avatarSize = fontAvatar.MeasureText(avatar, fontAvatarSize, FontSpacing);
        const auto contentWidth = std::max(nameSize.x, avatarSize.x);
        const auto contentHeight = nameSize.y + avatarSize.y + pad * 1.5f;
        const auto requiredSide = std::max(contentWidth, contentHeight) + pad * 2.0f;
        contentSizes[i] = r::Vector2{contentWidth, contentHeight};
        maxSide = std::max(maxSide, requiredSide);
    }
    const auto playerSize = static_cast<float>(std::size(players));
    const auto totalWidth = maxSide * playerSize + fontSpacingX * (playerSize - 1.0f);
    const auto startX = (VirtualW - totalWidth) / 2.0f;
    const auto y = (VirtualH - maxSide) / 2.0f;
    auto x = startX;
    for (auto&& [player, avatar] : players) {
        const auto& [id, name] = player;
        const auto [rectColor, textColor] = std::invoke([&]() -> std::pair<r::Color, r::Color> {
            const auto& scheme = ctx().settingsMenu.loadedColorScheme;
            const auto state = ctx().player(id).readyCheckState;
            if (state == ReadyCheckState::ACCEPTED) {
                if (scheme == "dracula") { return {getGuiColor(TEXT_COLOR_FOCUSED), getGuiColor(BASE_COLOR_FOCUSED)}; }
                if (scheme == "genesis") { return {getGuiColor(TEXT_COLOR_PRESSED), getGuiColor(TEXT_COLOR_FOCUSED)}; }
                if (scheme == "amber") { return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(TEXT_COLOR_PRESSED)}; }
                if (scheme == "dark") { return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(TEXT_COLOR_FOCUSED)}; }
                if (scheme == "cyber") { return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(BASE_COLOR_DISABLED)}; }
                if (scheme == "jungle") { return {getGuiColor(TEXT_COLOR_FOCUSED), getGuiColor(BASE_COLOR_NORMAL)}; }
                if (scheme == "lavanda") { return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(BASE_COLOR_NORMAL)}; }
                if (scheme == "bluish") { return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(TEXT_COLOR_PRESSED)}; }
                return {getGuiColor(BASE_COLOR_PRESSED), getGuiColor(TEXT_COLOR_PRESSED)};
            }
            if (state == ReadyCheckState::DECLINED) {
                if (rng::contains(std::array{"dracula", "genesis", "cyber", "jungle", "lavanda"}, scheme)) {
                    return {getGuiColor(TEXT_COLOR_DISABLED), getGuiColor(BASE_COLOR_DISABLED)};
                }
                return {getGuiColor(BASE_COLOR_DISABLED), getGuiColor(TEXT_COLOR_DISABLED)};
            }
            return {rectColorDefault, textColorDefault};
        });
        drawRectangleWithBorder({x, y, maxSide, maxSide}, rectColor, textColor);
        const auto nameSize = fontName.MeasureText(name, fontNameSize, FontSpacing);
        const auto namePos = r::Vector2{x + (maxSide - nameSize.x) / 2.0f, y + maxSide - pad - nameSize.y};
        fontName.DrawText(name, namePos, fontNameSize, FontSpacing, textColor);
        const auto avatarSize = fontAvatar.MeasureText(avatar, fontAvatarSize, FontSpacing);
        const auto avatarPos = r::Vector2{x + (maxSide - avatarSize.x) * 0.5f, y + (maxSide - avatarSize.y) * 0.5f};
        fontAvatar.DrawText(avatar, avatarPos, fontAvatarSize, FontSpacing, textColor);
        x += maxSide + fontSpacingX;
    }
    drawGuiLabelCentered(ctx().localizeText(GameText::CurrentPlayers), {VirtualW * 0.5f, y - ctx().fontSizeM()});
    static const auto buttonW = (maxSide * static_cast<float>(NumberOfPlayers)) //
        + (fontSpacingX * (static_cast<float>(NumberOfPlayers) - 1.f));
    static const auto buttonH = maxSide * 0.5f;
    static const auto acceptDenyButtonsW = (buttonW - fontSpacingX) * 0.5f;
    static const auto buttonY = y + maxSide + (fontSpacingX * 2.f);

    // FIXME: After reconnection, startGameButton is incorectly always visible
    if (ctx().startGameButton.isVisible
        and GuiButton(
            {(VirtualW * 0.5f) - (buttonW * 0.5f), buttonY, buttonW, buttonH},
            ctx().localizeText(GameText::PLAY).c_str())) {
        sendReadyCheck(ReadyCheckState::REQUESTED);
        startReadyCheck(ctx().myPlayerId);
        return;
    }
    if (not ctx().readyCheckPopUp.isVisible) { return; }
    const auto isAccepted
        = GuiButton({startX, buttonY, acceptDenyButtonsW, buttonH}, ctx().localizeText(GameText::ACCEPT).c_str());
    const auto isDeclined = GuiButton(
        {startX + acceptDenyButtonsW + fontSpacingX, buttonY, acceptDenyButtonsW, buttonH},
        ctx().localizeText(GameText::DECLINE).c_str());
    if (not isAccepted and not isDeclined) { return; }
    ctx().readyCheckPopUp.isVisible = false;
    ctx().myPlayer().readyCheckState = (isAccepted) //
        ? ReadyCheckState::ACCEPTED
        : ReadyCheckState::DECLINED;
    sendReadyCheck(ctx().myPlayer().readyCheckState);
    evaluateReadyCheck(ctx().myPlayerId);
}

[[nodiscard]] auto isCardPlayable(const std::list<const Card*>& hand, const CardNameView clickedCardName) -> bool
{
    const auto clickedSuit = cardSuit(clickedCardName);
    const auto trump = getTrump(ctx().bidding.bid);
    const auto hasSuit = [&](const std::string_view suit) {
        return rng::any_of(hand, [&](const CardNameView name) { return cardSuit(name) == suit; }, &Card::name);
    };
    // first card in the trick: any card is allowed
    if (std::empty(ctx().cardsOnTable) and not ctx().passGameTalon.exists()) { return true; }
    if (clickedSuit == ctx().leadSuit) { return true; } // follows lead suit
    if (hasSuit(ctx().leadSuit)) { return false; } // must follow lead suit
    if (clickedSuit == trump) { return true; } //  no lead suit cards, playing trump
    if (hasSuit(trump)) { return false; } // // must play trump if you have it
    return true; // no lead or trump suit cards, free to play
}

[[nodiscard]] auto tintForCard(const bool canPlay) -> r::Color
{
    static const auto lightGray = r::Color{150, 150, 150, 255};
    return canPlay ? r::Color::White() : lightGray;
}

auto drawCardShineEffect(const bool isHovered, const r::Vector2& cardPosition) -> void
{
    if (not isHovered) { return; }
    static constexpr auto speed = 90.f;
    static constexpr auto stripeWidth = CardWidth / 5.f;
    static constexpr auto maxIntensity = 0.5f; // 0..1 alpha
    static constexpr auto fadeMargin = stripeWidth * 0.5f;
    static constexpr auto fadeEdge = CardWidth * 0.5f - fadeMargin;
    static constexpr auto marginY = CardHeight / 20.f;
    const auto time = static_cast<float>(ctx().window.GetTime());
    const auto x = std::fmod(time * speed, CardWidth - stripeWidth);
    const auto rect
        = r::Rectangle{cardPosition.x + x, cardPosition.y + marginY * 0.5f, stripeWidth, CardHeight - marginY};
    const auto cardCenterX = cardPosition.x + CardWidth * 0.5f;
    const auto stripeCenterX = cardPosition.x + x + stripeWidth * 0.5f;
    const auto distanceToCenter = std::abs(stripeCenterX - cardCenterX);
    const auto overallIntensity = std::invoke([&] { // clang-format off
        return std::clamp((distanceToCenter >= fadeEdge)
            ? 0.f : std::cos((distanceToCenter / fadeEdge) * std::numbers::pi_v<float> * 0.5f), 0.f, 1.f);
    }); // clang-format on
    const auto whiteColor = r::Color::White();
    const auto topLeft = whiteColor.Fade(0.f * overallIntensity);
    const auto bottomLeft = whiteColor.Fade(0.f * overallIntensity);
    const auto bottomRight = whiteColor.Fade(maxIntensity * overallIntensity);
    const auto topRight = whiteColor.Fade(maxIntensity * overallIntensity);
    BeginBlendMode(BLEND_ADDITIVE);
    rect.DrawGradient(topLeft, bottomLeft, bottomRight, topRight);
    EndBlendMode();
}

auto drawHandTurnAura(const r::Rectangle& cardsRect) -> void
{
    static constexpr auto roundness = 0.05f;
    static constexpr auto segments = 0;
    static constexpr auto margin = 2.f;
    static constexpr auto size = 12.f;
    static constexpr auto speed = 4.f;
    static constexpr auto thick = CardWidth / 35.f;
    const auto time = static_cast<float>(ctx().window.GetTime());
    const auto pulse = (std::sin(time * speed) + 1.f) * 0.5f;
    const auto ringColor = getGuiColor(BORDER_COLOR_NORMAL);
    (cardsRect + (margin + pulse * size)).DrawRoundedLines(roundness, segments, thick, ringColor.Fade(0.95f));
}

auto drawCards(const r::Vector2 pos, Player& player, const Shift shift) -> void
{
    using enum Shift;
    const auto isMyHand = ctx().myPlayerId == player.id;
    const auto isTheirHand = ctx().myPlayer().playsOnBehalfOf == player.id;
    const auto isRightTurn = (isMyHand and isMyTurn() and not isSomeonePlayingOnMyBehalf())
        or (isTheirHand and isMyTurnOnBehalfOfSomeone());
    if (isRightTurn
        and ctx().stage == GameStage::PLAYING
        and not std::empty(player.hand)
        and not ctx().isGameFreezed
        and not ctx().bidding.isVisible
        and not ctx().cardsOnTable.contains(player.id)) {
        const auto handSize = static_cast<float>(std::size(player.hand));
        const auto cardsRect = hasShift(shift, Horizont)
            ? r::Rectangle{pos.x, pos.y, (handSize - 1.f) * CardOverlapX + CardWidth, CardHeight}
            : r::Rectangle{pos.x, pos.y, CardWidth, (handSize - 1.f) * CardOverlapY + CardHeight};
        drawHandTurnAura(cardsRect);
    }
    const auto mousePos = r::Mouse::GetPosition();
    const auto toPos = [&](const auto i, const float offset = 0.f) {
        return hasShift(shift, Horizont) ? r::Vector2{pos.x + static_cast<float>(i) * CardOverlapX, pos.y + offset}
                                         : r::Vector2{pos.x + offset, pos.y + static_cast<float>(i) * CardOverlapY};
    };
    const auto hoveredIndex = std::invoke([&] {
        auto reversed = rv::iota(0, std::ssize(player.hand)) | rv::reverse;
        const auto it = rng::find_if(reversed, [&](const auto i) {
            const auto card = toPos(i);
            return mousePos.CheckCollision({card.x, card.y, CardWidth, CardHeight});
        });
        return it == rng::end(reversed) ? -1 : *it;
    });
    for (auto&& [i, card] : player.hand | rv::enumerate) {
        const auto isCardPlayable = pref::isCardPlayable(player.hand, card->name);
        const auto isTalonPicking = ctx().stage == GameStage::TALON_PICKING;
        const auto isTalonRevealPending = isMyHand and isTalonPicking and not std::empty(ctx().pendingTalonReveal);
        const auto isSelectedForTalon = isMyHand and isTalonPicking and isTalonDiscardSelected(card->name);
        const auto isTalonSelectionLocked
            = isMyHand and isTalonPicking and (std::size(ctx().discardedTalon) == 2);
        const auto isHovered = (not ctx().isGameFreezed and isRightTurn)
            and not isTalonRevealPending
            and (ctx().stage == GameStage::PLAYING or ctx().stage == GameStage::TALON_PICKING)
            and not ctx().bidding.isVisible
            and isCardPlayable
            and (not isTalonSelectionLocked or isSelectedForTalon)
            and static_cast<int>(i) == hoveredIndex;
        static constexpr auto Offset = CardHeight / 10.f;
        const auto offset
            = (isHovered or isSelectedForTalon) ? (hasShift(shift, Positive) ? Offset : -Offset) : 0.f;
        const auto cardPosition = toPos(i, offset);
        ctx().cardPositions[card->name] = cardPosition;
        const auto canPlay = not ctx().cardsOnTable.contains(player.id) and isCardPlayable;
        const auto canPlayWithTalon
            = isTalonSelectionLocked ? (isSelectedForTalon ? true : false) : canPlay;
        card->texture.Draw(cardPosition, tintForCard(canPlayWithTalon));
        drawCardShineEffect(isHovered, cardPosition);
    }
}

auto drawBackCard(const float x, const float y) -> void
{
    // TODO: draw 12 cards when a talon is in the hand
    const auto card = r::Rectangle{x, y, CardWidth, CardHeight};
    const auto roundness = 0.2f;
    const auto segments = 0; // auto
    const auto borderThick = CardWidth / 25.f;
    const auto border = r::Rectangle{
        card.x + borderThick * 0.5f, card.y + borderThick * 0.5f, card.width - borderThick, card.height - borderThick};
    const auto bottomLeft = r::Vector2{border.x, border.y + border.height};
    const auto bottomRight = r::Vector2{border.x + border.width, border.y + border.height};
    const auto topRight = r::Vector2{border.x + border.width, border.y};
    card.DrawRounded(roundness, segments, getGuiColor(BASE_COLOR_NORMAL));
    DrawTriangle(bottomLeft, bottomRight, topRight, getGuiColor(BASE_COLOR_DISABLED));
    border.DrawRoundedLines(roundness, segments, borderThick, getGuiColor(BORDER_COLOR_NORMAL));
}

auto drawBackCards(const int cardCount, const r::Vector2& pos) -> void
{
    for (auto i = 0; i < cardCount; ++i) { drawBackCard(pos.x, pos.y + static_cast<float>(i) * CardOverlapY); }
}

auto drawCards(const r::Vector2& pos, Player& player, const Shift shift, const int cardCount) -> void
{
    if (cardCount == 0) { return; }
    std::empty(player.hand) ? drawBackCards(cardCount, pos) : drawCards(pos, player, shift);
}

[[nodiscard]] auto drawGameText(const r::Vector2& pos, const std::string_view gameText, const Shift shift)
    -> std::pair<bool, r::Vector2>
{
    using enum Shift;
    const auto text = std::string{gameText};
    const auto textSize = measureGuiText(text);
    const auto sign = hasShift(shift, Negative) ? -1.f : 1.f;
    const auto shiftX = sign * (textSize.x * 0.44f + textSize.y * 0.5f);
    const auto shiftY = sign * textSize.y;
    const auto anchor = hasShift(shift, Horizont) ? r::Vector2{pos.x + shiftX, pos.y} //
                                                  : r::Vector2{pos.x, pos.y + shiftY};
    if (std::empty(gameText)) { return {false, anchor}; }
    drawGuiLabelCentered(text, anchor);
    return {true, anchor};
}

[[nodiscard]] auto composePlayerName(const Player& player) -> std::string
{
    return fmt::format(
        "{}{}{}",
        ctx().turnPlayerId == player.id and not ctx().isGameFreezed ? fmt::format("{} ", PREF_ARROW_RIGHT) : "",
        player.name,
        player.id == ctx().forehandId ? fmt::format(" {}", PREF_FOREHAND_SIGN) : "");
}

[[nodiscard]] auto drawPlayerName(const r::Vector2& pos, const Player& player, const Shift shift) -> r::Vector2
{
    const auto turnColor = std::invoke([&] {
        const auto& scheme = ctx().settingsMenu.loadedColorScheme;
        if (scheme == "dracula") { return TEXT_COLOR_FOCUSED; }
        if (scheme == "jungle") { return TEXT_COLOR_PRESSED; }
        if (scheme == "jungle") { return BORDER_COLOR_PRESSED; }
        if (scheme == "lavanda") { return BASE_COLOR_FOCUSED; }
        if (scheme == "bluish") { return TEXT_COLOR_PRESSED; }
        return BASE_COLOR_PRESSED;
    });
    const auto trick = player.tricksTaken != 0 ? fmt::format(" {}", prettifyNumber(player.tricksTaken)) : "";
    auto anchor = withGuiStyle(LABEL, TEXT_COLOR_NORMAL, turnColor, player.id == ctx().turnPlayerId, [&] {
        return drawGameText(pos, composePlayerName(player), shift).second;
    });
    withGuiFont(ctx().fontL, [&] {
        withGuiStyle(DEFAULT, TEXT_SIZE, static_cast<int>(ctx().fontSizeL() * 0.85f), [&] {
            const auto y = player.id != ctx().myPlayerId ? (CardHeight * 0.6f) : (CardHeight * 0.15f);
            return drawGameText({anchor.x, anchor.y - y}, trick, Shift::None);
        });
    });
    return anchor;
}

[[nodiscard]] auto getCodepoint(const char* text) -> int
{
    auto codepointSize = 0;
    return GetCodepoint(text, &codepointSize);
}

template<typename... Args>
[[nodiscard]] auto makeCodepoints(const Args&... args) -> auto
{
    return std::array{getCodepoint(args)...};
}

[[nodiscard]] auto offerGameText() -> GameText
{
    return isMiser() ? GameText::NoMoreForMe : GameText::RemainingMine;
}

auto drawOfferButton() -> void
{
    if (ctx().myPlayer().whistingChoice == PREF_PASS) { return; }

    static constexpr auto buttonH = VirtualH * 0.06f;
    static constexpr auto shift = 450.f; // TOOD: make reative or calculate properly
    static constexpr auto pad = VirtualH * 0.02f;
    static constexpr auto buttonY = VirtualH - shift;
    const auto textAccept = ctx().localizeText(GameText::ACCEPT);
    const auto textDecline = ctx().localizeText(GameText::DECLINE);
    const auto acceptDeclineW = (pad * 2.f)
        + std::max(ctx().fontM.MeasureText(textAccept, ctx().fontSizeM(), FontSpacing).x,
                   ctx().fontM.MeasureText(textDecline, ctx().fontSizeM(), FontSpacing).x);
    const auto totalW = acceptDeclineW * 2.f + pad;
    if (ctx().offerPopUp.isVisible) {
        assert(not ctx().offerButton.isPossible);
        const auto acceptX = (VirtualW * 0.5f) - (totalW * 0.5f);
        const auto handleOffer = [&](Offer offer) {
            ctx().offerPopUp.isVisible = false;
            ctx().myPlayer().offer = offer;
            sendMakeOffer(offer);
            whenOfferDeclined(ctx().myPlayerId);
        };
        if (GuiButton({acceptX, buttonY, acceptDeclineW, buttonH}, textAccept.c_str())) {
            handleOffer(Offer::OFFER_ACCEPTED);
        } else if (GuiButton({acceptX + pad + acceptDeclineW, buttonY, acceptDeclineW, buttonH}, textDecline.c_str())) {
            handleOffer(Offer::OFFER_DECLINED);
        }
        return;
    }
    if (ctx().offerButton.isVisible) {
        assert(ctx().offerButton.isPossible);
        assert(not ctx().offerPopUp.isVisible);
        const auto offerOrReveal = ctx().offerButton.beenClicked ? GameText::Offer : GameText::RevealCardsAndOffer;
        const auto offerText
            = fmt::format("{}: {}", ctx().localizeText(offerOrReveal), ctx().localizeText(offerGameText()));
        const auto offerW = (pad * 2.0f) + ctx().fontM.MeasureText(offerText.c_str(), ctx().fontSizeM(), FontSpacing).x;
        if (GuiButton({(VirtualW * 0.5f) - (offerW * 0.5f), VirtualH - shift, offerW, buttonH}, offerText.c_str())) {
            sendMakeOffer(Offer::OFFER_REQUESTED);
            ctx().offerButton.beenClicked = true;
            evaluateOffer(ctx().myPlayerId);
            return;
        }
    }
}

[[nodiscard]] auto drawYourTurn(
    const r::Vector2& pos, const float gap, const float totalWidth, const PlayerId& playerId) -> float
{
    const auto isPlayerTurnOnBehalfOfSomeone = pref::isPlayerTurnOnBehalfOfSomeone(playerId);
    const auto text = std::invoke([&] {
        if (isPlayerTurnOnBehalfOfSomeone) {
            const auto& playerName = ctx().player(ctx().player(playerId).playsOnBehalfOf).name;
            return fmt::format("{} {}", ctx().localizeText(GameText::YourTurnFor), playerName);
        }
        return ctx().localizeText(GameText::YourTurn);
    });
    const auto textSize = measureGuiText(text);
    const auto textX = pos.x + (totalWidth - textSize.x) * 0.5f;
    const auto textY = (pos.y + BidOriginY + BidMenuH - textSize.y) * 0.5f;
    if (ctx().isGameFreezed
        or ((not isPlayerTurn(playerId) or isSomeonePlayingOnBehalfOf(playerId))
            and not isPlayerTurnOnBehalfOfSomeone)) {
        return textY;
    }
    static constexpr auto shift = 110.0f; // TOOD: make reative or calculate properly
    const auto rect = r::Rectangle{textX + gap * 0.5f, textY - gap * 0.5f - shift, textSize.x + gap, textSize.y + gap};
    if (isPlayerTurnOnBehalfOfSomeone) {
        static const auto arrowSize = ctx().fontSizeXL();
        const auto [leftOpponentId, _] = getOpponentIds();
        const auto isLeft = leftOpponentId == ctx().player(playerId).playsOnBehalfOf;
        const auto arrowY = rect.y + rect.height - arrowSize;
        const auto arrow = isLeft ? r::Vector2{rect.x - arrowSize, arrowY} //
                                  : r::Vector2{rect.x + rect.width, arrowY};
        const auto arrowStyle
            = ctx().settingsMenu.loadedColorScheme == "dracula" ? BASE_COLOR_NORMAL : TEXT_COLOR_NORMAL;
        ctx().fontAwesomeXL.DrawText(
            isLeft ? LeftArrowIcon : RightArrowIcon, arrow, arrowSize, getGuiColor(arrowStyle));
    } else if (ctx().stage == GameStage::PLAYING) {
        static const auto arrowSize = ctx().fontSizeXL();
        const auto arrow = r::Vector2{rect.x + rect.width, rect.y};
        const auto arrowStyle
            = ctx().settingsMenu.loadedColorScheme == "dracula" ? BASE_COLOR_NORMAL : TEXT_COLOR_NORMAL;
        ctx().fontAwesomeXL.DrawText(DownArrowIcon, arrow, arrowSize * .9f, getGuiColor(arrowStyle));
    }
    GuiLabel(rect, text.c_str());
    return textY;
}

[[nodiscard]] auto drawWhist(const r::Vector2& pos, const Player& player, const Shift shift) -> bool
{
    return drawGameText(pos, ctx().localizeText(whistingChoiceToGameText(player.whistingChoice)), shift).first;
}

[[nodiscard]] auto cardCount(const std::list<const Card*>& hand, const DrawPosition drawPosition) -> int
{
    return not std::empty(hand) ? std::ssize(hand)
                                : (isRight(drawPosition) ? ctx().rightCardCount : ctx().leftCardCount);
}

auto drawBid(const r::Vector2& pos, const Player& player, const DrawPosition drawPosition, const Shift shift) -> bool
{
    return withGuiFont(ctx().fontL, [&] {
        const auto scale = cardCount(player.hand, drawPosition) < 10 ? 1.5f : 1.f;
        return withGuiStyle(DEFAULT, TEXT_SIZE, static_cast<int>(ctx().fontSizeM() * scale), [&] {
            return drawGameText(pos, ctx().localizeBid(player.bid), shift).first;
        });
    });
}

auto drawSpeechBubble(const r::Vector2& pos, const PlayerId& playerId, const DrawPosition drawPosition) -> void
{
    if (not ctx().speechBubbleMenu.text.contains(playerId) or std::empty(ctx().speechBubbleMenu.text[playerId])) {
        return;
    }
    drawSpeechBubbleText(pos, ctx().speechBubbleMenu.text[playerId], drawPosition);
    waitFor(
        5s,
        [](void* ud) {
            auto id = static_cast<PlayerId*>(ud);
            auto _ = gsl::finally([&] { delete id; });
            ctx().speechBubbleMenu.text.erase(*id);
        },
        new PlayerId{playerId});
}

auto drawMyHand() -> void
{
    using enum Shift;
    using enum DrawPosition;
    auto& player = ctx().myPlayer();
    const auto cardCount = std::ssize(player.hand);
    const auto totalWidth = (static_cast<float>(cardCount) - 1.f) * CardOverlapX + CardWidth;
    const auto cardFirstLeftTopPos
        = r::Vector2{(VirtualW - totalWidth) * 0.5f, VirtualH - CardHeight - MyCardBorderMarginY};
    const auto cardCenterY = cardFirstLeftTopPos.y + CardHeight * 0.5f;
    const auto cardFirstLeftCenterPos = r::Vector2{cardFirstLeftTopPos.x, cardCenterY};
    const auto cardLastRightCenterPos = r::Vector2{cardFirstLeftTopPos.x + totalWidth, cardCenterY};
    static constexpr auto gap = CardHeight / 27.f;
    drawCards(cardFirstLeftTopPos, player, Negative | Horizont);
    const auto playerNameCenter = drawPlayerName(cardFirstLeftCenterPos, player, Negative | Horizont);
    const auto yourTurnTopY = drawYourTurn(cardFirstLeftTopPos, gap, totalWidth, ctx().myPlayerId);
    drawWhist(cardLastRightCenterPos, player, Positive | Horizont) //
        or drawBid(cardLastRightCenterPos, player, {}, Positive | Horizont);
    drawSpeechBubble({playerNameCenter.x, yourTurnTopY + gap * 2}, ctx().myPlayerId, Left);
    static constexpr auto shift = 40.0f; // TOOD: make reative or calculate properly
    if (ctx().offerPopUp.isVisible) {
        const auto playerName = players()
            | rv::filter([](const Player& player) { return player.bid != PREF_PASS; })
            | rv::transform(&Player::name)
            | rv::join
            | ToString;
        assert(not std::empty(playerName));
        const auto offerText = fmt::format("{}: {}", playerName, ctx().localizeText(offerGameText()));
        drawGuiLabelCentered(offerText, {VirtualW * 0.5f, yourTurnTopY - shift});
    }
    // drawDebugHorzLine(cardCenterY, "cardCenterY");
    // drawDebugDot(cardFirstLeftCenterPos, "cardFirstLeftCenterPos");
    // drawDebugDot(cardLastRightCenterPos, "cardLastRightCenterPos");
    // drawDebugVertLine(playerNameCenter.x, "playerNameCenterX");
    // drawDebugHorzLine(yourTurnTopY, "yourTurnTopY");
    // drawDebugVertLine(VirtualW * 0.5f, "screenCenter");
    // drawDebugHorzLine(VirtualH * 0.5f, "screnCenter");
}

auto drawOpponentHand(const DrawPosition drawPosition) -> void
{
    using enum Shift;
    const auto playerId = std::invoke([&] {
        const auto [leftId, rightId] = getOpponentIds();
        return isRight(drawPosition) ? rightId : leftId;
    });
    auto& player = ctx().player(playerId);
    const auto cardCount = pref::cardCount(player.hand, drawPosition);
    const auto totalHeight = (static_cast<float>(cardCount) - 1.f) * CardOverlapY + CardHeight;
    const auto cardFirstLeftTopPos = r::Vector2{
        isRight(drawPosition) ? VirtualW - CardWidth - CardBorderMargin : CardBorderMargin,
        (VirtualH - totalHeight) * 0.5f};
    const auto cardCenterX = cardFirstLeftTopPos.x + CardWidth * 0.5f;
    const auto cardFirstCenterTopPos = r::Vector2{cardCenterX, cardFirstLeftTopPos.y};
    const auto cardLastCenterBottomPos = r::Vector2{cardCenterX, cardFirstLeftTopPos.y + totalHeight};
    drawCards(cardFirstLeftTopPos, player, (isRight(drawPosition) ? Negative : Positive) | Vertical, cardCount);
    const auto playerNameCenter = drawPlayerName(cardFirstCenterTopPos, player, Negative | Vertical);
    drawWhist(cardLastCenterBottomPos, player, Positive | Vertical) //
        or drawBid(cardLastCenterBottomPos, player, drawPosition, Positive | Vertical);
    drawSpeechBubble({playerNameCenter.x, cardFirstLeftTopPos.y - VirtualH / 11.f}, playerId, drawPosition);
    // drawDebugVertLine(cardCenterX, "cardCenterX");
    // drawDebugDot(cardFirstCenterTopPos, "cardFirstCenterTopPos");
    // drawDebugDot(cardLastCenterBottomPos, "cardLastCenterBottomPos");
    // drawDebugVertLine(playerNameCenter.x, "playerNameCenterX");
}

auto drawRightHand() -> void
{
    drawOpponentHand(DrawPosition::Right);
}

auto drawLeftHand() -> void
{
    drawOpponentHand(DrawPosition::Left);
}

auto drawPlayedCards() -> void
{
    const auto& slots = playSlots();
    const auto [leftOpponentId, rightOpponentId] = getOpponentIds();
    if (ctx().passGameTalon.exists()) { ctx().passGameTalon.get().texture.Draw(slots.top); }
    drawMovingCards();
    if (std::empty(ctx().cardsOnTable)) { return; }
    if (ctx().cardsOnTable.contains(leftOpponentId) and not isCardMoving(leftOpponentId)) {
        ctx().cardsOnTable.at(leftOpponentId)->texture.Draw(slots.left);
    }
    if (ctx().cardsOnTable.contains(rightOpponentId) and not isCardMoving(rightOpponentId)) {
        ctx().cardsOnTable.at(rightOpponentId)->texture.Draw(slots.right);
    }
    if (ctx().cardsOnTable.contains(ctx().myPlayerId) and not isCardMoving(ctx().myPlayerId)) {
        ctx().cardsOnTable.at(ctx().myPlayerId)->texture.Draw(slots.bottom);
    }
}

auto drawPendingTalonReveal() -> void
{
    if (std::empty(ctx().pendingTalonReveal)) { return; }
    static constexpr auto gap = CardWidth / 5.f;
    const auto count = static_cast<float>(std::size(ctx().pendingTalonReveal));
    const auto totalWidth = CardWidth * count + gap * std::max(0.f, count - 1.f);
    const auto x = (VirtualW - totalWidth) * 0.5f;
    const auto y = (VirtualH - CardHeight) * 0.5f;
    for (const auto [i, cardName] : ctx().pendingTalonReveal | rv::enumerate) {
        const auto pos = r::Vector2{x + static_cast<float>(i) * (CardWidth + gap), y};
        getCard(cardName).texture.Draw(pos);
    }
}

#define PREF_GUI_PROPERTY(PREF_PREFIX, PREF_STATE)                                                                     \
    std::invoke([&] {                                                                                                  \
        switch (PREF_STATE) {                                                                                          \
        case STATE_NORMAL: return PREF_PREFIX##_COLOR_NORMAL;                                                          \
        case STATE_DISABLED: return PREF_PREFIX##_COLOR_DISABLED;                                                      \
        case STATE_PRESSED: return PREF_PREFIX##_COLOR_PRESSED;                                                        \
        case STATE_FOCUSED: return PREF_PREFIX##_COLOR_FOCUSED;                                                        \
        }                                                                                                              \
        std::unreachable();                                                                                            \
    })

[[nodiscard]] constexpr auto isBidDisabled(
    const std::string_view bid,
    const std::string_view myBid,
    const std::size_t rank,
    const std::size_t currentRank,
    const bool finalBid) noexcept -> bool
{
    const auto myFirstBid = std::empty(myBid);
    return (bid == PREF_PASS and finalBid) // Pass final bid
        or (bid == PREF_NINE_WT and finalBid) // Nine WT not allowed as final bid
        or (bid != PREF_PASS and currentRank != AllRanks and currentRank >= rank) // Rank restriction
        or (bid == PREF_MISER and not myFirstBid and myBid != PREF_MISER) // Miser first bid restrictions
        or (bid == PREF_MISER_WT
            and not myFirstBid
            and myBid != PREF_MISER
            and myBid != PREF_MISER_WT) // Miser WT first bid restrictions
        or ((myBid == PREF_MISER or myBid == PREF_MISER_WT)
            and finalBid
            and bid != myBid) // Final bid restrictions for Miser/Miser WT
        or ((myBid == PREF_MISER or myBid == PREF_MISER_WT) // Non-final bid restrictions for Miser/Miser WT
            and not finalBid
            and bid != PREF_MISER_WT
            and bid != PREF_PASS)
        or ((myBid == PREF_NINE_WT) and finalBid and (bid == PREF_NINE_WT or bid == PREF_PASS));
}

auto drawRankAndSuitText(
    const std::string_view text,
    const r::Font& font,
    const float fontSize,
    const r::Vector2& pos,
    const r::Color rankColor,
    const r::Color suitColor) -> void
{
    if (std::size(text) < 2) [[unlikely]] { return; }
    const auto [rankText, suitText] = text.starts_with(PREF_TEN)
        ? std::pair{std::string{text.substr(0, 2)}, std::string{text.substr(2)}}
        : std::pair{std::string{text.substr(0, 1)}, std::string{text.substr(1)}};
    const auto rankSize = font.MeasureText(rankText, fontSize, FontSpacing);
    font.DrawText(rankText, pos, fontSize, FontSpacing, rankColor);
    font.DrawText(suitText, {pos.x + rankSize.x, pos.y}, fontSize, FontSpacing, suitColor);
}

auto sendBid(const std::string& bid, const bool finalBid, const std::size_t rank) -> void
{
    ctx().bidding.bid = bid;
    ctx().myPlayer().bid = bid;
    if (bid != PREF_PASS) { ctx().bidding.rank = rank; }
    ctx().bidding.isVisible = false;
    if (finalBid) {
        sendDiscardTalon(bid);
    } else {
        sendBidding(bid);
    }
}

auto drawBiddingMenu() -> void
{
    if (not ctx().bidding.isVisible) { return; }
    const auto finalBid = (std::size(ctx().discardedTalon) == 2) or (ctx().stage == WITHOUT_TALON);
    auto enabled = BidTable | rv::join | rv::filter([&](const std::string_view bid) {
                       return not std::empty(bid)
                           and not isBidDisabled(bid, ctx().myPlayer().bid, bidRank(bid), ctx().bidding.rank, finalBid);
                   });
    if (rng::distance(enabled) == 1U) {
        const auto bid = std::string{rng::back(enabled)};
        sendBid(bid, finalBid, bidRank(bid));
        return;
    }
    for (int r = 0; r < BidRows; ++r) {
        for (int c = 0; c < BidCols; ++c) {
            const auto& bid = BidTable[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (std::empty(bid)) { continue; }
            const auto rank = bidRank(bid);
            auto state = isBidDisabled(bid, ctx().myPlayer().bid, rank, ctx().bidding.rank, finalBid)
                ? GuiState{STATE_DISABLED}
                : GuiState{STATE_NORMAL};
            const auto pos = r::Vector2{
                BidOriginX + static_cast<float>(c) * (BidCellW + BidGap), //
                BidOriginY + static_cast<float>(r) * (BidCellH + BidGap)};
            const auto cell = r::Rectangle{pos, {BidCellW, BidCellH}};
            const auto clicked = std::invoke([&] {
                if ((state == STATE_DISABLED) or not r::Mouse::GetPosition().CheckCollision(cell)) { return false; }
                state = r::Mouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
                return r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
            });
            const auto textColor = getGuiColor(BUTTON, PREF_GUI_PROPERTY(TEXT, state));
            drawRectangleWithBorder(
                cell,
                getGuiColor(BUTTON, PREF_GUI_PROPERTY(BASE, state)),
                getGuiColor(BUTTON, PREF_GUI_PROPERTY(BORDER, state)));
            auto text = std::string{ctx().localizeBid(bid)};
            auto textSize = ctx().fontM.MeasureText(text, ctx().fontSizeM(), FontSpacing);
            const auto textX = cell.x + (cell.width - textSize.x) * 0.5f;
            const auto textY = cell.y + (cell.height - textSize.y) * 0.5f;
            if (isRedSuit(text)) {
                const auto suitColor = state == STATE_DISABLED ? redColor().Alpha(0.4f) : redColor();
                drawRankAndSuitText(text, ctx().fontM, ctx().fontSizeM(), {textX, textY}, textColor, suitColor);
            } else {
                ctx().fontM.DrawText(text, {textX, textY}, ctx().fontSizeM(), FontSpacing, textColor);
            }
            if (clicked and (state != STATE_DISABLED)) {
                sendBid(std::string{bid}, finalBid, rank);
                return;
            }
        }
    }
}

// TODO: implement Z-ordering of windows
auto drawMenu(
    const auto& allButtons,
    const bool isVisible,
    std::invocable<GameText> auto&& click,
    std::invocable<GameText> auto&& filterButton) -> void
{
    if (not isVisible) { return; }
    static constexpr auto cellW = VirtualW / 6.f;
    static constexpr auto cellH = cellW * 0.5f;
    static constexpr auto gap = cellH / 10.f;
    auto buttons = allButtons | rv::filter(filterButton);
    const auto buttonsCount = rng::distance(buttons);
    const auto menuW = static_cast<float>(buttonsCount) * cellW + static_cast<float>(buttonsCount - 1) * gap;
    const auto originX = (VirtualW - menuW) * 0.5f;
    const auto originY = (VirtualH - cellH) * 0.5f;
    for (auto&& [i, buttonName] : buttons | rv::enumerate) {
        auto state = GuiState{STATE_NORMAL};
        const auto pos = r::Vector2{originX + static_cast<float>(i) * (cellW + gap), originY};
        const auto rect = r::Rectangle{pos, {cellW, cellH}};
        const auto clicked = std::invoke([&] {
            if (not r::Mouse::GetPosition().CheckCollision(rect)) { return false; }
            state = r::Mouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
            return r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
        });
        const auto borderColor = getGuiColor(BUTTON, PREF_GUI_PROPERTY(BORDER, state));
        const auto bgColor = getGuiColor(BUTTON, PREF_GUI_PROPERTY(BASE, state));
        const auto textColor = getGuiColor(BUTTON, PREF_GUI_PROPERTY(TEXT, state));
        rect.Draw(bgColor);
        rect.DrawLines(borderColor, getGuiButtonBorderWidth());
        const auto text = ctx().localizeText(buttonName);
        const auto textSize = ctx().fontM.MeasureText(text, ctx().fontSizeM(), FontSpacing);
        const auto textX = rect.x + (rect.width - textSize.x) * 0.5f;
        const auto textY = rect.y + (rect.height - textSize.y) * 0.5f;
        ctx().fontM.DrawText(text, {textX, textY}, ctx().fontSizeM(), FontSpacing, textColor);
        if (clicked) { click(buttonName); }
    }
}

[[nodiscard]] auto buildMiserCards(const std::vector<CardName>& remaining, const std::vector<CardName>& played)
    -> std::array<std::string, 36>
{
    static constexpr auto ranksShort
        = std::array<std::string_view, 8>{PREF_SEVEN, PREF_EIGHT, PREF_NINE, PREF_TEN, "J", "Q", "K", "A"};
    static constexpr auto ranksLong = std::array<std::string_view, 8>{
        PREF_SEVEN, PREF_EIGHT, PREF_NINE, PREF_TEN, PREF_JACK, PREF_QUEEN, PREF_KING, PREF_ACE};
    static constexpr auto suitsShort = std::array<std::string_view, 4>{SpadeSign, ClubSign, DiamondSign, HeartSign};
    static constexpr auto suitsLong
        = std::array<std::string_view, 4>{PREF_SPADES, PREF_CLUBS, PREF_DIAMONDS, PREF_HEARTS};
    auto result = std::array<std::string, std::size(suitsLong) * (1 + std::size(ranksShort))>{};
    auto idx = 0uz;
    for (auto&& [shortSuit, longSuit] : rv::zip(suitsShort, suitsLong)) {
        result[idx++] = shortSuit;
        for (auto&& [shortRank, longRank] : rv::zip(ranksShort, ranksLong)) {
            const auto card = fmt::format("{}{}{}", longRank, PREF_OF_, longSuit);
            if (rng::contains(remaining, card)) {
                result[idx++] = shortRank;
            } else if (rng::contains(played, card)) {
                result[idx++] = fmt::format("{}*", shortRank);
            } else {
                idx++;
            }
        }
    }
    return result;
}

auto drawMiserCards() -> void
{
    if (not ctx().miserCardsPanel.isVisible) { return; }
    static const auto screenCenter = r::Vector2{VirtualW, VirtualH} * 0.5f;
    static const auto maxCellSize = ctx().fontM.MeasureText("10", ctx().fontSizeM(), FontSpacing);
    static const auto cellSize = maxCellSize.x * 1.1f;
    static const auto gap = cellSize * 0.2f;
    static constexpr auto cols = 9.f;
    static constexpr auto rows = 4.f;
    static const auto panelWidth = gap * (cols + 1.f) + cellSize * cols;
    static const auto panelHeight = gap * (rows + 1.f) + cellSize * rows;
    static const auto startX = screenCenter.x - (panelWidth * 0.5f);
    static const auto startY = BorderMargin;
    static const auto panel = r::Rectangle{startX, startY, panelWidth, panelHeight};
    const auto cards = buildMiserCards(ctx().miserCardsPanel.remaining, ctx().miserCardsPanel.played);
    const auto cardsView
        = std::mdspan{std::data(cards), static_cast<std::size_t>(rows), static_cast<std::size_t>(cols)};
    drawRectangleWithBorder(panel, getGuiColor(BACKGROUND_COLOR), getGuiColor(BUTTON, BORDER_COLOR_NORMAL));
    for (auto j = 0uz; j < cardsView.extent(0); ++j) {
        for (auto i = 0uz; i < cardsView.extent(1); ++i) {
            const auto pos = r::Vector2{
                startX + gap + ((cellSize + gap) * static_cast<float>(i)),
                startY + gap + ((cellSize + gap) * static_cast<float>(j))};
            const auto cell = r::Rectangle{pos, {cellSize, cellSize}};
            auto textCell = cardsView[static_cast<std::size_t>(j), static_cast<std::size_t>(i)];
            const auto isPlayed = textCell.ends_with('*');
            if (isPlayed) { textCell.pop_back(); }
            const auto state = std::empty(textCell) or isPlayed ? GuiState{STATE_DISABLED} : GuiState{STATE_NORMAL};
            if (i != 0) {
                drawRectangleWithBorder(
                    cell,
                    getGuiColor(BUTTON, PREF_GUI_PROPERTY(BASE, state)),
                    getGuiColor(BUTTON, PREF_GUI_PROPERTY(BORDER, state)));
            }
            if (not std::empty(textCell)) {
                const auto textSize = ctx().fontM.MeasureText(textCell, ctx().fontSizeM(), FontSpacing);
                const auto textPos = r::Vector2{
                    pos.x + (cellSize - textSize.x) * 0.5f, //
                    pos.y + (cellSize - textSize.y) * 0.5f};
                const auto textColor = (textCell == DiamondSign or textCell == HeartSign)
                    ? redColor()
                    : getGuiColor(BUTTON, PREF_GUI_PROPERTY(TEXT, state));
                ctx().fontM.DrawText(textCell, textPos, ctx().fontSizeM(), FontSpacing, textColor);
            }
        }
    }
}

auto drawWhistingOrMiserMenu() -> void
{
    const auto checkHalfWhist = [&](const GameText text) {
        return ctx().whisting.canHalfWhist ? text != GameText::Pass : text != GameText::HalfWhist;
    };
    const auto click = [&](GameText buttonName) {
        ctx().whisting.isVisible = false;
        const auto choice = localizeText(buttonName, GameLang::English);
        ctx().whisting.choice = std::invoke([&] {
            if (choice == PREF_TRUST) { return std::string{PREF_PASS}; }
            if (choice == PREF_CATCH) { return std::string{PREF_WHIST}; }
            return std::string{choice};
        });
        ctx().myPlayer().whistingChoice = ctx().whisting.choice;
        sendWhisting(ctx().whisting.choice);
    };
    if (isMiser()) {
        drawMenu(MiserButtons, ctx().whisting.isVisible, click, checkHalfWhist);
    } else {
        drawMenu(WhistingButtons, ctx().whisting.isVisible, click, checkHalfWhist);
    }
    // TODO: draw player's name whose bid is
    static const auto& font = ctx().fontL;
    static const auto fontSize = ctx().fontSizeL();
    static const auto pos = r::Vector2{VirtualW * 0.5f, BorderMargin + fontSize * 0.5f};
    const auto bids = players() | rv::transform(&Player::bid);
    const auto ranks = bids
        | rv::filter(notEqualTo(PREF_PASS))
        | rv::filter(notEqualTo(""))
        | rv::transform(&bidRank)
        | rng::to_vector;
    const auto bid = std::empty(ranks) ? ctx().bidding.bid : BidsRank[rng::max(ranks)];
    const auto isPassGame = rng::all_of(bids, equalTo(PREF_PASS));
    const auto text = isPassGame and ctx().bidding.passRound != 0
        ? ctx().localizeText(GameText::Passing) + prettifyNumber(ctx().bidding.passRound)
        : std::string{ctx().localizeBid(bid)};
    if (isRedSuit(text)) {
        const auto textSize = font.MeasureText(text, fontSize, FontSpacing);
        const auto textPos = r::Vector2{pos.x - textSize.x * 0.5f, pos.y - textSize.y * 0.5f};
        drawRankAndSuitText(text, font, fontSize, textPos, getGuiColor(TEXT_COLOR_NORMAL), redColor());
    } else {
        withGuiFont(font, [&] {
            withGuiStyle(DEFAULT, TEXT_SIZE, static_cast<int>(fontSize), [&] { drawGuiLabelCentered(text, pos); });
        });
    }
}

auto drawHowToPlayMenu() -> void
{
    drawMenu(
        HowToPlayButtons,
        ctx().howToPlay.isVisible,
        [&](auto buttonName) {
            ctx().howToPlay.isVisible = false;
            const auto choice = std::string{localizeText(buttonName, GameLang::English)};
            ctx().howToPlay.choice = choice;
            ctx().myPlayer().howToPlayChoice = choice;
            sendHowToPlay(choice);
        },
        []([[maybe_unused]] const GameText _) { return true; });
}

[[nodiscard]] auto isTalonDiscardSelected(const CardNameView name) -> bool
{
    return rng::contains(ctx().discardedTalon, name);
}

auto removeCardsFromHand(Player& player, const std::vector<CardNameView>& cardNames) -> void
{
    for (const auto name : cardNames) {
        const auto it = rng::find_if(player.hand, [&](const Card* card) { return card->name == name; });
        if (it == player.hand.end()) { continue; }
        ctx().cardPositions.erase(name);
        player.hand.erase(it);
    }
}

auto handleTalonCardClick(std::list<const Card*>& hand) -> void
{
    if (not r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) { return; }
    const auto mousePos = r::Mouse::GetPosition();
    const auto hit = [&](const Card* c) {
        return r::Rectangle{ctx().cardPositions[c->name], c->texture.GetSize()}.CheckCollision(mousePos);
    };
    const auto reversed = hand | rv::reverse;
    if (const auto rit = rng::find_if(reversed, hit); rit != rng::cend(reversed)) {
        const auto it = std::next(rit).base();
        const auto cardName = (*it)->name;
        if (const auto selectedIt = rng::find(ctx().discardedTalon, cardName);
            selectedIt != rng::end(ctx().discardedTalon)) {
            ctx().discardedTalon.erase(selectedIt);
            if (std::size(ctx().discardedTalon) < 2) { ctx().talonDiscardPopUp.isVisible = false; }
            ctx().needsDraw = true;
            return;
        }
        if (std::size(ctx().discardedTalon) >= 2) { return; }
        ctx().discardedTalon.push_back(cardName);
        if (std::size(ctx().discardedTalon) == 2) { ctx().talonDiscardPopUp.isVisible = true; }
        ctx().needsDraw = true;
    }
}

auto handleCardClick(
    std::list<const Card*>& hand,
    std::invocable<const Card&> auto act,
    std::function<void(const Card&, const r::Vector2&)> beforeErase = {}) -> void
{
    if (not r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) { return; }
    const auto mousePos = r::Mouse::GetPosition();
    const auto hit = [&](const Card* c) {
        return r::Rectangle{ctx().cardPositions[c->name], c->texture.GetSize()}.CheckCollision(mousePos);
    };
    const auto reversed = hand | rv::reverse;
    if (const auto rit = rng::find_if(reversed, hit); rit != rng::cend(reversed)) {
        const auto it = std::next(rit).base();
        const auto& cardName = (*it)->name;
        if (not isCardPlayable(hand, cardName)) {
            PREF_W("Can't play {}", PREF_V(cardName));
            return;
        }
        const auto posIt = ctx().cardPositions.find(cardName);
        if (posIt == ctx().cardPositions.end()) { return; }
        if (beforeErase) { beforeErase(**it, posIt->second); }
        const auto _ = gsl::finally([&] {
            ctx().cardPositions.erase(posIt);
            hand.erase(it);
            playSound(ctx().sound.placeCard);
        });
        act(**it);
    }
}

[[nodiscard]] auto makeCodepoints() -> std::vector<int>
{
    const auto ascii = rv::closed_iota(0x20, 0x7E); // space..~
    const auto cyrillic = rv::closed_iota(0x0410, 0x044F); // А..я
    const auto extras = makeCodepoints( // clang-format off
        "Ё", "ё", "Ґ", "ґ", "Є", "є", "І", "і", "Ї", "ї", "è", "é",
        "♠", "♣", "♦", "♥", "’", "—",
        PREF_SPADE, PREF_CLUB, PREF_HEART, PREF_DIAMOND, PREF_ARROW_RIGHT, PREF_FOREHAND_SIGN,
        PREF_NUMBER_01, PREF_NUMBER_02, PREF_NUMBER_03, PREF_NUMBER_04, PREF_NUMBER_05,
        PREF_NUMBER_06, PREF_NUMBER_07, PREF_NUMBER_08, PREF_NUMBER_09, PREF_NUMBER_10
    ); // clang-format on
    return rv::concat(ascii, cyrillic, extras) | rng::to_vector;
}

[[nodiscard]] auto makeCodepointsLarge() -> std::vector<int>
{
    const auto codepoints = makeCodepoints();
    const auto cards = rv::closed_iota(PREF_ACE_OF_SPADES_CARD, PREF_KING_OF_CLUBS_CARD);
    const auto avatars = makeCodepoints(PREF_MOUSE_FACE, PREF_COW_FACE, PREF_CAT_FACE, PREF_MONKEY_FACE);
    return rv::concat(codepoints, cards, avatars) | rng::to_vector;
}

[[nodiscard]] auto makeAwesomeCodepoints()
{
    return makeCodepoints(
        ScoreSheetIcon,
        SettingsIcon,
        EnterFullScreenIcon,
        ExitFullScreenIcon,
        SpeechBubbleIcon,
        OverallScoreboardIcon,
        LogoutIcon,
        HandshakeIcon,
        MicOnIcon,
        MicOffIcon);
}

[[nodiscard]] auto makeAwesomeLargeCodepoints()
{
    return std::array{LeftArrowIcon, RightArrowIcon, DownArrowIcon};
}

auto loadFonts() -> void
{
    static constexpr auto FontSizeS = static_cast<int>(VirtualH / 54.f);
    static constexpr auto FontSizeM = static_cast<int>(VirtualH / 30.f);
    static constexpr auto FontSizeL = static_cast<int>(VirtualH / 11.25f);
    static constexpr auto FontSizeXL = static_cast<int>(VirtualH / 6.f);
    const auto fontPath = fonts("DejaVuSans.ttf");
    const auto fontAwesomePath = fonts("Font-Awesome-7-Free-Solid-900.otf");
    auto codepoints = makeCodepoints();
    auto codepointsLarge = makeCodepointsLarge();
    auto awesomeCodepoints = makeAwesomeCodepoints();
    auto awesomeLargeCodepoints = makeAwesomeLargeCodepoints();
    ctx().fontS = LoadFontEx(fontPath.c_str(), FontSizeS, std::data(codepoints), std::ssize(codepoints));
    ctx().fontM = LoadFontEx(fontPath.c_str(), FontSizeM, std::data(codepoints), std::ssize(codepoints));
    ctx().fontL = LoadFontEx(fontPath.c_str(), FontSizeL, std::data(codepointsLarge), std::ssize(codepointsLarge));
    ctx().fontAwesome = LoadFontEx(
        fontAwesomePath.c_str(), FontSizeM - 1, std::data(awesomeCodepoints), std::ssize(awesomeCodepoints));
    ctx().fontAwesomeXL = LoadFontEx(
        fontAwesomePath.c_str(), FontSizeXL, std::data(awesomeLargeCodepoints), std::ssize(awesomeLargeCodepoints));
    SetTextureFilter(ctx().fontS.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx().fontM.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx().fontL.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx().fontAwesome.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx().fontAwesomeXL.texture, TEXTURE_FILTER_BILINEAR);
    setDefaultFont();
}

auto loadColorScheme(const std::string_view style) -> void
{
    // TODO: highlighted a loaded scheme at startup
    const auto name = style | ToLower | ToString;
    const auto stylePath = resources("styles", fmt::format("style_{}.rgs", name));
    GuiLoadStyle(stylePath.c_str());
    GuiSetStyle(LISTVIEW, SCROLLBAR_WIDTH, ScrollBarWidth);
    ctx().settingsMenu.loadedColorScheme = name;
    saveToLocalStorage("color_scheme", name);
}

[[nodiscard]] constexpr auto textToEnglish(const std::string_view text) noexcept -> std::string_view
{
    for (auto i = 0uz; i < std::to_underlying(GameText::Count); ++i) {
        const auto key = static_cast<GameText>(i);
        const auto eng = localizeText(key, GameLang::English);
        if (text == eng
            or text == localizeText(key, GameLang::Ukrainian)
            or text == localizeText(key, GameLang::Alternative)) {
            return eng;
        }
    }
    return text;
}

auto loadLang(const std::string_view lang) -> void
{
    const auto name = lang | ToLower | ToString;
    ctx().lang = std::invoke([&] {
        if (name == "ukrainian") { return pref::GameLang::Ukrainian; }
        if (name == "alternative") { return pref::GameLang::Alternative; }
        return pref::GameLang::English;
    });
    ctx().settingsMenu.loadedLang = name;
    saveToLocalStorage("language", name);
}

auto toggleMic() -> void
{
    ctx().microphone.isMuted = not ctx().microphone.isMuted;
    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
    EM_ASM({
        const mute = $0 !== 0;
        window.prefDesiredMute = mute;
        const stream = window.stream;
        if (stream && stream.getAudioTracks) {
            stream.getAudioTracks().forEach(track => {
                track.enabled = !mute;
            });
        }
        Module.ccall('pref_on_microphone_status', null, ['string', 'number'], [mute ? 'OFF' : 'ON', 0]);
    }, ctx().microphone.isMuted);
#pragma GCC diagnostic pop
    // clang-format on
    ctx().needsDraw = true;
}

auto drawToolbarButton(
    const int indexFromRight, const char* icon, const auto onClick, bool isBottom = true, bool isRight = true) -> void
{
    assert(indexFromRight >= 1);
    static constexpr auto buttonW = VirtualW / 30.f;
    static constexpr auto buttonH = buttonW;
    static constexpr auto gapBetweenButtons = BorderMargin / 5.f;
    const auto i = static_cast<float>(indexFromRight);
    const auto bounds = r::Rectangle{
        isRight ? VirtualW - buttonW * i - BorderMargin - gapBetweenButtons * (i - 1)
                : BorderMargin + buttonW * (i - 1) + gapBetweenButtons * (i - 1),
        isBottom ? VirtualH - buttonH - BorderMargin : BorderMargin,
        buttonW,
        buttonH};
    withGuiFont(ctx().fontAwesome, [&] {
        if (GuiButton(bounds, icon)) { onClick(); }
    });
}

auto drawMenuButton(auto& menu, const int pos, const char* icon, bool isBottom = true) -> void
{
    withGuiState(STATE_PRESSED, menu.isVisible, [&] {
        drawToolbarButton(pos, icon, [&] { menu.isVisible = not menu.isVisible; }, isBottom);
    });
}

auto drawFullScreenButton() -> void
{
    drawToolbarButton(1, ctx().window.IsFullscreen() ? ExitFullScreenIcon : EnterFullScreenIcon, [&] {
        ctx().window.ToggleFullscreen();
    });
}

auto drawSettingsButton() -> void
{
    drawMenuButton(ctx().settingsMenu, 2, SettingsIcon);
}

auto drawOverallScoreboardButton() -> void
{
    drawMenuButton(ctx().overallScoreboard, 3, OverallScoreboardIcon);
}

auto drawScoreSheetButton() -> void
{
    drawMenuButton(ctx().scoreSheet, 4, ScoreSheetIcon);
}

auto drawSpeechBubbleButton() -> void
{
    drawMenuButton(ctx().speechBubbleMenu, 5, SpeechBubbleIcon);
}

auto drawHandshakeOfferButton() -> void
{
    if (const auto& bid = ctx().myPlayer().bid;
        ctx().stage != GameStage::PLAYING or std::empty(bid) or bid == PREF_PASS) {
        return;
    }
    withGuiState(
        STATE_DISABLED, not ctx().offerButton.isPossible, [&] { drawMenuButton(ctx().offerButton, 6, HandshakeIcon); });
}

auto drawLogoutButton() -> void
{
    if (not ctx().isLoggedIn) { return; }
    withGuiState(STATE_PRESSED, ctx().logoutMessage.isVisible, [&] {
        drawToolbarButton(
            1,
            LogoutIcon,
            [&] {
                ctx().settingsMenu.isVisible = false;
                ctx().logoutMessage.isVisible = true;
            },
            false);
    });
}

auto drawMicButton() -> void
{
    withGuiState(STATE_DISABLED, ctx().microphone.isError, [&] {
        drawToolbarButton(1, ctx().microphone.isMuted ? MicOffIcon : MicOnIcon, [&] { toggleMic(); }, false, false);
    });
}

auto speechBubbleCooldown() -> void
{
    waitFor(1s, [] {
        --ctx().speechBubbleMenu.cooldown;
        if (ctx().speechBubbleMenu.cooldown <= 0) {
            ctx().speechBubbleMenu.cooldown = SpeechBubbleMenu::SendCooldownInSec;
            ctx().speechBubbleMenu.isSendButtonActive = true;
            return;
        }
        speechBubbleCooldown();
    });
}

auto drawSpeechBubbleMenu() -> void
{
    if (not isVisible(ctx().speechBubbleMenu)) { return; }
    static const auto phrases = pref::phrases()
        | rv::split('\n')
        | rv::transform([](auto&& rng) { return rng | ToString; })
        | rv::filter([](auto&& str) { return not std::empty(str) and not str.starts_with('#'); })
        | rng::to_vector;
    static const auto joinedPhrases = phrases | rv::intersperse(";") | rv::join | ToString;
    static auto scrollIndex = -1;
    static auto activeIndex = -1;
    static const auto listViewEntryH = (getStyle(LISTVIEW, LIST_ITEMS_HEIGHT) + getStyle(LISTVIEW, LIST_ITEMS_SPACING));
    static constexpr auto phrasesToShow = 11.f;
    static const auto phrasesListViewH = phrasesToShow * listViewEntryH + getStyle(DEFAULT, BORDER_WIDTH) * 4.f;
    const auto phrasesListView = r::Vector2{
        ctx().speechBubbleMenu.windowBoxPos.x + SpeechBubbleMenu::Margin,
        ctx().speechBubbleMenu.windowBoxPos.y + RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT + SpeechBubbleMenu::Margin};
    const auto sendButton
        = r::Vector2{phrasesListView.x, phrasesListView.y + phrasesListViewH + SpeechBubbleMenu::Margin};
    withGuiFont(ctx().fontS, [&] {
        const auto sendText = ctx().speechBubbleMenu.isSendButtonActive
            ? ctx().localizeText(GameText::Send)
            : fmt::format("{}", ctx().speechBubbleMenu.cooldown);
        const auto sendTextSize = measureGuiText(sendText);
        const auto sendButtonH = sendTextSize.y * 1.5f;
        static const auto phrasesWindowBoxH
            = (sendButton.y + sendButtonH + SpeechBubbleMenu::Margin) - ctx().speechBubbleMenu.windowBoxPos.y;
        const auto phrasesText = ctx().localizeText(GameText::Phrases);
        ctx().speechBubbleMenu.isVisible = not GuiWindowBox(
            {ctx().speechBubbleMenu.windowBoxPos.x,
             ctx().speechBubbleMenu.windowBoxPos.y,
             SpeechBubbleMenu::windowBoxW,
             phrasesWindowBoxH},
            phrasesText.c_str());
        withGuiStyle(LISTVIEW, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT, [&] {
            GuiListView(
                {phrasesListView.x, phrasesListView.y, SpeechBubbleMenu::ListViewW, phrasesListViewH},
                joinedPhrases.c_str(),
                &scrollIndex,
                &activeIndex);
            withGuiState(STATE_DISABLED, not ctx().speechBubbleMenu.isSendButtonActive, [&] {
                const auto sent = GuiButton(
                    {sendButton.x, sendButton.y, SpeechBubbleMenu::ListViewW, sendButtonH}, sendText.c_str());
                if (sent and activeIndex >= 0 and activeIndex < std::ssize(phrases)) {
                    const auto& phrase = phrases[static_cast<std::size_t>(activeIndex)];
                    sendSpeechBubble(phrase);
                    ctx().speechBubbleMenu.text.insert_or_assign(ctx().myPlayerId, std::move(phrase));
                    ctx().speechBubbleMenu.isSendButtonActive = false;
                    speechBubbleCooldown();
                }
            });
        });
    });
}

auto drawSettingsMenu() -> void
{
    if (not isVisible(ctx().settingsMenu)) { return; }
    const auto lang = std::vector{
        ctx().localizeText(GameText::English),
        ctx().localizeText(GameText::Ukrainian),
        ctx().localizeText(GameText::Alternative),
    };
    const auto colorScheme = std::vector{
        ctx().localizeText(GameText::Dracula),
        ctx().localizeText(GameText::Genesis),
        ctx().localizeText(GameText::Amber),
        ctx().localizeText(GameText::Dark),
        ctx().localizeText(GameText::Cyber),
        ctx().localizeText(GameText::Jungle),
        ctx().localizeText(GameText::Lavanda),
        ctx().localizeText(GameText::Bluish),
    };
    const auto joinedLangs = lang | rv::intersperse(";") | rv::join | ToString;
    const auto joinedColorScheme = colorScheme | rv::intersperse(";") | rv::join | ToString;
    static constexpr auto margin = VirtualH / 60.f;
    static constexpr auto groupBoxW = SettingsMenu::windowBoxW - margin * 2.f;
    static constexpr auto listViewW = SettingsMenu::windowBoxW - margin * 4.f;
    static const auto listViewEntryH = (getStyle(LISTVIEW, LIST_ITEMS_HEIGHT) + getStyle(LISTVIEW, LIST_ITEMS_SPACING));
    static const auto langListViewH
        = static_cast<float>(std::size(lang)) * listViewEntryH + getStyle(LISTVIEW, BORDER_WIDTH) * 4.f;
    static const auto langGroupBoxH = langListViewH + margin * 2.f;
    const auto langGroupBox = r::Vector2{
        ctx().settingsMenu.windowBoxPos.x + margin,
        ctx().settingsMenu.windowBoxPos.y + RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT + margin * 1.5f};
    const auto langListView = r::Vector2{langGroupBox.x + margin, langGroupBox.y + margin};
    static const auto colorSchemeListViewH
        = static_cast<float>(std::size(colorScheme)) * listViewEntryH + getStyle(LISTVIEW, BORDER_WIDTH) * 4.f;
    static const auto colorSchemeGroupBoxH = colorSchemeListViewH + margin * 2.f;
    const auto colorSchemeGroupBox
        = r::Vector2{langListView.x - margin, langListView.y + langListViewH + margin * 2.5f};
    const auto colorSchemeListView = r::Vector2{colorSchemeGroupBox.x + margin, colorSchemeGroupBox.y + margin};
    static constexpr auto otherGroupBoxH = margin * 4.f;
    const auto otherGroupBox
        = r::Vector2{colorSchemeListView.x - margin, colorSchemeListView.y + colorSchemeListViewH + margin * 2.5f};
    const auto fpsCheckbox = r::Vector2{otherGroupBox.x + margin, otherGroupBox.y + margin};
    const auto soundEffectsCheckbox = r::Vector2{otherGroupBox.x + margin, fpsCheckbox.y + margin * 1.5f};
    static const auto windowBoxH = (fpsCheckbox.y + otherGroupBoxH) - ctx().settingsMenu.windowBoxPos.y;
    withGuiFont(ctx().fontS, [&] {
        const auto settingsText = ctx().localizeText(GameText::Settings);
        const auto languageText = ctx().localizeText(GameText::Language);
        const auto colorSchemeText = ctx().localizeText(GameText::ColorScheme);
        const auto otherText = ctx().localizeText(GameText::Other);
        const auto fpsText = ctx().localizeText(GameText::ShowFps);
        ctx().settingsMenu.isVisible = not GuiWindowBox(
            {ctx().settingsMenu.windowBoxPos.x,
             ctx().settingsMenu.windowBoxPos.y,
             SettingsMenu::windowBoxW,
             windowBoxH},
            settingsText.c_str());
        if (ctx().settingsMenu.isVisible) { ctx().logoutMessage.isVisible = false; }
        GuiGroupBox({langGroupBox.x, langGroupBox.y, groupBoxW, langGroupBoxH}, languageText.c_str());
        GuiListView(
            {langListView.x, langListView.y, listViewW, langListViewH},
            joinedLangs.c_str(),
            &ctx().settingsMenu.langIdScroll,
            &ctx().settingsMenu.langIdSelect);
        if (ctx().settingsMenu.langIdSelect >= 0
            and ctx().settingsMenu.langIdSelect < std::ssize(lang)
            and r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (const auto langToLoad = textToEnglish(lang[static_cast<std::size_t>(ctx().settingsMenu.langIdSelect)]);
                ctx().settingsMenu.loadedLang != langToLoad) {
                loadLang(langToLoad);
                updateOverallScoreboardTable();
            }
        }
        GuiGroupBox(
            {colorSchemeGroupBox.x, colorSchemeGroupBox.y, groupBoxW, colorSchemeGroupBoxH}, colorSchemeText.c_str());
        GuiListView(
            {colorSchemeListView.x, colorSchemeListView.y, listViewW, colorSchemeListViewH},
            joinedColorScheme.c_str(),
            &ctx().settingsMenu.colorSchemeIdScroll,
            &ctx().settingsMenu.colorSchemeIdSelect);
        if (ctx().settingsMenu.colorSchemeIdSelect >= 0
            and ctx().settingsMenu.colorSchemeIdSelect < std::ssize(colorScheme)
            and r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (const auto colorSchemeToLoad
                = textToEnglish(colorScheme[static_cast<std::size_t>(ctx().settingsMenu.colorSchemeIdSelect)]);
                ctx().settingsMenu.loadedColorScheme != colorSchemeToLoad) {
                loadColorScheme(colorSchemeToLoad);
            }
        }
        GuiGroupBox({otherGroupBox.x, otherGroupBox.y, groupBoxW, otherGroupBoxH}, otherText.c_str());
        const auto showPingAndFps = ctx().settingsMenu.showPingAndFps;
        GuiCheckBox(
            {fpsCheckbox.x, fpsCheckbox.y, margin, margin}, fpsText.c_str(), &ctx().settingsMenu.showPingAndFps);
        if (showPingAndFps != ctx().settingsMenu.showPingAndFps) {
            if (ctx().settingsMenu.showPingAndFps) {
                saveToLocalStorage("show_ping_and_fps", "true");
            } else {
                removeFromLocalStorage("show_ping_and_fps");
            }
        }
        const auto soundEffects = ctx().settingsMenu.soundEffects;
        const auto soundEffectsText = ctx().localizeText(GameText::SoundEffects);
        GuiCheckBox(
            {soundEffectsCheckbox.x, soundEffectsCheckbox.y, margin, margin},
            soundEffectsText.c_str(),
            &ctx().settingsMenu.soundEffects);
        if (soundEffects != ctx().settingsMenu.soundEffects) {
            if (ctx().settingsMenu.soundEffects) {
                removeFromLocalStorage("sound_effects");
            } else {
                saveToLocalStorage("sound_effects", "false");
                ctx().sound.withoutSoundEffects();
            }
        }
    });
}

auto drawScoreSheet() -> void
{
    if (not isVisible(ctx().scoreSheet)) { return; }
    static constexpr auto fSpace = FontSpacing;
    static constexpr auto rotateL = 90.f;
    static constexpr auto rotateR = 270.f;
    static const auto center = r::Vector2{VirtualW * 0.5f, VirtualH * 0.5f};
    static const auto sheetS = center.y * 1.45f;
    static const auto sheet = r::Vector2{center.x - sheetS * 0.5f, center.y - sheetS * 0.5f};
    static const auto radius = sheetS / 20.f;
    static const auto posm = 2.f / 9.f;
    static const auto poss = 5.f / 18.f;
    static const auto rl = r::Rectangle{sheet.x, sheet.y, sheetS, sheetS};
    static const auto rm
        = r::Rectangle{rl.x + sheetS * posm, rl.y, sheetS - sheetS * posm * 2.f, sheetS - sheetS * posm};
    static const auto rs
        = r::Rectangle{rl.x + sheetS * poss, rl.y, sheetS - sheetS * poss * 2.f, sheetS - sheetS * poss};
    static const auto fSize = ctx().fontSizeS();
    static const auto gap = fSize / 4.f;
    static const auto& font = ctx().fontS;
    const auto thick = getGuiButtonBorderWidth();
    const auto borderColor = getGuiColor(BORDER_COLOR_NORMAL);
    const auto sheetColor = getGuiColor(BASE_COLOR_NORMAL);
    const auto c = getGuiColor(LABEL, TEXT_COLOR_NORMAL);
    rl.Draw(sheetColor);
    rl.DrawLines(borderColor, thick);
    rs.DrawLines(borderColor, thick);
    rm.DrawLines(borderColor, thick);
    center.DrawLine({rl.x + thick, rl.y + rl.height - thick}, thick, borderColor);
    center.DrawLine({rl.x + rl.width - thick, rl.y + rl.height - thick}, thick, borderColor);
    center.DrawLine({rl.x + rl.width * 0.5f, rl.y}, thick, borderColor);
    center.DrawLine({rl.x + rl.width * 0.5f, rl.y}, thick, borderColor);
    center.DrawCircle(radius, borderColor);
    center.DrawCircle(radius - thick, sheetColor);
    r::Vector2{rl.x, center.y}.DrawLine({rm.x, center.y}, thick, borderColor);
    r::Vector2{rl.x + rl.width, center.y}.DrawLine({rm.x + rm.width, center.y}, thick, borderColor);
    r::Vector2{center.x, rl.y + rl.height}.DrawLine({center.x, rm.y + rm.height}, thick, borderColor);
    static const auto scoreTarget = fmt::format("{}", ScoreTarget);
    static const auto scoreTargetSize = ctx().fontM.MeasureText(scoreTarget, ctx().fontSizeM(), fSpace);
    ctx().fontM.DrawText(
        scoreTarget,
        {center.x - scoreTargetSize.x * 0.5f, center.y - scoreTargetSize.y * 0.5f},
        ctx().fontSizeM(),
        fSpace,
        c);
    const auto joinValues = [](const auto& values) {
        return fmt::format("{}", fmt::join(values | rv::filter(notEqualTo(0)) | rv::partial_sum(std::plus{}), "."));
    };
    const auto [leftId, rightId] = getOpponentIds();
    for (const auto& [playerId, score] : ctx().scoreSheet.score) {
        const auto resultValue = std::invoke([&]() -> std::optional<std::tuple<std::string, r::Vector2, r::Color>> {
            const auto finalResult = calculateFinalResult(makeFinalScore(ctx().scoreSheet.score));
            if (not finalResult.contains(playerId)) { return std::nullopt; }
            const auto value = finalResult.at(playerId);
            const auto result = fmt::format("{}{}", value > 0 ? "+" : "", value);
            return std::tuple{
                result,
                ctx().fontM.MeasureText(result, ctx().fontSizeM(), fSpace),
                result.starts_with('-') ? redColor() : (result.starts_with('+') ? greenColor() : c)};
        });
        if (playerId == ctx().myPlayerId) {
            if (resultValue) {
                const auto& [result, resultSize, color] = *resultValue;
                ctx().fontM.DrawText(
                    result, {center.x - (resultSize.x * 0.5f), center.y + radius}, ctx().fontSizeM(), fSpace, color);
            }
            const auto dumpValues = joinValues(score.dump);
            const auto poolValues = joinValues(score.pool);
            font.DrawText(dumpValues, {rs.x + radius, rs.y + rs.height - fSize}, fSize, fSpace, c);
            // TODO: properly center `poolValues` (here and below) instead of adding `gap * 0.5f`
            font.DrawText(poolValues, {rs.x + gap, rs.y + rs.height + gap * 0.5f}, fSize, fSpace, c);
            for (const auto& [toWhomWhistId, whist] : score.whists) {
                const auto whistValues = joinValues(whist);
                if (toWhomWhistId == leftId) {
                    font.DrawText(whistValues, {rm.x + gap, rm.y + rm.height}, fSize, fSpace, c);
                } else if (toWhomWhistId == rightId) {
                    font.DrawText(whistValues, {center.x + gap, rm.y + rm.height}, fSize, fSpace, c);
                }
            }
        } else if (playerId == rightId) {
            if (resultValue) {
                const auto& [result, resultSize, color] = *resultValue;
                ctx().fontM.DrawText(
                    result,
                    {center.x + radius, center.y + (resultSize.x * 0.5f)},
                    {},
                    rotateR,
                    ctx().fontSizeM(),
                    fSpace,
                    color);
            }
            const auto dumpValues = joinValues(score.dump);
            const auto poolValues = joinValues(score.pool);
            font.DrawText(
                dumpValues, {rs.x + rs.width - fSize, rs.y + rs.height - radius}, {}, rotateR, fSize, fSpace, c);
            font.DrawText(
                poolValues, {rs.x + rs.width + gap * 0.5f, rs.y + rs.height - gap}, {}, rotateR, fSize, fSpace, c);
            for (const auto& [toWhomWhistId, whist] : score.whists) {
                const auto whistValues = joinValues(whist);
                if (toWhomWhistId == ctx().myPlayerId) {
                    font.DrawText(
                        whistValues, {rm.x + rm.width, rm.y + rm.height - gap}, {}, rotateR, fSize, fSpace, c);
                } else if (toWhomWhistId == leftId) {
                    font.DrawText(whistValues, {rm.x + rm.width, center.y - gap}, {}, rotateR, fSize, fSpace, c);
                }
            }
        } else if (playerId == leftId) {
            if (resultValue) {
                const auto& [result, resultSize, color] = *resultValue;
                ctx().fontM.DrawText(
                    result,
                    {center.x - radius, center.y - (resultSize.x * 0.5f)},
                    {},
                    rotateL,
                    ctx().fontSizeM(),
                    fSpace,
                    color);
            }
            const auto dumpValues = joinValues(score.dump);
            const auto poolValues = joinValues(score.pool);
            font.DrawText(dumpValues, {rs.x + fSize, rl.y + gap}, {}, rotateL, fSize, fSpace, c);
            font.DrawText(poolValues, {rs.x - gap * 0.5f, rs.y + gap}, {}, rotateL, fSize, fSpace, c);
            for (const auto& [toWhomWhistId, whist] : score.whists) {
                const auto whistValues = joinValues(whist);
                if (toWhomWhistId == ctx().myPlayerId) {
                    font.DrawText(whistValues, {rm.x, center.y + gap}, {}, rotateL, fSize, fSpace, c);
                } else if (toWhomWhistId == rightId) {
                    font.DrawText(whistValues, {rm.x, rm.y + gap}, {}, rotateL, fSize, fSpace, c);
                }
            }
        }
    }
}

auto drawOverallScoreboard() -> void
{
    if (not isVisible(ctx().overallScoreboard) or std::size(ctx().overallScoreboard.table) < 2) { return; }
    static constexpr auto maxVisibleRowCount = 13.f;
    static const auto cellSize = r::Vector2{VirtualW / 10.f, RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT};
    static auto panelView = r::Rectangle{};
    static auto panelScroll = r::Vector2{};
    const auto rowCount = std::max(2.f, static_cast<float>(std::size(ctx().overallScoreboard.table) - 2));
    const auto panel = cellSize
        * r::Vector2{
            static_cast<float>(std::size(ctx().overallScoreboard.table[0])), std::min(maxVisibleRowCount, rowCount)};
    const auto windowBoxSize = panel + r::Vector2{cellSize.y * 2.f, cellSize.y * 5.f};
    if (r::Vector2::Zero() == ctx().overallScoreboard.windowBoxPos) {
        ctx().overallScoreboard.windowBoxPos = (r::Vector2{VirtualW, VirtualH} - windowBoxSize) * 0.5f;
    }
    const auto windowBox = r::Rectangle{ctx().overallScoreboard.windowBoxPos, windowBoxSize};
    if (0.f == ctx().overallScoreboard.windowBoxW) { ctx().overallScoreboard.windowBoxW = windowBox.width; }
    const auto scrollPanel = r::Rectangle{{windowBox.x + cellSize.y, windowBox.y + cellSize.y * 4.f}, panel};
    const auto content = r::Rectangle{
        scrollPanel.x,
        scrollPanel.y,
        scrollPanel.width - cellSize.y,
        rowCount * cellSize.y - getGuiButtonBorderWidth()};
    withGuiFont(ctx().fontS, [&] {
        const auto title = ctx().localizeText(GameText::OverallScoreboard);
        ctx().overallScoreboard.isVisible = not GuiWindowBox(windowBox, title.c_str());
        GuiScrollPanel(scrollPanel, nullptr, content, &panelScroll, &panelView);

        for (auto&& [j, row] : ctx().overallScoreboard.table | rv::enumerate) {
            for (auto&& [i, cell] : row | rv::enumerate) {
                const auto color = std::invoke([&] {
                    const auto& scheme = ctx().settingsMenu.loadedColorScheme;
                    static constexpr auto GoodWinrate = 51;
                    static constexpr auto BadWinrate = 39;
                    if (cell.starts_with('+')
                        or cell.starts_with(ctx().localizeText(GameText::Win))
                        or (cell.contains('%') and std::stoi(cell) >= GoodWinrate)) {
                        if (scheme == "bluish") { return r::Color::DarkGreen(); }
                        if (scheme == "dracula") { return greenColor(); }
                        return r::Color::Lime();
                    }
                    if ((cell.starts_with('-') and "-" != cell)
                        or cell.starts_with(ctx().localizeText(GameText::Lost))
                        or (cell.contains('%') and std::stoi(cell) <= BadWinrate)) {
                        if (scheme == "bluish") { return r::Color::Maroon(); }
                        if (scheme == "lavanda") { return r::Color::Pink(); }
                        return redColor();
                    }
                    if ("0" == cell
                        or cell.starts_with(ctx().localizeText(GameText::Draw))
                        or (cell.contains('%') and std::stoi(cell) < GoodWinrate and std::stoi(cell) > BadWinrate)) {
                        if (scheme == "dracula") { return r::Color{255, 184, 108}; }
                        return r::Color::Orange();
                    }
                    return getGuiColor(LABEL, TEXT_COLOR_NORMAL);
                });
                const auto cellPos = r::Vector2{
                    windowBox.x + cellSize.y + cellSize.x * static_cast<float>(i),
                    windowBox.y + cellSize.y * (2.f + static_cast<float>(j))};
                if (j == 0 or j == 1) {
                    GuiStatusBar(r::Rectangle{cellPos, cellSize}, cell.c_str());
                    continue;
                }
                if (j == 2) {
                    BeginScissorMode(
                        static_cast<int>(panelView.x),
                        static_cast<int>(panelView.y),
                        static_cast<int>(panelView.width),
                        static_cast<int>(panelView.height));
                }
                withGuiStyle(LABEL, TEXT_PADDING, GuiGetStyle(STATUSBAR, TEXT_PADDING), [&] {
                    withGuiStyle(LABEL, TEXT_COLOR_NORMAL, color, [&] {
                        GuiLabel(r::Rectangle{cellPos + panelScroll, cellSize}, cell.c_str());
                    });
                });
            }
        }
        EndScissorMode();
    });
}

[[nodiscard]] auto measureTextCodepoints(
    const r::Font& font, const std::span<const int> codepoints, const float fontSize, const float spacing) -> r::Vector2
{
    if (std::empty(codepoints)) { return {}; }
    const auto scale = fontSize / static_cast<float>(font.baseSize);
    auto width = 0.0f;
    for (const auto [i, cp] : codepoints | rv::enumerate) {
        const auto glyph = GetGlyphInfo(font, cp);
        const auto advance = glyph.advanceX > 0 ? static_cast<float>(glyph.advanceX) //
                                                : static_cast<float>(font.baseSize) * 0.5f;
        width += advance * scale;
        if (i < std::size(codepoints) - 1) { width += spacing * scale; };
    }
    return {width, fontSize};
}

auto drawLastTrickOrTalon() -> void
{
    if (std::empty(ctx().lastTrickOrTalon)) { return; }
    static constexpr auto cardCenterX = CardBorderMargin + CardWidth * 0.5f;
    static constexpr auto fontSize = CardHeight / 2.88f;
    static constexpr auto y = VirtualH - BorderMargin - fontSize;
    const auto lastTrickOrTalon = ctx().lastTrickOrTalon | rv::transform(&cardNameCodepoint) | rng::to_vector;
    const auto size = measureTextCodepoints(ctx().fontL, lastTrickOrTalon, fontSize, FontSpacing);
    const auto x = cardCenterX - size.x * 0.5f;
    ctx().fontL.DrawText(
        std::data(lastTrickOrTalon),
        std::ssize(lastTrickOrTalon),
        {x, y},
        fontSize,
        FontSpacing,
        getGuiColor(TEXT_COLOR_NORMAL));
}

[[nodiscard]] auto drawFps(const float y, const Font& font, const float fontSize, const float fontSpacing) -> float
{
    static constexpr auto borderX = VirtualW - BorderMargin;
    const auto fps = GetFPS();
    const auto fpsColor = std::invoke([&] {
        if (fps < 15) { return redColor(); }
        if (fps < 30) { return r::Color::Orange(); }
        if (ctx().settingsMenu.loadedColorScheme == "dracula") { return greenColor(); }
        return r::Color::Lime();
    });
    const auto fpsText = r::Text{fmt::format(" FPS {}", fps), fontSize, fpsColor, font, fontSpacing};
    const auto fpsX = borderX - fpsText.MeasureEx().x;
    fpsText.Draw({fpsX, y});
    return fpsX;
}

auto drawPing(const r::Vector2& pos, const Font& font, const float fontSize, const float fontSpacing) -> void
{
    if (static_cast<int>(ctx().ping.rtt) == Ping::InvalidRtt) { return; }
    const auto ping = static_cast<int>(ctx().ping.rtt);
    const auto pingColor = std::invoke([&] {
        if (ping <= 200) {
            if (ctx().settingsMenu.loadedColorScheme == "dracula") { return greenColor(); }
            return r::Color::Lime();
        }
        if (ping <= 600) { return r::Color::Orange(); }
        return redColor();
    });
    const auto pingText
        = r::Text{fmt::format("PING {} MS ", ping == 0 ? 1 : ping), fontSize, pingColor, font, fontSpacing};
    static const auto delimText = r::Text{"|", fontSize, getGuiColor(TEXT_COLOR_NORMAL), font, fontSpacing};
    const auto delimX = pos.x - delimText.MeasureEx().x;
    const auto pingX = delimX - pingText.MeasureEx().x;
    pingText.Draw({pingX, pos.y});
    delimText.Draw({delimX, pos.y});
}

[[nodiscard]] auto audioPeerIds() -> std::vector<std::string>
{
    auto ids = std::vector<std::string>{};
    for (const auto& playerId : ctx().players | rv::keys) {
        if (playerId == ctx().myPlayerId) { continue; }
        ids.push_back(playerId);
    }
    return ids;
}

[[nodiscard]] auto peerIdsAsJson() -> std::string
{
    const auto ids = audioPeerIds();
    auto json = std::string{"["};
    for (auto&& [i, id] : ids | rv::enumerate) {
        if (i != 0) { json += ","; }
        json += fmt::format("\"{}\"", id);
    }
    json += "]";
    return json;
}

auto syncAudioPeers() -> void
{
    if (std::empty(ctx().myPlayerId)) { return; }
    const auto peersJson = peerIdsAsJson();
    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
    EM_ASM(
        {
            if (!Module.prefAudio) { return; }
            Module.prefAudio.setPeers(JSON.parse(UTF8ToString($0)));
        },
        peersJson.c_str());
#pragma GCC diagnostic pop
    // clang-format on
}

auto teardownAudioEngine() -> void
{
    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma GCC diagnostic ignored "-Winvalid-pp-token"
    EM_ASM({
        if (Module.prefAudio) { Module.prefAudio.setPeers([]); Module.prefAudio.setSelf(''); }
        if (window.stream && window.stream.getTracks) {
            window.stream.getTracks().forEach(track => { try { track.stop(); } catch (_) {} });
        }
        window.stream = null;
        if (Module.prefAudio && Module.prefAudio.stopLocalStream) { Module.prefAudio.stopLocalStream(); }
    });
#pragma GCC diagnostic pop
    // clang-format on
}

auto initAudioEngine() -> void
{
    PREF_I();
    if (std::empty(ctx().myPlayerId)) { return; }
    const auto peersJson = peerIdsAsJson();
    // clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma GCC diagnostic ignored "-Winvalid-pp-token"
    EM_ASM({
        if (!Module.prefAudio) {
            Module.prefAudio = (function () {
                const peers = new Map();
                const audios = new Map();
                let selfId = '';
                let localStreamPromise = null;
                let desiredPeers = new Set();

                function log(...args) {
                    console.log('[audio]', ...args);
                }

                function send(to, kind, data) {
                    Module.ccall('pref_on_audio_signal', null, [ 'string', 'string', 'string' ], [ to, kind, data ]);
                }

                async function getLocalStream() {
                    if (window.stream) { return window.stream; }
                    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
                        Module.ccall(
                            'pref_on_microphone_status',
                            null,
                            [ 'string', 'number' ],
                            [ 'getUserMedia not available', 1 ]);
                        throw new Error('getUserMedia not available');
                    }
                    if (!localStreamPromise) {
                        localStreamPromise = navigator.mediaDevices.getUserMedia({ audio: true, video: false })
                            .then(stream => {
                                const mute = !!window.prefDesiredMute;
                                stream.getTracks().forEach(track => { track.enabled = !mute; });
                                window.stream = stream;
                                return stream;
                            })
                            .catch(error => {
                                Module.ccall(
                                    'pref_on_microphone_status',
                                    null,
                                    [ 'string', 'number' ],
                                    [ 'navigator.MediaDevices.getUserMedia error: ' + error.message + ' ' + error.name, 1 ]);
                                localStreamPromise = null;
                                throw error;
                            });
                    }
                    window.stream = await localStreamPromise;
                    return window.stream;
                }

                function shouldInitiate(remoteId) {
                    return selfId && remoteId && selfId < remoteId;
                }

                function closePeer(remoteId) {
                    const pc = peers.get(remoteId);
                    if (pc) {
                        pc.getSenders().forEach(sender => {
                            try {
                                pc.removeTrack(sender);
                            } catch (_) {}
                        });
                        pc.close();
                    }
                    peers.delete(remoteId);
                    const audio = audios.get(remoteId);
                    if (audio) {
                        audio.srcObject = null;
                        audio.remove();
                        audios.delete(remoteId);
                    }
                }

                async function ensurePeer(remoteId) {
                    if (!remoteId || remoteId === selfId) { return null; }
                    if (peers.has(remoteId)) { return peers.get(remoteId); }
                    const pc = new RTCPeerConnection({
                        iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
                    });
                    peers.set(remoteId, pc);
                    const stream = await getLocalStream();
                    stream.getTracks().forEach(track => pc.addTrack(track, stream));
                    pc.onicecandidate = ev => {
                        if (ev.candidate) {
                            send(remoteId, 'candidate', JSON.stringify(ev.candidate));
                        }
                    };
                    pc.ontrack = ev => {
                        const remoteStream = ev.streams[0];
                        let audio = audios.get(remoteId);
                        if (!audio) {
                            audio = new Audio();
                            audio.autoplay = true;
                            audio.controls = false;
                            audio.style.display = 'none';
                            audios.set(remoteId, audio);
                            document.body.appendChild(audio);
                        }
                        audio.srcObject = remoteStream;
                        audio.play().catch(() => {});
                    };
                    pc.onconnectionstatechange = () => {
                        if (['failed', 'closed', 'disconnected'].includes(pc.connectionState)) {
                            closePeer(remoteId);
                            if (desiredPeers.has(remoteId)) {
                                setTimeout(() => { if (desiredPeers.has(remoteId)) { void ensurePeer(remoteId); } }, 1000);
                            }
                        }
                    };
                    if (shouldInitiate(remoteId)) {
                        const offer = await pc.createOffer();
                        await pc.setLocalDescription(offer);
                        send(remoteId, 'offer', JSON.stringify(offer));
                    }
                    return pc;
                }

                async function handleSignal(from, kind, data) {
                    const pc = await ensurePeer(from);
                    if (!pc) { return; }
                    if (kind === 'offer') {
                        await pc.setRemoteDescription(JSON.parse(data));
                        const answer = await pc.createAnswer();
                        await pc.setLocalDescription(answer);
                        send(from, 'answer', JSON.stringify(answer));
                        return;
                    }
                    if (kind === 'answer') {
                        if (!pc.currentRemoteDescription) {
                            await pc.setRemoteDescription(JSON.parse(data));
                        }
                        return;
                    }
                    if (kind === 'candidate') {
                        try {
                            await pc.addIceCandidate(JSON.parse(data));
                        } catch (e) {
                            log('Failed to add candidate', e);
                        }
                    }
                }

                function setSelf(id) {
                    selfId = id;
                }

                function setPeers(ids) {
                    desiredPeers = new Set(ids);
                    desiredPeers.forEach(id => { void ensurePeer(id); });
                    for (const id of peers.keys()) { if (!desiredPeers.has(id)) { closePeer(id); } }
                }

                function stopLocalStream() {
                    if (localStreamPromise) { localStreamPromise = null; }
                    if (window.stream && window.stream.getTracks) {
                        window.stream.getTracks().forEach(track => { try { track.stop(); } catch (_) {} });
                    }
                    window.stream = null;
                }

                return { setSelf, setPeers, handleSignal, ensurePeer, closePeer, stopLocalStream};
            })();
        }
        Module.prefAudio.setSelf(UTF8ToString($0));
        Module.prefAudio.setPeers(JSON.parse(UTF8ToString($1)));
    }, ctx().myPlayerId.c_str(), peersJson.c_str());
#pragma GCC diagnostic pop
    // clang-format on
}

auto drawPingAndFps() -> void
{
    if (not ctx().settingsMenu.showPingAndFps) { return; }
    const auto font = GetFontDefault();
    static constexpr auto fontSize = 20.f;
    static constexpr auto fontSpacing = 2.f;
    static constexpr auto y = fontSize;
    const auto fpsX = drawFps(y, font, fontSize, fontSpacing);
    drawPing({fpsX, y}, font, fontSize, fontSpacing);
}

auto handleMousePress() -> void
{
    if (not r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON) or ctx().isGameFreezed) { return; }
    if (ctx().stage == GameStage::PLAYING) {
        if ((not isMyTurn() or isSomeonePlayingOnMyBehalf()) and not isMyTurnOnBehalfOfSomeone()) { return; }
        const auto& playerId
            = std::invoke([&] { return isMyTurn() ? ctx().myPlayerId : ctx().myPlayer().playsOnBehalfOf; });

        handleCardClick(
            ctx().player(playerId).hand,
            [&](const Card& card) {
                if (std::empty(ctx().cardsOnTable) and not ctx().passGameTalon.exists()) {
                    ctx().leadSuit = cardSuit(card.name);
                }
                ctx().cardsOnTable.insert_or_assign(playerId, &getCard(card.name));
                sendPlayCard(playerId, card.name);
            },
            [&](const Card& card, const r::Vector2& from) { startCardMove(playerId, card, from); });
        return;
    } else {
        if (not isMyTurn()) { return; }
    }
    if (ctx().stage != GameStage::TALON_PICKING) { return; }
    if (not std::empty(ctx().pendingTalonReveal)) { return; }
    handleTalonCardClick(ctx().myPlayer().hand);
}

// TODO: Prevent interaction with other buttons during settings menu movement
template<typename Menu>
auto updateMenuPosition(Menu& menu) -> void
{
    if (not menu.isVisible) { return; }
    const auto mouse = r::Mouse::GetPosition();
    if (r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        const auto titleBar = r::Rectangle{
            menu.windowBoxPos.x,
            menu.windowBoxPos.y,
            menu.windowBoxW - (RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT + RAYGUI_WINDOWBOX_CLOSEBUTTON_HEIGHT) * 0.5f,
            RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT};
        if (mouse.CheckCollision(titleBar)) {
            GuiLock();
            menu.moving = true;
            menu.grabOffset = r::Vector2{mouse.x - menu.windowBoxPos.x, mouse.y - menu.windowBoxPos.y};
        }
    }
    if (not menu.moving) { return; }
    menu.windowBoxPos.x = mouse.x - menu.grabOffset.x;
    menu.windowBoxPos.y = mouse.y - menu.grabOffset.y;
    if (r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON)) { menu.moving = false; };
}

auto needsDraw() -> bool
{
    const auto deltaMouse = r::Mouse::GetDelta();
    const auto now = emscripten_get_now();
    const auto timeToDraw = now >= ctx().tick;
    static constexpr auto fps = 36.0;
    static constexpr auto deltaTime = 1.0 / fps * 1000.0;
    if (timeToDraw) { ctx().tick = now + deltaTime; }
    if (not ctx().needsDraw
        && ((deltaMouse.x != 0.0f or deltaMouse.y != 0.0f)
            or timeToDraw
            or r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)
            or r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON)
            or r::Mouse::IsButtonDown(MOUSE_LEFT_BUTTON)
            or r::Keyboard::IsKeyPressed(KEY_ENTER)
            or r::Keyboard::IsKeyPressed(KEY_ESCAPE)
            or ctx().window.IsResized())) {
        ctx().needsDraw = true;
    }
    return ctx().needsDraw;
}

auto updateDrawFrame([[maybe_unused]] void* ud) -> void
{
    if (not needsDraw()) { return; }
    applyPendingTalonReveal();
    if (GuiIsLocked()
        and not ctx().settingsMenu.moving
        and not ctx().speechBubbleMenu.moving
        and not ctx().overallScoreboard.moving) {
        GuiUnlock();
    }
    handleMousePress();
    updateMenuPosition(ctx().settingsMenu);
    updateMenuPosition(ctx().speechBubbleMenu);
    updateMenuPosition(ctx().overallScoreboard);

    ctx().target.BeginMode();
    ctx().window.ClearBackground(getGuiColor(BACKGROUND_COLOR));
    drawWelcomeScreen();
    drawLoginScreen();
    drawLogoutButton();
    if (ctx().isLoggedIn and ctx().isGameStarted) {
        drawWhistingOrMiserMenu();
        drawHowToPlayMenu();
        drawBiddingMenu();
        drawMiserCards();
        drawMyHand();
        drawTalonDiscardPopUp();
        drawOfferButton();
        drawRightHand();
        drawLeftHand();
        drawPlayedCards();
        drawPendingTalonReveal();
        drawScoreSheetButton();
        drawScoreSheet();
        drawLastTrickOrTalon();
        drawSpeechBubbleButton();
        drawHandshakeOfferButton();
    } else {
        drawConnectedPlayers();
    }
    if (ctx().isLoggedIn) {
        drawOverallScoreboardButton();
        drawMicButton();
    }
    drawSettingsButton();
    drawFullScreenButton();
    drawPingAndFps();
    if (ctx().isGameStarted and ctx().isLoggedIn) { drawSpeechBubbleMenu(); }
    if (ctx().isLoggedIn) { drawOverallScoreboard(); }
    drawSettingsMenu();
    drawLogoutMessage();
    ctx().target.EndMode();
    ctx().window.BeginDrawing();
    ctx().window.ClearBackground(getGuiColor(BACKGROUND_COLOR));
    ctx().target.GetTexture().Draw(
        r::Rectangle{
            0,
            0,
            static_cast<float>(ctx().target.GetTexture().width),
            -static_cast<float>(ctx().target.GetTexture().height)}, // flip vertically
        r::Rectangle{ctx().offsetX, ctx().offsetY, VirtualW * ctx().scale, VirtualH * ctx().scale});
    ctx().window.EndDrawing();
    ctx().needsDraw = false;
}

constexpr auto usage = R"(
Usage:
    client [--url=<url>] [--language=<name>] [--color-scheme=<name>]

Options:
    -h --help               Show this screen.
    --url=<url>             WebSocket URL [default: ws://0.0.0.0:8080]
    --language=<name>       Language to use [default: english]. Options: english, ukrainian, alternative
    --color-scheme=<name>   Color scheme to use [default: dracula]
                            Options: dracula, genesis, amber, dark, cyber, jungle, lavanda, bluish)";
} // namespace

auto setMicStatus(std::string status, const bool isError) -> void
{
    if (isError) {
        PREF_W("error: {}", status);
    } else {
        PREF_DI(status);
    }
    ctx().microphone.status = std::move(status);
    ctx().microphone.isError = isError;
    ctx().needsDraw = true;
}

} // namespace pref

extern "C" EMSCRIPTEN_KEEPALIVE auto pref_on_microphone_status(const char* status, const int isError) -> void
{
    pref::setMicStatus(status ? status : "", isError != 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE auto pref_on_audio_signal(const char* toPlayerId, const char* kind, const char* data)
    -> void
{
    PREF_DI(toPlayerId, kind, data);
    if (not toPlayerId or not kind or not data) { return; }
    pref::sendAudioSignal(toPlayerId, kind, data);
}

int main(const int argc, const char* const argv[])
{
    spdlog::set_pattern("[%^%l%$][%!] %v");
    auto& ctx = pref::ctx();
    pref::updateWindowSize();
    EnableEventWaiting();
    SetTextureFilter(ctx.target.texture, TEXTURE_FILTER_BILINEAR);
    pref::loadFromLocalStoragePlayerId();
    pref::loadFromLocalStorageAuthToken();
    pref::loadFromLocalStoragePlayerName();
    ctx.settingsMenu.showPingAndFps = not std::empty(pref::loadFromLocalStorage("show_ping_and_fps"));
    ctx.settingsMenu.soundEffects = std::empty(pref::loadFromLocalStorage("sound_effects"));
    const auto args = docopt::docopt(pref::usage, {std::next(argv), std::next(argv, argc)});
    ctx.url = args.at("--url").asString();
    const auto lang = pref::loadFromLocalStorage("language");
    pref::loadLang(not std::empty(lang) ? lang : args.at("--language").asString());
    GuiLoadStyleDefault();
    const auto colorScheme = pref::loadFromLocalStorage("color_scheme");
    pref::loadColorScheme(not std::empty(colorScheme) ? colorScheme : args.at("--color-scheme").asString());
    ctx.initialFont = GuiGetFont();
    pref::loadFonts();
    pref::loadCards();
    const auto resize
        = []([[maybe_unused]] const int eventType, [[maybe_unused]] const auto* e, [[maybe_unused]] void* ud) {
              pref::updateWindowSize();
              return EM_TRUE;
          };
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, resize);
    emscripten_set_fullscreenchange_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, resize);
    if (not std::empty(ctx.myPlayerId) and not std::empty(ctx.authToken)) {
        ctx.isLoginInProgress = true;
        pref::setupWebsocket();
    }
    emscripten_set_main_loop_arg(pref::updateDrawFrame, nullptr, 60, true);
    emscripten_websocket_deinitialize();
    return 0;
}
