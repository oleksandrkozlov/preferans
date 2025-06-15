#if defined(PLATFORM_WEB)
#include "proto/pref.pb.h"

#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <emscripten/websocket.h>
#endif // PLATFORM_WEB

#include <fmt/format.h>
#include <range/v3/all.hpp>
#include <raylib-cpp.hpp>
#include <spdlog/spdlog.h>
#define RAYGUI_IMPLEMENTATION
#include "common/logger.hpp"

#include <raygui.h>

#include <array>
#include <cassert>
#include <initializer_list>
#include <list>
#include <string_view>
#include <vector>

namespace pref {
namespace {

constexpr const auto ratioWidth = 16;
constexpr const auto ratioHeigh = 9;
constexpr const auto screenWidth = 1920;
constexpr const auto screenHeight = (screenWidth * ratioHeigh) / ratioWidth;

constexpr auto originalCardHeight = 726.0f;
constexpr auto originalCardWidth = 500.0f;
constexpr auto cardAspectRatio = originalCardWidth / originalCardHeight;
constexpr auto cardHeight = screenHeight / 5.0f; // 5th part of screen's height
constexpr auto cardWidth = cardHeight * cardAspectRatio;
constexpr auto cardOverlapX = cardWidth * 0.6f; // 60% overlap
constexpr auto cardOverlapY = cardHeight * 0.2f;

using namespace std::literals;

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

struct BiddingMenu {
    bool isVisible{}; // render / accept clicks
    std::string selection; // text button
};

constexpr auto bidLabels = std::array<std::array<std::string_view, 6>, 6>{
    {{"6S", "6C", "6D", "6H", "6WT", ""},
     {"7S", "7C", "7D", "7H", "7WT", ""},
     {"8S", "8C", "8D", "8H", "8WT", ""},
     {"9S", "9C", "9D", "9H", "9WT", "9WP"},
     {"10S", "10C", "10D", "10H", "10WT", "10WP"},
     {"", "", "", "MISER", "MISER WP", "PASS"}}};

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
    EMSCRIPTEN_WEBSOCKET_T ws{};
    int leftCardCount = 10;
    int rightCardCount = 10;
    std::map<std::string, Card> cardsOnTable;
    std::string turnPlayerId;
    BiddingMenu bidding;
    std::string stage;
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

auto handlePlayerTurn(Context& ctx, const PlayerTurn& playerTurn) -> void
{
    ctx.turnPlayerId = playerTurn.player_id();
    ctx.stage = playerTurn.stage();
    if (ctx.turnPlayerId == ctx.myPlayerId) {
        INFO("Your turn");
        if (ctx.stage == "Bidding") {
            ctx.bidding.isVisible = true;
        }
    } else {
        INFO_VAR(ctx.turnPlayerId);
    }
}

auto handlePlayCard(Context& ctx, const PlayCard& playCard) -> void
{
    auto [leftOpponentId, rightOpponentId] = getOpponentIds(ctx);
    const auto& playerId = playCard.player_id();
    const auto& cardName = playCard.card(); // "queen_of_hearts"
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

    ctx.cardsOnTable.insert_or_assign(playerId, Card{cardName, RVector2{}});
}

#if defined(PLATFORM_WEB)

[[nodiscard]] auto toContext(void* userData) noexcept -> Context&
{
    return *static_cast<Context*>(userData);
}

[[nodiscard]] auto toUserData(Context& ctx) noexcept -> void*
{
    return static_cast<void*>(&ctx);
}

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

[[maybe_unused]] void clearPlayerIdFromLocalStorage()
{
    emscripten_run_script("localStorage.removeItem('preferans_player_id');");
}

EM_BOOL on_open([[maybe_unused]] int eventType, const EmscriptenWebSocketOpenEvent* e, void* userData)
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
    msg.set_method("JoinRequest");
    msg.set_payload(joinRequest.SerializeAsString());

    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(e->socket, data.data(), data.size());
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("error: could not send JoinRequest: {}", what(error));
    }
    return EM_TRUE;
}

auto on_message([[maybe_unused]] const int eventType, const EmscriptenWebSocketMessageEvent* e, void* userData)
    -> EM_BOOL
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
        for (const auto& player : joinResponse.players()) {
            if (player.player_id() == ctx.myPlayerId and ctx.myPlayerName != player.player_name()) {
                ctx.myPlayerName = player.player_name();
            }
            ctx.connectedPlayers.insert_or_assign(player.player_id(), player.player_name());
        }
    } else if (method == "PlayerJoined") {
        auto playerJoined = PlayerJoined{};
        if (not playerJoined.ParseFromString(msg.payload())) {
            printf("Failed to parse PlayerJoined\n");
            return EM_TRUE;
        }
        const auto& playerJoinedId = playerJoined.player_id();
        const auto& playerJoinedName = playerJoined.player_name();

        INFO("New player playerJoined: {} ({})", playerJoinedName, playerJoinedId);
        ctx.connectedPlayers.insert_or_assign(playerJoinedId, playerJoinedName);
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

        assert(std::size(dealCards.cards()) == 10u);
        const auto totalWidth = static_cast<float>(std::size(dealCards.cards()) - 1) * cardOverlapX + cardWidth;
        const auto startX = (screenWidth - totalWidth) / 2.0f;
        const auto y = screenHeight - cardHeight - 20.0f; // bottom padding

        for (int i = 0; i < std::size(dealCards.cards()); ++i) {
            const auto x = startX + static_cast<float>(i) * cardOverlapX;
            ctx.player.cards.emplace_back(dealCards.cards()[static_cast<int>(i)], RVector2{x, y});
        }
    } else if (msg.method() == "PlayerTurn") {
        auto playerTurn = PlayerTurn{};
        if (not playerTurn.ParseFromString(msg.payload())) {
            WARN("error: failed to parse PlayerTurn");
            return EM_TRUE;
        }
        handlePlayerTurn(ctx, playerTurn);
    } else if (msg.method() == "Bidding") {
        auto bidding = Bidding{};
        if (not bidding.ParseFromString(msg.payload())) {
            WARN("error: failed to parse Bidding");
            return EM_TRUE;
        }
        const auto& playerId = bidding.player_id();
        const auto& bid = bidding.bid();
        INFO_VAR(playerId, bid);
    } else if (msg.method() == "PlayCard") {
        auto playCard = PlayCard{};
        if (not playCard.ParseFromString(msg.payload())) {
            WARN("error: failed to parse PlayCard");
            return EM_TRUE;
        }
        handlePlayCard(ctx, playCard);
    } else {
        WARN("error: unknown method: {}", msg.method());
    }

    return EM_TRUE;
}

auto on_error(
    [[maybe_unused]] int eventType,
    [[maybe_unused]] const EmscriptenWebSocketErrorEvent* e,
    [[maybe_unused]] void* userData) -> EM_BOOL
{
    INFO("{}, socket: {}", VAR(eventType), e->socket);
    assert(userData);
    [[maybe_unused]] auto& ctx = toContext(userData);

    return EM_TRUE;
}

auto on_close(
    [[maybe_unused]] int eventType,
    [[maybe_unused]] const EmscriptenWebSocketCloseEvent* e,
    [[maybe_unused]] void* userData) -> EM_BOOL
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
    attr.url = "ws://localhost:8080"; // Or wss:// if using HTTPS
    attr.protocols = nullptr;
    attr.createOnMainThread = EM_TRUE;

    ctx.ws = emscripten_websocket_new(&attr);
    if (ctx.ws <= 0) {
        WARN("Failed to create WebSocket");
        return;
    }

    emscripten_websocket_set_onopen_callback(ctx.ws, &ctx, on_open);
    emscripten_websocket_set_onmessage_callback(ctx.ws, &ctx, on_message);
    emscripten_websocket_set_onerror_callback(ctx.ws, &ctx, on_error);
    emscripten_websocket_set_onclose_callback(ctx.ws, &ctx, on_close);
}
#endif // PLATFORM_WEB

void DrawGameplayScreen(Context& ctx)
{
    const std::string title = "PREFERANS GAME";
    const float fontSize = static_cast<float>(ctx.font.baseSize) * 3.0f;
    const auto textSize = MeasureText(title.c_str(), static_cast<int>(fontSize));
    const auto x = static_cast<float>(screenWidth - textSize) / 2.0f;
    RText::Draw(
        title,
        {x, 20},
        static_cast<int>(fontSize),
        RColor{static_cast<unsigned int>(GuiGetStyle(LABEL, TEXT_COLOR_NORMAL))});
}

void DrawEnterNameScreen(Context& ctx)
{
    static char nameBuffer[32] = "";
    static bool editMode = true;

    const RVector2 screenCenter = ctx.screen / 2.0f;
    const float boxWidth = 400.0f;
    const float boxHeight = 60.0f;

    const RVector2 boxPos{screenCenter.x - boxWidth / 2.0f, screenCenter.y};

    // Label
    const RVector2 labelPos{boxPos.x, boxPos.y - 40.0f};
    RText::Draw(
        "Enter your name:", labelPos, 20.0f, RColor{static_cast<unsigned int>(GuiGetStyle(LABEL, TEXT_COLOR_NORMAL))});

    // Text box
    RRectangle inputBox = {boxPos, {boxWidth, boxHeight}};
    GuiTextBox(inputBox, nameBuffer, sizeof(nameBuffer), editMode);

    // Button
    RRectangle buttonBox = {boxPos.x, boxPos.y + boxHeight + 20.0f, boxWidth, 40.0f};
    bool clicked = GuiButton(buttonBox, "Start");

    // Accept with Enter or button click
    if ((clicked || RKeyboard::IsKeyPressed(KEY_ENTER)) && nameBuffer[0] != '\0' && editMode) {
        ctx.myPlayerName = nameBuffer;
        ctx.hasEnteredName = true;
        editMode = false; // Disable further typing

#if defined(PLATFORM_WEB)
        setup_websocket(ctx);
#endif
    }
}

auto DrawConnectedPlayersPanel(const Context& ctx) -> void
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
    RText::Draw(
        "Connected Players", textPos, 20.0f, RColor{static_cast<unsigned int>(GuiGetStyle(LABEL, TEXT_COLOR_NORMAL))});

    // Move text position down for player list
    textPos.y += 30.0f;

    for (const auto& [id, name] : ctx.connectedPlayers) {
        // Highlight the current player's name in dark blue
        Color color = (id == ctx.myPlayerId)
            ? RColor{static_cast<unsigned int>(GuiGetStyle(DEFAULT, TEXT_COLOR_NORMAL))}
            : RColor{static_cast<unsigned int>(GuiGetStyle(DEFAULT, TEXT_COLOR_DISABLED))};
        // Draw the player's name
        RText::Draw(name, textPos, 18.0f, color);
        // ctx.font.DrawText(name, textPos, 18.0f, 1.0f, color);

        // Move down for the next name
        textPos.y += 24.0f;
    }
}

auto playCard(Context& ctx, const std::string& cardName) -> void
{
    auto playCard = PlayCard{};
    playCard.set_player_id(ctx.myPlayerId);
    playCard.set_card(cardName);
    auto msg = Message{};
    msg.set_method("PlayCard");
    msg.set_payload(playCard.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), data.size());
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("error: could not send PlayCard: {}", what(error));
    }
}

auto sendBidding(Context& ctx, const std::string_view bid) -> void
{
    auto bidding = Bidding{};
    bidding.set_player_id(ctx.myPlayerId);
    bidding.set_bid(std::string{bid});
    auto msg = Message{};
    msg.set_method("Bidding");
    msg.set_payload(bidding.SerializeAsString());
    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(ctx.ws, data.data(), data.size());
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("could not send Bidding: {}", what(error));
    }
}

auto DrawMyHand(Context& ctx) -> void
{
    for (const auto& card : ctx.player.cards) {
        card.texture.Draw(card.position);
    }
}

auto DrawOpponentHand(Context& ctx, int count, const float x) -> void
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

void DrawPlayedCards(Context& ctx)
{
    if (std::empty(ctx.cardsOnTable)) {
        return;
    }
    const float cardSpacing = cardWidth * 0.1f;

    const RVector2 centerPos = {screenWidth / 2.0f, screenHeight / 2.0f - cardHeight / 2.0f};
    const RVector2 leftPlayPos = {centerPos.x - cardWidth - cardSpacing, centerPos.y};
    const RVector2 middlePlayPos = centerPos;
    const RVector2 rightPlayPos = {centerPos.x + cardWidth + cardSpacing, centerPos.y};

    auto [leftOpponentId, rightOpponentId] = getOpponentIds(ctx);
    if (auto it = ctx.cardsOnTable.find(leftOpponentId); it != ctx.cardsOnTable.end()) {
        it->second.texture.Draw(leftPlayPos);
    }

    if (auto it = ctx.cardsOnTable.find(rightOpponentId); it != ctx.cardsOnTable.end()) {
        it->second.texture.Draw(rightPlayPos);
    }

    if (auto it = ctx.cardsOnTable.find(ctx.myPlayerId); it != ctx.cardsOnTable.end()) {
        it->second.texture.Draw(middlePlayPos);
    }
}

auto DrawBiddingMenu(Context& ctx) -> void
{
    if (!ctx.bidding.isVisible) {
        return;
    }

    constexpr float gap = 8.0f;
    constexpr float cellW = 120.0f;
    constexpr float cellH = 60.0f;
    constexpr int rows = bidLabels.size();
    constexpr int cols = bidLabels[0].size();

    const float menuW = cols * cellW + (cols - 1) * gap;
    const float menuH = rows * cellH + (rows - 1) * gap;
    const float originX = (screenWidth - menuW) / 2.0f;
    const float originY = (screenHeight - menuH) / 2.0f;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const auto& label = bidLabels[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (label.empty())
                continue;

            Rectangle rect{originX + c * (cellW + gap), originY + r * (cellH + gap), cellW, cellH};

            if (GuiButton(rect, label.data())) {
                ctx.bidding.selection = std::string(label);
                ctx.bidding.isVisible = false;
                sendBidding(ctx, label);
            }
        }
    }
}

auto UpdateDrawFrame(void* userData) -> void
{
    assert(userData);
    auto& ctx = toContext(userData);
    if (std::empty(ctx.myPlayerId)) {
        ctx.myPlayerId = loadPlayerIdFromLocalStorage();
    }

    if ((ctx.myPlayerId == ctx.turnPlayerId) //
        and (ctx.stage == "Playing") //
        and IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        const auto mousePosition = RMouse::GetPosition();
        const auto isClicked = [&](auto& card) {
            return RRectangle{card.position, card.texture.GetSize()}.CheckCollision(mousePosition);
        };
        for (auto&& card : ctx.player.cards //
                 | ranges::views::reverse //
                 | ranges::views::filter(isClicked) //
                 | ranges::views::take(1)) { // only first match
            const auto name = card.name;
            ctx.cardsOnTable.insert_or_assign(ctx.myPlayerId, std::move(card));
            ctx.player.cards.remove_if([&](const Card& card) { return (card.name == name); });
            playCard(ctx, name);
        }
    }

    ctx.window.BeginDrawing();
    ctx.window.ClearBackground(RColor{static_cast<unsigned int>(GuiGetStyle(DEFAULT, BACKGROUND_COLOR))});
    DrawGameplayScreen(ctx);
    DrawBiddingMenu(ctx);

    if (not ctx.hasEnteredName) {
        DrawEnterNameScreen(ctx);
        ctx.window.EndDrawing();
        return;
    }
    DrawConnectedPlayersPanel(ctx);

    DrawMyHand(ctx);
    auto leftX = 40.0f;
    auto rightX = screenWidth - cardWidth - 40.0f;
    DrawOpponentHand(ctx, ctx.leftCardCount, leftX); // Left
    DrawOpponentHand(ctx, ctx.rightCardCount, rightX); // Right
    DrawPlayedCards(ctx);
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
    emscripten_set_main_loop_arg(pref::UpdateDrawFrame, pref::toUserData(ctx), 0, true);
#else // PLATFORM_WEB
    while (!WindowShouldClose()) {
        pref::UpdateDrawFrame(pref::toUserData(ctx));
    }
#endif // PLATFORM_WEB
    return 0;
}
