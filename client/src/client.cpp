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
};

[[nodiscard]] auto getOpponentIds(Context& ctx) -> std::pair<std::string, std::string>
{
    assert(std::size(ctx.connectedPlayers) == 3U);
    std::vector<std::string> order;
    for (const auto& [id, _] : ctx.connectedPlayers) {
        order.push_back(id);
    }

    const auto it = std::find(order.begin(), order.end(), ctx.myPlayerId);
    assert(it != order.end());

    const auto selfIndex = std::distance(order.begin(), it);
    const auto leftIndex = (selfIndex + 1) % 3;
    const auto rightIndex = (selfIndex + 2) % 3;

    return {order[leftIndex], order[rightIndex]};
}

auto handlePlayCard(Context& ctx, const pref::PlayCard& playCard)
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

    JoinRequest join;
    join.set_player_name(ctx.myPlayerName);
    if (not std::empty(ctx.myPlayerId)) {
        join.set_player_id(ctx.myPlayerId);
    }
    Message msg;
    msg.set_method("JoinRequest");
    msg.set_payload(join.SerializeAsString());

    auto data = msg.SerializeAsString();
    if (const auto error = emscripten_websocket_send_binary(e->socket, data.data(), data.size());
        error != EMSCRIPTEN_RESULT_SUCCESS) {
        WARN("error: could not send JoinRequest: {}", what(error));
    }
    return EM_TRUE;
}

auto on_message([[maybe_unused]] int eventType, const EmscriptenWebSocketMessageEvent* e, void* userData) -> EM_BOOL
{
    INFO("{}, socket: {}, numBytes: {}, isText: {}", VAR(eventType), e->socket, e->numBytes, e->isText);
    assert(userData);
    auto& ctx = toContext(userData);

    if (e->isText) {
        WARN("error: expect binary data");
        return EM_TRUE;
    }
    Message msg;
    if (not msg.ParseFromArray(e->data, static_cast<int>(e->numBytes))) {
        WARN("Failed to parse Message");
        return EM_TRUE;
    }

    const std::string& method = msg.method();
    const std::string& payload = msg.payload();
    INFO("{}", method);

    if (method == "JoinResponse") {
        auto join = JoinResponse{};
        if (not join.ParseFromString(payload)) {
            WARN("Failed to parse JoinResponse");
            return EM_TRUE;
        }

        if (ctx.myPlayerId != join.player_id()) {
            ctx.myPlayerId = join.player_id();
            INFO("save myPlayerId: {}", ctx.myPlayerId);
            savePlayerIdToLocalStorage(ctx.myPlayerId);
        }

        ctx.connectedPlayers.clear();
        for (const auto& player : join.players()) {
            if (player.player_id() == ctx.myPlayerId and ctx.myPlayerName != player.player_name()) {
                ctx.myPlayerName = player.player_name();
            }
            ctx.connectedPlayers.insert_or_assign(player.player_id(), player.player_name());
        }
    } else if (method == "PlayerJoined") {
        auto joined = PlayerJoined{};
        if (not joined.ParseFromString(msg.payload())) {
            printf("Failed to parse PlayerJoined\n");
            return EM_TRUE;
        }
        const auto& joinedId = joined.player_id();
        const auto& joinedName = joined.player_name();

        INFO("New player joined: {} ({})", joinedName, joinedId);
        ctx.connectedPlayers.insert_or_assign(joinedId, joinedName);
    } else if (msg.method() == "PlayerLeft") {
        PlayerLeft playerLeft;
        if (not playerLeft.ParseFromString(msg.payload())) {
            WARN("Failed to parse PlayerLeft");
            return EM_TRUE;
        }

        const std::string& id = playerLeft.player_id();
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

void DrawConnectedPlayersPanel(const Context& ctx)
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

auto DrawMyHand(Context& ctx) -> void
{
    for (const auto& card : ctx.player.cards) {
        card.texture.Draw(card.position);
    }
}

auto DrawOpponentHand(Context& ctx, std::size_t count, const float x) -> void
{
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

auto UpdateDrawFrame(void* userData) -> void
{
    assert(userData);
    auto& ctx = toContext(userData);
    if (std::empty(ctx.myPlayerId)) {
        ctx.myPlayerId = loadPlayerIdFromLocalStorage();
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
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
