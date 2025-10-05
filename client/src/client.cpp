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
#define RAYGUI_TEXTSPLIT_MAX_ITEMS 128
#define RAYGUI_TEXTSPLIT_MAX_TEXT_SIZE 8192
#define RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT 36
#define RAYGUI_WINDOWBOX_CLOSEBUTTON_HEIGHT 27
#define RAYGUI_IMPLEMENTATION
#include <raygui.h>
#include <raylib-cpp.hpp>
#include <spdlog/spdlog.h>

#include <cassert>
#include <concepts>
#include <gsl/gsl>
#include <list>
#include <vector>

namespace pref {
namespace {

namespace r = raylib;

enum class Shift : std::uint8_t {
    Horizont = 1 << 0,
    Vertical = 1 << 1,
    Positive = 1 << 2,
    Negative = 1 << 3,
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

enum class DrawPosition {
    Left,
    Right,
};

[[nodiscard]] constexpr auto isRight(const DrawPosition drawPosition) noexcept -> bool
{
    return drawPosition == DrawPosition::Right;
}

[[nodiscard]] constexpr auto isLeft(const DrawPosition drawPosition) noexcept -> bool
{
    return not isRight(drawPosition);
}

template<typename... Args>
[[nodiscard]] auto resources(Args&&... args) -> std::string
{
    return (fs::path("resources") / ... / std::forward<Args>(args)).string();
}

struct Card {
    Card(CardName n, r::Vector2 pos)
        : name{std::move(n)}
        , position{pos}
        , image{r::Image{resources("cards", fmt::format("{}.png", name))}}
    {
        image.Resize(static_cast<int>(CardWidth), static_cast<int>(CardHeight));
        // TODO: Cache all card textures to avoid repeated load/unload operations
        texture = image.LoadTexture();
    }

    CardName name;
    r::Vector2 position;
    r::Image image;
    r::Texture texture{};
};

[[nodiscard]] auto suitValue(const std::string_view suit) -> int
{
    static const auto map = std::map<std::string_view, int>{{SPADES, 1}, {DIAMONDS, 2}, {CLUBS, 3}, {HEARTS, 4}};
    return map.at(suit);
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
        hand.sort([](const Card& lhs, const Card& rhs) {
            const auto lhsSuit = suitValue(cardSuit(lhs.name));
            const auto rhsSuit = suitValue(cardSuit(rhs.name));
            const auto lhsRank = rankValue(cardRank(lhs.name));
            const auto rhsRank = rankValue(cardRank(rhs.name));
            return std::tie(lhsSuit, lhsRank) < std::tie(rhsSuit, rhsRank);
        });
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
    std::list<Card> hand;
    std::string bid;
    std::string whistingChoice;
    std::string howToPlayChoice;
    PlayerId playsOnBehalfOf;
    int tricksTaken{};
};

struct BiddingMenu {
    bool isVisible{};
    std::string bid;
    std::size_t rank = AllRanks;

    auto clear() -> void
    {
        isVisible = false;
        bid.clear();
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
    return suit.ends_with(DIAMOND) or suit.ends_with(HEART);
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
        if (get() != prev) std::forward<Set>(set)(prev);
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
    return localization[std::to_underlying(lang)][std::to_underlying(text)];
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
    static constexpr float SettingsWindowW = VirtualW / 5.f;
    int colorSchemeIdScroll = -1;
    int colorSchemeIdSelect = -1;
    int langIdScroll = -1;
    int langIdSelect = -1;
    bool isVisible{};
    std::string loadedColorScheme;
    std::string loadedLang;
    bool showPingAndFps = false;
    r::Vector2 grabOffset{};
    bool moving{};
    r::Vector2 settingsWindow{VirtualW - BorderMargin - SettingsWindowW, BorderMargin};
};

struct ScoreSheetMenu {
    ScoreSheet score;
    bool isVisible{};
};

struct SpeechBubbleMenu {
    bool isVisible{};
    bool isSendButtonActive = true;
    static constexpr int SendCooldownInSec = 30;
    int cooldown = SendCooldownInSec;
    std::map<PlayerId, std::string> text;
};

struct Ping {
    static constexpr auto IntervalInMs = 15'000; // 15s;
    static constexpr auto InvalidRtt = -1;
    std::unordered_map<std::int32_t, double> sentAt;
    double rtt = static_cast<double>(InvalidRtt);
};

struct Context {
    // TODO: Figure out how to use one font with different sizes
    //       Should we use `fontL` and scale it down?
    r::Font fontS;
    r::Font fontM;
    r::Font fontL;
    r::Font initialFont;
    r::Font fontAwesome;
    float scale = 1.f;
    float offsetX = 0.f;
    float offsetY = 0.f;
    int windowWidth = static_cast<int>(VirtualW);
    int windowHeight = static_cast<int>(VirtualH);
    r::Window window{
        windowWidth,
        windowHeight,
        "Preferans",
        FLAG_MSAA_4X_HINT // smooth edges for circles, rings, etc.
            | FLAG_WINDOW_ALWAYS_RUN // don't throttle when tab is not focused
            | FLAG_WINDOW_RESIZABLE,
    };
    r::RenderTexture target{static_cast<int>(VirtualW), static_cast<int>(VirtualH)};
    PlayerId myPlayerId;
    PlayerName enteredPlayerName;
    bool hasEnteredName{};
    mutable std::map<PlayerId, Player> players;
    EMSCRIPTEN_WEBSOCKET_T ws{};
    int leftCardCount = 10;
    int rightCardCount = 10;
    std::map<PlayerId, Card> cardsOnTable;
    PlayerId turnPlayerId;
    BiddingMenu bidding;
    WhistingMenu whisting;
    HowToPlayMenu howToPlay;
    GameStage stage = GameStage::UNKNOWN;
    std::vector<CardName> discardedTalon;
    std::string leadSuit;
    GameLang lang{};
    SettingsMenu settingsMenu;
    ScoreSheetMenu scoreSheet;
    SpeechBubbleMenu speechBubbleMenu;
    Ping ping;

    auto clear() -> void
    {
        leftCardCount = 10;
        rightCardCount = 10;
        cardsOnTable.clear();
        turnPlayerId.clear();
        bidding.clear();
        whisting.clear();
        howToPlay.clear();
        stage = GameStage::UNKNOWN;
        discardedTalon.clear();
        leadSuit.clear();
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

[[nodiscard]] auto toContext(void* userData) noexcept -> Context&
{
    return *static_cast<Context*>(userData);
}

[[nodiscard]] auto toUserData(Context& ctx) noexcept -> void*
{
    return static_cast<void*>(&ctx);
}

auto savePlayerIdToLocalStorage(const PlayerId& playerId) -> void
{
    const auto js = std::string{"localStorage.setItem('preferans_player_id', '"} + playerId + "');";
    emscripten_run_script(js.c_str());
}

[[nodiscard]] auto isPlayerTurn(Context& ctx, const PlayerId& playerId) -> bool
{
    return not std::empty(ctx.turnPlayerId) and ctx.turnPlayerId == playerId;
}

[[nodiscard]] auto isMyTurn(Context& ctx) -> bool
{
    return isPlayerTurn(ctx, ctx.myPlayerId);
}

[[nodiscard]] auto isSomeonePlayingOnBehalfOf(Context& ctx, const PlayerId& playerId) -> bool
{
    return not std::empty(playerId)
        and rng::any_of(ctx.players | rv::values, equalTo(playerId), &Player::playsOnBehalfOf);
}

[[nodiscard]] auto isSomeonePlayingOnMyBehalf(Context& ctx) -> bool
{
    return isSomeonePlayingOnBehalfOf(ctx, ctx.myPlayerId);
}

[[nodiscard]] auto isPlayerTurnOnBehalfOfSomeone(Context& ctx, const PlayerId& playerId) -> bool
{
    return not std::empty(ctx.turnPlayerId)
        and not std::empty(playerId)
        and ctx.turnPlayerId == ctx.player(playerId).playsOnBehalfOf;
}

[[nodiscard]] auto isMyTurnOnBehalfOfSomeone(Context& ctx) -> bool
{
    return isPlayerTurnOnBehalfOfSomeone(ctx, ctx.myPlayerId);
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

auto sendMessage(const EMSCRIPTEN_WEBSOCKET_T ws, const Message& msg) -> bool
{
    if (ws == 0) {
        PREF_WARN("error: ws is not open");
        return false;
    }
    auto data = msg.SerializeAsString();
    if (const auto result = emscripten_websocket_send_binary(ws, data.data(), std::size(data));
        result != EMSCRIPTEN_RESULT_SUCCESS) {
        PREF_WARN("error: {}, method: {}", emResult(result), msg.method());
        return false;
    }
    return true;
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

[[nodiscard]] auto makeHowToPlay(const std::string& playerId, const std::string& choice) -> HowToPlay
{
    auto result = HowToPlay{};
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

[[nodiscard]] auto makeSpeechBubble(const std::string& playerId, const std::string& text) -> SpeechBubble
{
    auto result = SpeechBubble{};
    result.set_player_id(playerId);
    result.set_text(text);
    return result;
}

[[nodiscard]] auto makePingPong(const int id) -> PingPong
{
    auto result = PingPong{};
    result.set_id(id);
    return result;
}

auto sendJoinRequest(Context& ctx) -> void
{
    sendMessage(ctx.ws, makeMessage(makeJoinRequest(ctx.myPlayerId, ctx.enteredPlayerName)));
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

auto sendHowToPlay(Context& ctx, const std::string& choice) -> void
{
    sendMessage(ctx.ws, makeMessage(makeHowToPlay(ctx.myPlayerId, choice)));
}

auto sendPlayCard(Context& ctx, const PlayerId& playerId, const CardName& cardName) -> void
{
    sendMessage(ctx.ws, makeMessage(makePlayCard(playerId, cardName)));
}

[[maybe_unused]] auto sendLog(Context& ctx, const std::string& text) -> void
{
    sendMessage(ctx.ws, makeMessage(makeLog(ctx.myPlayerId, text)));
}

auto sendSpeechBubble(Context& ctx, const std::string& text) -> void
{
    sendMessage(ctx.ws, makeMessage(makeSpeechBubble(ctx.myPlayerId, text)));
}

[[nodiscard]] auto sendPingPong(Context& ctx, const std::int32_t id) -> bool
{
    return sendMessage(ctx.ws, makeMessage(makePingPong(id)));
}

auto repeatPingPong(Context& ct) -> void;

auto shedulePingPong(Context& ctx) -> void
{
    emscripten_async_call(
        [](void* userData) { repeatPingPong(toContext(userData)); }, toUserData(ctx), Ping::IntervalInMs);
}

auto repeatPingPong(Context& ctx) -> void
{
    static auto id = 0;
    const auto now = emscripten_get_now();
    ctx.ping.sentAt[++id] = now;
    if (not sendPingPong(ctx, id)) {
        ctx.ping.rtt = static_cast<double>(Ping::InvalidRtt);
        return;
    }
    shedulePingPong(ctx);
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
    for (auto& p : *(joinResponse->mutable_players())) {
        auto player = Player{*p.mutable_player_id(), std::move(*p.mutable_player_name())};
        ctx.players.insert_or_assign(std::move(*p.mutable_player_id()), std::move(player));
    }
    repeatPingPong(ctx);
}

auto handlePlayerJoined(Context& ctx, const Message& msg) -> void
{
    auto playerJoined = makeMethod<PlayerJoined>(msg);
    if (not playerJoined) { return; }
    auto& playerJoinedId = *playerJoined->mutable_player_id();
    auto& playerJoinedName = *playerJoined->mutable_player_name();
    PREF_INFO("New player playerJoined: {} ({})", playerJoinedName, playerJoinedId);
    auto player = Player{playerJoinedId, std::move(playerJoinedName)};
    ctx.players.insert_or_assign(std::move(playerJoinedId), std::move(player));
}

auto handlePlayerLeft(Context& ctx, const Message& msg) -> void
{
    const auto playerLeft = makeMethod<PlayerLeft>(msg);
    if (not playerLeft) { return; }
    ctx.players.erase(playerLeft->player_id());
}

auto handleDealCards(Context& ctx, const Message& msg) -> void
{
    auto dealCards = makeMethod<DealCards>(msg);
    if (not dealCards) { return; }
    const auto& playerId = dealCards->player_id();
    assert(std::size(dealCards->cards()) == 10);
    auto& player = std::invoke([&] -> Player& {
        if (playerId == ctx.myPlayerId) { ctx.clear(); }
        return ctx.player(playerId);
    });
    for (auto&& cardName : *(dealCards->mutable_cards())) {
        player.hand.emplace_back(std::move(cardName), r::Vector2{});
    }
    player.sortCards();
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
    PREF_INFO("turnPlayerId: {}, stage: {}", ctx.turnPlayerId, GameStage_Name(ctx.stage));
    if (not isMyTurn(ctx)) { return; }
    if (ctx.stage == GameStage::BIDDING) {
        ctx.bidding.isVisible = true;
        return;
    }
    if (ctx.stage == GameStage::TALON_PICKING) {
        for (auto&& card : *(playerTurn->mutable_talon())) {
            ctx.myPlayer().hand.emplace_back(std::move(card), r::Vector2{});
        }
        ctx.myPlayer().sortCards();
        return;
    }
    if (ctx.stage == GameStage::WHISTING) {
        ctx.whisting.canHalfWhist = playerTurn->can_half_whist();
        ctx.whisting.isVisible = true;
        return;
    }
    if (ctx.stage == GameStage::HOW_TO_PLAY) {
        ctx.howToPlay.isVisible = true;
        return;
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
    ctx.player(playerId).bid = bid;
    INFO_VAR(playerId, bid, rank);
}

auto handleWhisting(Context& ctx, const Message& msg) -> void
{
    const auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { return; }
    const auto& playerId = whisting->player_id();
    const auto& choice = whisting->choice();
    ctx.player(playerId).whistingChoice = choice;
    INFO_VAR(playerId, choice);
}

auto handleOpenWhistPlay(Context& ctx, const Message& msg) -> void
{
    auto openWhistPlay = makeMethod<OpenWhistPlay>(msg);
    if (not openWhistPlay) { return; }
    const auto& activeWhisterId = openWhistPlay->active_whister_id();
    auto& passiveWhisterId = *(openWhistPlay->mutable_passive_whister_id());
    INFO_VAR(activeWhisterId, passiveWhisterId);
    ctx.player(activeWhisterId).playsOnBehalfOf = std::move(passiveWhisterId);
}

auto handleHowToPlay(Context& ctx, const Message& msg) -> void
{
    const auto howToPlay = makeMethod<HowToPlay>(msg);
    if (not howToPlay) { return; }
    const auto& playerId = howToPlay->player_id();
    const auto& choice = howToPlay->choice();
    ctx.player(playerId).howToPlayChoice = choice;
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
    }

    ctx.player(playerId).hand |= rng::actions::remove_if(equalTo(cardName), &Card::name);
    if (std::empty(ctx.cardsOnTable)) { ctx.leadSuit = cardSuit(cardName); }
    ctx.cardsOnTable.emplace(std::move(playerId), Card{std::move(cardName), r::Vector2{}});
}

auto handleTrickFinished(Context& ctx, const Message& msg) -> void
{
    const auto trickFinished = makeMethod<TrickFinished>(msg);
    if (not trickFinished) { return; }
    for (auto&& tricks : trickFinished->tricks()) {
        auto&& playerId = tricks.player_id();
        auto&& tricksTaken = tricks.taken();
        INFO_VAR(playerId, tricksTaken);
        ctx.player(playerId).tricksTaken = tricksTaken;
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

auto handleSpeechBubble(Context& ctx, const Message& msg) -> void
{
    const auto speechBubble = makeMethod<SpeechBubble>(msg);
    if (not speechBubble) { return; }
    ctx.speechBubbleMenu.text.insert_or_assign(speechBubble->player_id(), speechBubble->text());
}

auto handlePingPong(Context& ctx, const Message& msg) -> void
{
    const auto pingPong = makeMethod<PingPong>(msg);
    if (not pingPong) { return; }
    const auto id = pingPong->id();
    if (not ctx.ping.sentAt.contains(id)) { return; }
    ctx.ping.rtt = emscripten_get_now() - ctx.ping.sentAt[id];
    ctx.ping.sentAt.erase(id);
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
    ctx.scale = std::fminf(windowW / VirtualW, windowH / VirtualH);
    ctx.offsetX = (windowW - VirtualW * ctx.scale) * 0.5f;
    ctx.offsetY = (windowH - VirtualH * ctx.scale) * 0.5f;
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
    PREF_INFO("{}", text);
    r::Mouse::SetOffset(static_cast<int>(-ctx.offsetX), static_cast<int>(-ctx.offsetY));
    r::Mouse::SetScale(1.f / ctx.scale, 1.f / ctx.scale);
}

auto dispatchMessage(Context& ctx, const std::optional<Message>& msg) -> void
{
    if (not msg) { return; }
    const auto& method = msg->method();
    if (method == "JoinResponse") { return handleJoinResponse(ctx, *msg); }
    if (method == "PlayerJoined") { return handlePlayerJoined(ctx, *msg); }
    if (method == "PlayerLeft") { return handlePlayerLeft(ctx, *msg); }
    if (method == "DealCards") { return handleDealCards(ctx, *msg); }
    if (method == "PlayerTurn") { return handlePlayerTurn(ctx, *msg); }
    if (method == "Bidding") { return handleBidding(ctx, *msg); }
    if (method == "Whisting") { return handleWhisting(ctx, *msg); }
    if (method == "OpenWhistPlay") { return handleOpenWhistPlay(ctx, *msg); }
    if (method == "HowToPlay") { return handleHowToPlay(ctx, *msg); }
    if (method == "PlayCard") { return handlePlayCard(ctx, *msg); }
    if (method == "TrickFinished") { return handleTrickFinished(ctx, *msg); }
    if (method == "DealFinished") { return handleDealFinished(ctx, *msg); }
    if (method == "SpeechBubble") { return handleSpeechBubble(ctx, *msg); }
    if (method == "PingPong") { return handlePingPong(ctx, *msg); }
    PREF_WARN("error: unknown {}", VAR(method));
}

[[nodiscard]] auto loadPlayerIdFromLocalStorage() -> PlayerId
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

auto onWsOpen(
    [[maybe_unused]] const int eventType, [[maybe_unused]] const EmscriptenWebSocketOpenEvent* event, void* userData)
    -> EM_BOOL
{
    assert(event);
    assert(userData);
    sendJoinRequest(toContext(userData));
    return EM_TRUE;
}

auto onWsMessage([[maybe_unused]] const int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData)
    -> EM_BOOL
{
    assert(event);
    assert(userData);
    dispatchMessage(toContext(userData), makeMessage(*event));
    return EM_TRUE;
}

auto onWsError([[maybe_unused]] const int eventType, const EmscriptenWebSocketErrorEvent* event, void* userData)
    -> EM_BOOL
{
    assert(event);
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

[[maybe_unused]] auto drawDebugDot(Context& ctx, const r::Vector2& pos, const std::string_view text) -> void
{
    pos.DrawCircle(5, r::Color::Red());
    ctx.fontS.DrawText(std::string{text}, pos, ctx.fontSizeS(), FontSpacing, r::Color::Black());
}

[[maybe_unused]] auto drawDebugVertLine(Context& ctx, const float x, const std::string_view text) -> void
{
    r::Vector2{x, 0.f}.DrawLine({x, VirtualH}, 1, r::Color::Red());
    ctx.fontS.DrawText(std::string{text}, {x, VirtualH * 0.5f}, ctx.fontSizeS(), FontSpacing, r::Color::Black());
}

[[maybe_unused]] auto drawDebugHorzLine(Context& ctx, const float y, const std::string_view text) -> void
{
    r::Vector2{0.f, y}.DrawLine({VirtualW, y}, 1, r::Color::Red());
    ctx.fontS.DrawText(std::string{text}, {VirtualW * 0.5f, y}, ctx.fontSizeS(), FontSpacing, r::Color::Black());
}

auto drawGuiLabelCentered(const std::string& text, const r::Vector2& anchor) -> void
{
    const auto size = measureGuiText(text);
    const auto shift = VirtualH / 135.f;
    const auto bounds = r::Rectangle{
        anchor.x - size.x * 0.5f - (shift / 2.f), // shift left to center and add left padding
        anchor.y - size.y * 0.5f - (shift / 2.f), // shift up to center and add top padding
        size.x + shift, // width = text + left + right padding
        size.y + shift // height = text + top + bottom padding
    };
    GuiLabel(bounds, text.c_str());
}

auto drawSpeechBubbleText(Context& ctx, const r::Vector2& p3, const std::string& text, const DrawPosition drawPosition)
    -> void
{
    using enum DrawPosition;
    static constexpr auto roundness = 0.7f;
    const auto fontSize = ctx.fontSizeM();
    const auto textSize = ctx.fontM.MeasureText(text.c_str(), fontSize, FontSpacing);
    const auto padding = textSize.y / 2.f;
    const auto bubbleWidth = textSize.x + padding * 2.f;
    const auto bubbleHeight = textSize.y + padding * 2.f;
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
    const auto thick = getGuiButtonBorderWidth(); // * 2;
    static constexpr auto segments = 64;
    r::Rectangle{rect.x - thick * 0.5f, rect.y - thick * 0.5f, rect.width + thick, rect.height + thick}.DrawRounded(
        roundness, segments, colorBackground);
    rect.DrawRoundedLines(roundness, segments, thick, colorBorder);
    isRight(drawPosition) ? DrawTriangle(p3, p2, p1, colorBackground) : DrawTriangle(p1, p2, p3, colorBackground);
    p3.DrawLine(p1, thick, colorBorder);
    p3.DrawLine(p2, thick, colorBorder);
    ctx.fontM.DrawText(text.c_str(), {rect.x + padding, rect.y + padding}, fontSize, FontSpacing, colorText);
}

auto drawGameplayScreen(Context& ctx) -> void
{
    const auto title = ctx.localizeText(GameText::Preferans);
    const auto textSize = ctx.fontL.MeasureText(title, ctx.fontSizeL(), FontSpacing);
    const auto x = (VirtualW - textSize.x) / 2.f;
    const auto y = VirtualH / 54.f;
    ctx.fontL.DrawText(title, {x, y}, ctx.fontSizeL(), FontSpacing, getGuiColor(LABEL, TEXT_COLOR_NORMAL));
}

auto drawEnterNameScreen(Context& ctx) -> void
{
    static constexpr auto boxWidth = VirtualW / 4.8f;
    static constexpr auto boxHeight = VirtualH / 18.f;
    const auto screenCenter = r::Vector2{VirtualW, VirtualH} / 2.f;
    const auto boxPos = r::Vector2{screenCenter.x - boxWidth / 2.f, screenCenter.y};
    const auto labelPos = r::Vector2{boxPos.x, boxPos.y - VirtualH / 27.f};
    ctx.fontM.DrawText(
        ctx.localizeText(GameText::EnterYourName),
        labelPos,
        ctx.fontSizeM(),
        FontSpacing,
        getGuiColor(LABEL, TEXT_COLOR_NORMAL));
    const auto inputBox = r::Rectangle{boxPos, {boxWidth, boxHeight}};
    static constexpr auto maxLenghtName = 11;
    static char nameBuffer[maxLenghtName] = "Player";
    static auto editMode = true;
    GuiTextBox(inputBox, nameBuffer, sizeof(nameBuffer), editMode);
    const auto buttonBox = r::Rectangle{boxPos.x, boxPos.y + boxHeight + VirtualH / 54.f, boxWidth, VirtualH / 27.f};
    const auto clicked = GuiButton(buttonBox, ctx.localizeText(GameText::Enter).c_str());
    if ((clicked or r::Keyboard::IsKeyPressed(KEY_ENTER)) and (nameBuffer[0] != '\0') and editMode) {
        ctx.enteredPlayerName = nameBuffer;
        ctx.hasEnteredName = true;
        editMode = false;
        setup_websocket(ctx);
    }
}

auto drawConnectedPlayersPanel(const Context& ctx) -> void
{
    static constexpr auto pad = VirtualH / 108.f;
    static constexpr auto minWidth = VirtualW / 9.6f;
    static constexpr auto minHeight = VirtualH / 13.5f;
    const auto fontSize = ctx.fontSizeS();
    const auto lineGap = fontSize * 1.2f;
    const auto headerGap = fontSize * .5f;
    const auto headerText = ctx.localizeText(GameText::CurrentPlayers);
    const auto headerSize = ctx.fontS.MeasureText(headerText, fontSize, FontSpacing);
    auto maxWidth = headerSize.x;
    for (const auto& [_, player] : ctx.players) {
        const auto sz = ctx.fontS.MeasureText(player.name, fontSize, FontSpacing);
        if (sz.x > maxWidth) maxWidth = sz.x;
    }
    const auto rows = std::size(ctx.players);
    const auto contentW = pad * 2.f + maxWidth;
    const auto contentH = pad * 2.f + headerSize.y + headerGap + static_cast<float>(rows) * lineGap;
    const auto panelW = std::max(minWidth, contentW);
    const auto panelH = std::max(minHeight, contentH);
    const auto r = r::Rectangle{{BorderMargin, BorderMargin}, {panelW, panelH}};
    GuiPanel(r, nullptr);
    auto textPos = r::Vector2{r.x + pad, r.y + pad};
    ctx.fontS.DrawText(headerText, textPos, fontSize, FontSpacing, getGuiColor(LABEL, TEXT_COLOR_NORMAL));
    textPos.y += headerSize.y + headerGap;
    for (const auto& [id, player] : ctx.players) {
        const auto color = (id == ctx.myPlayerId) ? getGuiColor(TEXT_COLOR_NORMAL) : getGuiColor(TEXT_COLOR_DISABLED);
        ctx.fontS.DrawText(player.name, textPos, fontSize, FontSpacing, color);
        textPos.y += lineGap;
    }
}

[[nodiscard]] auto isCardPlayable(const Context& ctx, const std::list<Card>& hand, const Card& clickedCard) -> bool
{
    const auto clickedSuit = cardSuit(clickedCard.name);
    const auto trump = getTrump(ctx.bidding.bid);
    const auto hasSuit = [&](const std::string_view suit) {
        return rng::any_of(hand, [&](const CardName& name) { return cardSuit(name) == suit; }, &Card::name);
    };
    if (std::empty(ctx.cardsOnTable)) { return true; } // first card in the trick: any card is allowed
    if (clickedSuit == ctx.leadSuit) { return true; } // follows lead suit
    if (hasSuit(ctx.leadSuit)) { return false; } // must follow lead suit
    if (clickedSuit == trump) { return true; } //  no lead suit cards, playing trump
    if (hasSuit(trump)) { return false; } // // must play trump if you have it
    return true; // no lead or trump suit cards, free to play
}

[[nodiscard]] auto tintForCard(const Context& ctx, const std::list<Card>& hand, const Card& card) -> r::Color
{
    const auto lightGray = r::Color{150, 150, 150, 255};
    return isCardPlayable(ctx, hand, card) ? r::Color::White() : lightGray;
}

auto drawCardShineEffect(Context& ctx, const bool isHovered, const Card& card) -> void
{
    if (not isHovered) { return; }
    struct CardShineEffect {
        float speed = 90.f;
        float stripeWidth = CardWidth / 5.f;
        float intensity = 0.3f; // 0..1 alpha
    } const shine;
    const auto time = static_cast<float>(ctx.window.GetTime());
    const auto shineX = std::fmod(time * shine.speed, CardWidth - shine.stripeWidth);
    const auto shineRect = r::Rectangle{card.position.x + shineX, card.position.y, shine.stripeWidth, CardHeight};
    const auto whiteColor = r::Color::White();
    const auto topLeft = whiteColor.Fade(shine.intensity);
    const auto bottomLeft = whiteColor.Fade(0.f);
    const auto& bottomRight = topLeft;
    const auto& topRight = bottomLeft;
    shineRect.DrawGradient(topLeft, bottomLeft, bottomRight, topRight);
}

auto drawCards(Context& ctx, const r::Vector2 pos, Player& player, const Shift shift) -> void
{
    using enum Shift;
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
        const auto isMyHand = ctx.myPlayerId == player.id;
        const auto isTheyHand = ctx.myPlayer().playsOnBehalfOf == player.id;
        const auto isHovered = ((isMyHand and isMyTurn(ctx) and not isSomeonePlayingOnMyBehalf(ctx))
                                or (isTheyHand and isMyTurnOnBehalfOfSomeone(ctx)))
            and (ctx.stage == GameStage::PLAYING or ctx.stage == GameStage::TALON_PICKING)
            and not ctx.bidding.isVisible
            and isCardPlayable(ctx, player.hand, card)
            and static_cast<int>(i) == hoveredIndex;
        static constexpr auto Offset = CardHeight / 10.f;
        const auto offset = isHovered ? (hasShift(shift, Positive) ? Offset : -Offset) : 0.f;
        card.position = toPos(i, offset);
        card.texture.Draw(card.position, tintForCard(ctx, player.hand, card));
        drawCardShineEffect(ctx, isHovered, card);
    }
}

auto drawBackCard(const float x, const float y) -> void
{
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

auto drawCards(Context& ctx, const r::Vector2& pos, Player& player, const Shift shift, const int cardCount) -> void
{
    std::empty(player.hand) ? drawBackCards(cardCount, pos) : drawCards(ctx, pos, player, shift);
}

[[nodiscard]] auto drawGameText(const r::Vector2& pos, const std::string_view gameText, const Shift shift)
    -> std::pair<bool, r::Vector2>
{
    using enum Shift;
    const auto text = std::string{gameText};
    const auto textSize = measureGuiText(text);
    const auto sign = hasShift(shift, Negative) ? -1.f : 1.f;
    const auto shiftX = sign * (textSize.x + textSize.y) * 0.5f;
    const auto shiftY = sign * textSize.y * 1.f;
    const auto anchor = hasShift(shift, Horizont) ? r::Vector2{pos.x + shiftX, pos.y} //
                                                  : r::Vector2{pos.x, pos.y + shiftY};
    if (std::empty(gameText)) { return {false, anchor}; }
    drawGuiLabelCentered(text, anchor);
    return {true, anchor};
}

[[nodiscard]] auto composePlayerName(Context& ctx, const Player& player, const DrawPosition drawPosition) -> std::string
{
    // -> LeftPlayerName (tricksTaken)
    // (tricksTaken) RightPlayerName <-
    const auto tricksTaken = player.tricksTaken;
    return fmt::format(
        "{arrowOrTricks}{name}{tricksOrArrow}",
        fmt::arg(
            "arrowOrTricks",
            isRight(drawPosition) ? (tricksTaken != 0 ? fmt::format("{} ", prettyTricksTaken(tricksTaken)) : "")
                                  : (ctx.turnPlayerId == player.id ? fmt::format("{} ", ARROW_RIGHT) : "")),
        fmt::arg("name", player.name),
        fmt::arg(
            "tricksOrArrow",
            isLeft(drawPosition) ? (tricksTaken != 0 ? fmt::format(" {}", prettyTricksTaken(tricksTaken)) : "")
                                 : (ctx.turnPlayerId == player.id ? fmt::format(" {}", ARROW_LEFT) : "")));
}

[[nodiscard]] auto drawPlayerName(
    Context& ctx, const r::Vector2& pos, const Player& player, const Shift shift, const DrawPosition drawPosition)
    -> r::Vector2
{
    return withGuiStyle(LABEL, TEXT_COLOR_NORMAL, TEXT_COLOR_PRESSED, player.id == ctx.turnPlayerId, [&] {
        return drawGameText(pos, composePlayerName(ctx, player, drawPosition), shift).second;
    });
}

[[nodiscard]] auto drawYourTurn(
    Context& ctx, const r::Vector2& pos, const float gap, const float totalWidth, const PlayerId& playerId) -> float
{
    const auto text = std::invoke([&] {
        if (isPlayerTurnOnBehalfOfSomeone(ctx, playerId)) {
            const auto& playerName = ctx.player(ctx.player(playerId).playsOnBehalfOf).name;
            return fmt::format("{} {}", ctx.localizeText(GameText::YourTurnFor), playerName);
        }
        return ctx.localizeText(GameText::YourTurn);
    });
    const auto textSize = measureGuiText(text);
    const auto textX = pos.x + (totalWidth - textSize.x) * 0.5f;
    const auto textY = (pos.y + BidOriginY + BidMenuH - textSize.y) * 0.5f;
    if ((not isPlayerTurn(ctx, playerId) or isSomeonePlayingOnBehalfOf(ctx, playerId))
        and not isPlayerTurnOnBehalfOfSomeone(ctx, playerId)) {
        return textY;
    }
    const auto rect = r::Rectangle{textX + gap * 0.5f, textY - gap * 0.5f, textSize.x + gap, textSize.y + gap};
    GuiLabel(rect, text.c_str());
    return textY;
}

[[nodiscard]] auto drawWhist(Context& ctx, const r::Vector2& pos, const Player& player, const Shift shift) -> bool
{
    return drawGameText(pos, ctx.localizeText(whistingChoiceToGameText(player.whistingChoice)), shift).first;
}

auto drawBid(Context& ctx, const r::Vector2& pos, const Player& player, const Shift shift) -> bool
{
    return drawGameText(pos, ctx.localizeBid(player.bid), shift).first;
}

auto drawSpeechBubble(Context& ctx, const r::Vector2& pos, const PlayerId& playerId, const DrawPosition drawPosition)
    -> void
{
    if (not ctx.speechBubbleMenu.text.contains(playerId) or std::empty(ctx.speechBubbleMenu.text[playerId])) { return; }
    drawSpeechBubbleText(ctx, pos, ctx.speechBubbleMenu.text[playerId], drawPosition);
    struct UserData {
        Context& ctx;
        std::string playerId;
    };
    emscripten_async_call(
        [](void* userData) {
            auto ud = static_cast<UserData*>(userData);
            auto _ = gsl::finally([&] { delete ud; });
            ud->ctx.speechBubbleMenu.text.erase(ud->playerId);
        },
        new UserData{ctx, playerId},
        5'000); // 5s
}

auto drawMyHand(Context& ctx) -> void
{
    using enum Shift;
    using enum DrawPosition;
    auto& player = ctx.myPlayer();
    const auto cardCount = std::ssize(player.hand);
    if (cardCount == 0) { return; }
    const auto totalWidth = (static_cast<float>(cardCount) - 1.f) * CardOverlapX + CardWidth;
    // card[First|Last][Left|Center|Right][Top|Center|Bottom]Pos
    const auto cardFirstLeftTopPos = r::Vector2{
        (VirtualW - totalWidth) / 2.f,
        VirtualH - CardHeight - CardHeight / 10.8f // bottom padding
    };
    const auto cardCenterY = cardFirstLeftTopPos.y + CardHeight * 0.5f;
    const auto cardFirstLeftCenterPos = r::Vector2{cardFirstLeftTopPos.x, cardCenterY};
    const auto cardLastRightCenterPos = r::Vector2{cardFirstLeftTopPos.x + totalWidth, cardCenterY};
    static constexpr auto gap = CardHeight / 27.f;

    drawCards(ctx, cardFirstLeftTopPos, player, Negative | Horizont);
    const auto playerNameCenter = drawPlayerName(ctx, cardFirstLeftCenterPos, player, Negative | Horizont, Left);
    const auto yourTurnTopY = drawYourTurn(ctx, cardFirstLeftTopPos, gap, totalWidth, ctx.myPlayerId);
    drawWhist(ctx, cardLastRightCenterPos, player, Positive | Horizont) //
        or drawBid(ctx, cardLastRightCenterPos, player, Positive | Horizont);
    drawSpeechBubble(ctx, {playerNameCenter.x, yourTurnTopY + gap * 2}, ctx.myPlayerId, Left);
    // drawDebugHorzLine(ctx, cardCenterY, "cardCenterY");
    // drawDebugDot(ctx, cardFirstLeftCenterPos, "cardFirstLeftCenterPos");
    // drawDebugDot(ctx, cardLastRightCenterPos, "cardLastRightCenterPos");
    // drawDebugVertLine(ctx, playerNameCenter.x, "playerNameCenterX");
    // drawDebugHorzLine(ctx, yourTurnTopY, "yourTurnTopY");
    // drawDebugVertLine(ctx, VirtualW * 0.5f, "screenCnter");
    // drawDebugHorzLine(ctx, VirtualH * 0.5f, "screnCenter");
}

auto drawOpponentHand(Context& ctx, const DrawPosition drawPosition) -> void
{
    using enum Shift;
    const auto playerId = std::invoke([&] {
        const auto [leftId, rightId] = getOpponentIds(ctx);
        return isRight(drawPosition) ? rightId : leftId;
    });

    auto& player = ctx.player(playerId);
    const auto cardCount = std::invoke([&] {
        return not std::empty(player.hand) ? std::ssize(player.hand)
                                           : (isRight(drawPosition) ? ctx.rightCardCount : ctx.leftCardCount);
    });
    if (cardCount == 0) { return; }
    const auto totalHeight = (static_cast<float>(cardCount) - 1.f) * CardOverlapY + CardHeight;

    // card[First|Last][Left|Center|Right][Top|Center|Bottom]Pos
    const auto cardFirstLeftTopPos = r::Vector2{
        isRight(drawPosition) ? VirtualW - CardWidth - VirtualW / 24.f //
                              : VirtualW / 24.f,
        (VirtualH - totalHeight) / 2.f};
    const auto cardCenterX = cardFirstLeftTopPos.x + CardWidth * 0.5f;
    const auto cardFirstCenterTopPos = r::Vector2{cardCenterX, cardFirstLeftTopPos.y};
    const auto cardLastCenterBottomPos = r::Vector2{cardCenterX, cardFirstLeftTopPos.y + totalHeight};

    drawCards(ctx, cardFirstLeftTopPos, player, (isRight(drawPosition) ? Negative : Positive) | Vertical, cardCount);
    const auto playerNameCenter = drawPlayerName(ctx, cardFirstCenterTopPos, player, Negative | Vertical, drawPosition);
    drawWhist(ctx, cardLastCenterBottomPos, player, Positive | Vertical) //
        or drawBid(ctx, cardLastCenterBottomPos, player, Positive | Vertical);
    drawSpeechBubble(ctx, {playerNameCenter.x, cardFirstLeftTopPos.y - VirtualH / 11.f}, playerId, drawPosition);
    // drawDebugVertLine(ctx, cardCenterX, "cardCenterX");
    // drawDebugDot(ctx, cardFirstCenterTopPos, "cardFirstCenterTopPos");
    // drawDebugDot(ctx, cardLastCenterBottomPos, "cardLastCenterBottomPos");
    // drawDebugVertLine(ctx, playerNameCenter.x, "playerNameCenterX");
}

auto drawRightHand(Context& ctx) -> void
{
    drawOpponentHand(ctx, DrawPosition::Right);
}

auto drawLeftHand(Context& ctx) -> void
{
    drawOpponentHand(ctx, DrawPosition::Left);
}

auto drawPlayedCards(Context& ctx) -> void
{
    if (std::empty(ctx.cardsOnTable)) { return; }
    const auto cardSpacing = CardWidth * 0.1f;
    const auto yOffset = CardHeight / 4.f;
    const auto centerPos = r::Vector2{VirtualW / 2.f - CardWidth / 2.f, VirtualH / 2.f - CardHeight / 2.f};
    const auto leftPlayPos = r::Vector2{centerPos.x - CardWidth - cardSpacing, centerPos.y - yOffset};
    const auto middlePlayPos = r::Vector2{centerPos.x, centerPos.y + yOffset};
    const auto rightPlayPos = r::Vector2{centerPos.x + CardWidth + cardSpacing, centerPos.y - yOffset};
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

[[nodiscard]] constexpr auto isBidDisabled(
    const std::string_view bid,
    const std::string_view myBid,
    const std::size_t rank,
    const std::size_t currentRank,
    const bool finalBid) noexcept -> bool
{
    const auto myFirstBid = std::empty(myBid);
    return (bid == PREF_PASS and finalBid) // Pass final bid
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
            and bid != PREF_PASS);
}

auto drawBiddingMenu(Context& ctx) -> void
{
    if (not ctx.bidding.isVisible) { return; }
    for (int r = 0; r < BidRows; ++r) {
        for (int c = 0; c < BidCols; ++c) {
            const auto& bid = BidTable[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
            if (std::empty(bid)) { continue; }
            const auto rank = bidRank(bid);
            const auto finalBid = std::size(ctx.discardedTalon) == 2;
            auto state = isBidDisabled(bid, ctx.myPlayer().bid, rank, ctx.bidding.rank, finalBid)
                ? GuiState{STATE_DISABLED}
                : GuiState{STATE_NORMAL};
            const auto pos = r::Vector2{
                BidOriginX + static_cast<float>(c) * (BidCellW + BidGap), //
                BidOriginY + static_cast<float>(r) * (BidCellH + BidGap)};
            const auto rect = r::Rectangle{pos, {BidCellW, BidCellH}};
            const auto clicked = std::invoke([&] {
                if ((state == STATE_DISABLED) or not r::Mouse::GetPosition().CheckCollision(rect)) { return false; }
                state = r::Mouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
                return r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
            });
            const auto borderColor = getGuiColor(BUTTON, GUI_PROPERTY(BORDER, state));
            const auto bgColor = getGuiColor(BUTTON, GUI_PROPERTY(BASE, state));
            const auto textColor = getGuiColor(BUTTON, GUI_PROPERTY(TEXT, state));
            rect.Draw(bgColor);
            rect.DrawLines(borderColor, getGuiButtonBorderWidth());
            auto text = std::string{ctx.localizeBid(bid)};
            auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), FontSpacing);
            const auto textX = rect.x + (rect.width - textSize.x) / 2.f;
            const auto textY = rect.y + (rect.height - textSize.y) / 2.f;
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
                ctx.fontM.DrawText(rankText, {textX, textY}, ctx.fontSizeM(), FontSpacing, textColor);
                const auto rankSize = ctx.fontM.MeasureText(rankText, ctx.fontSizeM(), FontSpacing);
                const auto suitColor = state == STATE_DISABLED ? r::Color::Red().Alpha(0.4f) : r::Color::Red();
                ctx.fontM.DrawText(
                    suitText.c_str(), {textX + rankSize.x, textY}, ctx.fontSizeM(), FontSpacing, suitColor);
            } else {
                ctx.fontM.DrawText(text, {textX, textY}, ctx.fontSizeM(), FontSpacing, textColor);
            }
            if (clicked and (state != STATE_DISABLED)) {
                ctx.bidding.bid = bid;
                ctx.myPlayer().bid = bid;
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

auto drawMenu(
    Context& ctx,
    const auto& allButtons,
    const bool isVisible,
    std::invocable<GameText> auto&& click,
    std::invocable<GameText> auto&& filterButton) -> void
{
    if (not isVisible) { return; }
    static constexpr auto cellW = VirtualW / 6.f;
    static constexpr auto cellH = cellW / 2.f;
    static constexpr auto gap = cellH / 10.f;
    auto buttons = allButtons | rv::filter(filterButton);
    const auto buttonsCount = rng::distance(buttons);
    const auto menuW = static_cast<float>(buttonsCount) * cellW + static_cast<float>(buttonsCount - 1) * gap;
    const auto originX = (VirtualW - menuW) / 2.f;
    const auto originY = (VirtualH - cellH) / 2.f;
    for (auto&& [i, buttonName] : buttons | rv::enumerate) {
        auto state = GuiState{STATE_NORMAL};
        const auto pos = r::Vector2{originX + static_cast<float>(i) * (cellW + gap), originY};
        const auto rect = r::Rectangle{pos, {cellW, cellH}};
        const auto clicked = std::invoke([&] {
            if (not r::Mouse::GetPosition().CheckCollision(rect)) { return false; }
            state = r::Mouse::IsButtonDown(MOUSE_LEFT_BUTTON) ? STATE_PRESSED : STATE_FOCUSED;
            return r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON);
        });
        const auto borderColor = getGuiColor(BUTTON, GUI_PROPERTY(BORDER, state));
        const auto bgColor = getGuiColor(BUTTON, GUI_PROPERTY(BASE, state));
        const auto textColor = getGuiColor(BUTTON, GUI_PROPERTY(TEXT, state));
        rect.Draw(bgColor);
        rect.DrawLines(borderColor, getGuiButtonBorderWidth());
        const auto text = ctx.localizeText(buttonName);
        const auto textSize = ctx.fontM.MeasureText(text, ctx.fontSizeM(), FontSpacing);
        const auto textX = rect.x + (rect.width - textSize.x) / 2.f;
        const auto textY = rect.y + (rect.height - textSize.y) / 2.f;
        ctx.fontM.DrawText(text, {textX, textY}, ctx.fontSizeM(), FontSpacing, textColor);
        if (clicked) { click(buttonName); }
    }
}

auto drawWhistingOrMiserMenu(Context& ctx) -> void
{
    const auto checkHalfWhist
        = [&](const GameText text) { return ctx.whisting.canHalfWhist or text != GameText::HalfWhist; };
    const auto click = [&](GameText buttonName) {
        ctx.whisting.isVisible = false;
        ctx.whisting.choice = localizeText(buttonName, GameLang::English);
        ctx.myPlayer().whistingChoice = ctx.whisting.choice;
        sendWhisting(ctx, ctx.whisting.choice);
    };
    if (ctx.bidding.bid == PREF_MISER_WT or ctx.bidding.bid == PREF_MISER) {
        drawMenu(ctx, MiserButtons, ctx.whisting.isVisible, click, checkHalfWhist);
    } else {
        drawMenu(ctx, WhistingButtons, ctx.whisting.isVisible, click, checkHalfWhist);
    }
}

auto drawHowToPlayMenu(Context& ctx) -> void
{
    drawMenu(
        ctx,
        HowToPlayButtons,
        ctx.howToPlay.isVisible,
        [&](auto buttonName) {
            ctx.howToPlay.isVisible = false;
            const auto choice = std::string{localizeText(buttonName, GameLang::English)};
            ctx.howToPlay.choice = choice;
            ctx.myPlayer().howToPlayChoice = choice;
            sendHowToPlay(ctx, choice);
        },
        []([[maybe_unused]] const GameText _) { return true; });
}

auto handleCardClick(Context& ctx, std::list<Card>& hand, std::invocable<Card&&> auto act) -> void
{
    if (not r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) { return; }
    const auto mousePos = r::Mouse::GetPosition();
    const auto hit = [&](Card& c) { return r::Rectangle{c.position, c.texture.GetSize()}.CheckCollision(mousePos); };
    const auto reversed = hand | rv::reverse;
    if (const auto rit = rng::find_if(reversed, hit); rit != rng::cend(reversed)) {
        const auto it = std::next(rit).base();
        if (not isCardPlayable(ctx, hand, *it)) {
            PREF_WARN("Can't play this card: {}", it->name);
            return;
        }
        const auto _ = gsl::finally([&] { hand.erase(it); });
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
        "Ё", "ё", "Ґ", "ґ", "Є", "є", "І", "і", "Ї", "ї", "è", "’", "—",
        SPADE, CLUB, HEART, DIAMOND, ARROW_RIGHT, ARROW_LEFT,
        PREF_TRICKS_01, PREF_TRICKS_02, PREF_TRICKS_03, PREF_TRICKS_04, PREF_TRICKS_05,
        PREF_TRICKS_06, PREF_TRICKS_07, PREF_TRICKS_08, PREF_TRICKS_09, PREF_TRICKS_10
    ); // clang-format on
    return rv::concat(ascii, cyrillic, extras) | rng::to_vector;
}

[[nodiscard]] auto makeAwesomeCodepoints() -> auto
{
    return makeCodepoints(ScoreSheetIcon, SettingsIcon, EnterFullScreenIcon, ExitFullScreenIcon, SpeechBubbleIcon);
}

auto setFont(const r::Font& font) -> void
{
    GuiSetFont(font);
    GuiSetStyle(DEFAULT, TEXT_SIZE, font.baseSize);
}

auto setDefaultFont(Context& ctx) -> void
{
    setFont(ctx.fontM);
}

auto withGuiFont(Context& ctx, const r::Font& font, std::invocable auto draw) -> void
{
    setFont(font);
    const auto _ = gsl::finally([&] { setDefaultFont(ctx); });
    draw();
}

auto loadFonts(Context& ctx) -> void
{
    static constexpr auto FontSizeS = static_cast<int>(VirtualH / 54.f);
    static constexpr auto FontSizeM = static_cast<int>(VirtualH / 30.f);
    static constexpr auto FontSizeL = static_cast<int>(VirtualH / 11.25f);
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
    const auto name = style | ToLower | ToString;
    if (name == "light" and not std::empty(name)) {
        // So that raygui doesn't unload the font
        GuiSetFont(ctx.initialFont);
        GuiLoadStyleDefault();
    } else {
        const auto stylePath = resources("styles", fmt::format("style_{}.rgs", name));
        GuiLoadStyle(stylePath.c_str());
    }
    GuiSetStyle(LISTVIEW, SCROLLBAR_WIDTH, ScrollBarWidth);
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
    const auto name = lang | ToLower | ToString;
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
    static constexpr auto buttonW = VirtualW / 26.f;
    static constexpr auto buttonH = buttonW;
    static constexpr auto gapBetweenButtons = BorderMargin / 5.f;
    const auto i = static_cast<float>(indexFromRight);
    const auto bounds = r::Rectangle{
        VirtualW - buttonW * i - BorderMargin - gapBetweenButtons * (i - 1),
        VirtualH - buttonH - BorderMargin,
        buttonW,
        buttonH};
    withGuiFont(ctx, ctx.fontAwesome, [&] {
        if (GuiButton(bounds, icon)) { onClick(); }
    });
}

auto drawFullScreenButton(Context& ctx) -> void
{
    drawToolbarButton(ctx, 1, ctx.window.IsFullscreen() ? ExitFullScreenIcon : EnterFullScreenIcon, [&] {
        ctx.window.ToggleFullscreen();
    });
}

auto drawSettingsButton(Context& ctx) -> void
{
    drawToolbarButton(ctx, 2, SettingsIcon, [&] { ctx.settingsMenu.isVisible = not ctx.settingsMenu.isVisible; });
}

auto drawSpeechBubbleButton(Context& ctx) -> void
{
    drawToolbarButton(
        ctx, 3, SpeechBubbleIcon, [&] { ctx.speechBubbleMenu.isVisible = not ctx.speechBubbleMenu.isVisible; });
}

auto drawScoreSheetButton(Context& ctx) -> void
{
    drawToolbarButton(ctx, 4, ScoreSheetIcon, [&] { ctx.scoreSheet.isVisible = not ctx.scoreSheet.isVisible; });
}

auto speechBubbleCooldown(Context& ctx) -> void
{
    emscripten_async_call(
        [](void* userData) {
            auto& ctx = toContext(userData);
            --ctx.speechBubbleMenu.cooldown;
            if (ctx.speechBubbleMenu.cooldown <= 0) {
                ctx.speechBubbleMenu.cooldown = SpeechBubbleMenu::SendCooldownInSec;
                ctx.speechBubbleMenu.isSendButtonActive = true;
                return;
            }
            speechBubbleCooldown(ctx);
        },
        toUserData(ctx),
        1'000); // 1s
}

auto drawSpeechBubbleMenu(Context& ctx) -> void
{
    if (not ctx.speechBubbleMenu.isVisible) { return; }
    if (r::Keyboard::IsKeyPressed(KEY_ESCAPE)) {
        ctx.speechBubbleMenu.isVisible = false;
        return;
    }
    static const auto phrases = pref::phrases()
        | rv::split('\n')
        | rv::transform([](auto&& rng) { return rng | ToString; })
        | rv::filter([](auto&& str) { return not std::empty(str) and not str.starts_with('#'); })
        | rng::to_vector;
    static const auto joinedPhrases = phrases | rv::intersperse(";") | rv::join | ToString;
    static auto scrollIndex = -1;
    static auto activeIndex = -1;
    static constexpr auto phrasesWindowBoxW = VirtualW / 5.f;
    static constexpr auto margin = VirtualH / 60.f;
    static constexpr auto phrasesListViewW = phrasesWindowBoxW - (margin * 2.f);
    static constexpr auto settingsButtomY = VirtualH / 1.9f; // approximately
    static const auto phrasesWindowBox = r::Vector2{
        VirtualW - BorderMargin - phrasesWindowBoxW, //
        settingsButtomY + margin};
    static const auto listViewEntryH = (getStyle(LISTVIEW, LIST_ITEMS_HEIGHT) + getStyle(LISTVIEW, LIST_ITEMS_SPACING));
    static constexpr auto phrasesToShow = 11.f;
    static const auto phrasesListViewH = phrasesToShow * listViewEntryH + getStyle(DEFAULT, BORDER_WIDTH) * 4.f;
    static const auto phrasesListView
        = r::Vector2{phrasesWindowBox.x + margin, phrasesWindowBox.y + RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT + margin};
    static const auto sendButton = r::Vector2{phrasesListView.x, phrasesListView.y + phrasesListViewH + margin};
    withGuiFont(ctx, ctx.fontS, [&] {
        const auto sendText = ctx.speechBubbleMenu.isSendButtonActive
            ? ctx.localizeText(GameText::Send)
            : fmt::format("{}", ctx.speechBubbleMenu.cooldown);
        const auto sendTextSize = measureGuiText(sendText);
        const auto sendButtonH = sendTextSize.y * 1.5f;
        static const auto phrasesWindowBoxH = (sendButton.y + sendButtonH + margin) - phrasesWindowBox.y;
        ctx.speechBubbleMenu.isVisible = not GuiWindowBox(
            {phrasesWindowBox.x, phrasesWindowBox.y, phrasesWindowBoxW, phrasesWindowBoxH},
            ctx.localizeText(GameText::Phrases).c_str());
        withGuiStyle(LISTVIEW, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT, [&] {
            GuiListView(
                {phrasesListView.x, phrasesListView.y, phrasesListViewW, phrasesListViewH},
                joinedPhrases.c_str(),
                &scrollIndex,
                &activeIndex);
            withGuiState(STATE_DISABLED, not ctx.speechBubbleMenu.isSendButtonActive, [&] {
                const auto sent
                    = GuiButton({sendButton.x, sendButton.y, phrasesListViewW, sendButtonH}, sendText.c_str());
                if (sent and activeIndex >= 0 and activeIndex < std::ssize(phrases)) {
                    const auto& phrase = phrases[static_cast<std::size_t>(activeIndex)];
                    sendSpeechBubble(ctx, phrase);
                    ctx.speechBubbleMenu.text.insert_or_assign(ctx.myPlayerId, std::move(phrase));
                    ctx.speechBubbleMenu.isSendButtonActive = false;
                    speechBubbleCooldown(ctx);
                }
            });
        });
    });
}

auto drawSettingsMenu(Context& ctx) -> void
{
    if (not ctx.settingsMenu.isVisible) { return; }
    if (r::Keyboard::IsKeyPressed(KEY_ESCAPE)) {
        ctx.settingsMenu.isVisible = false;
        return;
    }
    const auto lang = std::vector{
        ctx.localizeText(GameText::English),
        ctx.localizeText(GameText::Ukrainian),
        ctx.localizeText(GameText::Alternative),
    };
    const auto colorScheme = std::vector{
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
    const auto joinedLangs = lang | rv::intersperse(";") | rv::join | ToString;
    const auto joinedColorScheme = colorScheme | rv::intersperse(";") | rv::join | ToString;
    static constexpr auto margin = VirtualH / 60.f;
    static constexpr auto groupBoxW = SettingsMenu::SettingsWindowW - margin * 2.f;
    static constexpr auto listViewW = SettingsMenu::SettingsWindowW - margin * 4.f;
    static const auto listViewEntryH = (getStyle(LISTVIEW, LIST_ITEMS_HEIGHT) + getStyle(LISTVIEW, LIST_ITEMS_SPACING));
    static const auto langListViewH
        = static_cast<float>(std::size(lang)) * listViewEntryH + getStyle(LISTVIEW, BORDER_WIDTH) * 4.f;
    static const auto langGroupBoxH = langListViewH + margin * 2.f;
    const auto langGroupBox = r::Vector2{
        ctx.settingsMenu.settingsWindow.x + margin,
        ctx.settingsMenu.settingsWindow.y + RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT + margin * 1.5f};
    const auto langListView = r::Vector2{langGroupBox.x + margin, langGroupBox.y + margin};
    static const auto colorSchemeListViewH
        = static_cast<float>(std::size(colorScheme)) * listViewEntryH + getStyle(LISTVIEW, BORDER_WIDTH) * 4.f;
    static const auto colorSchemeGroupBoxH = colorSchemeListViewH + margin * 2.f;
    const auto colorSchemeGroupBox
        = r::Vector2{langListView.x - margin, langListView.y + langListViewH + margin * 2.5f};
    const auto colorSchemeListView = r::Vector2{colorSchemeGroupBox.x + margin, colorSchemeGroupBox.y + margin};
    static constexpr auto otherGroupBoxH = margin * 3.f;
    const auto otherGroupBox
        = r::Vector2{colorSchemeListView.x - margin, colorSchemeListView.y + colorSchemeListViewH + margin * 2.5f};
    const auto fpsCheckbox = r::Vector2{otherGroupBox.x + margin, otherGroupBox.y + margin};
    static const auto settingsWindowH = (fpsCheckbox.y + otherGroupBoxH) - ctx.settingsMenu.settingsWindow.y;
    withGuiFont(ctx, ctx.fontS, [&] {
        ctx.settingsMenu.isVisible = not GuiWindowBox(
            {ctx.settingsMenu.settingsWindow.x,
             ctx.settingsMenu.settingsWindow.y,
             SettingsMenu::SettingsWindowW,
             settingsWindowH},
            ctx.localizeText(GameText::Settings).c_str());
        GuiGroupBox(
            {langGroupBox.x, langGroupBox.y, groupBoxW, langGroupBoxH}, ctx.localizeText(GameText::Language).c_str());
        GuiListView(
            {langListView.x, langListView.y, listViewW, langListViewH},
            joinedLangs.c_str(),
            &ctx.settingsMenu.langIdScroll,
            &ctx.settingsMenu.langIdSelect);
        if (ctx.settingsMenu.langIdSelect >= 0
            and ctx.settingsMenu.langIdSelect < std::ssize(lang)
            and r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (const auto langToLoad = textToEnglish(lang[static_cast<std::size_t>(ctx.settingsMenu.langIdSelect)]);
                ctx.settingsMenu.loadedLang != langToLoad) {
                loadLang(ctx, langToLoad);
            }
        }
        GuiGroupBox(
            {colorSchemeGroupBox.x, colorSchemeGroupBox.y, groupBoxW, colorSchemeGroupBoxH},
            ctx.localizeText(GameText::ColorScheme).c_str());
        GuiListView(
            {colorSchemeListView.x, colorSchemeListView.y, listViewW, colorSchemeListViewH},
            joinedColorScheme.c_str(),
            &ctx.settingsMenu.colorSchemeIdScroll,
            &ctx.settingsMenu.colorSchemeIdSelect);
        if (ctx.settingsMenu.colorSchemeIdSelect >= 0
            and ctx.settingsMenu.colorSchemeIdSelect < std::ssize(colorScheme)
            and r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (const auto colorSchemeToLoad
                = textToEnglish(colorScheme[static_cast<std::size_t>(ctx.settingsMenu.colorSchemeIdSelect)]);
                ctx.settingsMenu.loadedColorScheme != colorSchemeToLoad) {
                loadColorScheme(ctx, colorSchemeToLoad);
            }
        }
        GuiGroupBox(
            {otherGroupBox.x, otherGroupBox.y, groupBoxW, otherGroupBoxH}, ctx.localizeText(GameText::Other).c_str());
        GuiCheckBox(
            {fpsCheckbox.x, fpsCheckbox.y, margin, margin},
            ctx.localizeText(GameText::ShowFps).c_str(),
            &ctx.settingsMenu.showPingAndFps);
    });
}

auto drawScoreSheet(Context& ctx) -> void
{
    if (not ctx.scoreSheet.isVisible) { return; }
    if (r::Keyboard::IsKeyPressed(KEY_ESCAPE)) {
        ctx.scoreSheet.isVisible = false;
        return;
    }
    static constexpr auto fSpace = FontSpacing;
    static constexpr auto rotateL = 90.f;
    static constexpr auto rotateR = 270.f;
    static const auto center = r::Vector2{VirtualW / 2.f, VirtualH / 2.f};
    static const auto sheetS = center.y * 1.45f;
    static const auto sheet = r::Vector2{center.x - sheetS / 2.f, center.y - sheetS / 2.f};
    static const auto radius = sheetS / 20.f;
    static const auto posm = 2.f / 9.f;
    static const auto poss = 5.f / 18.f;
    static const auto rl = r::Rectangle{sheet.x, sheet.y, sheetS, sheetS};
    static const auto rm
        = r::Rectangle{rl.x + sheetS * posm, rl.y, sheetS - sheetS * posm * 2.f, sheetS - sheetS * posm};
    static const auto rs
        = r::Rectangle{rl.x + sheetS * poss, rl.y, sheetS - sheetS * poss * 2.f, sheetS - sheetS * poss};
    static const auto fSize = ctx.fontSizeS();
    static const auto gap = fSize / 4.f;
    static const auto& font = ctx.fontS;
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
    center.DrawLine({rl.x + rl.width / 2.f, rl.y}, thick, borderColor);
    center.DrawLine({rl.x + rl.width / 2.f, rl.y}, thick, borderColor);
    center.DrawCircle(radius, borderColor);
    center.DrawCircle(radius - thick, sheetColor);
    r::Vector2{rl.x, center.y}.DrawLine({rm.x, center.y}, thick, borderColor);
    r::Vector2{rl.x + rl.width, center.y}.DrawLine({rm.x + rm.width, center.y}, thick, borderColor);
    r::Vector2{center.x, rl.y + rl.height}.DrawLine({center.x, rm.y + rm.height}, thick, borderColor);
    // TODO: don't hardcode `scoreTarget`
    static constexpr auto scoreTarget = "10";
    static const auto scoreTargetSize = ctx.fontM.MeasureText(scoreTarget, ctx.fontSizeM(), fSpace);
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
        const auto resultValue = std::invoke([&]() -> std::optional<std::tuple<std::string, r::Vector2, r::Color>> {
            const auto finalResult = calculateFinalResult(makeFinalScore(ctx.scoreSheet.score));
            if (not finalResult.contains(playerId)) { return std::nullopt; }
            const auto value = finalResult.at(playerId);
            const auto result = fmt::format("{}{}", value > 0 ? "+" : "", value);
            return std::tuple{
                result,
                ctx.fontM.MeasureText(result, ctx.fontSizeM(), fSpace),
                result.starts_with('-') ? r::Color::Red() : (result.starts_with('+') ? r::Color::DarkGreen() : c)};
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

[[nodiscard]] auto drawFps(const float y, const Font& font, const float fontSize, const float fontSpacing) -> float
{
    static constexpr auto borderX = VirtualW - BorderMargin;
    const auto fps = GetFPS();
    const auto fpsColor = std::invoke([&] {
        if (fps < 15) { return r::Color::Red(); }
        if (fps < 30) { return r::Color::Orange(); }
        return r::Color::Lime();
    });
    const auto fpsText = r::Text{fmt::format(" FPS {}", fps), fontSize, fpsColor, font, fontSpacing};
    const auto fpsX = borderX - fpsText.MeasureEx().x;
    fpsText.Draw({fpsX, y});
    return fpsX;
}

auto drawPing(Context& ctx, const r::Vector2& pos, const Font& font, const float fontSize, const float fontSpacing)
    -> void
{
    if (static_cast<int>(ctx.ping.rtt) == Ping::InvalidRtt) { return; }
    const auto ping = static_cast<int>(ctx.ping.rtt);
    const auto pingColor = std::invoke([&] {
        if (ping <= 200) { return r::Color::Lime(); }
        if (ping <= 600) { return r::Color::Orange(); }
        return r::Color::Red();
    });
    const auto pingText
        = r::Text{fmt::format("PING {} MS ", ping == 0 ? 1 : ping), fontSize, pingColor, font, fontSpacing};
    static const auto delimText = r::Text{"|", fontSize, getGuiColor(TEXT_COLOR_NORMAL), font, fontSpacing};
    const auto delimX = pos.x - delimText.MeasureEx().x;
    const auto pingX = delimX - pingText.MeasureEx().x;
    pingText.Draw({pingX, pos.y});
    delimText.Draw({delimX, pos.y});
}

auto drawPingAndFps(Context& ctx) -> void
{
    if (not ctx.settingsMenu.showPingAndFps) { return; }
    const auto font = GetFontDefault();
    static constexpr auto fontSize = 20.f;
    static constexpr auto fontSpacing = 2.f;
    static constexpr auto y = fontSize;
    const auto fpsX = drawFps(y, font, fontSize, fontSpacing);
    drawPing(ctx, {fpsX, y}, font, fontSize, fontSpacing);
}

auto handleMousePress(Context& ctx) -> void
{
    if (not r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) { return; }
    if (ctx.stage == GameStage::PLAYING) {
        if ((not isMyTurn(ctx) or isSomeonePlayingOnMyBehalf(ctx)) and not isMyTurnOnBehalfOfSomeone(ctx)) { return; }
        const auto& playerId
            = std::invoke([&] { return isMyTurn(ctx) ? ctx.myPlayerId : ctx.myPlayer().playsOnBehalfOf; });

        handleCardClick(ctx, ctx.player(playerId).hand, [&](Card&& card) {
            const auto name = card.name; // copy before move `card`
            if (std::empty(ctx.cardsOnTable)) { ctx.leadSuit = cardSuit(name); }
            ctx.cardsOnTable.insert_or_assign(playerId, std::move(card));
            sendPlayCard(ctx, playerId, name);
        });
        return;
    } else {
        if (not isMyTurn(ctx)) { return; }
    }
    if ((ctx.stage != GameStage::TALON_PICKING) or (std::size(ctx.discardedTalon) >= 2)) { return; }
    handleCardClick(ctx, ctx.myPlayer().hand, [&](Card&& card) { ctx.discardedTalon.push_back(card.name); });
    if (std::size(ctx.discardedTalon) != 2) { return; }
    if (const auto isSixSpade = ctx.bidding.rank == 0; isSixSpade) {
        ctx.bidding.rank = AllRanks;
    } else if (const auto isNotAllPassed = ctx.bidding.rank != AllRanks; isNotAllPassed) {
        --ctx.bidding.rank;
    }
    ctx.bidding.isVisible = true;
}

// Prevent interaction with other buttons during settings menu movement
auto updateSettingsMenuPosition(Context& ctx) -> void
{
    auto& settingsMenu = ctx.settingsMenu;
    if (not settingsMenu.isVisible) { return; }
    const auto mouse = r::Mouse::GetPosition();
    if (r::Mouse::IsButtonPressed(MOUSE_LEFT_BUTTON)) {
        const auto settingsTitleBar = r::Rectangle{
            settingsMenu.settingsWindow.x,
            settingsMenu.settingsWindow.y,
            SettingsMenu::SettingsWindowW
                - (RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT + RAYGUI_WINDOWBOX_CLOSEBUTTON_HEIGHT) * 0.5f,
            RAYGUI_WINDOWBOX_STATUSBAR_HEIGHT};
        if (mouse.CheckCollision(settingsTitleBar)) {
            GuiLock();
            settingsMenu.moving = true;
            settingsMenu.grabOffset
                = r::Vector2{mouse.x - settingsMenu.settingsWindow.x, mouse.y - settingsMenu.settingsWindow.y};
        }
    }
    if (not settingsMenu.moving) { return; }
    settingsMenu.settingsWindow.x = mouse.x - settingsMenu.grabOffset.x;
    settingsMenu.settingsWindow.y = mouse.y - settingsMenu.grabOffset.y;
    if (r::Mouse::IsButtonReleased(MOUSE_LEFT_BUTTON)) { settingsMenu.moving = false; };
}

auto updateDrawFrame(void* userData) -> void
{
    assert(userData);
    auto& ctx = toContext(userData);
    if (std::empty(ctx.myPlayerId)) { ctx.myPlayerId = loadPlayerIdFromLocalStorage(); }
    if (GuiIsLocked() and not ctx.settingsMenu.moving) { GuiUnlock(); }
    handleMousePress(ctx);
    updateSettingsMenuPosition(ctx);

    ctx.target.BeginMode();
    ctx.window.ClearBackground(getGuiColor(BACKGROUND_COLOR));
    drawBiddingMenu(ctx);
    drawWhistingOrMiserMenu(ctx);
    drawHowToPlayMenu(ctx);
    if (not ctx.hasEnteredName) {
        drawGameplayScreen(ctx);
        drawEnterNameScreen(ctx);
        drawSettingsButton(ctx);
        drawFullScreenButton(ctx);
        drawPingAndFps(ctx);
        drawSettingsMenu(ctx);
        ctx.target.EndMode();
        goto end;
    }
    if (ctx.areAllPlayersJoined()) {
        drawMyHand(ctx);
        drawRightHand(ctx);
        drawLeftHand(ctx);
        drawPlayedCards(ctx);
        drawScoreSheetButton(ctx);
        drawScoreSheet(ctx);
        drawSpeechBubbleButton(ctx);
        drawSpeechBubbleMenu(ctx);
    } else {
        drawConnectedPlayersPanel(ctx);
    }
    drawSettingsButton(ctx);
    drawFullScreenButton(ctx);
    drawPingAndFps(ctx);
    drawSettingsMenu(ctx);
    ctx.target.EndMode();
end:
    ctx.window.BeginDrawing();
    ctx.window.ClearBackground(getGuiColor(BACKGROUND_COLOR));
    ctx.target.GetTexture().Draw(
        r::Rectangle{
            0,
            0,
            static_cast<float>(ctx.target.GetTexture().width),
            -static_cast<float>(ctx.target.GetTexture().height)}, // flip vertically
        r::Rectangle{ctx.offsetX, ctx.offsetY, VirtualW * ctx.scale, VirtualH * ctx.scale});
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
            pref::updateWindowSize(pref::toContext(userData));
            return EM_FALSE;
        });
    static constexpr auto fps = 60;
    emscripten_set_main_loop_arg(pref::updateDrawFrame, pref::toUserData(ctx), fps, true);
    return 0;
}
