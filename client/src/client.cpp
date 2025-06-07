#include <array>
#if defined(PLATFORM_WEB)
#include "proto/pref.pb.h"

#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#endif // PLATFORM_WEB

#include <fmt/format.h>
#include <range/v3/all.hpp>
#include <raylib-cpp.hpp>
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>

#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr const auto ratioWidth = 16;
constexpr const auto ratioHeigh = 9;
constexpr const auto screenWidth = 1920;
constexpr const auto screenHeight = (screenWidth * ratioHeigh) / ratioWidth;

using namespace std::literals;

struct Card {
    Card(const std::string_view suit, const std::string_view rank, RVector2 pos)
        : position{pos}
        , image{RImage{fmt::format("resources/cards/{}_of_{}.png", rank, suit)}}
    {

        // image = LoadImage("resources/cards/cardBackRed.png");
        const auto newImageHeight = screenHeight / 5.0; // 5th part of screen's height
        const auto imageScale = image.height / newImageHeight;
        const auto newImageWidth = image.width / imageScale;
        image.Resize(static_cast<int>(newImageWidth), static_cast<int>(newImageHeight));
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
    Context()
    {
        const auto cardCount = 32 + 1;
        const auto suits = std::vector<std::string>{"spades", "diamonds", "clubs", "hearts"};
        const auto ranks = std::vector<std::string>{"7", "8", "9", "10", "jack", "queen", "king", "ace"};
        auto deck = ranges::views::cartesian_product(suits, ranks) //
            | ranges::to_vector //
            | ranges::actions::shuffle(std::mt19937{std::invoke(std::random_device{})});

        for (int p = 0; p < 3; ++p) {
            auto x = 600.0f;
            for (int i = 0; i < 10; ++i) {
                auto& [suit, rank] = deck.back();
                auto card = Card{suit, rank, RVector2{x, (p + 1) * 250.f}};
                players[p].cards.push_back(std::move(card));
                deck.pop_back();
                const auto cardVisability = (screenWidth /* - card.image.width*/) / cardCount;
                x += cardVisability;
            }
        }
    }

    RFont font{"resources/mecha.png"};
    RVector2 screen{screenWidth, screenHeight};
    RWindow window{static_cast<int>(screen.x), static_cast<int>(screen.y), "Preferans"};
    std::array<Player, 3> players;

    std::string playerName;
    bool hasEnteredName = false;
    std::map<std::string, std::string> connectedPlayers;
    std::string myPlayerId;
};

#if defined(PLATFORM_WEB)
EM_BOOL on_open([[maybe_unused]] int eventType, const EmscriptenWebSocketOpenEvent* e, void* userData)
{
    assert(userData);
    auto& ctx = *static_cast<Context*>(userData);

    printf("WebSocket opened!\n");

    pref::JoinRequest join;
    join.set_player_name(ctx.playerName);

    pref::RpcEnvelope env;
    env.set_method("JoinRequest");
    env.set_payload(join.SerializeAsString());

    auto data = env.SerializeAsString();
    emscripten_websocket_send_binary(e->socket, data.data(), data.size());
    /*
        auto env = pref::RpcEnvelope{};
        env.set_method("SayHello");
        auto request = pref::HelloRequest{};
        request.set_name("Venichka");
        auto requestPayload = std::string{};
        if (not request.SerializeToString(&requestPayload)) {
            printf("Failed to serialize HelloRequest\n");
            return EM_FALSE;
        }
        env.set_payload(requestPayload);
        auto envPayload = std::string{};
        if (not env.SerializeToString(&envPayload)) {
            printf("Failed to serialize RpcEnvelope\n");
            return EM_FALSE;
        }

        auto result = emscripten_websocket_send_binary(e->socket, envPayload.data(), envPayload.size());

        if (result != EMSCRIPTEN_RESULT_SUCCESS) {
            printf("Failed to send protobuf message: %d\n", result);
        }
        */
    return EM_TRUE;
}

auto on_message([[maybe_unused]] int eventType, const EmscriptenWebSocketMessageEvent* e, void* userData) -> EM_BOOL
{
    printf("on_message\n");
    assert(userData);
    auto& ctx = *static_cast<Context*>(userData);

    if (e->isText) {
        return EM_TRUE;
    }
    pref::RpcEnvelope env;
    if (!env.ParseFromArray(e->data, static_cast<int>(e->numBytes))) {
        printf("Failed to parse RpcEnvelope\n");
        return EM_TRUE;
    }

    const std::string& method = env.method();
    const std::string& payload = env.payload();
    printf("%s\n", method.c_str());

    if (env.method() == "JoinResponse") {
        pref::JoinResponse msg;
        if (!msg.ParseFromString(env.payload())) {
            printf("Failed to parse JoinResponse\n");
            return EM_TRUE;
        }

        ctx.connectedPlayers.clear();

        for (const auto& player : msg.players()) {
            ctx.connectedPlayers[player.player_id()] = player.player_name();
        }

        printf("You joined as: %s\n", msg.player_id().c_str());
        printf("Connected players:\n");
        for (const auto& [id, name] : ctx.connectedPlayers) {
            printf("  - %s (%s)\n", name.c_str(), id.c_str());
        }
    }

    if (method == "JoinResponse") {
        pref::JoinResponse join;
        if (!join.ParseFromString(payload)) {
            printf("Failed to parse JoinResponse\n");
            return EM_TRUE;
        }

        ctx.myPlayerId = join.player_id();
        ctx.connectedPlayers.clear();
        for (const auto& player : join.players()) {
            ctx.connectedPlayers.insert_or_assign(player.player_id(), player.player_name());
        }
    } else if (env.method() == "PlayerLeft") {
        pref::PlayerLeft msg;
        if (!msg.ParseFromString(env.payload())) {
            printf("Failed to parse PlayerLeft\n");
            return EM_TRUE;
        }

        const std::string& id = msg.player_id();
        auto it = ctx.connectedPlayers.find(id);

        if (it != ctx.connectedPlayers.end()) {
            printf("Player left: %s (%s)\n", it->second.c_str(), id.c_str());
            ctx.connectedPlayers.erase(it);
            printf("Updated player list:\n");
            for (const auto& [i, name] : ctx.connectedPlayers) {
                printf("  - %s (%s)\n", name.c_str(), i.c_str());
            }
        } else {
            printf("Player with ID %s left (name unknown)\n", id.c_str());
        }
    }

    /*
    if (!e->isText) {
        pref::RpcEnvelope env;
        if (!env.ParseFromArray(e->data, e->numBytes)) {
            printf("Failed to parse RpcEnvelope\n");
            return EM_TRUE;
        }

        const std::string& method = env.method();
        const std::string& payload = env.payload();

        if (method == "SayHelloResponse") {
            pref::HelloResponse response;
            if (!response.ParseFromString(payload)) {
                printf("Failed to parse HelloResponse\n");
                return EM_TRUE;
            }

            printf("Got message: %s\n", response.message().c_str());
        } else {
            printf("Unknown RPC method: %s\n", method.c_str());
        }
    }
    */

    return EM_TRUE;
}

EM_BOOL on_error(
    [[maybe_unused]] int eventType,
    [[maybe_unused]] const EmscriptenWebSocketErrorEvent* websocketEvent,
    [[maybe_unused]] void* userData)
{
    puts("on_error");

    return EM_TRUE;
}
EM_BOOL on_close(
    [[maybe_unused]] int eventType,
    [[maybe_unused]] const EmscriptenWebSocketCloseEvent* websocketEvent,
    [[maybe_unused]] void* userData)
{
    puts("on_close");

    return EM_TRUE;
}

void setup_websocket(Context& ctx)
{
    if (!emscripten_websocket_is_supported()) {
        printf("WebSocket is not supported\n");
        return;
    }

    EmscriptenWebSocketCreateAttributes attr = {};
    attr.url = "ws://localhost:8080"; // Or wss:// if using HTTPS
    attr.protocols = nullptr;
    attr.createOnMainThread = EM_TRUE;

    EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attr);
    if (ws <= 0) {
        printf("Failed to create WebSocket\n");
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
    RRectangle{{}, ctx.screen}.Draw(DARKGREEN);
    ctx.font.DrawText("PREFERANS GAME", {20, 10}, static_cast<float>(ctx.font.baseSize) * 3.0f, 4, BLACK);
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
    ctx.font.DrawText("Enter your name:", labelPos, 20.0f, 1.0f, BLACK);

    // Text box
    RRectangle inputBox = {boxPos, {boxWidth, boxHeight}};
    GuiTextBox(inputBox, nameBuffer, sizeof(nameBuffer), editMode);

    // Button
    RRectangle buttonBox = {boxPos.x, boxPos.y + boxHeight + 20.0f, boxWidth, 40.0f};
    bool clicked = GuiButton(buttonBox, "Start");

    // Accept with Enter or button click
    if ((clicked || RKeyboard::IsKeyPressed(KEY_ENTER)) && nameBuffer[0] != '\0' && editMode) {
        ctx.playerName = nameBuffer;
        ctx.hasEnteredName = true;
        editMode = false; // Disable further typing

#if defined(PLATFORM_WEB)
        setup_websocket(ctx);
#endif
    }
}

void DrawConnectedPlayersPanel(const Context& ctx)
{
    const float x = ctx.screen.x - 260.0f;
    const float y = 20.0f;
    const float width = 240.0f;
    const float height = 160.0f; // Reduced height to fit 4 names comfortably

    Rectangle panelRect = {x, y, width, height};
    GuiPanel(panelRect, nullptr); // No title

    RVector2 textPos = {x + 10.0f, y + 10.0f};
    ctx.font.DrawText("Connected Players", textPos, 20.0f, 1.0f, BLACK);

    textPos.y += 30.0f;

    for (const auto& [_, name] : ctx.connectedPlayers) {
        Color color = (name == ctx.playerName) ? DARKBLUE : DARKGRAY;
        ctx.font.DrawText(name, textPos, 18.0f, 1.0f, color);
        textPos.y += 24.0f;
    }
}

void UpdateDrawFrame(void* userData)
{
    assert(userData);
    auto& ctx = *static_cast<Context*>(userData);
    /*
        const auto mouse = RMouse::GetPosition();
        // Handle mouse press - start isDragging if on top of a card
        auto isCarPicked = false;
        for (auto& player : ctx.players) {
            for (auto i = static_cast<int>(std::size(player.cards)) - 1; i >= 0; --i) {

                auto& card = player.cards[static_cast<std::size_t>(i)];
                auto cardBounds = RRectangle{card.position, card.texture.GetSize()};

                if (RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON) and mouse.CheckCollision(cardBounds)) {
                    card.isDragging = true;
                    card.offset = RVector2{mouse.x - card.position.x, mouse.y - card.position.y};
                    auto draggedCard = std::move(card);
                    player.cards.erase(std::begin(player.cards) + i);
                    player.cards.push_back(std::move(draggedCard)); // now on top
                    isCarPicked = true;
                    break; // Only pick the topmost card
                }
            }
            if (isCarPicked) {
                isCarPicked = false;
                break;
            }
            if (auto draggedCard = ranges::find_if(player.cards, &Card::isDragging);
                draggedCard != ranges::end(player.cards)) {
                if (RMouse::IsButtonReleased(MOUSE_LEFT_BUTTON)) {
                    draggedCard->isDragging = false;
                } else if (RMouse::IsButtonDown(MOUSE_LEFT_BUTTON)) {
                    draggedCard->position.x = mouse.x - draggedCard->offset.x;
                    draggedCard->position.y = mouse.y - draggedCard->offset.y;
                }
            }
        }
        */

    ctx.window.BeginDrawing();
    ctx.window.ClearBackground();
    DrawGameplayScreen(ctx);

    if (!ctx.hasEnteredName) {
        DrawEnterNameScreen(ctx);
        ctx.window.EndDrawing();
        return;
    }
    DrawConnectedPlayersPanel(ctx);

    // Draw all cards
    /*
        for (const auto& player : ctx.players) {
            for (const auto& card : player.cards) {
                card.texture.Draw(card.position);
            }
        }
        */

    ctx.window.DrawFPS(screenWidth - 80, 0);
    ctx.window.EndDrawing();
}

} // namespace

auto main() -> int
{
    auto ctx = Context{};

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop_arg(UpdateDrawFrame, static_cast<void*>(&ctx), 0, true);
#else // PLATFORM_WEB
    while (!WindowShouldClose()) {
        UpdateDrawFrame(static_cast<void*>(&ctx));
    }
#endif // PLATFORM_WEB
    return 0;
}
