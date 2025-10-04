#include "client.hpp"

#include "common/logger.hpp"
#include "proto/pref.pb.h"

#include <docopt.h>
#include <emscripten/emscripten.h>
#include <emscripten/val.h>
#include <emscripten/websocket.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>
#include <raylib-cpp.hpp>
#include <spdlog/spdlog.h>

#include <cassert>
#include <gsl/gsl>
#include <list>
#include <vector>

namespace pref {
namespace {

template<typename... Args>
[[nodiscard]] auto resources(Args&&... args) -> std::string
{
    return (fs::path("resources") / ... / std::forward<Args>(args)).string();
}

struct Card {
    Card(CardName n, RVector2 pos)
        : name{std::move(n)}
        , position{pos}
        , image{RImage{resources("cards", std::format("{}.png", name))}}
    {
        image.Resize(static_cast<int>(cardWidth), static_cast<int>(cardHeight));
        texture = image.LoadTexture();
    }

    CardName name;
    RVector2 position;
    RImage image;
    RTexture texture{};
};

[[nodiscard]] auto suitValue(const std::string_view suit) -> int
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

    auto clear() -> void
    {
        cards.clear();
        whistingChoice.clear();
        bid.clear();
        tricksTaken = 0;
    }

    PlayerName name;
    std::list<Card> cards;
    std::string whistingChoice;
    std::string bid;
    int tricksTaken{};
};

struct BiddingMenu {
    bool isVisible{};
    std::string bid;
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
    bool canHalfWhist{};
    std::string choice;

    auto clear() -> void
    {
        isVisible = false;
        canHalfWhist = false;
        choice.clear();
    }
};

[[nodiscard]] constexpr auto bidRank(const std::string_view bid) noexcept -> std::size_t
{
    for (auto i = 0uz; i < std::size(bidsRank); ++i) {
        if (bidsRank[i] == bid) { return i; }
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

[[nodiscard]] auto getGuiColor(const int property) -> RColor
{
    return getGuiColor(DEFAULT, property);
}

[[nodiscard]] auto getGuiButtonBorderWidth() noexcept -> float
{
    return static_cast<float>(GuiGetStyle(BUTTON, BORDER_WIDTH));
}

[[nodiscard]] auto getGuiDefaultTextSize() noexcept -> float
{
    return static_cast<float>(GuiGetStyle(DEFAULT, TEXT_SIZE));
}

[[nodiscard]] constexpr auto localizeText(const GameText text, const GameLang lang) noexcept -> std::string_view
{
    return localization[std::to_underlying(lang)][std::to_underlying(text)];
}

[[nodiscard]] constexpr auto whistingChoiceToGameText(const std::string_view whistingChoice) noexcept -> GameText
{
    if (whistingChoice == localizeText(GameText::Whist, GameLang::English)) { return GameText::Whist; }
    if (whistingChoice == localizeText(GameText::HalfWhist, GameLang::English)) { return GameText::HalfWhist; }
    if (whistingChoice == localizeText(GameText::Catch, GameLang::English)) { return GameText::Catch; }
    if (whistingChoice == localizeText(GameText::Trust, GameLang::English)) { return GameText::Trust; }
    return GameText::Pass;
}

[[nodiscard]] auto fontSize(const RFont& font) noexcept -> float
{
    return static_cast<float>(font.baseSize);
}

struct SettingsMenu {
    int colorSchemeIdScroll = -1;
    int colorSchemeIdSelect = -1;
    int langIdScroll = -1;
    int langIdSelect = -1;
    bool isVisible{};
    std::string loadedColorScheme;
    std::string loadedLang;
};

struct ScoreSheetMenu {
    ScoreSheet score;
    bool isVisible{};
};

struct Context {
    // TODO: Figure out how to use one font with different sizes
    //       Should we use `fontL` and scale it down?
    RFont fontS;
    RFont fontM;
    RFont fontL;
    RFont initialFont;
    RFont fontAwesome;
    float scale = 1.f;
    float offsetX = 0.f;
    float offsetY = 0.f;
    int windowWidth = virtualWidth;
    int windowHeight = virtualHeight;
    RWindow window{
        windowWidth,
        windowHeight,
        "Preferans",
        FLAG_MSAA_4X_HINT // smooth edges for circles, rings, etc.
            | FLAG_WINDOW_ALWAYS_RUN // don't throttle when tab is not focused
            | FLAG_WINDOW_RESIZABLE,
    };
    RRenderTexture target{virtualWidth, virtualHeight};
    Player player;
    PlayerId myPlayerId;
    PlayerName myPlayerName;
    bool hasEnteredName{};
    std::map<PlayerId, Player> players;
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
    SettingsMenu settingsMenu;
    ScoreSheetMenu scoreSheet;

    auto clear() -> void
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
        player.clear();
        for (auto& p : players | rv::values) { p.clear(); }
    }

    [[nodiscard]] auto localizeText(const GameText text) const -> std::string
    {
        return std::string{::pref::localizeText(text, lang)};
    }

    [[nodiscard]] constexpr auto localizeBid(const std::string_view bid) const noexcept -> std::string_view
    {
        if (lang == GameLang::Alternative) {
            if (bid == NINE_WT) { return NINE " БП"; }
            if (bid == TEN_WT) { return TEN " БП"; }
            if (bid == PREF_MISER) { return pref::localizeText(GameText::Miser, lang); }
            if (bid == PREF_MISER_WT) { return "Миз.БП"; }
            if (bid == PREF_PASS) { return pref::localizeText(GameText::Pass, lang); }
        } else if (lang == GameLang::Ukrainian) {
            if (bid == NINE_WT) { return NINE " БП"; }
            if (bid == TEN_WT) { return TEN " БП"; }
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
};

auto savePlayerIdToLocalStorage(const PlayerId& playerId) -> void
{
    const auto js = std::string{"localStorage.setItem('preferans_player_id', '"} + playerId + "');";
    emscripten_run_script(js.c_str());
}

[[nodiscard]] auto getOpponentIds(Context& ctx) -> std::pair<PlayerId, PlayerId>
{
    assert(ctx.areAllPlayersJoined());
    auto order = std::vector<PlayerId>{};
    for (const auto& [id, _] : ctx.players) { order.push_back(id); }
    const auto it = rng::find(order, ctx.myPlayerId);
    assert(it != order.end());
    const auto selfIndex = std::distance(order.begin(), it);
    const auto leftIndex = (selfIndex + 1) % 3;
    const auto rightIndex = (selfIndex + 2) % 3;
    return {order[static_cast<std::size_t>(leftIndex)], order[static_cast<std::size_t>(rightIndex)]};
}

auto sendMessage(const EMSCRIPTEN_WEBSOCKET_T ws, const Message& msg) -> void
{
    if (ws == 0) {
        PREF_WARN("error: ws is not open");
        return;
    }
    auto data = msg.SerializeAsString();
    if (const auto result = emscripten_websocket_send_binary(ws, data.data(), std::size(data));
        result != EMSCRIPTEN_RESULT_SUCCESS) {
        PREF_WARN("error: {}, method: {}", emResult(result), msg.method());
    }
}

[[nodiscard]] auto makeMessage(const EmscriptenWebSocketMessageEvent& event) -> std::optional<Message>
{
    if (event.isText) {
        PREF_WARN("error: expect binary data");
        return {};
    }
    if (auto result = Message{}; result.ParseFromArray(event.data, static_cast<int>(event.numBytes))) { return result; }
    PREF_WARN("error: failed to make Message from array");
    return {};
}

[[nodiscard]] auto makeJoinRequest(const std::string& playerId, const std::string& playerName) -> JoinRequest
{
    auto result = JoinRequest{};
    if (not std::empty(playerId)) { result.set_player_id(playerId); }
    result.set_player_name(playerName);
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
    const std::string& playerId, const std::string& bid, const std::vector<CardName>& discardedTalon) -> DiscardTalon
{
    auto result = DiscardTalon{};
    result.set_player_id(playerId);
    result.set_bid(bid);
    for (const auto& card : discardedTalon) { result.add_cards(card); }
    return result;
}

[[nodiscard]] auto makeWhisting(const std::string& playerId, const std::string& choice) -> Whisting
{
    auto result = Whisting{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return result;
}

[[nodiscard]] auto makePlayCard(const std::string& playerId, const CardName& cardName) -> PlayCard
{
    auto result = PlayCard{};
    result.set_player_id(playerId);
    result.set_card(cardName);
    return result;
}

[[nodiscard]] auto makeLog(const std::string& playerId, const std::string& text) -> Log
{
    auto result = Log{};
    result.set_player_id(playerId);
    result.set_text(text);
    return result;
}

auto sendJoinRequest(Context& ctx) -> void
{
    sendMessage(ctx.ws, makeMessage(makeJoinRequest(ctx.myPlayerId, ctx.myPlayerName)));
}

auto sendBidding(Context& ctx, const std::string& bid) -> void
{
    sendMessage(ctx.ws, makeMessage(makeBidding(ctx.myPlayerId, bid)));
}

auto sendDiscardTalon(Context& ctx, const std::string& bid) -> void
{
    sendMessage(ctx.ws, makeMessage(makeDiscardTalon(ctx.myPlayerId, bid, ctx.discardedTalon)));
}

auto sendWhisting(Context& ctx, const std::string& choice) -> void
{
    sendMessage(ctx.ws, makeMessage(makeWhisting(ctx.myPlayerId, choice)));
}

auto sendPlayCard(Context& ctx, const CardName& cardName) -> void
{
    sendMessage(ctx.ws, makeMessage(makePlayCard(ctx.myPlayerId, cardName)));
}

[[maybe_unused]] auto sendLog(Context& ctx, const std::string& text) -> void
{
    sendMessage(ctx.ws, makeMessage(makeLog(ctx.myPlayerId, text)));
}

auto handleJoinResponse(Context& ctx, const Message& msg) -> void
{
    auto joinResponse = makeMethod<JoinResponse>(msg);
    if (not joinResponse) { return; }
    if (ctx.myPlayerId != joinResponse->player_id()) {
        ctx.myPlayerId = joinResponse->player_id();
        PREF_INFO("save myPlayerId: {}", ctx.myPlayerId);
        savePlayerIdToLocalStorage(ctx.myPlayerId);
    }
    ctx.players.clear();
    for (auto& player : *(joinResponse->mutable_players())) {
        if (player.player_id() == ctx.myPlayerId and ctx.myPlayerName != player.player_name()) {
            ctx.myPlayerName = player.player_name();
        }
        ctx.players.insert_or_assign(
            std::move(*player.mutable_player_id()), Player{std::move(*player.mutable_player_name()), {}, {}, {}});
    }
}

auto handlePlayerJoined(Context& ctx, const Message& msg) -> void
{
    auto playerJoined = makeMethod<PlayerJoined>(msg);
    if (not playerJoined) { return; }
    auto& playerJoinedId = *playerJoined->mutable_player_id();
    auto& playerJoinedName = *playerJoined->mutable_player_name();
    PREF_INFO("New player playerJoined: {} ({})", playerJoinedName, playerJoinedId);
    ctx.players.insert_or_assign(std::move(playerJoinedId), Player{std::move(playerJoinedName), {}, {}, {}});
}

auto handlePlayerLeft(Context& ctx, const Message& msg) -> void
{
    const auto playerLeft = makeMethod<PlayerLeft>(msg);
    if (not playerLeft) { return; }
    const auto& id = playerLeft->player_id();
    if (ctx.players.contains(id)) {
        PREF_INFO("Player left: {} ({})", ctx.players[id].name, id);
        ctx.players.erase(id);
        PREF_INFO("Updated player list:");
        for (const auto& [i, player] : ctx.players) { PREF_INFO("  - {} ({})", player.name, i); }
    } else {
        PREF_WARN("Player with ID {} left (name unknown)", id);
    }
}

auto handleDealCards(Context& ctx, const Message& msg) -> void
{
    auto dealCards = makeMethod<DealCards>(msg);
    if (not dealCards) { return; }
    ctx.clear();
    assert(std::size(dealCards->cards()) == 10);
    for (auto&& cardName : *(dealCards->mutable_cards())) {
        ctx.player.cards.emplace_back(std::move(cardName), RVector2{});
    }
    ctx.player.sortCards();
}

auto handlePlayerTurn(Context& ctx, const Message& msg) -> void
{
    auto playerTurn = makeMethod<PlayerTurn>(msg);
    if (not playerTurn) { return; }
    if (std::size(ctx.cardsOnTable) >= 3) {
        // TODO: remove sleep
        // std::this_thread::sleep_for(1s);
        ctx.cardsOnTable.clear();
    }
    ctx.turnPlayerId = playerTurn->player_id();
    ctx.stage = playerTurn->stage();
    if (ctx.turnPlayerId == ctx.myPlayerId) {
        PREF_INFO("Your turn");
        if (ctx.stage == "Bidding") {
            ctx.bidding.isVisible = true;
            return;
        }
        if (ctx.stage == "TalonPicking") {
            for (auto&& card : *(playerTurn->mutable_talon())) {
                ctx.player.cards.emplace_back(std::move(card), RVector2{});
            }
            ctx.player.sortCards();
        }
        if (ctx.stage == "Whisting") {
            ctx.whisting.canHalfWhist = playerTurn->can_half_whist();
            ctx.whisting.isVisible = true;
            return;
        }
    } else {
        INFO_VAR(ctx.turnPlayerId);
    }
}

auto handleBidding(Context& ctx, const Message& msg) -> void
{
    const auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) { return; }
    const auto& playerId = bidding->player_id();
    const auto& bid = bidding->bid();
    const auto rank = bidRank(bid);
    if (bid != PREF_PASS) { ctx.bidding.rank = rank; }
    ctx.bidding.bid = bid;
    if (not ctx.players.contains(playerId)) {
        PREF_WARN("error: unknown {}", VAR(playerId));
        return;
    }
    ctx.players[playerId].bid = bid;
    INFO_VAR(playerId, bid, rank);
}

auto handleWhisting(Context& ctx, const Message& msg) -> void
{
    const auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { return; }
    const auto& playerId = whisting->player_id();
    if (not ctx.players.contains(playerId)) {
        PREF_WARN("error: unknown {}", VAR(playerId));
        return;
    }
    const auto& choice = whisting->choice();
    ctx.players[playerId].whistingChoice = choice;
    if (playerId == ctx.myPlayerId) { ctx.player.whistingChoice = choice; }
    INFO_VAR(playerId, choice);
}

auto handlePlayCard(Context& ctx, const Message& msg) -> void
{
    auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) { return; }
    auto [leftOpponentId, rightOpponentId] = getOpponentIds(ctx);
    auto& playerId = *(playCard->mutable_player_id());
    auto& cardName = *(playCard->mutable_card());
    INFO_VAR(playerId, cardName);

    if (playerId == leftOpponentId) {
        if (ctx.leftCardCount > 0) { --ctx.leftCardCount; }
    } else if (playerId == rightOpponentId) {
        if (ctx.rightCardCount > 0) { --ctx.rightCardCount; }
    } else {
        // Not an opponent - possibly local player or spectator
        return;
    }

    if (std::empty(ctx.cardsOnTable)) { ctx.leadSuit = cardSuit(cardName); }
    ctx.cardsOnTable.insert_or_assign(std::move(playerId), Card{std::move(cardName), RVector2{}});
}

auto handleTrickFinished(Context& ctx, const Message& msg) -> void
{
    const auto trickFinished = makeMethod<TrickFinished>(msg);
    if (not trickFinished) { return; }
    for (auto&& tricks : trickFinished->tricks()) {
        auto&& playerId = tricks.player_id();
        auto&& tricksTaken = tricks.taken();
        INFO_VAR(playerId, tricksTaken);
        if (ctx.players.contains(playerId)) { ctx.players.at(playerId).tricksTaken = tricksTaken; }
    }
}

auto handleDealFinished(Context& ctx, const Message& msg) -> void
{
    const auto dealFinished = makeMethod<DealFinished>(msg);
    if (not dealFinished) { return; }
    ctx.scoreSheet.score = dealFinished->score_sheet() // clang-format off
        | rv::transform(unpair([](const auto& playerId, const auto& score) {
        return std::pair{playerId, Score{
            .dump = score.dump().values() | rng::to_vector,
            .pool = score.pool().values() | rng::to_vector,
            .whists = score.whists() | rv::transform(unpair([](const auto& id, const auto& whist) {
                return std::pair{id, whist.values() | rng::to_vector};
        })) | rng::to<std::map>}};
    })) | rng::to<ScoreSheet>; // clang-format on
}

auto updateWindowSize(Context& ctx) -> void
{
    EmscriptenFullscreenChangeEvent f;
    emscripten_get_fullscreen_status(&f);
    const auto t = fmt::format(
        "isFullscreen: {}, fullscreenEnabled: {}, nodeName {}, id: {}, elementWidth: {}, elementHeight: {}, "
        "screenWidth: {}, screenHeight: {}",
        f.isFullscreen,
        f.fullscreenEnabled,
        f.nodeName,
        f.id,
        f.elementWidth,
        f.elementHeight,
        f.screenWidth,
        f.screenHeight);
    sendLog(ctx, t);
    PREF_INFO("{}", t);

    auto canvasCssW = 0.0;
    auto canvasCssH = 0.0;
    if (emscripten_get_element_css_size("#canvas", &canvasCssW, &canvasCssH) != EMSCRIPTEN_RESULT_SUCCESS) { return; }
    const auto dpr = emscripten_get_device_pixel_ratio();
    ctx.windowWidth = static_cast<int>(canvasCssW * dpr);
    ctx.windowHeight = static_cast<int>(canvasCssH * dpr);
    emscripten_set_canvas_element_size("#canvas", ctx.windowWidth, ctx.windowHeight);
    ctx.window.SetSize(ctx.windowWidth, ctx.windowHeight);
    const auto windowW = static_cast<float>(ctx.windowWidth);
    const auto windowH = static_cast<float>(ctx.windowHeight);
    ctx.scale = std::fminf(windowW / virtualW, windowH / virtualH);
    ctx.offsetX = (windowW - virtualW * ctx.scale) * 0.5f;
    ctx.offsetY = (windowH - virtualH * ctx.scale) * 0.5f;
    // TODO: Fix incorrect mouse scaling/offset when entering fullscreen mode
    // in smartphone browsers
    auto text = fmt::format(
        "[updateWindowSize] cssW: {}, cssH: {}, dpr: {}, w: {}, h: {}, scale: {}, offX: {}, "
        "offY: {}",
        canvasCssW,
        canvasCssH,
        dpr,
        ctx.windowWidth,
        ctx.windowHeight,
        ctx.scale,
        ctx.offsetX,
        ctx.offsetY);
    sendLog(ctx, text);
    PREF_INFO("{}", text);
    RMouse::SetOffset(static_cast<int>(-ctx.offsetX), static_cast<int>(-ctx.offsetY));
    RMouse::SetScale(1.f / ctx.scale, 1.f / ctx.scale);
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

[[maybe_unused]] auto clearPlayerIdFromLocalStorage() -> void
{
    emscripten_run_script("localStorage.removeItem('preferans_player_id');");
}

auto onWsOpen(const int eventType, const EmscriptenWebSocketOpenEvent* e, void* userData) -> EM_BOOL
{
    PREF_INFO("{}, socket: {}", VAR(eventType), e->socket);
    assert(userData);
    sendJoinRequest(toContext(userData));
    return EM_TRUE;
}

auto onWsMessage(const int eventType, const EmscriptenWebSocketMessageEvent* e, void* userData) -> EM_BOOL
{
    assert(e);
    assert(userData);
    PREF_INFO("{}, socket: {}, numBytes: {}, isText: {}", VAR(eventType), e->socket, e->numBytes, e->isText);
    const auto msg = makeMessage(*e);
    if (not msg) { return EM_TRUE; }
    const auto& method = msg->method();
    INFO_VAR(method);
    auto& ctx = toContext(userData);
    if (method == "JoinResponse") {
        handleJoinResponse(ctx, *msg);
    } else if (method == "PlayerJoined") {
        handlePlayerJoined(ctx, *msg);
    } else if (method == "PlayerLeft") {
        handlePlayerLeft(ctx, *msg);
    } else if (method == "DealCards") {
        handleDealCards(ctx, *msg);
    } else if (method == "PlayerTurn") {
        handlePlayerTurn(ctx, *msg);
    } else if (method == "Bidding") {
        handleBidding(ctx, *msg);
    } else if (method == "Whisting") {
        handleWhisting(ctx, *msg);
    } else if (method == "PlayCard") {
        handlePlayCard(ctx, *msg);
    } else if (method == "TrickFinished") {
        handleTrickFinished(ctx, *msg);
    } else if (method == "DealFinished") {
        handleDealFinished(ctx, *msg);
    } else {
        PREF_WARN("error: unknown {}", VAR(method));
    }
    return EM_TRUE;
}

auto onWsError(const int eventType, const EmscriptenWebSocketErrorEvent* e, void* userData) -> EM_BOOL
{
    PREF_INFO("{}, socket: {}", VAR(eventType), e->socket);
    assert(userData);
    [[maybe_unused]] auto& ctx = toContext(userData);
    return EM_TRUE;
}

auto onWsClosed(const int eventType, const EmscriptenWebSocketCloseEvent* e, void* userData) -> EM_BOOL
{
    PREF_INFO(
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
        PREF_WARN("WebSocket is not supported");
        return;
    }

    EmscriptenWebSocketCreateAttributes attr = {};
    attr.url = "ws://192.168.2.194:8080"; // Or wss:// if using HTTPS
    attr.protocols = nullptr;
    attr.createOnMainThread = EM_TRUE;

    ctx.ws = emscripten_websocket_new(&attr);
    if (ctx.ws <= 0) {
        PREF_WARN("Failed to create WebSocket");
        return;
    }

    emscripten_websocket_set_onopen_callback(ctx.ws, &ctx, onWsOpen);
    emscripten_websocket_set_onmessage_callback(ctx.ws, &ctx, onWsMessage);
    emscripten_websocket_set_onerror_callback(ctx.ws, &ctx, onWsError);
    emscripten_websocket_set_onclose_callback(ctx.ws, &ctx, onWsClosed);
}

auto drawGuiLabelCentered(const std::string& text, const RVector2& anchor) -> void
{
    const auto size = MeasureTextEx(GuiGetFont(), text.c_str(), getGuiDefaultTextSize(), fontSpacing);
    const auto shift = virtualH / 135.f;
    const auto bounds = RRectangle{
        anchor.x - size.x * 0.5f - (shift / 2.f), // shift left to center and add left padding
        anchor.y - size.y * 0.5f - (shift / 2.f), // shift up to center and add top padding
        size.x + shift, // width = text + left + right padding
        size.y + shift // height = text + top + bottom padding
    };
    GuiLabel(bounds, text.c_str());
}

[[maybe_unused]] auto drawSpeechBubble(Context& ctx, const RVector2& pos, const std::string& text) -> void
{
    const auto roundness = 0.8f;
    const auto fontSize = ctx.fontSizeM();
    const auto textSize = ctx.fontM.MeasureText(text.c_str(), fontSize, fontSpacing);
    const auto padding = textSize.y / 2.f;
    const auto bubbleWidth = textSize.x + padding * 2.f;
    const auto bubbleHeight = textSize.y + padding * 2.f;
    const auto rect = RRectangle{pos.x, pos.y, bubbleWidth, bubbleHeight};
    const auto p1 = RVector2{pos.x, pos.y + bubbleHeight * 0.66f};
    const auto p2 = RVector2{pos.x, pos.y + bubbleHeight * 0.33f};
    const auto p3 = RVector2{pos.x - textSize.y, pos.y + bubbleHeight * 0.5f};
    const auto colorBorder = getGuiColor(BORDER_COLOR_NORMAL);
    const auto colorBackground = getGuiColor(BACKGROUND_COLOR);
    const auto colorText = getGuiColor(TEXT_COLOR_NORMAL);
    const auto thick = getGuiButtonBorderWidth();
    const auto segments = 0;
    rect.DrawRounded(roundness, segments, colorBackground);
    rect.DrawRoundedLines(roundness, segments, thick, colorBorder);
    DrawTriangle(p1, p2, p3, colorBackground);
    p3.DrawLine(p1, thick, colorBorder);
    p3.DrawLine(p2, thick, colorBorder);
    ctx.fontM.DrawText(text.c_str(), {rect.x + padding, rect.y + padding}, fontSize, fontSpacing, colorText);
}

auto drawGameplayScreen(Context& ctx) -> void
{
    const auto title = ctx.localizeText(GameText::Preferans);
    const auto textSize = ctx.fontL.MeasureText(title, ctx.fontSizeL(), fontSpacing);
    const auto x = (virtualW - textSize.x) / 2.0f;
    const auto y = virtualH / 54.f;
    ctx.fontL.DrawText(title, {x, y}, ctx.fontSizeL(), fontSpacing, getGuiColor(LABEL, TEXT_COLOR_NORMAL));
}

auto drawEnterNameScreen(Context& ctx) -> void
{
    static constexpr auto boxWidth = virtualW / 4.8f;
    static constexpr auto boxHeight = virtualH / 18.f;
    const auto screenCenter = RVector2{virtualWidth, virtualHeight} / 2.0f;
    const auto boxPos = RVector2{screenCenter.x - boxWidth / 2.0f, screenCenter.y};
    const auto labelPos = RVector2{boxPos.x, boxPos.y - virtualH / 27.f};
    ctx.fontM.DrawText(
        ctx.localizeText(GameText::EnterYourName),
        labelPos,
        ctx.fontSizeM(),
        fontSpacing,
        getGuiColor(LABEL, TEXT_COLOR_NORMAL));

    const auto inputBox = RRectangle{boxPos, {boxWidth, boxHeight}};
    static constexpr auto maxLenghtName = 11;
    static char nameBuffer[maxLenghtName] = "Player";
    static auto editMode = true;
    GuiTextBox(inputBox, nameBuffer, sizeof(nameBuffer), editMode);
    const auto buttonBox = RRectangle{boxPos.x, boxPos.y + boxHeight + virtualH / 54.f, boxWidth, virtualH / 27.f};
    const auto clicked = GuiButton(buttonBox, ctx.localizeText(GameText::Enter).c_str());
    if ((clicked or RKeyboard::IsKeyPressed(KEY_ENTER)) and (nameBuffer[0] != '\0') and editMode) {
        ctx.myPlayerName = nameBuffer;
        ctx.hasEnteredName = true;
        editMode = false;
        setup_websocket(ctx);
    }
}

auto drawConnectedPlayersPanel(const Context& ctx) -> void
{
    static constexpr auto pad = virtualH / 108.f;
    static constexpr auto minWidth = virtualW / 9.6f;
    static constexpr auto minHeight = virtualH / 13.5f;
    const auto fontSize = ctx.fontSizeS();
    const auto lineGap = fontSize * 1.2f;
    const auto headerGap = fontSize * .5f;
    const auto headerText = ctx.localizeText(GameText::CurrentPlayers);
    const auto headerSize = ctx.fontS.MeasureText(headerText, fontSize, fontSpacing);
    auto maxWidth = headerSize.x;
    for (const auto& [_, player] : ctx.players) {
        const auto sz = ctx.fontS.MeasureText(player.name, fontSize, fontSpacing);
        if (sz.x > maxWidth) maxWidth = sz.x;
    }
    const auto rows = std::size(ctx.players);
    const auto contentW = pad * 2.f + maxWidth;
    const auto contentH = pad * 2.f + headerSize.y + headerGap + static_cast<float>(rows) * lineGap;
    const auto panelW = std::max(minWidth, contentW);
    const auto panelH = std::max(minHeight, contentH);
    const auto r = RRectangle{{borderMargin, borderMargin}, {panelW, panelH}};
    GuiPanel(r, nullptr);
    auto textPos = RVector2{r.x + pad, r.y + pad};
    ctx.fontS.DrawText(headerText, textPos, fontSize, fontSpacing, getGuiColor(LABEL, TEXT_COLOR_NORMAL));
    textPos.y += headerSize.y + headerGap;
    for (const auto& [id, player] : ctx.players) {
        const auto color = (id == ctx.myPlayerId) ? getGuiColor(TEXT_COLOR_NORMAL) : getGuiColor(TEXT_COLOR_DISABLED);
        ctx.fontS.DrawText(player.name, textPos, fontSize, fontSpacing, color);
        textPos.y += lineGap;
    }
}

[[nodiscard]] auto isCardPlayable(const Context& ctx, const Card& clickedCard) -> bool
{
    const auto clickedSuit = cardSuit(clickedCard.name);
    const auto trump = getTrump(ctx.bidding.bid);
    const auto hasSuit = [&](const std::string_view suit) {
        return rng::any_of(ctx.player.cards, [&](const CardName& name) { return cardSuit(name) == suit; }, &Card::name);
    };
    if (std::empty(ctx.cardsOnTable)) { return true; } // first card in the trick: any card is allowed
    if (clickedSuit == ctx.leadSuit) { return true; } // follows lead suit
    if (hasSuit(ctx.leadSuit)) { return false; } // must follow lead suit
    if (clickedSuit == trump) { return true; } //  no lead suit cards, playing trump
    if (hasSuit(trump)) { return false; } // // must play trump if you have it
    return true; // no lead or trump suit cards, free to play
}

[[nodiscard]] auto tintForCard(const Context& ctx, const Card& card) -> RColor
{
    const auto lightGray = RColor{150, 150, 150, 255};
    return isCardPlayable(ctx, card) ? RColor::White() : lightGray;
}

auto drawMyHand(Context& ctx) -> void
{
    auto& hand = ctx.player.cards;
    const auto totalWidth = static_cast<float>(std::ssize(hand) - 1) * cardOverlapX + cardWidth;
    const auto startX = (virtualWidth - totalWidth) / 2.0f;
    const auto myHandTopY = virtualHeight - cardHeight - cardHeight / 10.8f; // bottom padding
    const auto tricksTaken = ctx.players.at(ctx.myPlayerId).tricksTaken;
    static constexpr auto gap = cardHeight / 27.f;

    if (ctx.turnPlayerId == ctx.myPlayerId) {
        const auto text = ctx.localizeText(GameText::YourTurn);
        const auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), fontSpacing);
        const auto bidEndY = bidOriginY + bidMenuH;
        const auto spaceH = myHandTopY - bidEndY;
        const auto textY = bidEndY + ((spaceH - textSize.y) * 0.5f);
        const auto textX = startX + std::abs(totalWidth - textSize.x) * 0.5f;
        const auto rect = RRectangle{textX + gap * 0.5f, textY - gap * 0.5f, textSize.x + gap, textSize.y + gap};
        GuiLabel(rect, text.c_str());
    }
    {
        if (ctx.turnPlayerId == ctx.myPlayerId) { GuiSetState(STATE_PRESSED); }
        const auto name = fmt::format(
            "{}{}{}",
            ctx.turnPlayerId == ctx.myPlayerId ? fmt::format("{} ", ARROW_RIGHT) : "",
            ctx.myPlayerName,
            tricksTaken != 0 ? fmt::format(" ({})", tricksTaken) : "");
        const auto textSize = ctx.fontM.MeasureText(name, ctx.fontSizeM(), fontSpacing);
        const auto anchor = RVector2{
            startX - textSize.x * 0.5f - virtualW / 96.f, // to the left of the first card
            myHandTopY + cardHeight * 0.5f, // vertically centred on the card
        };
        // TODO: still draw name if no cards left
        drawGuiLabelCentered(name, anchor);
        if (ctx.turnPlayerId == ctx.myPlayerId) { GuiSetState(STATE_NORMAL); }
    }
    const auto mousePos = RMouse::GetPosition();
    const auto toX = [&](const auto i) { return startX + static_cast<float>(i) * cardOverlapX; };
    const auto hoveredIndex = std::invoke([&] {
        auto reversed = rv::iota(0, std::ssize(hand)) | rv::reverse;
        const auto it = rng::find_if(reversed, [&](const auto i) {
            return mousePos.CheckCollision({toX(i), myHandTopY, cardWidth, cardHeight});
        });
        return it == rng::end(reversed) ? -1 : *it;
    });
    struct CardShineEffect {
        float speed = 90.f;
        float stripeWidth = cardWidth / 5.f;
        float intensity = 0.3f; // 0..1 alpha
    };
    const auto shine = CardShineEffect{};
    for (auto&& [i, card] : hand | rv::enumerate) {
        const auto isHovered = ctx.turnPlayerId == ctx.myPlayerId
            and (ctx.stage == "Playing" or ctx.stage == "TalonPicking")
            and not ctx.bidding.isVisible
            and isCardPlayable(ctx, card)
            and static_cast<int>(i) == hoveredIndex;
        static constexpr auto offset = cardHeight / 10.f;
        const auto yOffset = isHovered ? -offset : 0.f;
        card.position = RVector2{toX(i), myHandTopY + yOffset};
        card.texture.Draw(card.position, tintForCard(ctx, card));
        if (isHovered) {
            const auto time = static_cast<float>(ctx.window.GetTime());
            const auto shineX = std::fmod(time * shine.speed, cardWidth - shine.stripeWidth);
            const auto shineRect = RRectangle{card.position.x + shineX, card.position.y, shine.stripeWidth, cardHeight};
            const auto whiteColor = RColor::White();
            shineRect.DrawGradient(
                whiteColor.Fade(shine.intensity), // top left
                whiteColor.Fade(0.f), // bottom left
                whiteColor.Fade(shine.intensity), // bottom right
                whiteColor.Fade(0.f)); // top right
        }
    }
    if (not std::empty(ctx.player.whistingChoice)) {
        const auto text = ctx.localizeText(whistingChoiceToGameText(ctx.player.whistingChoice));
        const auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), fontSpacing);
        auto anchor = RVector2{
            startX + textSize.x * 0.5f + virtualW / 96.f + totalWidth, // right of last card
            myHandTopY + cardHeight * 0.5f // vertically centered
        };
        drawGuiLabelCentered(text, anchor);
        return;
    }
    if (not std::empty(ctx.player.bid)) {
        auto text = std::string{ctx.localizeBid(ctx.player.bid)};
        const auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), fontSpacing);
        auto anchor = RVector2{
            startX + textSize.x * 0.5f + virtualW / 96.f + totalWidth, // right of last card
            myHandTopY + cardHeight * 0.5f // vertically centered
        };
        // TODO: draw ♥ and ♦ in red
        drawGuiLabelCentered(text, anchor);
        return;
    }
}

auto drawBackCard(const float x, const float y) -> void
{
    const auto card = RRectangle{x, y, cardWidth, cardHeight};
    const auto roundness = 0.2f;
    const auto segments = 0; // auto
    card.DrawRounded(roundness, segments, getGuiColor(BASE_COLOR_NORMAL));
    const auto borderThick = cardWidth / 25.f;
    const auto border = RRectangle{
        card.x + borderThick * 0.5f, card.y + borderThick * 0.5f, card.width - borderThick, card.height - borderThick};
    DrawTriangle(
        {border.x, border.y + border.height}, // bottom-left
        {border.x + border.width, border.y + border.height}, // bottom-right
        {border.x + border.width, border.y}, // top-right
        getGuiColor(BASE_COLOR_DISABLED));
    border.DrawRoundedLines(roundness, segments, borderThick, getGuiColor(BORDER_COLOR_NORMAL));
}

auto drawOpponentHand(Context& ctx, const int cardCount, const float x, const PlayerId& playerId, const bool isRight)
    -> void
{
    if (std::empty(ctx.player.cards)) { return; }
    if (cardCount == 0) { return; }
    const auto countF = static_cast<float>(cardCount);
    const auto startY = (virtualHeight - cardHeight - (countF - 1.0f) * cardOverlapY) * 0.5f;

    for (auto i = 0.0f; i < countF; ++i) {
        const auto posY = startY + i * cardOverlapY;
        drawBackCard(x, posY);
    }
    // centred name plate above the topmost card
    const auto fontSize = getGuiDefaultTextSize();
    const auto centerX = x + (cardWidth * 0.5f);
    auto anchor = RVector2{centerX, startY - fontSize};
    {
        if (ctx.turnPlayerId == playerId) { GuiSetState(STATE_PRESSED); }
        const auto tricksTaken = ctx.players.at(playerId).tricksTaken;
        //    PlayerName1             PlayerName2
        // -> PlayerName1             PlayerName2 <-
        //    PlayerName1 (1)     (1) PlayerName2
        // -> PlayerName1 (1)     (1) PlayerName2 <-
        const auto name = fmt::format(
            "{}{}{}",
            isRight ? (tricksTaken != 0 ? fmt::format("({}) ", tricksTaken) : "")
                    : (ctx.turnPlayerId == playerId ? fmt::format("{} ", ARROW_RIGHT) : ""),
            ctx.players.at(playerId).name,
            not isRight ? (tricksTaken != 0 ? fmt::format(" ({})", tricksTaken) : "")
                        : (ctx.turnPlayerId == playerId ? fmt::format(" {}", ARROW_LEFT) : ""));
        // TODO: still draw name if no cards left
        drawGuiLabelCentered(name, anchor);
        if (ctx.turnPlayerId == playerId) { GuiSetState(STATE_NORMAL); }
    }
    if (const auto& choice = ctx.players.at(playerId).whistingChoice; not std::empty(choice)) {
        const auto endY = startY + (countF - 1.0f) * cardOverlapY + cardHeight;
        anchor = RVector2{centerX, endY + fontSize};
        const auto text = ctx.localizeText(whistingChoiceToGameText(choice));
        drawGuiLabelCentered(text, anchor);
        return;
    }
    if (const auto& bid = ctx.players.at(playerId).bid; not std::empty(bid)) {
        const auto endY = startY + (countF - 1.0f) * cardOverlapY + cardHeight;
        anchor = RVector2{centerX, endY + fontSize};
        auto text = std::string{ctx.localizeBid(bid)};
        drawGuiLabelCentered(text, anchor);
        return;
    }
}

auto drawPlayedCards(Context& ctx) -> void
{
    if (std::empty(ctx.cardsOnTable)) { return; }
    const auto cardSpacing = cardWidth * 0.1f;
    const auto yOffset = cardHeight / 4.0f;
    const auto centerPos = RVector2{virtualWidth / 2.0f, virtualHeight / 2.0f - cardHeight / 2.0f};
    const auto leftPlayPos = RVector2{centerPos.x - cardWidth - cardSpacing, centerPos.y - yOffset};
    const auto middlePlayPos = RVector2{centerPos.x, centerPos.y + yOffset};
    const auto rightPlayPos = RVector2{centerPos.x + cardWidth + cardSpacing, centerPos.y - yOffset};
    const auto [leftOpponentId, rightOpponentId] = getOpponentIds(ctx);
    if (ctx.cardsOnTable.contains(leftOpponentId)) { ctx.cardsOnTable.at(leftOpponentId).texture.Draw(leftPlayPos); }
    if (ctx.cardsOnTable.contains(rightOpponentId)) { ctx.cardsOnTable.at(rightOpponentId).texture.Draw(rightPlayPos); }
    if (ctx.cardsOnTable.contains(ctx.myPlayerId)) { ctx.cardsOnTable.at(ctx.myPlayerId).texture.Draw(middlePlayPos); }
}

#define GUI_PROPERTY(PREFIX, STATE)                                                                                    \
    std::invoke([&] {                                                                                                  \
        switch (STATE) {                                                                                               \
        case STATE_NORMAL: return PREFIX##_COLOR_NORMAL;                                                               \
        case STATE_DISABLED: return PREFIX##_COLOR_DISABLED;                                                           \
        case STATE_PRESSED: return PREFIX##_COLOR_PRESSED;                                                             \
        case STATE_FOCUSED: return PREFIX##_COLOR_FOCUSED;                                                             \
        }                                                                                                              \
        std::unreachable();                                                                                            \
    })

auto drawBiddingMenu(Context& ctx) -> void
{
    if (not ctx.bidding.isVisible) { return; }

    for (int r = 0; r < bidRows; ++r) {
        for (int c = 0; c < bidCols; ++c) {
            const auto& bid = bidTable[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (std::empty(bid)) { continue; }
            const auto rank = bidRank(bid);
            const auto myFirstBid = std::empty(ctx.player.bid);
            const auto finalBid = std::size(ctx.discardedTalon) == 2;
            const auto myBid = ctx.player.bid;
            const auto currentRank = ctx.bidding.rank;
            const auto disable = (
                // Pass final bid
                (bid == PREF_PASS and finalBid)
                // Rank restriction
                or (bid != PREF_PASS and currentRank != allRanks and currentRank >= rank)
                // Miser first bid restrictions
                or (bid == PREF_MISER and not myFirstBid and myBid != PREF_MISER)
                // Miser WT first bid restrictions
                or (bid == PREF_MISER_WT and not myFirstBid and myBid != PREF_MISER and myBid != PREF_MISER_WT)
                // Final bid restrictions for Miser/Miser WT
                or ((myBid == PREF_MISER or myBid == PREF_MISER_WT) and finalBid and bid != myBid)
                // Non-final bid restrictions for Miser/Miser WT
                or ((myBid == PREF_MISER or myBid == PREF_MISER_WT)
                    and not finalBid
                    and bid != PREF_MISER_WT
                    and bid != PREF_PASS));
            auto state = disable ? GuiState{STATE_DISABLED} : GuiState{STATE_NORMAL};
            const auto pos = RVector2{
                bidOriginX + static_cast<float>(c) * (bidCellW + bidGap), //
                bidOriginY + static_cast<float>(r) * (bidCellH + bidGap)};
            const auto rect = RRectangle{pos, {bidCellW, bidCellH}};
            const auto clicked = std::invoke([&] {
                if ((state == STATE_DISABLED) or not RMouse::GetPosition().CheckCollision(rect)) { return false; }
                state = RMouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
                return RMouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
            });
            const auto borderColor = getGuiColor(BUTTON, GUI_PROPERTY(BORDER, state));
            const auto bgColor = getGuiColor(BUTTON, GUI_PROPERTY(BASE, state));
            const auto textColor = getGuiColor(BUTTON, GUI_PROPERTY(TEXT, state));
            rect.Draw(bgColor);
            rect.DrawLines(borderColor, getGuiButtonBorderWidth());
            auto text = std::string{ctx.localizeBid(bid)};
            auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), fontSpacing);
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
                ctx.fontM.DrawText(rankText, {textX, textY}, ctx.fontSizeM(), fontSpacing, textColor);
                const auto rankSize = ctx.fontM.MeasureText(rankText, ctx.fontSizeM(), fontSpacing);
                const auto suitColor = state == STATE_DISABLED ? RColor::Red().Alpha(0.4f) : RColor::Red();
                ctx.fontM.DrawText(
                    suitText.c_str(), {textX + rankSize.x, textY}, ctx.fontSizeM(), fontSpacing, suitColor);
            } else {
                ctx.fontM.DrawText(text, {textX, textY}, ctx.fontSizeM(), fontSpacing, textColor);
            }
            if (clicked and (state != STATE_DISABLED)) {
                ctx.bidding.bid = bid;
                ctx.player.bid = bid;
                if (bid != PREF_PASS) { ctx.bidding.rank = rank; }
                ctx.bidding.isVisible = false;
                if (finalBid) {
                    sendDiscardTalon(ctx, std::string{bid});
                } else {
                    sendBidding(ctx, std::string{bid});
                }
                return;
            }
        }
    }
}

auto drawWhistingMenu(Context& ctx) -> void
{
    if (not ctx.whisting.isVisible) { return; }
    static constexpr auto cellW = virtualW / 6.f;
    static constexpr auto cellH = cellW / 2.f;
    static constexpr auto gap = cellH / 10.f;
    const auto checkHalfWhist
        = [&](const GameText text) { return ctx.whisting.canHalfWhist or text != GameText::HalfWhist; };
    const auto drawButtons = [&](const auto& allButtons) {
        auto buttons = allButtons | rv::filter(checkHalfWhist);
        const auto buttonsCount = rng::distance(buttons);
        const auto menuW = static_cast<float>(buttonsCount) * cellW + static_cast<float>(buttonsCount - 1) * gap;
        const auto originX = (virtualW - menuW) / 2.0f;
        const auto originY = (virtualH - cellH) / 2.0f;
        for (auto&& [i, buttonName] : buttons | rv::enumerate) {
            auto state = GuiState{STATE_NORMAL};
            const auto pos = RVector2{originX + static_cast<float>(i) * (cellW + gap), originY};
            const auto rect = RRectangle{pos, {cellW, cellH}};
            const auto clicked = std::invoke([&] {
                if (not RMouse::GetPosition().CheckCollision(rect)) { return false; }
                state = RMouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
                return RMouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
            });
            const auto borderColor = getGuiColor(BUTTON, GUI_PROPERTY(BORDER, state));
            const auto bgColor = getGuiColor(BUTTON, GUI_PROPERTY(BASE, state));
            const auto textColor = getGuiColor(BUTTON, GUI_PROPERTY(TEXT, state));
            rect.Draw(bgColor);
            rect.DrawLines(borderColor, getGuiButtonBorderWidth());
            const auto text = ctx.localizeText(buttonName);
            const auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), fontSpacing);
            const auto textX = rect.x + (rect.width - textSize.x) / 2.0f;
            const auto textY = rect.y + (rect.height - textSize.y) / 2.0f;
            ctx.fontM.DrawText(text, {textX, textY}, ctx.fontSizeM(), fontSpacing, textColor);
            if (clicked) {
                ctx.whisting.isVisible = false;
                ctx.whisting.choice = localizeText(buttonName, GameLang::English);
                ctx.player.whistingChoice = ctx.whisting.choice;
                sendWhisting(ctx, ctx.whisting.choice);
            }
        }
    };
    if (ctx.bidding.bid == PREF_MISER_WT or ctx.bidding.bid == PREF_MISER) {
        drawButtons(miserButtons);
    } else {
        drawButtons(whistingButtons);
    }
}

template<typename Action>
auto handleCardClick(Context& ctx, Action act) -> void
{
    if (not RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) { return; }
    const auto mousePos = RMouse::GetPosition();
    const auto hit = [&](Card& c) { return RRectangle{c.position, c.texture.GetSize()}.CheckCollision(mousePos); };
    const auto reversed = ctx.player.cards | rv::reverse;
    if (const auto rit = rng::find_if(reversed, hit); rit != rng::cend(reversed)) {
        const auto it = std::next(rit).base();
        if (not isCardPlayable(ctx, *it)) {
            PREF_WARN("Can't play this card: {}", it->name);
            return;
        }
        const auto _ = gsl::finally([&] { ctx.player.cards.erase(it); });
        act(std::move(*it));
    }
}

template<typename... Args>
[[nodiscard]] auto makeCodepoints(Args&&... args) -> auto
{
    auto codepointSize = 0;
    return std::array{GetCodepoint(std::forward<Args>(args), &codepointSize)...};
}

[[nodiscard]] auto makeCodepoints() -> std::vector<int>
{
    const auto ascii = rv::closed_iota(0x20, 0x7E); // space..~
    const auto cyrillic = rv::closed_iota(0x0410, 0x044F); // А..я
    const auto extras = makeCodepoints( // clang-format off
        "Ё", "ё", "Ґ", "ґ", "Є", "є", "І", "і", "Ї", "ї", "è", "’",
        SPADE, CLUB, HEART, DIAMOND, ARROW_RIGHT, ARROW_LEFT); // clang-format on
    return rv::concat(ascii, cyrillic, extras) | rng::to_vector;
}

[[nodiscard]] auto makeAwesomeCodepoints() -> auto
{
    return makeCodepoints(scoreSheetIcon, settingsIcon, enterFullScreenIcon, exitFullScreenIcon);
}

auto setFont(const RFont& font) -> void
{
    GuiSetFont(font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, font.baseSize);
}

auto setDefaultFont(Context& ctx) -> void
{
    setFont(ctx.fontM);
}

auto loadFonts(Context& ctx) -> void
{
    static constexpr auto FontSizeS = static_cast<int>(virtualHeight / 54.f);
    static constexpr auto FontSizeM = static_cast<int>(virtualHeight / 30.f);
    static constexpr auto FontSizeL = static_cast<int>(virtualHeight / 11.25f);
    const auto fontPath = resources("fonts", "DejaVuSans.ttf");
    const auto fontAwesomePath = resources("fonts", "Font-Awesome-7-Free-Solid-900.otf");
    auto codepoints = makeCodepoints();
    auto awesomeCodepoints = makeAwesomeCodepoints();
    ctx.fontS = LoadFontEx(fontPath.c_str(), FontSizeS, std::data(codepoints), std::ssize(codepoints));
    ctx.fontM = LoadFontEx(fontPath.c_str(), FontSizeM, std::data(codepoints), std::ssize(codepoints));
    ctx.fontL = LoadFontEx(fontPath.c_str(), FontSizeL, std::data(codepoints), std::ssize(codepoints));
    ctx.fontAwesome = LoadFontEx(
        fontAwesomePath.c_str(), FontSizeM - 1, std::data(awesomeCodepoints), std::ssize(awesomeCodepoints));
    SetTextureFilter(ctx.fontS.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx.fontM.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx.fontL.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(ctx.fontAwesome.texture, TEXTURE_FILTER_BILINEAR);
    setDefaultFont(ctx);
}

auto loadColorScheme(Context& ctx, const std::string_view style) -> void
{
    const auto name = style | rv::transform([](unsigned char c) { return std::tolower(c); }) | rng::to<std::string>;
    if (name == "light" and not std::empty(name)) {
        // So that raygui doesn't unload the font
        GuiSetFont(ctx.initialFont);
        GuiLoadStyleDefault();
    } else {
        const auto stylePath = resources("styles", std::format("style_{}.rgs", name));
        GuiLoadStyle(stylePath.c_str());
    }
    ctx.settingsMenu.loadedColorScheme = name;
}

[[nodiscard]] constexpr auto textToEnglish(const std::string_view text) noexcept -> std::string_view
{
    for (auto i = 0uz; i < std::to_underlying(GameText::Count); ++i) {
        const auto en = localization[std::to_underlying(GameLang::English)][i];
        if (text == en
            or text == localization[std::to_underlying(GameLang::Ukrainian)][i]
            or text == localization[std::to_underlying(GameLang::Alternative)][i]) {
            return en;
        }
    }
    return text;
}

auto loadLang(Context& ctx, const std::string_view lang) -> void
{
    const auto name = lang | rv::transform([](unsigned char c) { return std::tolower(c); }) | rng::to<std::string>;
    ctx.lang = std::invoke([&] {
        if (name == "ukrainian") { return pref::GameLang::Ukrainian; }
        if (name == "alternative") { return pref::GameLang::Alternative; }
        return pref::GameLang::English;
    });
    ctx.settingsMenu.loadedLang = name;
}

auto drawToolbarButton(Context& ctx, const int indexFromRight, const char* icon, const auto onClick) -> void
{
    assert(indexFromRight >= 1);
    static constexpr auto buttonW = virtualWidth / 26.f;
    static constexpr auto buttonH = buttonW;
    static constexpr auto gapBetweenButtons = borderMargin / 5.f;
    const auto i = static_cast<float>(indexFromRight);
    const auto bounds = RRectangle{
        virtualWidth - buttonW * i - borderMargin - gapBetweenButtons * (i - 1),
        virtualHeight - buttonH - borderMargin,
        buttonW,
        buttonH};
    setFont(ctx.fontAwesome);
    if (GuiButton(bounds, icon)) { onClick(); }
    setDefaultFont(ctx);
}

auto drawFullScreenButton(Context& ctx) -> void
{
    drawToolbarButton(ctx, 1, ctx.window.IsFullscreen() ? exitFullScreenIcon : enterFullScreenIcon, [&] {
        ctx.window.ToggleFullscreen();
    });
}

auto drawSettingsButton(Context& ctx) -> void
{
    drawToolbarButton(ctx, 2, settingsIcon, [&] { ctx.settingsMenu.isVisible = not ctx.settingsMenu.isVisible; });
}

auto drawScoreSheetButton(Context& ctx) -> void
{
    drawToolbarButton(ctx, 3, scoreSheetIcon, [&] { ctx.scoreSheet.isVisible = not ctx.scoreSheet.isVisible; });
}

auto drawSettingsMenu(Context& ctx) -> void
{
    drawSettingsButton(ctx);
    if (not ctx.settingsMenu.isVisible) { return; }
    if (RKeyboard::IsKeyPressed(KEY_ESCAPE)) {
        ctx.settingsMenu.isVisible = false;
        return;
    }
    const auto rowH = GuiGetStyle(LISTVIEW, LIST_ITEMS_HEIGHT);
    const auto textS = getGuiDefaultTextSize();
    const auto labelGap = virtualH / 180.f; // gap between label and its list
    const auto labelH = textS + labelGap; // readable label height
    const auto spacing = virtualH / 90.f; // vertical spacing between sections
    const auto headerH = virtualH / 30.f; // panel title strip
    const auto footerH = virtualH / 15.f; // bottom area for button
    const auto margin = virtualH / 90.f;
    const auto langs = std::vector{
        ctx.localizeText(GameText::English),
        ctx.localizeText(GameText::Ukrainian),
        ctx.localizeText(GameText::Alternative),
    };
    const auto colorSchemes = std::vector{
        ctx.localizeText(GameText::Light),
        ctx.localizeText(GameText::Bluish),
        ctx.localizeText(GameText::Ashes),
        ctx.localizeText(GameText::Dark),
        ctx.localizeText(GameText::Amber),
        ctx.localizeText(GameText::Genesis),
        ctx.localizeText(GameText::Cyber),
        ctx.localizeText(GameText::Jungle),
        ctx.localizeText(GameText::Lavanda),
    };
    const auto colorSchemesH = static_cast<float>((std::ssize(colorSchemes) + 1) * rowH);
    const auto langsH = static_cast<float>((std::ssize(langs) + 1) * rowH);
    const auto totalH = headerH + footerH + labelH + labelGap + langsH + spacing + labelH + labelGap + colorSchemesH;
    static constexpr auto totalW = virtualW / 5.f;
    const auto panelBounds = RRectangle{virtualWidth - borderMargin - totalW, virtualH / 7.2f, totalW, totalH};
    setFont(ctx.fontS);
    GuiPanel(panelBounds, ctx.localizeText(GameText::Settings).c_str());
    const auto inner = RRectangle{
        panelBounds.x + margin,
        panelBounds.y + headerH,
        panelBounds.width - margin * 2,
        panelBounds.height - headerH - footerH};
    const auto langsLabelBounds = RRectangle{inner.x, inner.y, inner.width, labelH};
    auto prev = GuiGetStyle(LABEL, TEXT_COLOR_NORMAL);
    GuiSetStyle(LABEL, TEXT_COLOR_NORMAL, GuiGetStyle(LABEL, TEXT_COLOR_PRESSED));
    GuiLabel(langsLabelBounds, ctx.localizeText(GameText::Language).c_str());
    GuiSetStyle(LABEL, TEXT_COLOR_NORMAL, prev);
    const auto joinedLangs = langs | rv::intersperse(";") | rv::join | rng::to<std::string>;
    const auto langsBounds
        = RRectangle{inner.x, langsLabelBounds.y + langsLabelBounds.height + labelGap, inner.width, langsH};
    GuiListView(langsBounds, joinedLangs.c_str(), &ctx.settingsMenu.langIdScroll, &ctx.settingsMenu.langIdSelect);
    if (ctx.settingsMenu.langIdSelect >= 0
        and ctx.settingsMenu.langIdSelect < std::ssize(langs)
        and RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (const auto langToLoad = textToEnglish(langs[static_cast<std::size_t>(ctx.settingsMenu.langIdSelect)]);
            ctx.settingsMenu.loadedLang != langToLoad) {
            loadLang(ctx, langToLoad);
        }
    }

    const auto colorSchemeLabelBounds
        = RRectangle{inner.x, langsBounds.y + langsBounds.height + spacing, inner.width, labelH};
    prev = GuiGetStyle(LABEL, TEXT_COLOR_NORMAL);
    GuiSetStyle(LABEL, TEXT_COLOR_NORMAL, GuiGetStyle(LABEL, TEXT_COLOR_PRESSED));
    GuiLabel(colorSchemeLabelBounds, ctx.localizeText(GameText::ColorScheme).c_str());
    GuiSetStyle(LABEL, TEXT_COLOR_NORMAL, prev);
    const auto joinedColorSchemes = colorSchemes | rv::intersperse(";") | rv::join | rng::to<std::string>;
    const auto colorSchemesBounds = RRectangle{
        inner.x, colorSchemeLabelBounds.y + colorSchemeLabelBounds.height + labelGap, inner.width, colorSchemesH};
    GuiListView(
        colorSchemesBounds,
        joinedColorSchemes.c_str(),
        &ctx.settingsMenu.colorSchemeIdScroll,
        &ctx.settingsMenu.colorSchemeIdSelect);
    if (ctx.settingsMenu.colorSchemeIdSelect >= 0
        and ctx.settingsMenu.colorSchemeIdSelect < std::ssize(colorSchemes)
        and RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (const auto colorSchemeToLoad
            = textToEnglish(colorSchemes[static_cast<std::size_t>(ctx.settingsMenu.colorSchemeIdSelect)]);
            ctx.settingsMenu.loadedColorScheme != colorSchemeToLoad) {
            loadColorScheme(ctx, colorSchemeToLoad);
        }
    }

    const auto okW = virtualW / 20.f;
    const auto okH = virtualH / 36.f;
    const auto okBounds = RRectangle{
        inner.x + inner.width - okW, panelBounds.y + panelBounds.height - footerH + (footerH - okH) * 0.5f, okW, okH};
    if (GuiButton(okBounds, "OK")) { ctx.settingsMenu.isVisible = false; }
    setDefaultFont(ctx);
}

auto drawScoreSheet(Context& ctx) -> void
{
    drawScoreSheetButton(ctx);
    if (not ctx.scoreSheet.isVisible) { return; }
    if (RKeyboard::IsKeyPressed(KEY_ESCAPE)) {
        ctx.scoreSheet.isVisible = false;
        return;
    }
    static constexpr auto fSpace = fontSpacing;
    static constexpr auto rotateL = 90.f;
    static constexpr auto rotateR = 270.f;
    const auto center = RVector2{virtualWidth / 2.f, virtualHeight / 2.f};
    const auto sheetS = center.y * 1.5f;
    const auto sheet = RVector2{center.x - sheetS / 2.f, center.y - sheetS / 2.f};
    const auto radius = sheetS / 20.f;
    const auto thick = getGuiButtonBorderWidth();
    const auto posm = 2.f / 9.f;
    const auto poss = 5.f / 18.f;
    const auto rl = RRectangle{sheet.x, sheet.y, sheetS, sheetS};
    const auto rm = RRectangle{rl.x + sheetS * posm, rl.y, sheetS - sheetS * posm * 2.f, sheetS - sheetS * posm};
    const auto rs = RRectangle{rl.x + sheetS * poss, rl.y, sheetS - sheetS * poss * 2.f, sheetS - sheetS * poss};
    const auto borderColor = getGuiColor(BORDER_COLOR_NORMAL);
    const auto sheetColor = getGuiColor(BASE_COLOR_NORMAL);
    const auto fSize = ctx.fontSizeS();
    const auto c = getGuiColor(LABEL, TEXT_COLOR_NORMAL);
    const auto gap = fSize / 4.f;
    const auto& font = ctx.fontS;
    rl.Draw(sheetColor);
    rl.DrawLines(borderColor, thick);
    rs.DrawLines(borderColor, thick);
    rm.DrawLines(borderColor, thick);
    center.DrawLine({rl.x + thick, rl.y + rl.height - thick}, thick, borderColor);
    center.DrawLine({rl.x + rl.width - thick, rl.y + rl.height - thick}, thick, borderColor);
    center.DrawLine({rl.x + rl.width / 2.f, rl.y}, thick, borderColor);
    center.DrawLine({rl.x + rl.width / 2.f, rl.y}, thick, borderColor);
    center.DrawCircle(radius, borderColor);
    center.DrawCircle(radius - thick, sheetColor);
    RVector2{rl.x, center.y}.DrawLine({rm.x, center.y}, thick, borderColor);
    RVector2{rl.x + rl.width, center.y}.DrawLine({rm.x + rm.width, center.y}, thick, borderColor);
    RVector2{center.x, rl.y + rl.height}.DrawLine({center.x, rm.y + rm.height}, thick, borderColor);
    // TODO: don't hardcode `scoreTarget`
    static constexpr auto scoreTarget = "10";
    const auto scoreTargetSize = ctx.fontM.MeasureText(scoreTarget, ctx.fontSizeM(), fSpace);
    ctx.fontM.DrawText(
        scoreTarget,
        {center.x - scoreTargetSize.x / 2.f, center.y - scoreTargetSize.y / 2.f},
        ctx.fontSizeM(),
        fSpace,
        c);
    const auto joinValues = [](const auto& values) {
        return fmt::format("{}", fmt::join(values | rv::filter(notEqualTo(0)) | rv::partial_sum(std::plus{}), "."));
    };
    const auto [leftId, rightId] = getOpponentIds(ctx);
    for (const auto& [playerId, score] : ctx.scoreSheet.score) {
        const auto resultValue = std::invoke([&]() -> std::optional<std::tuple<std::string, RVector2, RColor>> {
            const auto finalResult = calculateFinalResult(makeFinalScore(ctx.scoreSheet.score));
            if (not finalResult.contains(playerId)) { return std::nullopt; }
            const auto value = finalResult.at(playerId);
            const auto result = fmt::format("{}{}", value > 0 ? "+" : "", value);
            return std::tuple{
                result,
                ctx.fontM.MeasureText(result, ctx.fontSizeM(), fSpace),
                result.starts_with('-') ? RColor::Red() : (result.starts_with('+') ? RColor::DarkGreen() : c)};
        });
        // TODO: use `assert(cond)` instead of `else if(cond)`?
        if (playerId == ctx.myPlayerId) {
            if (resultValue) {
                const auto& [result, resultSize, color] = *resultValue;
                ctx.fontM.DrawText(
                    result, {center.x - (resultSize.x / 2.f), center.y + radius}, ctx.fontSizeM(), fSpace, color);
            }
            const auto dumpValues = joinValues(score.dump);
            const auto poolValues = joinValues(score.pool);
            font.DrawText(dumpValues, {rs.x + radius, rs.y + rs.height - fSize}, fSize, fSpace, c);
            // TODO: properly center `poolValues` (here and below) instead of adding `gap / 2.f`
            font.DrawText(poolValues, {rs.x + gap, rs.y + rs.height + gap / 2.f}, fSize, fSpace, c);
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
                ctx.fontM.DrawText(
                    result,
                    {center.x + radius, center.y + (resultSize.x / 2.f)},
                    {},
                    rotateR,
                    ctx.fontSizeM(),
                    fSpace,
                    color);
            }
            const auto dumpValues = joinValues(score.dump);
            const auto poolValues = joinValues(score.pool);
            font.DrawText(
                dumpValues, {rs.x + rs.width - fSize, rs.y + rs.height - radius}, {}, rotateR, fSize, fSpace, c);
            font.DrawText(
                poolValues, {rs.x + rs.width + gap / 2.f, rs.y + rs.height - gap}, {}, rotateR, fSize, fSpace, c);
            for (const auto& [toWhomWhistId, whist] : score.whists) {
                const auto whistValues = joinValues(whist);
                if (toWhomWhistId == ctx.myPlayerId) {
                    font.DrawText(
                        whistValues, {rm.x + rm.width, rm.y + rm.height - gap}, {}, rotateR, fSize, fSpace, c);
                } else if (toWhomWhistId == leftId) {
                    font.DrawText(whistValues, {rm.x + rm.width, center.y - gap}, {}, rotateR, fSize, fSpace, c);
                }
            }
        } else if (playerId == leftId) {
            if (resultValue) {
                const auto& [result, resultSize, color] = *resultValue;
                ctx.fontM.DrawText(
                    result,
                    {center.x - radius, center.y - (resultSize.x / 2.f)},
                    {},
                    rotateL,
                    ctx.fontSizeM(),
                    fSpace,
                    color);
            }
            const auto dumpValues = joinValues(score.dump);
            const auto poolValues = joinValues(score.pool);
            font.DrawText(dumpValues, {rs.x + fSize, rl.y + gap}, {}, rotateL, fSize, fSpace, c);
            font.DrawText(poolValues, {rs.x - gap / 2, rs.y + gap}, {}, rotateL, fSize, fSpace, c);
            for (const auto& [toWhomWhistId, whist] : score.whists) {
                const auto whistValues = joinValues(whist);
                if (toWhomWhistId == ctx.myPlayerId) {
                    font.DrawText(whistValues, {rm.x, center.y + gap}, {}, rotateL, fSize, fSpace, c);
                } else if (toWhomWhistId == rightId) {
                    font.DrawText(whistValues, {rm.x, rm.y + gap}, {}, rotateL, fSize, fSpace, c);
                }
            }
        }
    }
}

auto updateDrawFrame(void* userData) -> void
{
    assert(userData);
    auto& ctx = toContext(userData);

    if (RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        const auto mousePos = RMouse::GetPosition();
        sendLog(ctx, fmt::format("[mousePressed] x: {}, y: {}", mousePos.x, mousePos.y));
        PREF_INFO("x: {}, y: {}", mousePos.x, mousePos.y);
    }
    if (std::empty(ctx.myPlayerId)) { ctx.myPlayerId = loadPlayerIdFromLocalStorage(); }
    if ((ctx.myPlayerId == ctx.turnPlayerId) and RMouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (ctx.stage == "Playing") {
            handleCardClick(ctx, [&](Card&& card) {
                const auto name = card.name; // copy before move `card`
                if (std::empty(ctx.cardsOnTable)) { ctx.leadSuit = cardSuit(name); }
                ctx.cardsOnTable.insert_or_assign(ctx.myPlayerId, std::move(card));
                sendPlayCard(ctx, name);
            });
        } else if ((ctx.stage == "TalonPicking") and (std::size(ctx.discardedTalon) < 2)) {
            handleCardClick(ctx, [&](Card&& card) { ctx.discardedTalon.push_back(card.name); });
            if (std::size(ctx.discardedTalon) == 2) {
                if (const auto isSixSpade = ctx.bidding.rank == 0; isSixSpade) {
                    ctx.bidding.rank = allRanks;
                } else if (const auto isNotAllPassed = ctx.bidding.rank != allRanks; isNotAllPassed) {
                    --ctx.bidding.rank;
                }
                ctx.bidding.isVisible = true;
            }
        }
    }
    ctx.target.BeginMode();
    ctx.window.ClearBackground(getGuiColor(BACKGROUND_COLOR));
    drawBiddingMenu(ctx);
    drawWhistingMenu(ctx);
    if (not ctx.hasEnteredName) {
        drawGameplayScreen(ctx);
        drawEnterNameScreen(ctx);
        drawSettingsMenu(ctx);
        drawFullScreenButton(ctx);
        ctx.window.DrawFPS(virtualWidth - virtualWidth / 24, 0);
        ctx.target.EndMode();
        goto end;
    }
    if (ctx.areAllPlayersJoined()) {
        drawMyHand(ctx);
        auto leftX = virtualW / 24.f;
        auto rightX = virtualWidth - cardWidth - leftX;
        const auto [leftId, rightId] = getOpponentIds(ctx);
        drawOpponentHand(ctx, ctx.leftCardCount, leftX, leftId, false);
        drawOpponentHand(ctx, ctx.rightCardCount, rightX, rightId, true);
        drawPlayedCards(ctx);
        drawScoreSheet(ctx);
    } else {
        drawConnectedPlayersPanel(ctx);
    }
    drawSettingsMenu(ctx);
    drawFullScreenButton(ctx);
    ctx.window.DrawFPS(virtualWidth - virtualWidth / 24, 0);
    ctx.target.EndMode();
end:
    ctx.window.BeginDrawing();
    ctx.window.ClearBackground(getGuiColor(BACKGROUND_COLOR));
    ctx.target.GetTexture().Draw(
        RRectangle{
            0,
            0,
            static_cast<float>(ctx.target.GetTexture().width),
            -static_cast<float>(ctx.target.GetTexture().height)}, // flip vertically
        RRectangle{ctx.offsetX, ctx.offsetY, virtualWidth * ctx.scale, virtualHeight * ctx.scale});
    ctx.window.EndDrawing();
}

constexpr auto usage = R"(
Usage:
    client [--language=<name>] [--color-scheme=<name>]

Options:
    -h --help               Show this screen.
    --language=<name>       Language to use [default: english]. Options: english, ukrainian, alternative
    --color-scheme=<name>   Color scheme to use [default: light]
                            Options: light, bluish, ashes, dark, amber, genesis, cyber, jungle, lavanda
)";
} // namespace
} // namespace pref

int main(const int argc, const char* const argv[])
{
    spdlog::set_pattern("[%^%l%$][%!] %v");
    auto ctx = pref::Context{};
    pref::updateWindowSize(ctx);
    SetTextureFilter(ctx.target.texture, TEXTURE_FILTER_BILINEAR);
    const auto args = docopt::docopt(pref::usage, {std::next(argv), std::next(argv, argc)});
    pref::loadLang(ctx, args.at("--language").asString());
    GuiLoadStyleDefault();
    ctx.initialFont = GuiGetFont();
    pref::loadColorScheme(ctx, args.at("--color-scheme").asString());
    pref::loadFonts(ctx);
    emscripten_set_resize_callback(
        EMSCRIPTEN_EVENT_TARGET_WINDOW,
        &ctx,
        true,
        []([[maybe_unused]] const int eventType, [[maybe_unused]] const EmscriptenUiEvent* e, void* userData) {
            PREF_INFO("{}", pref::htmlEvent(eventType));
            sendLog(pref::toContext(userData), "resize");
            pref::updateWindowSize(pref::toContext(userData));
            return EM_TRUE;
        });
    emscripten_set_fullscreenchange_callback(
        EMSCRIPTEN_EVENT_TARGET_WINDOW,
        &ctx,
        true,
        []([[maybe_unused]] const int eventType,
           [[maybe_unused]] const EmscriptenFullscreenChangeEvent* e,
           void* userData) {
            PREF_INFO("{}", pref::htmlEvent(eventType));
            sendLog(pref::toContext(userData), "fullscreen");
            pref::updateWindowSize(pref::toContext(userData));
            return EM_FALSE;
        });
    static constexpr const auto fps = 60;
    emscripten_set_main_loop_arg(pref::updateDrawFrame, pref::toUserData(ctx), fps, true);
    return 0;
}
