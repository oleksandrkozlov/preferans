#include "server.hpp"

#include "common/common.hpp"
#include "proto/pref.pb.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <fmt/ranges.h>
#include <range/v3/all.hpp>

#include <array>
#include <cassert>
#include <expected>
#include <functional>
#include <gsl/assert>
#include <gsl/gsl>
#include <iterator>
#include <utility>

namespace pref {
namespace {

// NOLINTBEGIN(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)

using TcpAcceptor = net::ip::tcp::acceptor;

auto sendTrickFinished(const Context::Players& players) -> Awaitable<>;
auto dealFinished(Context& ctx) -> Awaitable<>;

[[nodiscard]] auto makeMessage(const beast::flat_buffer& buffer) -> std::optional<Message>
{
    if (auto result = Message{}; result.ParseFromArray(buffer.data().data(), gsl::narrow<int>(buffer.size()))) {
        return result;
    }
    PREF_WARN("error: failed to make Message from array");
    return {};
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
    PREF_INFO();
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
    if (not rng::contains(std::initializer_list{"Bidding", "TalonPicking"}, stage)) { return; }
    while (ctx.player(ctx.whoseTurnId()).bid == PREF_PASS) { advanceWhoseTurn(ctx); }
}

// TODO: combine with advanceWhoseTurn?
auto setNextDealTurn(Context& ctx) -> void
{
    assert((std::size(ctx.players) == NumberOfPlayers) and "all players joined");
    assert(ctx.players.contains(ctx.forehandId) and "forehand player exists");
    const auto nextIt = std::next(ctx.players.find(ctx.forehandId));
    setWhoseTurn(ctx, nextIt != std::cend(ctx.players) ? nextIt : std::cbegin(ctx.players));
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

[[nodiscard]] constexpr auto makeContractLevel(const std::string_view contract) noexcept -> ContractLevel
{
    using enum ContractLevel;
    if (contract.starts_with("6")) { return Six; }
    if (contract.starts_with("7")) { return Seven; }
    if (contract.starts_with("8")) { return Eight; }
    if (contract.starts_with("9")) { return Nine; }
    if (contract.starts_with("10")) { return Ten; }
    if (contract.starts_with("Mi")) { return Miser; }
    std::unreachable();
}

[[nodiscard]] constexpr auto contractPrice(const ContractLevel level) noexcept -> int
{ // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
    using enum ContractLevel;
    switch (level) {
    case Six: return 2;
    case Seven: return 4;
    case Eight: return 6;
    case Nine: return 8;
    case Ten: [[fallthrough]];
    case Miser: return 10;
    };
    std::unreachable();
}

[[nodiscard]] constexpr auto declarerReqTricks(const ContractLevel level) noexcept -> int
{
    using enum ContractLevel;
    switch (level) {
    case Six: return 6;
    case Seven: return 7;
    case Eight: return 8;
    case Nine: return 9;
    case Ten: return 10;
    case Miser: return 0;
    };
    std::unreachable();
} // NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

[[nodiscard]] constexpr auto twoWhistersReqTricks(const ContractLevel level) noexcept -> int
{
    using enum ContractLevel;
    switch (level) {
    case Six: return 4;
    case Seven: return 2;
    case Eight:
    case Nine: [[fallthrough]];
    case Ten: return 1;
    case Miser: return 0;
    };
    std::unreachable();
}

[[nodiscard]] constexpr auto oneWhisterReqTricks(const ContractLevel level) noexcept -> int
{
    using enum ContractLevel;
    switch (level) {
    case Six: return 2;
    case Seven:
    case Eight:
    case Nine: [[fallthrough]];
    case Ten: return 1;
    case Miser: return 0;
    };
    std::unreachable();
}

[[nodiscard]] constexpr auto makeWhistChoise(const std::string_view choise) noexcept -> WhistChoise
{
    using enum WhistChoise;
    if (choise == PREF_WHIST) { return Whist; }
    if (choise == PREF_CATCH) { return Whist; }
    if (choise == PREF_PASS) { return Pass; }
    if (choise == PREF_TRUST) { return Pass; }
    if (choise == PREF_HALF_WHIST) { return HalfWhist; }
    if (choise == PREF_PASS_WHIST) { return PassWhist; }
    if (choise == PREF_PASS_PASS) { return PassPass; }
    std::unreachable();
}

[[nodiscard]] auto findDeclarerId(const Context::Players& players) -> std::expected<Player::Id, std::string>
{
    for (const auto& [playerId, player] : players) {
        if (player.bid != PREF_PASS) { return playerId; }
    }
    return std::unexpected{"could not find declarerId"};
}

[[nodiscard]] auto getDeclarer(Context::Players& players) -> Player&
{
    const auto declarerId = findDeclarerId(players);
    assert(declarerId.has_value() and players.contains(*declarerId) and "declarer exists");
    return players[*declarerId];
}

[[nodiscard]] auto findWhisterIds(const Context::Players& players) -> std::vector<Player::Id>
{
    return players
        | rv::values
        | rv::filter(equalTo(PREF_PASS), &Player::bid)
        | rv::transform(&Player::id)
        | rng::to_vector;
}

[[nodiscard]] auto getWhisters(Context::Players& players) -> std::array<std::reference_wrapper<Player>, 2>
{
    const auto whisterIds = findWhisterIds(players);
    assert(std::size(whisterIds) == 2 and "there are two whisters");
    const auto& w0 = whisterIds[0];
    const auto& w1 = whisterIds[1];
    assert(players.contains(w0) and players.contains(w1) and "whisters exist");
    return {players[w0], players[w1]};
}

[[nodiscard]] auto isNewPlayer(const Player::Id& playerId, Context::Players& players) -> bool
{
    return std::empty(playerId) or not players.contains(playerId);
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
    if (player.reconnectTimer) { player.reconnectTimer->cancel(); }
    if (not player.ws->is_open()) { co_return; }
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

auto sendToOne(const Message& msg, const std::shared_ptr<Stream>& ws) -> Awaitable<sys::error_code>
{
    if (not ws->is_open()) { co_return net::error::not_connected; }
    const auto data = msg.SerializeAsString();
    const auto [error, _] = co_await ws->async_write(net::buffer(data), net::as_tuple);
    co_return error;
}

auto sendToMany(const Message msg, const std::vector<std::shared_ptr<Stream>>& wss) -> Awaitable<>
{
    for (const auto& ws : wss) {
        if (const auto error = co_await sendToOne(msg, ws)) { PREF_WARN("{}: failed to send message", VAR(error)); }
    }
}

auto sendToAll(Message msg, const Context::Players& players) -> Awaitable<>
{
    const auto wss = players | rv::values | rv::transform(&Player::ws) | rng::to_vector;
    co_await sendToMany(std::move(msg), wss);
}

auto sendToAllExcept(Message msg, const Context::Players& players, const Player::Id& excludedId) -> Awaitable<>
{
    static constexpr auto GetPlayerId = &Context::Players::value_type::first;
    const auto wss = players
        | rv::filter(notEqualTo(excludedId), GetPlayerId)
        | rv::values
        | rv::transform(&Player::ws)
        | rng::to_vector;
    co_await sendToMany(std::move(msg), wss);
}

auto forwardMessage(Context& ctx, Message msg, const Player::Id& playerId) -> Awaitable<>
{
    return sendToAllExcept(std::move(msg), ctx.players, playerId);
}

auto addCardToHand(Context& ctx, const Player::Id& playerId, const std::string& card) -> void
{
    INFO_VAR(playerId, card);
    assert(not ctx.player(playerId).hand.contains(card) and "card doesn't exists");
    ctx.player(playerId).hand.insert(card);
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

[[nodiscard]] auto makePlayerLeft(const Player::Id& playerId) -> PlayerLeft
{
    INFO_VAR(playerId);
    auto result = PlayerLeft{};
    result.set_player_id(playerId);
    return result;
}

[[nodiscard]] auto makeDealCards(const auto& hand) -> DealCards
{
    auto result = DealCards{};
    for (const auto& card : hand) { *result.add_cards() = card; }
    return result;
}

[[nodiscard]] auto makePlayerTurn(Context& ctx, const std::string_view stage) -> PlayerTurn
{
    auto result = PlayerTurn{};
    const auto& playerId = ctx.whoseTurnId();
    result.set_player_id(playerId);
    result.set_stage(std::string{stage});
    result.set_can_half_whist(false); // default
    if (stage == "TalonPicking") {
        assert((std::size(ctx.talon) == 2) and "talon is two cards");
        for (const auto& card : ctx.talon) {
            addCardToHand(ctx, playerId, card);
            result.add_talon(card);
        }
    } else if (stage == "Whisting") {
        if (const auto contractLevel = makeContractLevel(getDeclarer(ctx.players).bid);
            contractLevel == ContractLevel::Six or contractLevel == ContractLevel::Seven) {
            const auto canHalfWhist = [&](const Player& self, const Player& other) {
                return self.id == playerId and std::empty(self.whistChoice) and other.whistChoice == PREF_PASS;
            };
            if (const auto& [w0, w1] = getWhisters(ctx.players);
                canHalfWhist(w0.get(), w1.get()) or canHalfWhist(w1.get(), w0.get())) {
                result.set_can_half_whist(true);
            }
        }
    }
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerId, playerName, stage);
    return result;
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

[[nodiscard]] auto makeWhisting(const Player::Id& playerId, const std::string& choice) -> Whisting
{
    auto result = Whisting{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return result;
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

[[nodiscard]] auto makeDealFinished(const ScoreSheet& scoreSheet) -> DealFinished
{
    auto result = DealFinished{};
    for (const auto& [playerId, score] : scoreSheet) {
        auto& data = (*result.mutable_score_sheet())[playerId];
        for (const auto value : score.dump) { data.mutable_dump()->add_values(value); }
        for (const auto value : score.pool) { data.mutable_pool()->add_values(value); }
        for (const auto& [whistPlayerId, values] : score.whists) {
            for (const auto value : values) { (*data.mutable_whists())[whistPlayerId].add_values(value); }
        }
    }
    return result;
}

auto sendPlayerJoined(Context& ctx, const PlayerSession& session) -> Awaitable<>
{
    return sendToAllExcept(makeMessage(makePlayerJoined(session)), ctx.players, session.playerId);
}

auto sendJoinResponse(Context& ctx, const PlayerSession& session, const std::shared_ptr<Stream>& ws) -> Awaitable<>
{
    if (const auto error = co_await sendToOne(makeMessage(makeJoinResponse(ctx.players, session)), ws)) {
        PREF_WARN("{}: failed to send JoinResponse", VAR(error));
    }
}

auto sendPlayerLeft(Context& ctx, const Player::Id& playerId) -> Awaitable<>
{
    return sendToAll(makeMessage(makePlayerLeft(playerId)), ctx.players);
}

auto sendDealCards(const auto& hand, const std::shared_ptr<Stream>& ws) -> Awaitable<>
{
    if (const auto error = co_await sendToOne(makeMessage(makeDealCards(hand)), ws)) {
        PREF_WARN("{}: failed to send DealCards", VAR(error));
    }
}

auto sendPlayerTurn(Context& ctx, const std::string_view stage) -> Awaitable<>
{
    return sendToAll(makeMessage(makePlayerTurn(ctx, stage)), ctx.players);
}

auto sendBidding(Context& ctx, const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    return sendToAllExcept(makeMessage(makeBidding(ctx, playerId, bid)), ctx.players, playerId);
}

auto sendWhisting(const Context::Players& players, const Player::Id& playerId, const std::string& choise) -> Awaitable<>
{
    return sendToAll(makeMessage(makeWhisting(playerId, choise)), players);
}

auto sendTrickFinished(const Context::Players& players) -> Awaitable<>
{
    return sendToAll(makeMessage(makeTrickFinished(players)), players);
}

auto sendDealFinished(Context& ctx) -> Awaitable<>
{
    return sendToAll(makeMessage(makeDealFinished(ctx.scoreSheet)), ctx.players);
}

auto removeCardFromHand(Context& ctx, const Player::Id& playerId, const std::string& card) -> void
{
    INFO_VAR(playerId, card);
    assert(ctx.player(playerId).hand.contains(card) and "card exists");
    ctx.player(playerId).hand.erase(card);
}

auto dealCards(Context& ctx) -> Awaitable<>
{
    const auto suits = std::array{SPADES, DIAMONDS, CLUBS, HEARTS};
    const auto ranks = std::array{SEVEN, EIGHT, NINE, TEN, JACK, QUEEN, KING, ACE};
    const auto toCard = [](const auto& card) {
        const auto& [rank, suit] = card;
        return fmt::format("{}" PREF_OF_ "{}", rank, suit);
    };
    const auto deck = rv::cartesian_product(ranks, suits)
        | rv::transform(toCard)
        | rng::to_vector
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
    for (const auto& [ws, hand] : rv::zip(wss, hands)) { co_await sendDealCards(hand, ws); }
}

auto disconnected(Context& ctx, const Player::Id playerId) -> Awaitable<>
{
    INFO_VAR(playerId);
    auto& player = ctx.player(playerId);
    if (not player.reconnectTimer) { player.reconnectTimer.emplace(co_await net::this_coro::executor); }
    player.reconnectTimer->expires_after(10s);
    if (const auto [error] = co_await player.reconnectTimer->async_wait(net::as_tuple); error) {
        if (error != net::error::operation_aborted) { WARN_VAR(error); }
        co_return;
    }
    assert(ctx.players.contains(playerId) and "player exists");
    PREF_INFO("removed {} after timeout", VAR(playerId));
    ctx.players.erase(playerId);
    co_await sendPlayerLeft(ctx, playerId);
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

auto updateScoreSheetForDeal(Context& ctx) -> void
{ // clang-format off
  // NOLINTNEXTLINE(bugprone-unused-return-value)
    auto _ = findDeclarerId(ctx.players).transform([&](const Player::Id& declarerId) {
        const auto& declarerPlayer = ctx.players.at(declarerId);
        const auto declarer
            = Declarer{.id = declarerId, .contractLevel = makeContractLevel(declarerPlayer.bid), .tricksTaken = declarerPlayer.tricksTaken};
        auto whisters = std::vector<Whister>{};
        for (const auto& whisterPlayer : getWhisters(ctx.players)) {
            whisters.emplace_back(whisterPlayer.get().id, makeWhistChoise(whisterPlayer.get().whistChoice), whisterPlayer.get().tricksTaken);
        }
        for (const auto& [id, entry] : calculateDealScore(declarer, whisters)) {
            ctx.scoreSheet[id].dump.push_back(entry.dump);
            ctx.scoreSheet[id].pool.push_back(entry.pool);
            if (id != declarerId) { ctx.scoreSheet[id].whists[declarerId].push_back(entry.whist); }
        }
    }).or_else([](const std::string& error) {
        WARN_VAR(error);
        return std::expected<void, std::string>(std::unexpect, error);
    });
} // clang-format on

auto dealFinished(Context& ctx) -> Awaitable<>
{
    updateScoreSheetForDeal(ctx);
    for (const auto& [playerId, score] : ctx.scoreSheet) {
        INFO_VAR(playerId, score.dump, score.pool);
        for (const auto& [id, whists] : score.whists) { PREF_INFO("whists: {} -> {}", whists, id); }
    }
    co_await sendDealFinished(ctx);
    resetGameState(ctx);
}

[[nodiscard]] auto stageGame(Context& ctx) -> std::string_view
{
    const auto bids = ctx.players | rv::values | rv::transform(&Player::bid);
    if (std::size(bids) == NumberOfPlayers) {
        if (rng::count(bids, PREF_PASS) == NumberOfPlayers) {
            // TODO: implement Pass Game
            PREF_ERROR("The Pass Game is not implemented");
            return "Playing";
        }
        if ((rng::count_if(bids, notEqualTo(PREF_PASS)) == 1) and (rng::count(bids, PREF_PASS) == 2)) {
            return "TalonPicking";
        }
    }
    return "Bidding";
}

auto handleJoinRequest(Context& ctx, const Message msg, const std::shared_ptr<Stream>& ws) -> Awaitable<PlayerSession>
{
    const auto joinRequest = makeMethod<JoinRequest>(msg);
    if (not joinRequest) { co_return PlayerSession{}; }
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
    co_await sendJoinResponse(ctx, session, ws);
    if (playerId == session.playerId) { co_return session; }
    co_await sendPlayerJoined(ctx, session);
    if (std::size(ctx.players) == NumberOfPlayers) {
        co_await dealCards(ctx);
        resetWhoseTurn(ctx);
        setForehandId(ctx);
        co_await sendPlayerTurn(ctx, "Bidding");
    }
    co_return session;
}

auto handleBidding(Context& ctx, Message msg) -> Awaitable<>
{
    const auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) { co_return; }
    const auto& playerId = bidding->player_id();
    const auto& bid = bidding->bid();
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, bid);
    ctx.player(playerId).bid = bid;
    co_await forwardMessage(ctx, std::move(msg), playerId);
    const auto stage = stageGame(ctx);
    advanceWhoseTurn(ctx, stage);
    co_await sendPlayerTurn(ctx, stage);
}

auto handleDiscardTalon(Context& ctx, const Message& msg) -> Awaitable<>
{
    const auto discardTalon = makeMethod<DiscardTalon>(msg);
    if (not discardTalon) { co_return; }
    const auto& playerId = discardTalon->player_id();
    const auto& bid = discardTalon->bid();
    const auto& discardedCards = discardTalon->cards();
    for (const auto& card : discardedCards) { removeCardFromHand(ctx, playerId, card); }
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, discardedCards, bid);
    co_await sendBidding(ctx, playerId, bid); // final bid
    advanceWhoseTurn(ctx);
    co_await sendPlayerTurn(ctx, "Whisting");
}

auto finishDeal(Context& ctx) -> Awaitable<>
{
    co_await dealFinished(ctx);
    co_await dealCards(ctx);
    setNextDealTurn(ctx);
    co_await sendPlayerTurn(ctx, "Bidding");
}

auto updateDeclarerTakenTricks(Context& ctx) -> void
{
    auto& declarer = getDeclarer(ctx.players);
    declarer.tricksTaken = declarerReqTricks(makeContractLevel(declarer.bid));
}

[[nodiscard]] auto playerByWhistChoise(Context::Players& players, const WhistChoise choise) -> Player&
{
    auto values = players | rv::values;
    auto it = rng::find_if(values, [&](const auto& player) { return makeWhistChoise(player.whistChoice) == choise; });
    assert(it != rng::end(values));
    return *it;
}

auto handleWhisting(Context& ctx, Message msg) -> Awaitable<>
{
    using enum WhistChoise;
    auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { co_return; }
    const auto& playerId = whisting->player_id();
    const auto& choice = whisting->choice();
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, choice);
    ctx.player(playerId).whistChoice += choice; // Pass + Whist, Pass + Pass, etc.
    co_await forwardMessage(ctx, std::move(msg), playerId);
    if (ctx.isHalfWhistAfterPass()) {
        advanceWhoseTurn(ctx); // skip declarer
        advanceWhoseTurn(ctx);
        co_return co_await sendPlayerTurn(ctx, "Whisting");
    }
    if (ctx.isWhistAfterHalfWhist()) {
        {
            auto& whister = playerByWhistChoise(ctx.players, HalfWhist);
            auto pass = std::string{PREF_PASS};
            co_await sendWhisting(ctx.players, whister.id, pass);
            whister.whistChoice = std::move(pass);
        }
        {
            auto& whister = playerByWhistChoise(ctx.players, PassWhist);
            // no need to sendWhisting(), already whist
            whister.whistChoice = PREF_WHIST;
        }
        forehandsTurn(ctx);
        co_return co_await sendPlayerTurn(ctx, "Playing");
    }
    if (ctx.isPassAfterHalfWhist()) {
        playerByWhistChoise(ctx.players, PassPass).whistChoice = PREF_PASS;
        updateDeclarerTakenTricks(ctx);
        co_return co_await finishDeal(ctx);
    }
    if (ctx.areWhistersPass()) {
        updateDeclarerTakenTricks(ctx);
        co_return co_await finishDeal(ctx);
    }
    if (ctx.areWhistersPassOrWhist()) {
        forehandsTurn(ctx);
        co_return co_await sendPlayerTurn(ctx, "Playing");
    }
    advanceWhoseTurn(ctx);
    co_return co_await sendPlayerTurn(ctx, "Whisting");
}

auto handlePlayCard(Context& ctx, Message msg) -> Awaitable<>
{
    const auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) { co_return; }
    const auto& playerId = playCard->player_id();
    const auto& card = playCard->card();
    const auto& playerName = ctx.playerName(playerId);
    removeCardFromHand(ctx, playerId, card);
    ctx.trick.emplace_back(playerId, card);
    INFO_VAR(playerName, playerId, card);
    co_await forwardMessage(ctx, std::move(msg), playerId);
    if (const auto isTrickFinished = (std::size(ctx.trick) == 3); isTrickFinished) {
        const auto winnerId = finishTrick(ctx);
        const auto& winnerName = ctx.playerName(winnerId);
        INFO_VAR(winnerId, winnerName);
        co_await sendTrickFinished(ctx.players);
        if (const auto isDealFinished = rng::all_of(ctx.players | rv::values, &Hand::empty, &Player::hand);
            isDealFinished) {
            co_return co_await finishDeal(ctx);
        }
        setWhoseTurn(ctx, ctx.players.find(winnerId));
    } else {
        advanceWhoseTurn(ctx);
    }
    co_await sendPlayerTurn(ctx, "Playing");
}

auto handleLog(const Message& msg) -> void
{
    if (const auto log = makeMethod<Log>(msg); log) {
        PREF_INFO("[client] {}, playerId: {}", log->text(), log->player_id());
    }
}

auto dispatchMessage(
    Context& ctx, PlayerSession& session, const std::shared_ptr<Stream>& ws, std::optional<Message> msg) -> Awaitable<>
{ // clang-format off
    if (not msg) { co_return; }
    const auto& method = msg->method();
    if (method == "JoinRequest") { session = co_await handleJoinRequest(ctx, *msg, ws); co_return; }
    if (method == "Bidding") { co_return co_await handleBidding(ctx, std::move(*msg)); }
    if (method == "DiscardTalon") { co_return co_await handleDiscardTalon(ctx, *msg); }
    if (method == "Whisting") { co_return co_await handleWhisting(ctx, std::move(*msg)); }
    if (method == "PlayCard") { co_return co_await handlePlayCard(ctx, std::move(*msg)); }
    if (method == "Log") { handleLog(*msg); co_return; }
    PREF_WARN("error: unknown {}", VAR(method));
} // clang-format on

auto launchSession(Context& ctx, const std::shared_ptr<Stream> ws) -> Awaitable<>
{
    PREF_INFO();
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
            if (error == web::error::closed
                or error == net::error::not_connected
                or error == net::error::connection_reset
                or error == net::error::eof) {
                PREF_INFO("{}: disconnected: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else if (error == net::error::operation_aborted) {
                PREF_WARN("{}: read: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else {
                ERROR_VAR(session.playerName, session.playerId, error);
                // TODO: maybe throw to not handle a disconnection?
            }
            break;
        }
        co_await dispatchMessage(ctx, session, ws, makeMessage(buffer));
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

auto Context::playerName(const Player::Id& playerId) const -> const Player::Name&
{
    return player(playerId).name;
}

auto Context::countWhistChoice(const std::initializer_list<WhistChoise> choices) const -> std::ptrdiff_t
{
    return rng::count_if(
        players
            | rv::values
            | rv::transform(&Player::whistChoice)
            | rv::filter(rng::not_fn(&std::string::empty))
            | rv::transform(&makeWhistChoise),
        [&](const WhistChoise choise) { return rng::contains(choices, choise); });
}

auto Context::isHalfWhistAfterPass() const -> bool
{
    return 1 == countWhistChoice({WhistChoise::Pass}) //
        and 1 == countWhistChoice({WhistChoise::HalfWhist});
}

auto Context::isWhistAfterHalfWhist() const -> bool
{
    return 1 == countWhistChoice({WhistChoise::PassWhist});
}

auto Context::isPassAfterHalfWhist() const -> bool
{
    return 1 == countWhistChoice({WhistChoise::PassPass});
}

auto Context::areWhistersPassOrWhist() const -> bool
{
    return 2 == countWhistChoice({WhistChoise::Pass, WhistChoise::Whist});
}

auto Context::areWhistersPass() const -> bool
{
    return 2 == countWhistChoice({WhistChoise::Pass});
}

auto beats(const Beat beat) -> bool
{
    const auto [candidate, best, leadSuit, trump] = beat;
    const auto candidateSuit = cardSuit(candidate);
    const auto bestSuit = cardSuit(best);
    if (candidateSuit == bestSuit) { return rankValue(cardRank(candidate)) > rankValue(cardRank(best)); }
    if (not std::empty(trump) and (candidateSuit == trump) and (bestSuit != trump)) { return true; }
    if (not std::empty(trump) and (candidateSuit != trump) and (bestSuit == trump)) { return false; }
    return candidateSuit == leadSuit;
}

// return winnderId
auto finishTrick(const std::vector<PlayedCard>& trick, const std::string_view trump) -> Player::Id
{
    assert(std::size(trick) == NumberOfPlayers and "all players played cards");
    const auto& firstCard = trick.front();
    const auto leadSuit = cardSuit(firstCard.name); // clang-format off
        return rng::accumulate(trick, firstCard, [&](const PlayedCard& best, const PlayedCard& candidate) {
            return beats({candidate.name, best.name, leadSuit, trump}) ? candidate : best; // NOLINT(modernize-use-designated-initializers)
        }).playerId; // clang-format on
}

// NOLINTBEGIN(readability-function-cognitive-complexity, hicpp-uppercase-literal-suffix)
[[nodiscard]] auto calculateDealScore(const Declarer& declarer, const std::vector<Whister>& whisters) -> DealScore
{
    assert(std::size(whisters) == 2);

    const auto& w1 = whisters[0];
    const auto& w2 = whisters[1];
    static constexpr auto totalTricksPerDeal = 10;

    assert(not std::empty(declarer.id) and not std::empty(w1.id) and not std::empty(w2.id));
    assert(declarer.id != w1.id and declarer.id != w2.id and w1.id != w2.id);
    assert(0 <= w1.tricksTaken and w1.tricksTaken <= totalTricksPerDeal);
    assert(0 <= w2.tricksTaken and w2.tricksTaken <= totalTricksPerDeal);
    assert(0 <= declarer.tricksTaken and declarer.tricksTaken <= totalTricksPerDeal);

    const auto contractPrice = pref::contractPrice(declarer.contractLevel);
    const auto declarerReqTricks = pref::declarerReqTricks(declarer.contractLevel);
    const auto twoWhistersReqTricks = pref::twoWhistersReqTricks(declarer.contractLevel);

    const auto whistersTakenTricks = w1.tricksTaken + w2.tricksTaken;
    assert(0 <= whistersTakenTricks and whistersTakenTricks <= totalTricksPerDeal);

    const auto deficit = [](auto req, auto got) { return std::max(0, req - got); };
    const auto declarerFailedTricks = declarer.contractLevel == ContractLevel::Miser
        ? deficit(declarer.tricksTaken, declarerReqTricks)
        : deficit(declarerReqTricks, declarer.tricksTaken);

    using Cmp = std::function<bool(int, int)>;
    const auto compareTricks = std::invoke([&]() {
        return (declarer.contractLevel == ContractLevel::Miser) ? Cmp{std::less_equal{}} : Cmp{std::greater_equal{}};
    });

    const auto makeDeclarerScore = [&] {
        auto result = DealScoreEntry{};
        if (compareTricks(declarer.tricksTaken, declarerReqTricks)) {
            result.pool = contractPrice;
        } else {
            result.dump = declarerFailedTricks * contractPrice;
        }
        return result;
    };

    const auto makeWhisterScore = [&](const Whister& w) {
        auto result = DealScoreEntry{};
        if (declarer.contractLevel == ContractLevel::Miser) { return result; }
        if (w.choise == WhistChoise::Whist) {
            result.whist += w.tricksTaken * contractPrice;
            if (deficit(twoWhistersReqTricks, whistersTakenTricks) > 0) {
                const auto reqTricks = rng::all_of(whisters, equalTo(WhistChoise::Whist), &Whister::choise)
                    ? oneWhisterReqTricks(declarer.contractLevel)
                    : twoWhistersReqTricks;
                result.dump += deficit(reqTricks, w.tricksTaken) * contractPrice;
            }
        } else if (w.choise == WhistChoise::HalfWhist) {
            result.whist += static_cast<std::int32_t>(
                0.5f * static_cast<float>(twoWhistersReqTricks) * static_cast<float>(contractPrice));
        }
        result.whist += declarerFailedTricks * contractPrice;
        return result;
    };

    return {
        {declarer.id, makeDeclarerScore()},
        {w1.id, makeWhisterScore(w1)},
        {w2.id, makeWhisterScore(w2)},
    };
}
// NOLINTEND(readability-function-cognitive-complexity, hicpp-uppercase-literal-suffix)

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
