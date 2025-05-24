#include <array>
#if defined(PLATFORM_WEB)
#include "proto/pref.pb.h"

#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#endif // PLATFORM_WEB

#include <fmt/format.h>
#include <range/v3/all.hpp>
#include <raylib-cpp.hpp>

#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

#if defined(PLATFORM_WEB)
EM_BOOL on_open(int eventType, const EmscriptenWebSocketOpenEvent* e, void* userData)
{
    printf("WebSocket opened!\n");

    auto envelope = pref::RpcEnvelope{};
    envelope.set_method("SayHello");
    auto request = pref::HelloRequest{};
    request.set_name("Venichka");
    auto requestPayload = std::string{};
    if (not request.SerializeToString(&requestPayload)) {
        printf("Failed to serialize HelloRequest\n");
        return EM_FALSE;
    }
    envelope.set_payload(requestPayload);
    auto envelopePayload = std::string{};
    if (not envelope.SerializeToString(&envelopePayload)) {
        printf("Failed to serialize RpcEnvelope\n");
        return EM_FALSE;
    }

    auto result = emscripten_websocket_send_binary(e->socket, envelopePayload.data(), envelopePayload.size());

    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        printf("Failed to send protobuf message: %d\n", result);
    }
    return EM_TRUE;
}

EM_BOOL on_message(int eventType, const EmscriptenWebSocketMessageEvent* e, void* userData)
{
    if (!e->isText) {
        pref::RpcEnvelope envelope;
        if (!envelope.ParseFromArray(e->data, e->numBytes)) {
            printf("Failed to parse RpcEnvelope\n");
            return EM_TRUE;
        }

        const std::string& method = envelope.method();
        const std::string& payload = envelope.payload();

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

    return EM_TRUE;
}

EM_BOOL on_error(int eventType, const EmscriptenWebSocketErrorEvent* websocketEvent, void* userData)
{
    puts("on_error");

    return EM_TRUE;
}
EM_BOOL on_close(int eventType, const EmscriptenWebSocketCloseEvent* websocketEvent, void* userData)
{
    puts("on_close");

    return EM_TRUE;
}

void setup_websocket()
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

    emscripten_websocket_set_onopen_callback(ws, nullptr, on_open);
    emscripten_websocket_set_onmessage_callback(ws, nullptr, on_message);
    emscripten_websocket_set_onerror_callback(ws, nullptr, on_error);
    emscripten_websocket_set_onclose_callback(ws, nullptr, on_close);
}
#endif // PLATFORM_WEB

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
    RWindow window{static_cast<int>(screen.x), static_cast<int>(screen.y), "Преферанс | Preferans"};
    std::array<Player, 3> players;
};

void DrawGameplayScreen(Context& ctx)
{
    RRectangle{{}, ctx.screen}.Draw(DARKGREEN);
    ctx.font.DrawText("PREFERANS GAME", {20, 10}, static_cast<float>(ctx.font.baseSize) * 3.0f, 4, BLACK);
}

void UpdateDrawFrame(void* userData)
{

    assert(userData);
    auto& ctx = *static_cast<Context*>(userData);

    const auto mouse = RMouse::GetPosition();
    // Handle mouse press — start isDragging if on top of a card
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

    ctx.window.BeginDrawing();
    ctx.window.ClearBackground();
    DrawGameplayScreen(ctx);

    // Draw all cards

    for (const auto& player : ctx.players) {
        for (const auto& card : player.cards) {
            card.texture.Draw(card.position);
        }
    }

    ctx.window.DrawFPS(screenWidth - 80, 0);
    ctx.window.EndDrawing();
}

} // namespace

auto main() -> int
{
    auto ctx = Context{};

#if defined(PLATFORM_WEB)
    setup_websocket();
    emscripten_set_main_loop_arg(UpdateDrawFrame, static_cast<void*>(&ctx), 0, true);
#else // PLATFORM_WEB
    while (!WindowShouldClose()) {
        UpdateDrawFrame(static_cast<void*>(&ctx));
    }
#endif // PLATFORM_WEB
    return 0;
}
