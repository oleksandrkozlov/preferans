#include "common/common.hpp"
#include "common/logger.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>
#include <raylib-cpp.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

#include <array>
#include <cassert>
#include <gsl/gsl>
#include <initializer_list>
#include <list>
#include <string_view>
#include <vector>

#if defined(PLATFORM_WEB)
#include "proto/pref.pb.h"

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <emscripten/websocket.h>
#endif // PLATFORM_WEB

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

using namespace std::literals;
namespace rng = ranges;
namespace rv = rng::views;

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

#if defined(PLATFORM_WEB)
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
#endif // PLATFORM_WEB

struct Card {
    Card(std::string n, RVector2 pos)
        : name{std::move(n)}
        , position{pos}
        , image{RImage{fmt::format("resources/cards/{}.png", name)}}
    {
        image.Resize(static_cast<int>(cardWidth), static_cast<int>(cardHeight));
        texture = image.LoadTexture();
    }

    std::string name;
    RVector2 position;
    RImage image;
    RTexture texture{};
};

struct Player {
    std::list<Card> cards;
};

// clang-format off
constexpr auto bidsFlat = std::array{
      "6S",  "6C",  "6D",  "6H",    "6WT",
      "7S",  "7C",  "7D",  "7H",    "7WT",
      "8S",  "8C",  "8D",  "8H",    "8WT",
      "9S",  "9C",  "9D",  "9H",    "9WT",      "9WP",
      "10S", "10C", "10D", "10H",   "10WT",     "10WP",
                           "MISER", "MISER WP", "PASS"};

constexpr auto bidTable = std::array<std::array<std::string_view, 6>, 6>{
    {{"6S",  "6C",  "6D",  "6H",    "6WT",      ""},
     {"7S",  "7C",  "7D",  "7H",    "7WT",      ""},
     {"8S",  "8C",  "8D",  "8H",    "8WT",      ""},
     {"9S",  "9C",  "9D",  "9H",    "9WT",      "9WP"},
     {"10S", "10C", "10D", "10H",   "10WT",     "10WP"},
     {"",    "",    "",    "MISER", "MISER WP", "PASS"}}};
// clang-format on

constexpr auto allRanks = bidsFlat.size();

struct BiddingMenu {
    bool isVisible{};
    std::string bid; // text button
    std::size_t rank = allRanks;
};

constexpr auto rankOf(const std::string_view bid) noexcept -> std::size_t
{
    for (std::size_t i = 0; i < bidsFlat.size(); ++i) {
        if (bidsFlat[i] == bid) {
            return i;
        }
    }
    return allRanks;
}

[[nodiscard]] auto getGuiColor(const int control, const int property) -> RColor
{
    return {static_cast<unsigned int>(GuiGetStyle(control, property))};
}

struct Context {
    RFont font{"resources/mecha.png"};
    RVector2 screen{screenWidth, screenHeight};
    RWindow window{static_cast<int>(screen.x), static_cast<int>(screen.y), "Preferans"};
    Player player;
    std::string myPlayerId;
    std::string myPlayerName;
    bool hasEnteredName{};
    std::map<std::string, std::string> connectedPlayers;
    Card backCard{"cardBackRed", {}};
#if defined(PLATFORM_WEB)
    EMSCRIPTEN_WEBSOCKET_T ws{};
#endif // PLATFORM_WEB
    int leftCardCount = 10;
    int rightCardCount = 10;
    std::map<std::string, Card> cardsOnTable;
    std::string turnPlayerId;
    BiddingMenu bidding;
    std::string stage;
    std::vector<std::string> discardedTalon;
    std::string leadSuit;
};

[[nodiscard]] auto getOpponentIds(Context& ctx) -> std::pair<std::string, std::string>
{
    assert(std::size(ctx.connectedPlayers) == 3U);
    auto order = std::vector<std::string>{};
    for (const auto& [id, _] : ctx.connectedPlayers) {
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
        ctx.leadSuit = suitOf(cardName);
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

#if defined(PLATFORM_WEB)
std::string loadPlayerIdFromLocalStorage()
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

    return std::string(buffer);
}

void savePlayerIdToLocalStorage(const std::string& playerId)
{
    std::string js = "localStorage.setItem('preferans_player_id', '" + playerId + "');";
    emscripten_run_script(js.c_str());
}

[[maybe_unused]] auto clearPlayerIdFromLocalStorage() -> void
{
    emscripten_run_script("localStorage.removeItem('preferans_player_id');");
}

EM_BOOL onWsOpen(const int eventType, const EmscriptenWebSocketOpenEvent* e, void* userData)
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
    if (const auto error = emscripten_websocket_send_binary(e->socket, data.data(), data.size());
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

        ctx.connectedPlayers.clear();
        for (auto& player : *joinResponse.mutable_players()) {
            if (player.player_id() == ctx.myPlayerId and ctx.myPlayerName != player.player_name()) {
                ctx.myPlayerName = player.player_name();
            }
            ctx.connectedPlayers.insert_or_assign(
                std::move(*player.mutable_player_id()), //
                std::move(*player.mutable_player_name()));
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
        ctx.connectedPlayers.insert_or_assign(std::move(playerJoinedId), std::move(playerJoinedName));
    } else if (msg.method() == "PlayerLeft") {
        auto playerLeft = PlayerLeft{};
        if (not playerLeft.ParseFromString(msg.payload())) {
            WARN("Failed to parse PlayerLeft");
            return EM_TRUE;
        }

        const auto& id = playerLeft.player_id();
        auto it = ctx.connectedPlayers.find(id);

        if (it != ctx.connectedPlayers.end()) {
            INFO("Player left: {} ({})", it->second, id);
            ctx.connectedPlayers.erase(it);
            INFO("Updated player list:");
            for (const auto& [i, name] : ctx.connectedPlayers) {
                INFO("  - {} ({})", name, i);
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

        assert(std::size(dealCards.cards()) == 10);
        for (auto&& cardName : *dealCards.mutable_cards()) {
            ctx.player.cards.emplace_back(std::move(cardName), RVector2{});
        }
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
        const auto rank = rankOf(bid);
        INFO_VAR(playerId, bid, rank);
        if (bid != "PASS") {
            ctx.bidding.rank = rank;
        }
        ctx.bidding.bid = bid;
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

void setup_websocket(Context& ctx)
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
#endif // PLATFORM_WEB

void drawGameplayScreen(Context& ctx)
{
    const std::string title = "PREFERANS GAME";
    const float fontSize = static_cast<float>(ctx.font.baseSize) * 3.0f;
    const auto textSize = MeasureText(title.c_str(), static_cast<int>(fontSize));
    const auto x = static_cast<float>(screenWidth - textSize) / 2.0f;
    RText::Draw(title, {x, 20}, static_cast<int>(fontSize), getGuiColor(LABEL, TEXT_COLOR_NORMAL));
}

void drawEnterNameScreen(Context& ctx)
{
    static char nameBuffer[32] = "";
    static bool editMode = true;

    const RVector2 screenCenter = ctx.screen / 2.0f;
    const float boxWidth = 400.0f;
    const float boxHeight = 60.0f;

    const RVector2 boxPos{screenCenter.x - boxWidth / 2.0f, screenCenter.y};

    // Label
    const RVector2 labelPos{boxPos.x, boxPos.y - 40.0f};
    RText::Draw("Enter your name:", labelPos, 20.0f, getGuiColor(LABEL, TEXT_COLOR_NORMAL));

    // Text box
    RRectangle inputBox = {boxPos, {boxWidth, boxHeight}};
    GuiTextBox(inputBox, nameBuffer, sizeof(nameBuffer), editMode);

    // Button
    RRectangle buttonBox = {boxPos.x, boxPos.y + boxHeight + 20.0f, boxWidth, 40.0f};
    bool clicked = GuiButton(buttonBox, "Start");

    // Accept with Enter or button click
    if ((clicked or RKeyboard::IsKeyPressed(KEY_ENTER)) and (nameBuffer[0] != '\0') and editMode) {
        ctx.myPlayerName = nameBuffer;
        ctx.hasEnteredName = true;
        editMode = false; // Disable further typing

#if defined(PLATFORM_WEB)
        setup_websocket(ctx);
#endif
    }
}

auto drawConnectedPlayersPanel(const Context& ctx) -> void
{
    // from the left edge
    const float x = 20.0f;
    // from the top edge
    const float y = 50.0f;

    // Set panel dimensions
    const float width = 240.0f;
    const float height = 160.0f; // Reduced height to fit 4 names comfortably

    // Define the panel rectangle
    Rectangle panelRect = {x, y, width, height};

    // Draw the panel (no title bar)
    GuiPanel(panelRect, nullptr);

    // Start drawing text 10px inside the panel
    RVector2 textPos = {x + 10.0f, y + 10.0f};

    // Draw panel heading
    RText::Draw("Connected Players", textPos, 20.0f, getGuiColor(LABEL, TEXT_COLOR_NORMAL));

    // Move text position down for player list
    textPos.y += 30.0f;

    for (const auto& [id, name] : ctx.connectedPlayers) {
        // Highlight the current player's name in dark blue
        Color color = (id == ctx.myPlayerId) ? getGuiColor(DEFAULT, TEXT_COLOR_NORMAL)
                                             : getGuiColor(DEFAULT, TEXT_COLOR_DISABLED);
        // Draw the player's name
        RText::Draw(name, textPos, 18.0f, color);
        // ctx.font.DrawText(name, textPos, 18.0f, 1.0f, color);

        // Move down for the next name
        textPos.y += 24.0f;
    }
}

#if defined(PLATFORM_WEB)
auto playCard(Context& ctx, const std::string& cardName) -> void
{
    auto playCard = PlayCard{};
    playCard.set_player_id(ctx.myPlayerId);
    playCard.set_card(cardName);
    auto msg = Message{};
    static constexpr auto method = "PlayCard";
    msg.set_method(method);
    msg.set_payload(playCard.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), data.size());
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
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), data.size());
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
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), data.size());
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("could not send {}: {}", VAR(method), what(error));
    }
}
#endif // PLATFORM_WEB

auto drawMyHand(Context& ctx) -> void
{
    const auto totalWidth = static_cast<float>(std::size(ctx.player.cards) - 1) * cardOverlapX + cardWidth;
    const auto startX = (screenWidth - totalWidth) / 2.0f;
    const auto y = screenHeight - cardHeight - 20.0f; // bottom padding
    for (auto&& [i, card] : ctx.player.cards | rv::enumerate) {
        const auto x = startX + static_cast<float>(i) * cardOverlapX;
        card.position = RVector2{x, y};
        card.texture.Draw(card.position);
    }
}

auto drawOpponentHand(Context& ctx, int count, const float x) -> void
{
    // draw if I have cards
    if (std::empty(ctx.player.cards)) {
        return;
    }
    const auto countF = static_cast<float>(count);
    const auto startY = (screenHeight - cardHeight - (countF - 1.0f) * cardOverlapY) / 2.0f;
    for (auto i = 0.0f; i < countF; ++i) {
        const auto posY = startY + i * cardOverlapY;
        ctx.backCard.texture.Draw(RVector2{x, posY});
    }
}

void drawPlayedCards(Context& ctx)
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
    if (const auto it = ctx.cardsOnTable.find(leftOpponentId); it != ctx.cardsOnTable.cend()) {
        it->second.texture.Draw(leftPlayPos);
    }
    if (const auto it = ctx.cardsOnTable.find(rightOpponentId); it != ctx.cardsOnTable.cend()) {
        it->second.texture.Draw(rightPlayPos);
    }
    if (const auto it = ctx.cardsOnTable.find(ctx.myPlayerId); it != ctx.cardsOnTable.cend()) {
        it->second.texture.Draw(middlePlayPos);
    }
}

auto drawBiddingMenu(Context& ctx) -> void
{
    if (!ctx.bidding.isVisible) {
        return;
    }
    static constexpr auto gap = 8.0f;
    static constexpr auto cellW = 120.0f;
    static constexpr auto cellH = 60.0f;
    static constexpr auto rows = static_cast<int>(bidTable.size());
    static constexpr auto cols = static_cast<int>(bidTable[0].size());
    static constexpr auto menuW = cols * cellW + (cols - 1) * gap;
    static constexpr auto menuH = rows * cellH + (rows - 1) * gap;
    static constexpr auto originX = (screenWidth - menuW) / 2.0f;
    static constexpr auto originY = (screenHeight - menuH) / 2.0f;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const auto& myBid = bidTable[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (std::empty(myBid)) {
                continue;
            }
            const auto myRank = rankOf(myBid);
            const auto isPass = (myBid == "PASS");
            const auto disabled = (not isPass) and (ctx.bidding.rank != allRanks) and (myRank <= ctx.bidding.rank);
            const auto pos = RVector2{
                originX + static_cast<float>(c) * (cellW + gap), //
                originY + static_cast<float>(r) * (cellH + gap)};
            const auto rect = RRectangle{pos, {cellW, cellH}};
            if (disabled) {
                GuiDisable();
            }
            const auto pressed = GuiButton(rect, std::string{myBid}.c_str());
            if (disabled) {
                GuiEnable();
            }
            if (pressed) {
                ctx.bidding.bid = myBid;
                if (not isPass) {
                    ctx.bidding.rank = myRank;
                }
                ctx.bidding.isVisible = false;
                if (std::size(ctx.discardedTalon) == 2) {
                    discardTalon(ctx, myBid); // final bid
                } else {
                    sendBidding(ctx, myBid);
                }
                return;
            }
        }
    }
}

[[nodiscard]] auto isCardPlayable(const Context& ctx, const Card& clickedCard) -> bool
{ // clang-format off
    const auto clickedSuit = suitOf(clickedCard.name);
    const auto trump = getTrump(ctx.bidding.bid);
    const auto hasSuit = [&](const std::string_view suit) {
        return rng::any_of(ctx.player.cards, [&](const std::string& name) { return suitOf(name) == suit; }, &Card::name);
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
auto handleCardClick1(Context& ctx, Action act) -> void
{
    if (!IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        return;
    }
    const auto mousePos = RMouse::GetPosition();
    const auto hit = [&](Card& c) { return RRectangle{c.position, c.texture.GetSize()}.CheckCollision(mousePos); };
    const auto reversed = ctx.player.cards | rv::reverse;
    if (const auto rit = rng::find_if(reversed, hit); rit != rng::cend(reversed)) {
        const auto it = std::next(rit).base();
        const auto _ = gsl::finally([&] { ctx.player.cards.erase(it); });
        act(std::move(*it));
    }
}

template<typename Action>
auto handleCardClick(Context& ctx, Action act) -> void
{
    if (!IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
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
                    ctx.leadSuit = suitOf(name);
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
    drawGameplayScreen(ctx);
    drawBiddingMenu(ctx);

    if (not ctx.hasEnteredName) {
        drawEnterNameScreen(ctx);
        ctx.window.EndDrawing();
        return;
    }
    drawConnectedPlayersPanel(ctx);
    drawMyHand(ctx);
    auto leftX = 40.0f;
    auto rightX = screenWidth - cardWidth - 40.0f;
    drawOpponentHand(ctx, ctx.leftCardCount, leftX);
    drawOpponentHand(ctx, ctx.rightCardCount, rightX);
    drawPlayedCards(ctx);
    ctx.window.DrawFPS(screenWidth - 80, 0);
    ctx.window.EndDrawing();
}

} // namespace
} // namespace pref

auto main() -> int
{
    spdlog::set_pattern("[%^%l%$][%!] %v");
    auto ctx = pref::Context{};
    GuiLoadStyleDefault();
    GuiSetStyle(DEFAULT, TEXT_SIZE, 20);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 2);
    // GuiLoadStyle("resources/styles/style_amber.rgs");

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop_arg(pref::updateDrawFrame, pref::toUserData(ctx), 0, true);
#else // PLATFORM_WEB
    while (!WindowShouldClose()) {
        pref::updateDrawFrame(pref::toUserData(ctx));
    }
#endif // PLATFORM_WEB
    return 0;
}
