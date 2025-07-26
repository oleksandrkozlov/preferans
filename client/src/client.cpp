// IWYU pragma: no_include <Vector2.hpp>
// IWYU pragma: no_include <Color.hpp>

#include "common/common.hpp"
#include "common/logger.hpp"

#include <docopt.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>
#include <raylib-cpp.hpp>
#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <thread>
#include <utility>

#define RAYGUI_IMPLEMENTATION
#include "proto/pref.pb.h"

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <emscripten/websocket.h>
#include <raygui.h>

#include <array>
#include <cassert>
#include <gsl/gsl>
#include <initializer_list>
#include <list>
#include <string_view>
#include <vector>

namespace pref {
namespace {

// 1920x1080 @ 16:9 - Full HD
[[maybe_unused]] constexpr auto ratioWidth = 16;
[[maybe_unused]] constexpr auto ratioHeigh = 9;
constexpr auto screenWidth = 1920;
constexpr auto screenHeight = 1080;

// 3440x1440 @ 21:9 - Ultra-Wide
// constexpr auto ratioWidth = 21;
// constexpr auto ratioHeigh = 9;
// constexpr auto screenWidth = 3440;
// constexpr auto screenHeight = 1440;

constexpr auto originalCardHeight = 726.0f;
constexpr auto originalCardWidth = 500.0f;
constexpr auto cardAspectRatio = originalCardWidth / originalCardHeight;
constexpr auto cardHeight = screenHeight / 5.0f; // 5th part of screen's height
constexpr auto cardWidth = cardHeight * cardAspectRatio;
constexpr auto cardOverlapX = cardWidth * 0.6f; // 60% overlap
constexpr auto cardOverlapY = cardHeight * 0.2f;

constexpr const auto FontSpacing = 1.0f;

using namespace std::literals;
namespace rng = ranges;
namespace rv = rng::views;

using PlayerId = std::string;
using PlayerName = std::string;
using CardName = std::string;

[[nodiscard]] constexpr auto getCloseReason(const std::uint16_t code) noexcept -> std::string_view
{
    switch (code) { // clang-format off
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
    } // clang-format on
    return "Unknown";
}

[[nodiscard]] constexpr auto what(const EMSCRIPTEN_RESULT result) noexcept -> std::string_view
{
    switch (result) { // clang-format off
    case EMSCRIPTEN_RESULT_SUCCESS: return "Success";
    case EMSCRIPTEN_RESULT_DEFERRED: return "Deferred";
    case EMSCRIPTEN_RESULT_NOT_SUPPORTED: return "WebSockets not supported";
    case EMSCRIPTEN_RESULT_FAILED_NOT_DEFERRED: return "Send failed, not deferred";
    case EMSCRIPTEN_RESULT_INVALID_TARGET: return "Invalid WebSocket handle";
    case EMSCRIPTEN_RESULT_UNKNOWN_TARGET: return "Unknown WebSocket target";
    case EMSCRIPTEN_RESULT_INVALID_PARAM: return "Invalid parameter";
    case EMSCRIPTEN_RESULT_FAILED: return "Generic failure";
    case EMSCRIPTEN_RESULT_NO_DATA: return "No data to send";
    case EMSCRIPTEN_RESULT_TIMED_OUT: return "Operation timed out";
    } // clang-format on
    return "Unknown";
}

enum class GameLang : std::size_t {
    En,
    Ua,
    Ru,
    Count,
};

enum class GameText : std::size_t { // clang-format off
    Preferans, CurrentPlayers, EnterYourName, Enter, Whist, HalfWhist, Pass, Count,
}; // clang-format on

constexpr auto localization = std::
    array<std::array<std::string_view, std::to_underlying(GameText::Count)>, std::to_underlying(GameLang::Count)>{{
        {"PREFERANS", "Current Players", "Enter your name:", "Enter", "Whist", "Half-Whist", "Pass"},
        {"ПРЕФЕРАНС", "Поточні гравці:", "Введіть своє ім’я:", "Увійти", "Віст", "Піввіста", "Пас"},
        {"ПРЕФЕРАНС", "Текущие игроки:", "Введите своё имя:", "Войти", "Вист", "Полвиста", "Пас"},
    }};

struct Card {
    Card(CardName n, RVector2 pos)
        : name{std::move(n)}
        , position{pos}
        , image{RImage{fmt::format("resources/cards/{}.png", name)}}
    {
        image.Resize(static_cast<int>(cardWidth), static_cast<int>(cardHeight));
        texture = image.LoadTexture();
    }

    CardName name;
    RVector2 position;
    RImage image;
    RTexture texture{};
};

[[nodiscard]] inline auto suitValue(const std::string_view suit) -> int
{
    static const auto map = std::map<std::string_view, int>{{SPADES, 1}, {DIAMONDS, 2}, {CLUBS, 3}, {HEARTS, 4}};
    return map.at(suit);
}

struct Player {
    auto sortCards() -> void
    {
        cards.sort([](const Card& lhs, const Card& rhs) {
            const auto lhsSuit = suitValue(cardSuit(lhs.name));
            const auto rhsSuit = suitValue(cardSuit(rhs.name));
            const auto lhsRank = rankValue(cardRank(lhs.name));
            const auto rhsRank = rankValue(cardRank(rhs.name));
            return std::tie(lhsSuit, lhsRank) < std::tie(rhsSuit, rhsRank);
        });
    }

    PlayerName name;
    std::list<Card> cards;
    PlayerName whistingChoice;
};

// ♠ Spades
// ♣ Clubs
// ♦ Diamonds
// ♥ Hearts

// TODO: come up with a solution that doesn't require both `bidsFlat` and `bidTable`.

// clang-format off
constexpr auto bidsFlat = std::array{
      SIX SPADE,   SIX CLUB,   SIX DIAMOND,    SIX HEART,      SIX,
    SEVEN SPADE, SEVEN CLUB,  SEVEN DIAMOND, SEVEN HEART,    SEVEN,
    EIGHT SPADE, EIGHT CLUB,  EIGHT DIAMOND, EIGHT HEART,    EIGHT,
     NINE SPADE,  NINE CLUB,   NINE DIAMOND,  NINE HEART,     NINE, NINE_WT,
      TEN SPADE,   TEN CLUB,    TEN DIAMOND,   TEN HEART,      TEN,  TEN_WT,
                                                   MISER, MISER_WT,    PASS};

constexpr auto bidTable = std::array<std::array<std::string_view, 6>, 6>{
    {{   SIX SPADE,   SIX CLUB,   SIX DIAMOND,   SIX HEART,      SIX,      "" },
     { SEVEN SPADE, SEVEN CLUB, SEVEN DIAMOND, SEVEN HEART,    SEVEN,      "" },
     { EIGHT SPADE, EIGHT CLUB, EIGHT DIAMOND, EIGHT HEART,    EIGHT,      "" },
     {  NINE SPADE,  NINE CLUB,  NINE DIAMOND,  NINE HEART,     NINE, NINE_WT },
     {   TEN SPADE,   TEN CLUB,   TEN DIAMOND,   TEN HEART,      TEN,  TEN_WT },
     {          "",         "",            "",       MISER, MISER_WT,    PASS }}};

// clang-format on

constexpr auto allRanks = std::size(bidsFlat);

constexpr auto whistingFlat = std::array{GameText::Whist, GameText::HalfWhist, GameText::Pass};

struct BiddingMenu {
    bool isVisible{};
    std::string bid; // text button
    std::size_t rank = allRanks;

    auto clear() -> void
    {
        isVisible = false;
        bid.clear();
        rank = allRanks;
    }
};

struct WhistingMenu {
    bool isVisible{};
    std::string choice;

    auto clear() -> void
    {
        isVisible = false;
    }
};

[[nodiscard]] constexpr auto bidRank(const std::string_view bid) noexcept -> std::size_t
{
    for (auto i = 0uz; i < std::size(bidsFlat); ++i) {
        if (bidsFlat[i] == bid) {
            return i;
        }
    }
    return allRanks;
}

[[nodiscard]] constexpr auto isRedSuit(const std::string_view suit) noexcept -> bool
{
    return suit.ends_with(DIAMOND) or suit.ends_with(HEART);
}

[[nodiscard]] auto getGuiColor(const int control, const int property) -> RColor
{
    return {gsl::narrow_cast<unsigned int>(GuiGetStyle(control, property))};
}

[[nodiscard]] auto localizeText(const GameText text, const GameLang lang) -> std::string
{
    return std::string{localization[std::to_underlying(lang)][std::to_underlying(text)]};
}

[[nodiscard]] constexpr auto whistingChoiceToGameText(const std::string_view whistingChoice) noexcept -> GameText
{
    if (whistingChoice == localizeText(GameText::Whist, GameLang::En)) {
        return GameText::Whist;
    }
    if (whistingChoice == localizeText(GameText::HalfWhist, GameLang::En)) {
        return GameText::HalfWhist;
    }
    return GameText::Pass;
}

struct Context {
    // TODO: Figure out how to use one font with different sizes
    RFont font20;
    RFont font36;
    RFont font96;
    RVector2 screen{screenWidth, screenHeight};
    RWindow window{static_cast<int>(screen.x), static_cast<int>(screen.y), "Preferans"};
    Player player;
    PlayerId myPlayerId;
    PlayerName myPlayerName;
    bool hasEnteredName{};
    std::map<PlayerId, Player> players;
    // TODO: make back of cards match the selected style
    Card backCard{"cardBackRed", {}};
    EMSCRIPTEN_WEBSOCKET_T ws{};
    int leftCardCount = 10;
    int rightCardCount = 10;
    std::map<PlayerId, Card> cardsOnTable;
    PlayerId turnPlayerId;
    BiddingMenu bidding;
    WhistingMenu whisting;
    std::string stage;
    std::vector<CardName> discardedTalon;
    std::string leadSuit;
    GameLang lang{};

    auto reset() -> void
    {
        leftCardCount = 10;
        rightCardCount = 10;
        cardsOnTable.clear();
        turnPlayerId.clear();
        bidding.clear();
        whisting.clear();
        stage.clear();
        discardedTalon.clear();
        leadSuit.clear();
    }

    [[nodiscard]] auto localizeText(const GameText text) const -> std::string
    {
        return ::pref::localizeText(text, lang);
    }

    [[nodiscard]] constexpr auto localizeBid(const std::string_view bid) const noexcept -> std::string_view
    {
        if (lang == GameLang::Ru) { // clang-format off
            if (bid == NINE_WT) { return NINE " БП"; }
            if (bid == TEN_WT) { return TEN " БП"; }
            if (bid == MISER) { return "МИЗЕР"; }
            if (bid == MISER_WT) { return "МИЗ.БП"; }
            if (bid == PASS) { return "ПАС"; }
        } else if (lang == GameLang::Ua) {
            if (bid == NINE_WT) { return NINE " БП"; }
            if (bid == TEN_WT) { return TEN " БП"; }
            if (bid == MISER) { return "МІЗЕР"; }
            if (bid == MISER_WT) { return "МІЗ.БП"; }
            if (bid == PASS) { return "ПАС"; }
        } // clang-format on
        return bid;
    }
};

[[nodiscard]] auto getOpponentIds(Context& ctx) -> std::pair<PlayerId, PlayerId>
{
    assert(std::size(ctx.players) == 3U);
    auto order = std::vector<PlayerId>{};
    for (const auto& [id, _] : ctx.players) {
        order.push_back(id);
    }
    const auto it = ranges::find(order, ctx.myPlayerId);
    assert(it != order.end());
    const auto selfIndex = std::distance(order.begin(), it);
    const auto leftIndex = (selfIndex + 1) % 3;
    const auto rightIndex = (selfIndex + 2) % 3;

    return {order[static_cast<std::size_t>(leftIndex)], order[static_cast<std::size_t>(rightIndex)]};
}

auto handlePlayerTurn(Context& ctx, PlayerTurn playerTurn) -> void
{
    if (std::size(ctx.cardsOnTable) >= 3) {
        // TODO: remove sleep
        std::this_thread::sleep_for(1s);
        ctx.cardsOnTable.clear();
    }
    ctx.turnPlayerId = playerTurn.player_id();
    ctx.stage = playerTurn.stage();
    if (ctx.turnPlayerId == ctx.myPlayerId) {
        INFO("Your turn");
        if (ctx.stage == "Bidding") {
            ctx.bidding.isVisible = true;
            return;
        }
        if (ctx.stage == "TalonPicking") {
            for (auto&& card : *playerTurn.mutable_talon()) {
                ctx.player.cards.emplace_back(std::move(card), RVector2{});
            }
            ctx.player.sortCards();
        }
        if (ctx.stage == "Whisting") {
            ctx.whisting.isVisible = true;
            return;
        }
    } else {
        INFO_VAR(ctx.turnPlayerId);
    }
}

auto handlePlayCard(Context& ctx, PlayCard playCard) -> void
{
    auto [leftOpponentId, rightOpponentId] = getOpponentIds(ctx);
    auto& playerId = *playCard.mutable_player_id();
    auto& cardName = *playCard.mutable_card(); // "queen_of_hearts"
    INFO_VAR(playerId, cardName);

    if (playerId == leftOpponentId) {
        if (ctx.leftCardCount > 0) {
            --ctx.leftCardCount;
        }
    } else if (playerId == rightOpponentId) {
        if (ctx.rightCardCount > 0) {
            --ctx.rightCardCount;
        }
    } else {
        // Not an opponent - possibly local player or spectator
        return;
    }

    if (std::empty(ctx.cardsOnTable)) {
        ctx.leadSuit = cardSuit(cardName);
    }
    ctx.cardsOnTable.insert_or_assign(std::move(playerId), Card{std::move(cardName), RVector2{}});
}

[[nodiscard]] auto toContext(void* userData) noexcept -> Context&
{
    return *static_cast<Context*>(userData);
}

[[nodiscard]] auto toUserData(Context& ctx) noexcept -> void*
{
    return static_cast<void*>(&ctx);
}

auto loadPlayerIdFromLocalStorage() -> PlayerId
{
    char buffer[128] = {};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
    EM_ASM_(
        {
            var id = localStorage.getItem("preferans_player_id") || "";
            stringToUTF8(id, $0, $1);
        },
        buffer,
        sizeof(buffer));
#pragma GCC diagnostic pop

    return {buffer};
}

auto savePlayerIdToLocalStorage(const PlayerId& playerId) -> void
{
    const auto js = std::string{"localStorage.setItem('preferans_player_id', '"} + playerId + "');";
    emscripten_run_script(js.c_str());
}

[[maybe_unused]] auto clearPlayerIdFromLocalStorage() -> void
{
    emscripten_run_script("localStorage.removeItem('preferans_player_id');");
}

auto onWsOpen(const int eventType, const EmscriptenWebSocketOpenEvent* e, void* userData) -> EM_BOOL
{
    INFO("{}, socket: {}", VAR(eventType), e->socket);
    assert(userData);
    auto& ctx = toContext(userData);

    auto joinRequest = JoinRequest{};
    joinRequest.set_player_name(ctx.myPlayerName);
    if (not std::empty(ctx.myPlayerId)) {
        joinRequest.set_player_id(ctx.myPlayerId);
    }
    auto msg = Message{};
    static constexpr auto method = "JoinRequest";
    msg.set_method(method);
    msg.set_payload(joinRequest.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(e->socket, data.data(), std::size(data));
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("error: could not send {}: {}", VAR(method), what(error));
    }
    return EM_TRUE;
}

auto onWsMessage(const int eventType, const EmscriptenWebSocketMessageEvent* e, void* userData) -> EM_BOOL
{
    INFO("{}, socket: {}, numBytes: {}, isText: {}", VAR(eventType), e->socket, e->numBytes, e->isText);
    assert(userData);
    auto& ctx = toContext(userData);

    if (e->isText) {
        WARN("error: expect binary data");
        return EM_TRUE;
    }
    auto msg = Message{};
    if (not msg.ParseFromArray(e->data, static_cast<int>(e->numBytes))) {
        WARN("Failed to parse Message");
        return EM_TRUE;
    }

    const auto& method = msg.method();
    const auto& payload = msg.payload();
    INFO_VAR(method);

    if (method == "JoinResponse") {
        auto joinResponse = JoinResponse{};
        if (not joinResponse.ParseFromString(payload)) {
            WARN("Failed to parse JoinResponse");
            return EM_TRUE;
        }
        if (ctx.myPlayerId != joinResponse.player_id()) {
            ctx.myPlayerId = joinResponse.player_id();
            INFO("save myPlayerId: {}", ctx.myPlayerId);
            savePlayerIdToLocalStorage(ctx.myPlayerId);
        }

        ctx.players.clear();
        for (auto& player : *joinResponse.mutable_players()) {
            if (player.player_id() == ctx.myPlayerId and ctx.myPlayerName != player.player_name()) {
                ctx.myPlayerName = player.player_name();
            }
            ctx.players.insert_or_assign(
                std::move(*player.mutable_player_id()), //
                Player{std::move(*player.mutable_player_name()), {}, {}});
        }
    } else if (method == "PlayerJoined") {
        auto playerJoined = PlayerJoined{};
        if (not playerJoined.ParseFromString(msg.payload())) {
            printf("Failed to parse PlayerJoined\n");
            return EM_TRUE;
        }
        auto& playerJoinedId = *playerJoined.mutable_player_id();
        auto& playerJoinedName = *playerJoined.mutable_player_name();

        INFO("New player playerJoined: {} ({})", playerJoinedName, playerJoinedId);
        ctx.players.insert_or_assign(std::move(playerJoinedId), Player{std::move(playerJoinedName), {}, {}});
    } else if (msg.method() == "PlayerLeft") {
        auto playerLeft = PlayerLeft{};
        if (not playerLeft.ParseFromString(msg.payload())) {
            WARN("Failed to parse PlayerLeft");
            return EM_TRUE;
        }

        const auto& id = playerLeft.player_id();
        if (ctx.players.contains(id)) {
            INFO("Player left: {} ({})", ctx.players[id].name, id);
            ctx.players.erase(id);
            INFO("Updated player list:");
            for (const auto& [i, player] : ctx.players) {
                INFO("  - {} ({})", player.name, i);
            }
        } else {
            WARN("Player with ID {} left (name unknown)", id);
        }
        return EM_TRUE;
    } else if (msg.method() == "DealCards") {
        auto dealCards = DealCards{};
        if (not dealCards.ParseFromString(payload)) {
            WARN("Failed to parse DealCards");
            return EM_TRUE;
        }
        ctx.reset();
        assert(std::size(dealCards.cards()) == 10);
        for (auto&& cardName : *dealCards.mutable_cards()) {
            ctx.player.cards.emplace_back(std::move(cardName), RVector2{});
        }
        ctx.player.sortCards();
    } else if (msg.method() == "PlayerTurn") {
        auto playerTurn = PlayerTurn{};
        if (not playerTurn.ParseFromString(msg.payload())) {
            WARN("error: failed to parse PlayerTurn");
            return EM_TRUE;
        }
        handlePlayerTurn(ctx, std::move(playerTurn));
    } else if (msg.method() == "Bidding") {
        auto bidding = Bidding{};
        if (not bidding.ParseFromString(msg.payload())) {
            WARN("error: failed to parse Bidding");
            return EM_TRUE;
        }
        const auto& playerId = bidding.player_id();
        const auto& bid = bidding.bid();
        const auto rank = bidRank(bid);
        INFO_VAR(playerId, bid, rank);
        if (bid != PASS) {
            ctx.bidding.rank = rank;
        }
        ctx.bidding.bid = bid;
    } else if (msg.method() == "Whisting") {
        auto whisting = Whisting{};
        if (not whisting.ParseFromString(msg.payload())) {
            WARN("error: failed to parse Whisting");
            return EM_TRUE;
        }
        const auto& playerId = whisting.player_id();
        if (not ctx.players.contains(playerId)) {
            WARN("error: unknown {}", VAR(playerId));
            return EM_TRUE;
        }
        const auto& choice = whisting.choice();
        ctx.players[playerId].whistingChoice = choice;
        INFO_VAR(playerId, choice);
    } else if (msg.method() == "PlayCard") {
        auto playCard = PlayCard{};
        if (not playCard.ParseFromString(msg.payload())) {
            WARN("error: failed to parse PlayCard");
            return EM_TRUE;
        }
        handlePlayCard(ctx, std::move(playCard));
    } else {
        WARN("error: unknown method: {}", msg.method());
    }

    return EM_TRUE;
}

auto onWsError(const int eventType, const EmscriptenWebSocketErrorEvent* e, void* userData) -> EM_BOOL
{
    INFO("{}, socket: {}", VAR(eventType), e->socket);
    assert(userData);
    [[maybe_unused]] auto& ctx = toContext(userData);

    return EM_TRUE;
}

auto onWsClosed(const int eventType, const EmscriptenWebSocketCloseEvent* e, void* userData) -> EM_BOOL
{
    INFO(
        "{}, socket: {}, wasClean: {}, code: {}, reason: {}",
        VAR(eventType),
        e->socket,
        e->wasClean,
        getCloseReason(e->code),
        e->reason);
    assert(userData);
    [[maybe_unused]] auto& ctx = toContext(userData);
    emscripten_websocket_delete(e->socket);
    // TODO: try to reconnect if not clean?
    /*
        emscripten_async_call(
            [&](void*) {
                setup_websocket(&ctx); // try again
            },
            nullptr,
            2000); // after 2 seconds
    */
    return EM_TRUE;
}

auto setup_websocket(Context& ctx) -> void
{
    if (not emscripten_websocket_is_supported()) {
        WARN("WebSocket is not supported");
        return;
    }

    EmscriptenWebSocketCreateAttributes attr = {};
    attr.url = "ws://192.168.2.194:8080"; // Or wss:// if using HTTPS
    attr.protocols = nullptr;
    attr.createOnMainThread = EM_TRUE;

    ctx.ws = emscripten_websocket_new(&attr);
    if (ctx.ws <= 0) {
        WARN("Failed to create WebSocket");
        return;
    }

    emscripten_websocket_set_onopen_callback(ctx.ws, &ctx, onWsOpen);
    emscripten_websocket_set_onmessage_callback(ctx.ws, &ctx, onWsMessage);
    emscripten_websocket_set_onerror_callback(ctx.ws, &ctx, onWsError);
    emscripten_websocket_set_onclose_callback(ctx.ws, &ctx, onWsClosed);
}

auto drawGuiLabelCentered(const std::string& text, const RVector2& anchor) -> void
{
    const auto size
        = MeasureTextEx(GuiGetFont(), text.c_str(), static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE)), FontSpacing);
    auto bounds = RRectangle{
        anchor.x - size.x * 0.5f - 4.0f, // shift left to center and add left padding
        anchor.y - size.y * 0.5f - 4.0f, // shift up to center and add top padding
        size.x + 8.0f, // width = text + left + right padding
        size.y + 8.0f // height = text + top + bottom padding
    };
    GuiLabel(bounds, text.c_str());
}

auto drawGameplayScreen(Context& ctx) -> void
{
    const auto title = ctx.localizeText(GameText::Preferans);
    const auto fontSize = static_cast<float>(ctx.font96.baseSize);
    const auto textSize = ctx.font96.MeasureText(title, fontSize, FontSpacing);
    const auto x = static_cast<float>(screenWidth - textSize.x) / 2.0f;
    ctx.font96.DrawText(title, {x, 20}, fontSize, FontSpacing, getGuiColor(LABEL, TEXT_COLOR_NORMAL));
}

auto drawEnterNameScreen(Context& ctx) -> void
{
    static char nameBuffer[16] = "";
    static bool editMode = true;

    const RVector2 screenCenter = ctx.screen / 2.0f;
    const float boxWidth = 400.0f;
    const float boxHeight = 60.0f;

    const RVector2 boxPos{screenCenter.x - boxWidth / 2.0f, screenCenter.y};

    // Label
    const RVector2 labelPos{boxPos.x, boxPos.y - 40.0f};
    const auto fontSize = static_cast<float>(ctx.font36.baseSize);
    ctx.font36.DrawText(
        ctx.localizeText(GameText::EnterYourName),
        labelPos,
        fontSize,
        FontSpacing,
        getGuiColor(LABEL, TEXT_COLOR_NORMAL));

    // Text box
    RRectangle inputBox = {boxPos, {boxWidth, boxHeight}};
    GuiTextBox(inputBox, nameBuffer, sizeof(nameBuffer), editMode);

    // Button
    RRectangle buttonBox = {boxPos.x, boxPos.y + boxHeight + 20.0f, boxWidth, 40.0f};
    bool clicked = GuiButton(buttonBox, ctx.localizeText(GameText::Enter).c_str());

    // Accept with Enter or button click
    if ((clicked or RKeyboard::IsKeyPressed(KEY_ENTER)) and (nameBuffer[0] != '\0') and editMode) {
        ctx.myPlayerName = nameBuffer;
        ctx.hasEnteredName = true;
        editMode = false; // Disable further typing

        setup_websocket(ctx);
    }
}

auto drawConnectedPlayersPanel(const Context& ctx) -> void
{
    // from the left edge
    const float x = 20.0f;
    // from the top edge
    const float y = 20.0f;

    // Set panel dimensions
    const float width = 240.0f;
    const float height = 160.0f; // Reduced height to fit 4 names comfortably

    // Define the panel rectangle
    Rectangle panelRect = {x, y, width, height};

    // Draw the panel (no title bar)
    GuiPanel(panelRect, nullptr);

    // Start drawing text 10px inside the panel
    RVector2 textPos = {x + 10.0f, y + 10.0f};

    const auto fontSize = static_cast<float>(ctx.font20.baseSize);
    // Draw panel heading
    ctx.font20.DrawText(
        ctx.localizeText(GameText::CurrentPlayers),
        textPos,
        fontSize,
        FontSpacing,
        getGuiColor(LABEL, TEXT_COLOR_NORMAL));

    // Move text position down for player list
    textPos.y += 30.0f;

    for (const auto& [id, player] : ctx.players) {
        const auto& name = player.name;
        // Highlight the current player's name
        Color color = (id == ctx.myPlayerId) ? getGuiColor(DEFAULT, TEXT_COLOR_NORMAL)
                                             : getGuiColor(DEFAULT, TEXT_COLOR_DISABLED);
        // Draw the player's name
        ctx.font20.DrawText(name, textPos, fontSize, FontSpacing, color);

        // Move down for the next name
        textPos.y += 24.0f;
    }
}

auto playCard(Context& ctx, const CardName& cardName) -> void
{
    auto playCard = PlayCard{};
    playCard.set_player_id(ctx.myPlayerId);
    playCard.set_card(cardName);
    auto msg = Message{};
    static constexpr auto method = "PlayCard";
    msg.set_method(method);
    msg.set_payload(playCard.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), std::size(data));
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("error: could not send {}: {}", VAR(method), what(error));
    }
}

auto sendBidding(Context& ctx, const std::string_view bid) -> void
{
    auto bidding = Bidding{};
    bidding.set_player_id(ctx.myPlayerId);
    bidding.set_bid(std::string{bid});
    auto msg = Message{};
    static constexpr auto method = "Bidding";
    msg.set_method(method);
    msg.set_payload(bidding.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), std::size(data));
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("could not send {}: {}", VAR(method), what(error));
    }
}

auto sendWhisting(Context& ctx, const std::string_view choice) -> void
{
    auto whisting = Whisting{};
    whisting.set_player_id(ctx.myPlayerId);
    whisting.set_choice(std::string{choice});
    auto msg = Message{};
    static constexpr auto method = "Whisting";
    msg.set_method(method);
    msg.set_payload(whisting.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), std::size(data));
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("could not send {}: {}", VAR(method), what(error));
    }
}

auto discardTalon(Context& ctx, const std::string_view bid) -> void
{
    auto discardTalon = DiscardTalon{};
    discardTalon.set_player_id(ctx.myPlayerId);
    discardTalon.set_bid(std::string{bid});
    for (const auto& card : ctx.discardedTalon) {
        discardTalon.add_cards(card);
    }
    auto msg = Message{};
    static constexpr auto method = "DiscardTalon";
    msg.set_method(method);
    msg.set_payload(discardTalon.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), std::size(data));
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("could not send {}: {}", VAR(method), what(error));
    }
}

auto drawMyHand(Context& ctx) -> void
{
    auto& hand = ctx.player.cards;
    const auto totalWidth = static_cast<float>(std::size(hand) - 1) * cardOverlapX + cardWidth;
    const auto startX = (screenWidth - totalWidth) / 2.0f;
    const auto y = screenHeight - cardHeight - 20.0f; // bottom padding
    const auto fontSize = static_cast<float>(ctx.font36.baseSize);
    const auto size = ctx.font36.MeasureText(ctx.myPlayerName, fontSize, FontSpacing);

    auto label = Rectangle{
        startX - size.x - 20.0f, // to the left of the first card
        y + cardHeight * 0.5f - size.y * 0.5f - 4, // vertically centred on the card
        size.x + 8.0f,
        size.y + 8.0f};
    // TODO: still draw name if no cards left
    GuiLabel(label, ctx.myPlayerName.c_str());

    // cards themselves
    for (auto&& [i, card] : hand | rv::enumerate) {
        const float x = startX + static_cast<float>(i) * cardOverlapX;
        card.position = RVector2{x, y};
        card.texture.Draw(card.position);
    }
}

auto drawOpponentHand(Context& ctx, const int cardCount, const float x, const PlayerId& playerId) -> void
{
    // draw if I have cards
    if (std::empty(ctx.player.cards)) {
        return;
    }
    if (cardCount == 0) {
        return;
    }
    const auto countF = static_cast<float>(cardCount);
    const float startY = (screenHeight - cardHeight - (countF - 1.0f) * cardOverlapY) * 0.5f;

    // draw back-faces
    for (float i = 0.0f; i < countF; ++i) {
        const auto posY = startY + i * cardOverlapY;
        ctx.backCard.texture.Draw(RVector2{x, posY});
    }
    const float centerX = x + (cardWidth * 0.5f);

    // TODO: still draw name if no cards left

    // centred name plate above the topmost card
    const auto& name = ctx.players.at(playerId).name;
    const auto fontSize = static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE));
    auto anchor = RVector2{centerX, startY - fontSize};
    drawGuiLabelCentered(name, anchor);

    // FIXME: a whisting choice sometimes is not drawen
    if (const auto& choice = ctx.players.at(playerId).whistingChoice; not std::empty(choice)) {
        const float endY = startY + (countF - 1.0f) * cardOverlapY + cardHeight;
        anchor = RVector2{centerX, endY + fontSize};
        drawGuiLabelCentered(ctx.localizeText(whistingChoiceToGameText(choice)), anchor);
    }
}

auto drawPlayedCards(Context& ctx) -> void
{
    if (std::empty(ctx.cardsOnTable)) {
        return;
    }
    const auto cardSpacing = cardWidth * 0.1f;
    const auto yOffset = cardHeight / 4.0f;
    const auto centerPos = RVector2{screenWidth / 2.0f, screenHeight / 2.0f - cardHeight / 2.0f};
    const auto leftPlayPos = RVector2{centerPos.x - cardWidth - cardSpacing, centerPos.y - yOffset};
    const auto middlePlayPos = RVector2{centerPos.x, centerPos.y + yOffset};
    const auto rightPlayPos = RVector2{centerPos.x + cardWidth + cardSpacing, centerPos.y - yOffset};
    const auto [leftOpponentId, rightOpponentId] = getOpponentIds(ctx);
    if (ctx.cardsOnTable.contains(leftOpponentId)) {
        ctx.cardsOnTable.at(leftOpponentId).texture.Draw(leftPlayPos);
    }
    if (ctx.cardsOnTable.contains(rightOpponentId)) {
        ctx.cardsOnTable.at(rightOpponentId).texture.Draw(rightPlayPos);
    }
    if (ctx.cardsOnTable.contains(ctx.myPlayerId)) {
        ctx.cardsOnTable.at(ctx.myPlayerId).texture.Draw(middlePlayPos);
    }
}

#define GUI_PROPERTY(PREFIX, STATE)                                                                                    \
    std::invoke([&] {                                                                                                  \
        switch (STATE) {                                                                                               \
        case STATE_NORMAL:                                                                                             \
            return PREFIX##_COLOR_NORMAL;                                                                              \
        case STATE_DISABLED:                                                                                           \
            return PREFIX##_COLOR_DISABLED;                                                                            \
        case STATE_PRESSED:                                                                                            \
            return PREFIX##_COLOR_PRESSED;                                                                             \
        case STATE_FOCUSED:                                                                                            \
            return PREFIX##_COLOR_FOCUSED;                                                                             \
        }                                                                                                              \
        std::unreachable();                                                                                            \
    })

auto drawBiddingMenu(Context& ctx) -> void
{
    if (!ctx.bidding.isVisible) {
        return;
    }

    static constexpr auto cellW = screenWidth / 13;
    static constexpr auto cellH = cellW / 2;
    static constexpr auto gap = cellH / 10;
    static constexpr auto rows = static_cast<int>(std::size(bidTable));
    static constexpr auto cols = static_cast<int>(std::size(bidTable[0]));
    static constexpr auto menuW = cols * cellW + (cols - 1) * gap;
    static constexpr auto menuH = rows * cellH + (rows - 1) * gap;
    static constexpr auto originX = (screenWidth - menuW) / 2.0f;
    static constexpr auto originY = (screenHeight - menuH) / 2.0f;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const auto& bid = bidTable[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (std::empty(bid)) {
                continue;
            }
            const auto rank = bidRank(bid);
            const auto isPass = (bid == PASS);
            const auto isFinalBid = std::size(ctx.discardedTalon) == 2;
            // TODO: Enable Miser and Miser WT only as a first bid
            auto state = (not isPass //
                          and (ctx.bidding.rank != allRanks) //
                          and (rank <= ctx.bidding.rank))
                    or (isPass and isFinalBid)
                ? GuiState{STATE_DISABLED}
                : GuiState{STATE_NORMAL};
            const auto pos = RVector2{
                originX + static_cast<float>(c) * (cellW + gap), //
                originY + static_cast<float>(r) * (cellH + gap)};
            const auto rect = RRectangle{pos, {cellW, cellH}};
            const auto clicked = std::invoke([&] {
                if ((state == STATE_DISABLED) or not RMouse::GetPosition().CheckCollision(rect)) {
                    return false;
                }
                state = RMouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
                return RMouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
            });
            const auto borderColor = getGuiColor(BUTTON, GUI_PROPERTY(BORDER, state));
            const auto bgColor = getGuiColor(BUTTON, GUI_PROPERTY(BASE, state));
            const auto textColor = getGuiColor(BUTTON, GUI_PROPERTY(TEXT, state));
            rect.Draw(bgColor);
            rect.DrawLines(borderColor, static_cast<float>(GuiGetStyle(BUTTON, BORDER_WIDTH)));
            const auto fontSize = static_cast<float>(ctx.font36.baseSize);
            auto text = std::string{ctx.localizeBid(bid)};
            auto textSize = ctx.font36.MeasureText(text, fontSize, FontSpacing);
            const auto textX = rect.x + (rect.width - textSize.x) / 2.0f;
            const auto textY = rect.y + (rect.height - textSize.y) / 2.0f;
            if (isRedSuit(text)) {
                auto count = 0;
                auto codepoints = LoadCodepoints(text.c_str(), &count);
                auto _ = gsl::finally([&] { UnloadCodepoints(codepoints); });
                auto rankText = std::string{};
                for (int i = 0; i < count - 1; ++i) {
                    auto size = 0;
                    auto str = CodepointToUTF8(codepoints[i], &size);
                    rankText += std::string(str, static_cast<std::size_t>(size));
                }
                auto size = 0;
                auto str = CodepointToUTF8(codepoints[count - 1], &size);
                auto suitText = std::string(str, gsl::narrow_cast<std::size_t>(size));
                ctx.font36.DrawText(rankText, {textX, textY}, fontSize, FontSpacing, textColor);
                const auto rankSize = ctx.font36.MeasureText(rankText, fontSize, FontSpacing);
                const auto suitColor = state == STATE_DISABLED ? RColor::Red().Alpha(0.4f) : RColor::Red();
                ctx.font36.DrawText(suitText.c_str(), {textX + rankSize.x, textY}, fontSize, FontSpacing, suitColor);
            } else {
                ctx.font36.DrawText(text, {textX, textY}, fontSize, FontSpacing, textColor);
            }
            if (clicked and (state != STATE_DISABLED)) {
                ctx.bidding.bid = bid;
                if (not isPass) {
                    ctx.bidding.rank = rank;
                }
                ctx.bidding.isVisible = false;
                if (isFinalBid) {
                    discardTalon(ctx, bid); // final bid
                } else {
                    sendBidding(ctx, bid);
                }
                return;
            }
        }
    }
}

auto drawWhistingMenu(Context& ctx) -> void
{
    if (!ctx.whisting.isVisible) {
        return;
    }
    static constexpr auto cellW = screenWidth / 6;
    static constexpr auto cellH = cellW / 2;
    static constexpr auto gap = cellH / 10;
    static constexpr auto menuW = std::size(whistingFlat) * cellW + (std::size(whistingFlat) - 1) * gap;
    static constexpr auto originX = (screenWidth - menuW) / 2.0f;
    static constexpr auto originY = (screenHeight - cellH) / 2.0f;

    for (auto i = 0uz; i < std::size(whistingFlat); ++i) {
        auto state = GuiState{STATE_NORMAL};
        const auto pos = RVector2{originX + static_cast<float>(i) * (cellW + gap), originY};
        const auto rect = RRectangle{pos, {cellW, cellH}};
        const auto clicked = std::invoke([&] {
            if (not RMouse::GetPosition().CheckCollision(rect)) {
                return false;
            }
            state = RMouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
            return RMouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
        });
        const auto borderColor = getGuiColor(BUTTON, GUI_PROPERTY(BORDER, state));
        const auto bgColor = getGuiColor(BUTTON, GUI_PROPERTY(BASE, state));
        const auto textColor = getGuiColor(BUTTON, GUI_PROPERTY(TEXT, state));
        rect.Draw(bgColor);
        rect.DrawLines(borderColor, static_cast<float>(GuiGetStyle(BUTTON, BORDER_WIDTH)));
        const auto fontSize = static_cast<float>(ctx.font36.baseSize);
        const auto text = ctx.localizeText(whistingFlat[i]);
        const auto textSize = ctx.font36.MeasureText(text, fontSize, FontSpacing);
        const auto textX = rect.x + (rect.width - textSize.x) / 2.0f;
        const auto textY = rect.y + (rect.height - textSize.y) / 2.0f;
        ctx.font36.DrawText(text, {textX, textY}, fontSize, FontSpacing, textColor);
        if (clicked) {
            ctx.whisting.isVisible = false;
            ctx.whisting.choice = localizeText(whistingFlat[i], GameLang::En);
            sendWhisting(ctx, ctx.whisting.choice);
        }
    }
}

[[nodiscard]] auto isCardPlayable(const Context& ctx, const Card& clickedCard) -> bool
{ // clang-format off
    const auto clickedSuit = cardSuit(clickedCard.name);
    const auto trump = getTrump(ctx.bidding.bid);
    const auto hasSuit = [&](const std::string_view suit) {
        return rng::any_of(ctx.player.cards, [&](const CardName& name) { return cardSuit(name) == suit; }, &Card::name);
    };
    INFO_VAR(clickedCard.name, clickedSuit, trump);
    if (std::empty(ctx.cardsOnTable)) { return true; } // first card in the trick: any card is allowed
    if (clickedSuit == ctx.leadSuit) { return true; } // follows lead suit
    if (hasSuit(ctx.leadSuit)) { return false; } // must follow lead suit
    if (clickedSuit == trump) { return true; } //  no lead suit cards, playing trump
    if (hasSuit(trump)) { return false;  } // // must play trump if you have it
    return true; // no lead or trump suit cards, free to play
} // clang-format on

template<typename Action>
auto handleCardClick(Context& ctx, Action act) -> void
{
    if (not RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        return;
    }
    const auto mousePos = RMouse::GetPosition();
    const auto hit = [&](Card& c) { return RRectangle{c.position, c.texture.GetSize()}.CheckCollision(mousePos); };
    const auto reversed = ctx.player.cards | rv::reverse;
    if (const auto rit = rng::find_if(reversed, hit); rit != rng::cend(reversed)) {
        const auto it = std::next(rit).base();
        if (not isCardPlayable(ctx, *it)) {
            WARN("Can't play this card: {}", it->name);
            return;
        }
        const auto _ = gsl::finally([&] { ctx.player.cards.erase(it); });
        act(std::move(*it));
    }
}

auto updateDrawFrame(void* userData) -> void
{
    assert(userData);
    auto& ctx = toContext(userData);
    if (std::empty(ctx.myPlayerId)) {
        ctx.myPlayerId = loadPlayerIdFromLocalStorage();
    }
    if ((ctx.myPlayerId == ctx.turnPlayerId) and RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (ctx.stage == "Playing") {
            handleCardClick(ctx, [&](Card&& card) {
                const auto name = card.name; // copy before move `card`
                if (std::empty(ctx.cardsOnTable)) {
                    ctx.leadSuit = cardSuit(name);
                }
                ctx.cardsOnTable.insert_or_assign(ctx.myPlayerId, std::move(card));
                playCard(ctx, name);
            });
        } else if ((ctx.stage == "TalonPicking") and (std::size(ctx.discardedTalon) < 2)) {
            handleCardClick(ctx, [&](Card&& card) { ctx.discardedTalon.push_back(card.name); });
            if (std::size(ctx.discardedTalon) == 2) {
                INFO_VAR(ctx.discardedTalon, ctx.bidding.rank);
                if (ctx.bidding.rank != 0) {
                    --ctx.bidding.rank; // allow the final bid as before
                } else {
                    ctx.bidding.rank = allRanks;
                }
                ctx.bidding.isVisible = true;
            }
        }
    }
    ctx.window.BeginDrawing();
    ctx.window.ClearBackground(getGuiColor(DEFAULT, BACKGROUND_COLOR));
    drawBiddingMenu(ctx);
    drawWhistingMenu(ctx);
    if (not ctx.hasEnteredName) {
        drawGameplayScreen(ctx);
        drawEnterNameScreen(ctx);
        ctx.window.EndDrawing();
        return;
    }
    if (std::size(ctx.players) == 3U) {
        drawMyHand(ctx);
        auto leftX = 40.0f;
        auto rightX = screenWidth - cardWidth - 40.0f;
        const auto [leftId, rightId] = getOpponentIds(ctx);
        drawOpponentHand(ctx, ctx.leftCardCount, leftX, leftId);
        drawOpponentHand(ctx, ctx.rightCardCount, rightX, rightId);
        drawPlayedCards(ctx);
    } else {
        drawConnectedPlayersPanel(ctx);
    }
    ctx.window.DrawFPS(screenWidth - 80, 0);
    ctx.window.EndDrawing();
}

constexpr auto usage = R"(
Usage:
    client [--language=<lang>] [--style=<style>]

Options:
    -h --help               Show this screen.
    --language=<lang>       Language to use [default: en]. Options: en, ua, ru
    --style=<style>         UI style to use [default: light]
                            Options: enefete, lavanda, light,   terminal, candy,
                                     jungle,  cyber,   dark,    rltech,   bluish,
                                     amber,   cherry,  genesis, ashes,    sunny
)";
} // namespace
} // namespace pref

int main(const int argc, const char* const argv[])
{
    const auto args = docopt::docopt(pref::usage, {std::next(argv), std::next(argv, argc)});
    for (auto const& arg : args) {
        std::cout << arg.first << arg.second << std::endl;
    }
    spdlog::set_pattern("[%^%l%$][%!] %v");
    auto ctx = pref::Context{};
    const auto lang = args.at("--language").asString();
    ctx.lang = std::invoke([&] { // clang-format off
        if (lang == "ua") { return pref::GameLang::Ua; }
        if (lang == "ru") { return pref::GameLang::Ru; }
        return pref::GameLang::En;
    }); // clang-format on
    GuiLoadStyleDefault();
    ctx.window.SetTargetFPS(60);
    // TODO: Select style via a menu
    const auto style = args.at("--style").asString();
    if (style != "light") {
        GuiLoadStyle(fmt::format("resources/styles/style_{}.rgs", style).c_str());
    }
    const auto fontPath = "resources/fonts/DejaVuSans.ttf";
    auto codepointSize = 0;
    const auto codepoint = GetCodepoint(DIAMOND, &codepointSize);
    ctx.font20 = LoadFontEx(fontPath, 20, nullptr, codepoint);
    ctx.font36 = LoadFontEx(fontPath, 36, nullptr, codepoint);
    ctx.font96 = LoadFontEx(fontPath, 96, nullptr, codepoint);
    // FIXME: raygui resets the font style because the fonts loaded with errors
    GuiSetStyle(DEFAULT, TEXT_SIZE, 36);
    GuiSetStyle(DEFAULT, TEXT_SPACING, static_cast<int>(pref::FontSpacing));
    SetTextureFilter(ctx.font20.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx.font36.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx.font96.texture, TEXTURE_FILTER_BILINEAR);
    GuiSetFont(ctx.font36);

    emscripten_set_main_loop_arg(pref::updateDrawFrame, pref::toUserData(ctx), 0, true);
    return 0;
}
