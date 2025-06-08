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
#include <string_view>
#include <vector>

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

[[nodiscard]] constexpr auto getCloseReason(const std::uint16_t code) -> std::string_view
{
    switch (code) { // clang-format off
    case 1000: return "Normal Closure";
    case 1001: return "Going Away";
    case 1002: return "Protocol Error";
    case 1003: return "Unsupported Data";
    case 1005: return "No Status Received";
    case 1006: return "Abnormal Closure";
    case 1007: return "Invalid Payload";
    case 1008: return "Policy Violation";
    case 1009: return "Message Too Big";
    case 1010: return "Mandatory Extension";
    case 1011: return "Internal Error";
    case 1012: return "Service Restart";
    case 1013: return "Try Again Later";
    case 1014: return "Bad Gateway";
    case 1015: return "TLS Handshake Failed";
    } // clang-format on
    return "Unknown";
}


struct Card {
    Card(const std::string_view card, RVector2 pos)
        : position{pos}
        , image{RImage{fmt::format("resources/cards/{}.png", card)}}
    {
        image.Resize(static_cast<int>(cardWidth), static_cast<int>(cardHeight));
        texture = image.LoadTexture();
    }

    RVector2 position;
    bool isDragging{};
    RVector2 offset;
    RImage image;
    RTexture texture{};
};

struct Player {
    std::vector<Card> cards;
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
};

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

    pref::JoinRequest join;
    join.set_player_name(ctx.myPlayerName);
    if (not std::empty(ctx.myPlayerId)) {
        join.set_player_id(ctx.myPlayerId);
    }
    pref::Message msg;
    msg.set_method("JoinRequest");
    msg.set_payload(join.SerializeAsString());

    auto data = msg.SerializeAsString();
    emscripten_websocket_send_binary(e->socket, data.data(), data.size());
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
    pref::Message msg;
    if (not msg.ParseFromArray(e->data, static_cast<int>(e->numBytes))) {
        WARN("Failed to parse Message");
        return EM_TRUE;
    }

    const std::string& method = msg.method();
    const std::string& payload = msg.payload();
    INFO("{}", method);

    if (method == "JoinResponse") {
        auto join = pref::JoinResponse{};
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
        auto joined = pref::PlayerJoined{};
        if (not joined.ParseFromString(msg.payload())) {
            printf("Failed to parse PlayerJoined\n");
            return EM_TRUE;
        }
        const auto& joinedId = joined.player_id();
        const auto& joinedName = joined.player_name();

        INFO("New player joined: {} ({})", joinedName, joinedId);
        ctx.connectedPlayers.insert_or_assign(joinedId, joinedName);
    } else if (msg.method() == "PlayerLeft") {
        pref::PlayerLeft playerLeft;
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
        auto dealCards = pref::DealCards{};
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

    EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attr);
    if (ws <= 0) {
        WARN("Failed to create WebSocket");
        return;
    }

    emscripten_websocket_set_onopen_callback(ws, &ctx, on_open);
    emscripten_websocket_set_onmessage_callback(ws, &ctx, on_message);
    emscripten_websocket_set_onerror_callback(ws, &ctx, on_error);
    emscripten_websocket_set_onclose_callback(ws, &ctx, on_close);
}
#endif // PLATFORM_WEB

void DrawGameplayScreen(Context& ctx)
{
    const std::string title = "PREFERANS GAME";
    const float fontSize = static_cast<float>(ctx.font.baseSize) * 3.0f;
    const auto textSize = MeasureText(title.c_str(), fontSize);
    const auto x = (screenWidth - textSize) / 2.0f;
    RText::Draw(title, {x, 20}, fontSize, GetColor(GuiGetStyle(LABEL, TEXT_COLOR_NORMAL)));
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
    RText::Draw("Enter your name:", labelPos, 20.0f, GetColor(GuiGetStyle(LABEL, TEXT_COLOR_NORMAL)));

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
    RText::Draw("Connected Players", textPos, 20.0f, GetColor(GuiGetStyle(LABEL, TEXT_COLOR_NORMAL)));

    // Move text position down for player list
    textPos.y += 30.0f;

    for (const auto& [id, name] : ctx.connectedPlayers) {
        // Highlight the current player's name in dark blue
        Color color = (id == ctx.myPlayerId) ? GetColor(GuiGetStyle(DEFAULT, TEXT_COLOR_NORMAL))
                                             : GetColor(GuiGetStyle(DEFAULT, TEXT_COLOR_DISABLED));
        // Draw the player's name
        RText::Draw(name, textPos, 18.0f, color);
        // ctx.font.DrawText(name, textPos, 18.0f, 1.0f, color);

        // Move down for the next name
        textPos.y += 24.0f;
    }
}

void DrawVerticalStackedCards(Context& ctx, int count, float x)
{
    const auto startY = (screenHeight - (count - 1) * cardOverlapY - cardHeight) / 2.0f;
    for (int i = 0; i < count; ++i) {
        const auto posY = startY + i * cardOverlapY;
        ctx.backCard.texture.Draw(RVector2{x, posY});
    }
}

void UpdateDrawFrame(void* userData)
{
    assert(userData);
    auto& ctx = toContext(userData);
    if (std::empty(ctx.myPlayerId)) {
        ctx.myPlayerId = loadPlayerIdFromLocalStorage();
    }

    const auto mouse = RMouse::GetPosition();
    // Handle mouse press - start isDragging if on top of a card
    for (auto i = static_cast<int>(std::size(ctx.player.cards)) - 1; i >= 0; --i) {

        auto& card = ctx.player.cards[static_cast<std::size_t>(i)];
        auto cardBounds = RRectangle{card.position, card.texture.GetSize()};

        if (RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON) and mouse.CheckCollision(cardBounds)) {
            card.isDragging = true;
            card.offset = RVector2{mouse.x - card.position.x, mouse.y - card.position.y};
            auto draggedCard = std::move(card);
            ctx.player.cards.erase(std::begin(ctx.player.cards) + i);
            ctx.player.cards.push_back(std::move(draggedCard)); // now on top
            break; // Only pick the topmost card
        }
    }
    if (auto draggedCard = ranges::find_if(ctx.player.cards, &Card::isDragging);
        draggedCard != ranges::end(ctx.player.cards)) {
        if (RMouse::IsButtonReleased(MOUSE_LEFT_BUTTON)) {
            draggedCard->isDragging = false;
        } else if (RMouse::IsButtonDown(MOUSE_LEFT_BUTTON)) {
            draggedCard->position.x = mouse.x - draggedCard->offset.x;
            draggedCard->position.y = mouse.y - draggedCard->offset.y;
        }
    }

    ctx.window.BeginDrawing();
    ctx.window.ClearBackground(GetColor(GuiGetStyle(DEFAULT, BACKGROUND_COLOR)));
    DrawGameplayScreen(ctx);

    if (not ctx.hasEnteredName) {
        DrawEnterNameScreen(ctx);
        ctx.window.EndDrawing();
        return;
    }
    DrawConnectedPlayersPanel(ctx);

    for (const auto& card : ctx.player.cards) {
        card.texture.Draw(card.position);
    }

    DrawVerticalStackedCards(ctx, 10, 40.0f); // Left
    DrawVerticalStackedCards(ctx, 10, screenWidth - cardWidth - 40.0f); // Right

    ctx.window.DrawFPS(screenWidth - 80, 0);
    ctx.window.EndDrawing();
}

} // namespace

auto main() -> int
{
    spdlog::set_pattern("[%^%l%$][%!] %v");
    auto ctx = Context{};
    GuiLoadStyleDefault();
    // GuiLoadStyle("resources/styles/style_amber.rgs");

#if defined(PLATFORM_WEB)
        emscripten_set_main_loop_arg(UpdateDrawFrame, toUserData(ctx), 0, true);
#else // PLATFORM_WEB
        while (!WindowShouldClose()) {
        UpdateDrawFrame(toUserData(ctx));
    }
#endif // PLATFORM_WEB
    return 0;
}
