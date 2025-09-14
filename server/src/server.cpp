#include "server.hpp"

#include "proto/pref.pb.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <fmt/ranges.h>
#include <range/v3/all.hpp>

#include <array>
#include <expected>
#include <gsl/assert>
#include <gsl/gsl>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace pref {
namespace {

// NOLINTBEGIN(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)

using TcpAcceptor = net::ip::tcp::acceptor;

auto trickFinished(const Context::Players& players) -> Awaitable<>;
auto dealFinished(Context& ctx) -> Awaitable<>;

[[nodiscard]] auto makeMessage(const beast::flat_buffer& buffer) -> std::expected<Message, std::string>
{
    auto result = Message{};
    if (not result.ParseFromArray(buffer.data().data(), gsl::narrow<int>(buffer.size()))) {
        static constexpr auto error = "failed to make Message from array";
        WARN_VAR(error);
        return std::unexpected{error};
    }
    return result;
}

// TODO: use C++26 Reflection
// clang-format off
template<typename T>
           [[nodiscard]] constexpr auto methodName               () noexcept -> std::string_view { return "Unknown";       }
template<> [[nodiscard]] constexpr auto methodName<Bidding>      () noexcept -> std::string_view { return "Bidding";       }
template<> [[nodiscard]] constexpr auto methodName<DealCards>    () noexcept -> std::string_view { return "DealCards";     }
template<> [[nodiscard]] constexpr auto methodName<DiscardTalon> () noexcept -> std::string_view { return "DiscardTalon";  }
template<> [[nodiscard]] constexpr auto methodName<JoinRequest>  () noexcept -> std::string_view { return "JoinRequest";   }
template<> [[nodiscard]] constexpr auto methodName<JoinResponse> () noexcept -> std::string_view { return "JoinResponse";  }
template<> [[nodiscard]] constexpr auto methodName<PlayCard>     () noexcept -> std::string_view { return "PlayCard";      }
template<> [[nodiscard]] constexpr auto methodName<PlayerJoined> () noexcept -> std::string_view { return "PlayerJoined";  }
template<> [[nodiscard]] constexpr auto methodName<PlayerLeft>   () noexcept -> std::string_view { return "PlayerLeft";    }
template<> [[nodiscard]] constexpr auto methodName<PlayerTurn>   () noexcept -> std::string_view { return "PlayerTurn";    }
template<> [[nodiscard]] constexpr auto methodName<DealFinished> () noexcept -> std::string_view { return "DealFinished"; }
template<> [[nodiscard]] constexpr auto methodName<TrickFinished>() noexcept -> std::string_view { return "TrickFinished"; }
template<> [[nodiscard]] constexpr auto methodName<Whisting>     () noexcept -> std::string_view { return "Whisting";      }
// clang-format on

template<typename Method>
[[nodiscard]] auto makeMessage(const Method& method) -> Message
{
    auto result = Message{};
    result.set_method(std::string{methodName<Method>()});
    result.set_payload(method.SerializeAsString());
    return result;
}

template<typename Method>
[[nodiscard]] auto makeMethod(const Message& msg) -> std::expected<Method, std::string>
{
    auto result = Method{};
    if (not result.ParseFromString(msg.payload())) {
        const auto error = fmt::format("failed to make {} from string", methodName<Method>());
        WARN_VAR(error);
        return std::unexpected{error};
    }
    return result;
}

[[maybe_unused]] auto generateUuid() -> Player::Id
{
    return boost::uuids::to_string(std::invoke(boost::uuids::random_generator{}));
}

auto setWhoseTurn(Context& ctx, const auto& it) -> void
{
    ctx.whoseTurnIt = it;
    const auto& [playerId, player] = *ctx.whoseTurnIt;
    INFO_VAR(playerId, player.name);
}

auto setForehandId(Context& ctx) -> void
{
    ctx.forehandId = ctx.whoseTurnId();
    INFO_VAR(ctx.forehandId);
}

auto resetWhoseTurn(Context& ctx) -> void
{
    assert((std::size(ctx.players) == NumberOfPlayers) and "all players joined");
    setWhoseTurn(ctx, std::cbegin(ctx.players));
}

auto advanceWhoseTurn(Context& ctx) -> void
{
    assert((std::size(ctx.players) == NumberOfPlayers) and "all players joined");
    if (const auto nextTurnIt = std::next(ctx.whoseTurnIt); nextTurnIt != std::cend(ctx.players)) {
        setWhoseTurn(ctx, nextTurnIt);
        return;
    }
    resetWhoseTurn(ctx);
}

auto forehandsTurn(Context& ctx) -> void
{
    assert(ctx.players.contains(ctx.forehandId) and "forehand player exists");
    setWhoseTurn(ctx, ctx.players.find(ctx.forehandId));
}

auto advanceWhoseTurn(Context& ctx, const std::string_view stage) -> void
{
    INFO_VAR(stage);
    advanceWhoseTurn(ctx);
    if (not rng::contains(std::initializer_list{"Bidding", "TalonPicking"}, stage)) {
        return;
    }
    while (ctx.player(ctx.whoseTurnId()).bid == PASS) {
        advanceWhoseTurn(ctx);
    }
}

// TODO: combine with advanceWhoseTurn?
auto setNextDealTurn(Context& ctx) -> void
{
    assert((std::size(ctx.players) == NumberOfPlayers) and "all players joined");
    assert(ctx.players.contains(ctx.forehandId) and "forehand player exists");
    auto nextForehandIt = std::invoke([&] {
        if (auto nextIt = std::next(ctx.players.find(ctx.forehandId)); nextIt != std::end(ctx.players)) {
            return nextIt;
        }
        return std::begin(ctx.players);
    });
    setWhoseTurn(ctx, nextForehandIt);
    setForehandId(ctx);
}

[[nodiscard]] auto finishTrick(Context& ctx) -> Player::Id
{
    const auto winnerId = finishTrick(ctx.trick, ctx.trump);
    const auto& winnerName = ctx.playerName(winnerId);
    const auto tricksTaken = ++ctx.player(winnerId).tricksTaken;
    INFO_VAR(winnerId, winnerName, tricksTaken);
    ctx.trick.clear();
    return winnerId;
}

[[nodiscard]] auto makeDealFinished(const ScoreSheet& scoreSheet) -> DealFinished
{
    auto result = DealFinished{};
    for (const auto& [playerId, score] : scoreSheet) {
        auto& data = (*result.mutable_score_sheet())[playerId];
        for (const auto value : score.dump) {
            data.mutable_dump()->add_values(value);
        }
        for (const auto value : score.pool) {
            data.mutable_pool()->add_values(value);
        }
        for (const auto& [whistPlayerId, values] : score.whists) {
            for (const auto value : values) {
                (*data.mutable_whists())[whistPlayerId].add_values(value);
            }
        }
    }
    return result;
}

[[nodiscard]] auto isNewPlayer(const Player::Id& playerId, Context::Players& players) -> bool
{
    return std::empty(playerId) or not players.contains(playerId);
}

auto sendToOne(const Message& msg, const std::shared_ptr<Stream>& ws) -> Awaitable<sys::error_code>
{
    if (not ws->is_open()) {
        co_return net::error::not_connected;
    }
    const auto data = msg.SerializeAsString();
    const auto [error, _] = co_await ws->async_write(net::buffer(data), net::as_tuple);
    co_return error;
}

auto sendToMany(const Message& msg, const std::vector<std::shared_ptr<Stream>>& wss) -> Awaitable<>
{
    for (const auto& ws : wss) {
        if (const auto error = co_await sendToOne(msg, ws)) {
            PREF_WARN("{}: failed to send message", VAR(error));
        }
    }
}

auto sendToAll(const Message& msg, const Context::Players& players) -> Awaitable<>
{
    const auto wss = players | rv::values | rv::transform(&Player::ws) | rng::to_vector;
    co_await sendToMany(msg, wss);
}

auto sendToAllExcept(const Message& msg, const Context::Players& players, const Player::Id& excludedId) -> Awaitable<>
{
    static constexpr auto GetPlayerId = &Context::Players::value_type::first;
    const auto wss = players //
        | rv::filter(notEqualTo(excludedId), GetPlayerId) //
        | rv::values //
        | rv::transform(&Player::ws) //
        | rng::to_vector;
    co_await sendToMany(msg, wss);
}

auto joinPlayer(const std::shared_ptr<Stream>& ws, Context::Players& players, PlayerSession& session) -> void
{
    session.playerId = generateUuid();
    INFO_VAR(session.playerId, session.playerName, session.id);
    players.emplace(session.playerId, Player{session.playerId, session.playerName, session.id, ws});
}

auto prepareNewSession(const Player::Id& playerId, Context::Players& players, PlayerSession& session) -> Awaitable<>
{
    INFO_VAR(playerId, session.playerId, session.playerName, session.id);
    assert(players.contains(playerId) and "player exists");
    auto& player = players[playerId];
    session.id = ++player.sessionId;
    session.playerId = playerId;
    session.playerName = player.name; // keep the first connected player's name
    if (player.reconnectTimer) {
        player.reconnectTimer->cancel();
    }
    if (not player.ws->is_open()) {
        co_return;
    }
    if (const auto [error] = co_await player.ws->async_close(
            web::close_reason(web::close_code::policy_error, "Another tab connected"), net::as_tuple);
        error) {
        PREF_WARN("{}: failed to close ws", VAR(error));
    }
}

auto replaceStream(const Player::Id& playerId, Context::Players& players, const std::shared_ptr<Stream>& ws) -> void
{
    assert(players.contains(playerId) and "player exists");
    auto& player = players[playerId];
    player.ws = ws;
}

auto reconnectPlayer(
    const std::shared_ptr<Stream>& ws, const Player::Id& playerId, Context::Players& players, PlayerSession& session)
    -> Awaitable<>
{
    co_await prepareNewSession(playerId, players, session);
    replaceStream(playerId, players, ws);
    INFO_VAR(session.playerName, session.playerId, session.id);
}

[[nodiscard]] auto makePlayerJoined(const PlayerSession& session) -> PlayerJoined
{
    INFO_VAR(session.playerId, session.playerName);
    auto result = PlayerJoined{};
    result.set_player_id(session.playerId);
    result.set_player_name(session.playerName);
    return result;
}

[[nodiscard]] auto makeJoinResponse(const Context::Players& players, const PlayerSession& session) -> JoinResponse
{
    auto result = JoinResponse{};
    result.set_player_id(session.playerId);
    for (const auto& [id, player] : players) {
        auto p = result.add_players();
        p->set_player_id(id);
        p->set_player_name(player.name);
    }
    return result;
}

auto joined(Context& ctx, Message msg, const std::shared_ptr<Stream> ws) -> Awaitable<PlayerSession>
{
    const auto joinRequest = makeMethod<JoinRequest>(msg);
    if (not joinRequest) {
        PREF_WARN("makeMessage error: {}", joinRequest.error());
        co_return PlayerSession{};
    }
    auto session = PlayerSession{};
    const auto& playerId = joinRequest->player_id();
    session.playerName = joinRequest->player_name();
    const auto address = ws->next_layer().socket().remote_endpoint().address().to_string();
    INFO_VAR(playerId, session.playerName, address); // TODO: hide address
    if (isNewPlayer(playerId, ctx.players)) {
        joinPlayer(ws, ctx.players, session);
    } else {
        co_await reconnectPlayer(ws, playerId, ctx.players, session);
    }
    if (const auto error = co_await sendToOne(makeMessage(makeJoinResponse(ctx.players, session)), ws)) {
        PREF_WARN("{}: failed to send JoinResponse", VAR(error));
    }
    if (playerId == session.playerId) {
        co_return session;
    }
    co_await sendToAllExcept(makeMessage(makePlayerJoined(session)), ctx.players, session.playerId);
    co_return session;
}

auto addCardToHand(Context& ctx, const Player::Id& playerId, const std::string& card) -> void
{
    INFO_VAR(playerId, card);
    assert(not ctx.player(playerId).hand.contains(card) and "card doesn't exists");
    ctx.player(playerId).hand.insert(card);
}

auto removeCardFromHand(Context& ctx, const Player::Id& playerId, const std::string& card) -> void
{
    INFO_VAR(playerId, card);
    assert(ctx.player(playerId).hand.contains(card) and "card exists");
    ctx.player(playerId).hand.erase(card);
}

[[nodiscard]] auto makePlayerTurn(Context& ctx, const std::string_view stage) -> PlayerTurn
{
    auto result = PlayerTurn{};
    const auto& playerId = ctx.whoseTurnId();
    result.set_player_id(playerId);
    result.set_stage(std::string{stage});
    if (stage == "TalonPicking") {
        assert((std::size(ctx.talon) == 2) and "talon is two cards");
        for (const auto& card : ctx.talon) {
            addCardToHand(ctx, playerId, card);
            result.add_talon(card);
        }
    }
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerId, playerName, stage);
    return result;
}

auto playerTurn(Context& ctx, const std::string_view stage) -> Awaitable<>
{
    co_await sendToAll(makeMessage(makePlayerTurn(ctx, stage)), ctx.players);
}

[[nodiscard]] auto makeDealCards(const auto& hand) -> DealCards
{
    auto result = DealCards{};
    for (const auto& card : hand) {
        *result.add_cards() = card;
    }
    return result;
}

auto dealCards(Context& ctx) -> Awaitable<>
{
    const auto suits = std::array{SPADES, DIAMONDS, CLUBS, HEARTS};
    const auto ranks = std::array{SEVEN, EIGHT, NINE, TEN, JACK, QUEEN, KING, ACE};
    const auto toCard = [](const auto& card) {
        const auto& [rank, suit] = card;
        return fmt::format("{}" _OF_ "{}", rank, suit);
    };
    const auto deck = rv::cartesian_product(ranks, suits) //
        | rv::transform(toCard) //
        | rng::to_vector //
        | rng::actions::shuffle(std::mt19937{std::invoke(std::random_device{})});
    const auto chunks = deck | rv::chunk(10);
    const auto hands = chunks | rv::take(NumberOfPlayers) | rng::to_vector;
    ctx.talon = chunks | rv::drop(NumberOfPlayers) | rv::join | rng::to_vector;
    assert((std::size(ctx.talon) == 2) and "talon is two cards");
    assert((std::size(ctx.players) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (const auto& [playerId, hand] : rv::zip(ctx.players | rv::keys, hands)) {
        ctx.player(playerId).hand = hand | rng::to<Hand>;
    }
    INFO_VAR(ctx.talon, hands);
    const auto wss = ctx.players | rv::values | rv::transform(&Player::ws) | rng::to_vector;
    assert((std::size(wss) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (const auto& [ws, hand] : rv::zip(wss, hands)) {
        co_await sendToOne(makeMessage(makeDealCards(hand)), ws);
    }
}

auto cardPlayed(Context& ctx, const Message& msg) -> Awaitable<>
{
    const auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) {
        co_return;
    }
    const auto& playerId = playCard->player_id();
    const auto& card = playCard->card();
    const auto& playerName = ctx.playerName(playerId);
    removeCardFromHand(ctx, playerId, card);
    ctx.trick.emplace_back(playerId, card);
    INFO_VAR(playerName, playerId, card);
    co_await sendToAllExcept(msg, ctx.players, playerId);
    if (const auto isTrickFinished = (std::size(ctx.trick) == 3); isTrickFinished) {
        const auto winnerId = finishTrick(ctx);
        const auto& winnerName = ctx.playerName(winnerId);
        INFO_VAR(winnerId, winnerName);
        co_await trickFinished(ctx.players);
        if (const auto isDealFinished = rng::all_of(ctx.players | rv::values, &Hand::empty, &Player::hand);
            isDealFinished) {
            co_await dealFinished(ctx);
            co_await dealCards(ctx);
            setNextDealTurn(ctx);
            co_await playerTurn(ctx, "Bidding");
            co_return;
        } else {
            setWhoseTurn(ctx, ctx.players.find(winnerId));
        }
    } else {
        advanceWhoseTurn(ctx);
    }
    co_await playerTurn(ctx, "Playing");
}

[[nodiscard]] auto talonDiscarded(Context& ctx, const Message& msg) -> std::pair<Player::Id, std::string>
{
    const auto discardTalon = makeMethod<DiscardTalon>(msg);
    if (not discardTalon) {
        // TODO: throw?
        return {};
    }
    const auto& playerId = discardTalon->player_id();
    const auto& bid = discardTalon->bid();
    const auto& discardedCards = discardTalon->cards();
    for (const auto& card : discardedCards) {
        removeCardFromHand(ctx, playerId, card);
    }
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, discardedCards, bid);
    return {playerId, bid};
}

[[nodiscard]] auto makePlayerLeft(const Player::Id& playerId) -> PlayerLeft
{
    INFO_VAR(playerId);
    auto result = PlayerLeft{};
    result.set_player_id(playerId);
    return result;
}

auto disconnected(Context& ctx, const Player::Id playerId) -> Awaitable<>
{
    INFO_VAR(playerId);
    auto& player = ctx.player(playerId);
    if (not player.reconnectTimer) {
        player.reconnectTimer.emplace(co_await net::this_coro::executor);
    }
    player.reconnectTimer->expires_after(10s);
    if (const auto [error] = co_await player.reconnectTimer->async_wait(net::as_tuple); error) {
        if (error != net::error::operation_aborted) {
            WARN_VAR(error);
        }
        co_return;
    }
    assert(ctx.players.contains(playerId) and "player exists");
    PREF_INFO("removed {} after timeout", VAR(playerId));
    ctx.players.erase(playerId);
    co_await sendToAll(makeMessage(makePlayerLeft(playerId)), ctx.players);
}

auto bid(Context& ctx, const Message& msg) -> Awaitable<>
{
    const auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) {
        co_return;
    }
    const auto& playerId = bidding->player_id();
    const auto& bid = bidding->bid();
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, bid);
    ctx.player(playerId).bid = bid;
    co_await sendToAllExcept(msg, ctx.players, playerId);
}

auto whistChoice(Context& ctx, const Message& msg) -> Awaitable<>
{
    auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) {
        co_return;
    }
    const auto& playerId = whisting->player_id();
    const auto& choice = whisting->choice();
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, choice);
    ctx.player(playerId).whistChoice = choice;
    co_await sendToAllExcept(msg, ctx.players, playerId);
}

[[nodiscard]] auto makeBidding(Context& ctx, const Player::Id& playerId, const std::string& bid) -> Bidding
{
    auto result = Bidding{};
    result.set_player_id(playerId);
    result.set_bid(bid);
    ctx.trump = std::string{getTrump(bid)};
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, bid, ctx.trump);
    return result;
}

auto finalBid(Context& ctx, const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    co_await sendToAllExcept(makeMessage(makeBidding(ctx, playerId, bid)), ctx.players, playerId);
}

auto resetGameState(Context& ctx) -> void
{
    ctx.talon.clear();
    ctx.trick.clear();
    ctx.trump.clear();
    for (auto&& [_, p] : ctx.players) {
        p.hand.clear();
        p.bid.clear();
        p.whistChoice.clear();
        p.tricksTaken = 0;
    }
}

[[nodiscard]] constexpr auto makeContractLevel(const std::string_view contract) noexcept -> ContractLevel
{ // clang-format off
    using enum ContractLevel;
    if (contract.starts_with("6"))  { return Six;   }
    if (contract.starts_with("7"))  { return Seven; }
    if (contract.starts_with("8"))  { return Eight; }
    if (contract.starts_with("9"))  { return Nine;  }
    if (contract.starts_with("10")) { return Ten;   }
    if (contract.starts_with("Mi")) { return Miser; }
    std::unreachable();
} // clang-format on

// ┌────────┬─────┐
// │Contract│Price│
// ├────────┼─────┤
// │    6   │  2  │
// ├────────┼─────┤
// │    7   │  4  │
// ├────────┼─────┤
// │    8   │  6  │
// ├────────┼─────┤
// │    9   │  8  │
// ├────────┼─────┤
// │   10   │ 10  │
// ├────────┼─────┤
// │  MISER │ 10  │
// └────────┴─────┘
[[nodiscard]] constexpr auto contractPrice(const ContractLevel level) noexcept -> int
{ // clang-format off
    using enum ContractLevel;
    switch (level) {
        case Six  : return  2;
        case Seven: return  4;
        case Eight: return  6;
        case Nine : return  8;
        case Ten  : return 10;
        case Miser: return 10;
    };
    std::unreachable();
} // clang-format on

// ┌────────┬──────┐
// │Contract│Tricks│
// ├────────┼──────┤
// │    6   │   6  │
// ├────────┼──────┤
// │    7   │   7  │
// ├────────┼──────┤
// │    8   │   8  │
// ├────────┼──────┤
// │    9   │   9  │
// ├────────┼──────┤
// │   10   │  10  │
// ├────────┼──────┤
// │  MISER │   0  │
// └────────┴──────┘
[[nodiscard]] constexpr auto obligatoryTricksForDeclarer(const ContractLevel level) noexcept -> int
{ // clang-format off
    using enum ContractLevel;
    switch (level) {
        case Six  : return  6;
        case Seven: return  7;
        case Eight: return  8;
        case Nine : return  9;
        case Ten  : return 10;
        case Miser: return 10;
    };
    std::unreachable();
} // clang-format on

// ┌────────┬──────┐
// │Contract│Tricks│
// ├────────┼──────┤
// │    6   │   4  │
// ├────────┼──────┤
// │    7   │   2  │
// ├────────┼──────┤
// │    8   │   1  │
// ├────────┼──────┤
// │    9   │   1  │
// ├────────┼──────┤
// │   10   │   1  │
// ├────────┼──────┤
// │  MISER │   0  │
// └────────┴──────┘
[[nodiscard]] constexpr auto obligatoryTricksForBothWhisters(const ContractLevel level) noexcept -> int
{ // clang-format off
    using enum ContractLevel;
    switch (level) {
        case Six  : return 4;
        case Seven: return 2;
        case Eight: return 1;
        case Nine : return 1;
        case Ten  : return 1;
        case Miser: return 0;
    };
    std::unreachable();
} // clang-format on

[[nodiscard]] constexpr auto obligatoryTricksForOneWhisters(const ContractLevel level) noexcept -> int
{ // clang-format off
    using enum ContractLevel;
    switch (level) {
        case Six  : return 2;
        case Seven: return 1;
        case Eight: return 1;
        case Nine : return 1;
        case Ten  : return 1;
        case Miser: return 0;
    };
    std::unreachable();
} // clang-format on

[[nodiscard]] constexpr auto makeWhistChoise(const std::string_view contract) noexcept -> WhistChoise
{ // clang-format off
    if (contract == WHIST)      { return WhistChoise::Whist;     }
    if (contract == PASS)       { return WhistChoise::Pass;      }
    if (contract == HALF_WHIST) { return WhistChoise::HalfWhist; }
    std::unreachable();
} // clang-format on

[[nodiscard]] auto findDeclarerId(const Context::Players& players) -> std::expected<Player::Id, std::string>
{
    for (const auto& [playerId, player] : players) {
        if (player.bid != "Pass") {
            return playerId;
        }
    }
    return std::unexpected{"could not find declarerId"};
}

[[nodiscard]] auto findWhisterIds(const Context::Players& players) -> std::vector<Player::Id>
{
    auto result = std::vector<Player::Id>{};
    for (const auto& [playerId, player] : players) {
        if (not std::empty(player.whistChoice)) {
            result.push_back(playerId);
        }
    }
    return result;
}

auto updateScoreSheetForDeal(Context& ctx) -> void
{ // clang-format off
  // NOLINTNEXTLINE(bugprone-unused-return-value)
    auto _ = findDeclarerId(ctx.players).transform([&](const Player::Id& declarerId) {
        const auto& declarerPlayer = ctx.players.at(declarerId);
        const auto declarer
            = Declarer{.id = declarerId, .contractLevel = makeContractLevel(declarerPlayer.bid)};
        auto whisters = std::vector<Whister>{};
        for (const auto& id : findWhisterIds(ctx.players)) {
            const auto& whisterPlayer = ctx.players.at(id);
            whisters.emplace_back(id, makeWhistChoise(whisterPlayer.whistChoice), whisterPlayer.tricksTaken);
        }
        for (const auto& [id, entry] : calculateScoreEntry(declarer, whisters)) {
            ctx.scoreSheet[id].dump.push_back(entry.dump);
            ctx.scoreSheet[id].pool.push_back(entry.pool);
            ctx.scoreSheet[id].whists[declarerId].push_back(entry.whist);
        }
    }).or_else([](const std::string& error) {
        WARN_VAR(error);
        return std::expected<void, std::string>(std::unexpect, error);
    });
} // clang-format on

auto dealFinished(Context& ctx) -> Awaitable<>
{
    PREF_INFO();
    updateScoreSheetForDeal(ctx);
    co_await sendToAll(makeMessage(makeDealFinished(ctx.scoreSheet)), ctx.players);
    resetGameState(ctx);
    // TODO: Reset taken tricks on the client side upon receiving `DealFinished`
    // so we don't have to send a `TrickFinished` with zero tricks
    co_await trickFinished(ctx.players);
}

[[nodiscard]] auto makeTrickFinished(const Context::Players& players) -> TrickFinished
{
    PREF_INFO();
    auto result = TrickFinished{};
    for (const auto& [playerId, player] : players) {
        auto tricks = result.add_tricks();
        tricks->set_player_id(playerId);
        tricks->set_taken(player.tricksTaken);
    }
    return result;
}

auto trickFinished(const Context::Players& players) -> Awaitable<>
{
    co_await sendToAll(makeMessage(makeTrickFinished(players)), players);
}

[[nodiscard]] auto stageGame(Context& ctx) -> std::string_view
{
    const auto bids = ctx.players | rv::values | rv::transform(&Player::bid);
    if (std::size(bids) == NumberOfPlayers) {
        if (rng::count(bids, PASS) == NumberOfPlayers) {
            // TODO: implement Pass Game
            PREF_ERROR("The Pass Game is not implemented");
            return "Playing";
        }
        if ((rng::count_if(bids, std::bind_front(std::not_equal_to{}, PASS)) == 1) and (rng::count(bids, PASS) == 2)) {
            return "TalonPicking";
        }
    }
    return "Bidding";
}

auto launchSession(Context& ctx, const std::shared_ptr<Stream> ws) -> Awaitable<>
{
    PREF_INFO();
    // TODO: What if we received a text?
    ws->binary(true);
    ws->set_option(web::stream_base::timeout::suggested(beast::role_type::server));
    ws->set_option(web::stream_base::decorator([](web::response_type& res) {
        res.set(beast::http::field::server, std::string{BOOST_BEAST_VERSION_STRING} + " preferans-server");
    }));

    co_await ws->async_accept();
    auto session = PlayerSession{};

    while (true) {
        auto buffer = beast::flat_buffer{};
        if (const auto [error, _] = co_await ws->async_read(buffer, net::as_tuple); error) {
            if (error == web::error::closed //
                or error == net::error::not_connected //
                or error == net::error::connection_reset //
                or error == net::error::eof) {
                PREF_INFO("{}: disconnected: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else if (error == net::error::operation_aborted) {
                PREF_WARN("{}: read: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else {
                ERROR_VAR(session.playerName, session.playerId, error);
                // TODO(olkozlo): maybe throw to not handle a disconnection?
            }
            break;
        }
        const auto msg = makeMessage(buffer);
        if (not msg) {
            continue;
        }
        if (msg->method() == "JoinRequest") {
            session = co_await joined(ctx, *msg, ws);
            if (std::size(ctx.players) == NumberOfPlayers) {
                co_await dealCards(ctx);
                resetWhoseTurn(ctx);
                setForehandId(ctx);
                co_await playerTurn(ctx, "Bidding");
            }
        } else if (msg->method() == "Bidding") {
            co_await bid(ctx, *msg);
            const auto stage = stageGame(ctx);
            advanceWhoseTurn(ctx, stage);
            co_await playerTurn(ctx, stage);
        } else if (msg->method() == "Whisting") {
            co_await whistChoice(ctx, *msg);
            if (ctx.isWhistingDone()) {
                forehandsTurn(ctx);
                co_await playerTurn(ctx, "Playing");
            } else {
                advanceWhoseTurn(ctx);
                co_await playerTurn(ctx, "Whisting");
            }
        } else if (msg->method() == "PlayCard") {
            co_await cardPlayed(ctx, *msg);
        } else if (msg->method() == "DiscardTalon") {
            const auto [playerId, bid] = talonDiscarded(ctx, *msg);
            co_await finalBid(ctx, playerId, bid);
            // TODO: is the player turn advance correctly?
            advanceWhoseTurn(ctx);
            co_await playerTurn(ctx, "Whisting");
        } else {
            PREF_WARN("error: unknown method: {}", msg->method());
            continue;
        }
    }
    if (std::empty(session.playerId) or not ctx.players.contains(session.playerId)) {
        // TODO: Can `playerId` ever be set but not present in `players`? If not, replace with `assert()`.
        PREF_WARN("error: empty or unknown {}", VAR(session.playerId));
        co_return;
    }
    auto& player = ctx.player(session.playerId);
    if (session.id != player.sessionId) {
        PREF_INFO("{} reconnected with {} => {}", VAR(session.playerId), VAR(session.id), player.sessionId);
        // TODO: send the game state to the reconnected player
        co_return;
    }
    PREF_WARN("disconnected {}, waiting for reconnection", VAR(session.playerId));
    net::co_spawn(co_await net::this_coro::executor, disconnected(ctx, session.playerId), Detached("disconnected"));
}

// NOLINTEND(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)
} // namespace

Player::Player(Id aId, Name aName, PlayerSession::Id aSessionId, const std::shared_ptr<Stream>& aWs)
    : id{std::move(aId)}
    , name{std::move(aName)}
    , sessionId{aSessionId}
    , ws{aWs}
{
}

auto Context::whoseTurnId() const -> const Player::Id&
{
    return whoseTurnIt->first;
}

auto Context::player(const Player::Id& playerId) const -> Player&
{
    assert(players.contains(playerId) and "player exists");
    return players[playerId];
}

auto Context::playerName(const Player::Id& playerId) const -> const std::string&
{
    return player(playerId).name;
}

auto Context::isWhistingDone() const -> bool
{
    return 2 == rng::count_if(players | rv::values, rng::not_fn(&std::string::empty), &Player::whistChoice);
}

auto beats(const Beat beat) -> bool
{
    const auto [candidate, best, leadSuit, trump] = beat;
    const auto candidateSuit = cardSuit(candidate);
    const auto bestSuit = cardSuit(best);
    if (candidateSuit == bestSuit) {
        return rankValue(cardRank(candidate)) > rankValue(cardRank(best));
    }
    if (not std::empty(trump) and (candidateSuit == trump) and (bestSuit != trump)) {
        return true;
    }
    if (not std::empty(trump) and (candidateSuit != trump) and (bestSuit == trump)) {
        return false;
    }
    return candidateSuit == leadSuit;
}

// return winnderId
auto finishTrick(const std::vector<PlayedCard>& trick, const std::string_view trump) -> Player::Id
{
    assert(std::size(trick) == NumberOfPlayers and "all players played cards");
    const auto& firstCard = trick.front();
    const auto leadSuit = cardSuit(firstCard.name); // clang-format off
    return rng::accumulate(trick, firstCard, [&](const PlayedCard& best, const PlayedCard& candidate) {
        return beats({candidate.name, best.name, leadSuit, trump}) ? candidate : best; // NOLINT
    }).playerId; // clang-format on
}

[[nodiscard]] auto calculateScoreEntry(const Declarer& declarer, const std::vector<Whister>& whisters)
    -> std::map<Player::Id, ScoreEntry>
{
    assert(std::size(whisters) == 2uz);

    const auto& w1 = whisters[0];
    const auto& w2 = whisters[1];

    assert(not std::empty(declarer.id) and not std::empty(w1.id) and not std::empty(w2.id));
    assert(declarer.id != w1.id and declarer.id != w2.id and w1.id != w2.id);
    assert(0 <= w1.tricksTaken and w1.tricksTaken <= 10);
    assert(0 <= w2.tricksTaken and w2.tricksTaken <= 10);

    static constexpr auto totalTricksPerDeal = 10;

    const auto contractPrice = pref::contractPrice(declarer.contractLevel);
    const auto declarerReqTricks = obligatoryTricksForDeclarer(declarer.contractLevel);
    const auto whistersReqTricksTotal = obligatoryTricksForBothWhisters(declarer.contractLevel);

    const auto whistersTakenTricks = w1.tricksTaken + w2.tricksTaken;
    assert(0 <= whistersTakenTricks and whistersTakenTricks <= totalTricksPerDeal);

    const auto declarerTakenTricks = totalTricksPerDeal - whistersTakenTricks;
    const auto declarerSucceeded = declarerTakenTricks >= declarerReqTricks;

    const auto deficit = [](auto need, auto got) { return std::max(0, need - got); };

    const auto declarerFailedTricks = deficit(declarerReqTricks, declarerTakenTricks);
    const auto whistersFailedTricks = deficit(whistersReqTricksTotal, whistersTakenTricks);
    const auto bothSaidWhist = rng::all_of(whisters, [](const auto& w) { return w.choise == WhistChoise::Whist; });
    const auto singleWhisterReqTricks = obligatoryTricksForOneWhisters(declarer.contractLevel);

    const auto makeDeclarerScore = [&] {
        auto s = ScoreEntry{};
        if (declarerSucceeded) {
            s.pool = contractPrice;
        } else {
            s.dump = declarerFailedTricks * contractPrice;
        }
        return s;
    };

    const auto makeWhisterScore = [&](const Whister& w) {
        auto s = ScoreEntry{};
        if (w.choise == WhistChoise::Whist) {
            s.whist += w.tricksTaken * contractPrice;
            if (whistersFailedTricks > 0) {
                const auto needForThisWhister = bothSaidWhist ? singleWhisterReqTricks : whistersReqTricksTotal;
                const auto underForThisWhister = deficit(needForThisWhister, w.tricksTaken);
                s.dump += underForThisWhister * contractPrice;
            }
        }
        s.whist += declarerFailedTricks * contractPrice;
        return s;
    };

    return {
        {declarer.id, makeDeclarerScore()},
        {w1.id, makeWhisterScore(w1)},
        {w2.id, makeWhisterScore(w2)},
    };
}

auto acceptConnectionAndLaunchSession(Context& ctx, const net::ip::tcp::endpoint endpoint) -> Awaitable<>
{
    PREF_INFO();
    const auto ex = co_await net::this_coro::executor;
    auto acceptor = TcpAcceptor{ex, endpoint};
    while (true) {
        net::co_spawn(
            ex,
            launchSession(ctx, std::make_shared<Stream>(co_await acceptor.async_accept())),
            Detached("launchSession"));
    }
}

} // namespace pref
