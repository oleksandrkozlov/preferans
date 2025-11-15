// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#include "server.hpp"

#include "auth.hpp"
#include "common/common.hpp"
#include "common/time.hpp"
#include "game_data.hpp"
#include "proto/pref.pb.h"
#include "senders.hpp"
#include "serialization.hpp"

#include <boost/beast.hpp>
#include <range/v3/algorithm/all_of.hpp>
#include <range/v3/all.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <tuple>
#include <utility>

namespace pref {
namespace {

using net::ip::tcp;

auto setWhoseTurn(const Context::Players::const_iterator it) -> void
{
    ctx().whoseTurnIt = it;
}

auto setForehandId() -> void
{
    ctx().forehandId = ctx().whoseTurnId();
    PREF_I("playerId: {}", ctx().forehandId);
}

auto resetWhoseTurn() -> void
{
    assert((std::size(ctx().players) == NumberOfPlayers) and "all players joined");
    setWhoseTurn(std::cbegin(ctx().players));
}

auto advanceWhoseTurn() -> void
{
    assert((std::size(ctx().players) == NumberOfPlayers) and "all players joined");
    if (const auto nextTurnIt = std::next(ctx().whoseTurnIt); nextTurnIt != std::cend(ctx().players)) {
        setWhoseTurn(nextTurnIt);
    } else {
        resetWhoseTurn();
    }
    PREF_I("playerId: {}", ctx().whoseTurnId());
}

auto forehandsTurn() -> void
{
    PREF_I();
    assert(ctx().players.contains(ctx().forehandId) and "forehand player exists");
    setWhoseTurn(ctx().players.find(ctx().forehandId));
}

auto advanceWhoseTurn(const GameStage stage) -> void
{
    using enum GameStage;
    PREF_I("stage: {}", GameStage_Name(stage));
    advanceWhoseTurn();
    if (not rng::contains(std::array{BIDDING, TALON_PICKING, WITHOUT_TALON}, stage)) { return; }
    while (ctx().player(ctx().whoseTurnId()).bid == PREF_PASS) { advanceWhoseTurn(); }
}

auto setNextDealTurn() -> void
{
    assert((std::size(ctx().players) == NumberOfPlayers) and "all players joined");
    assert(ctx().players.contains(ctx().forehandId) and "forehand player exists");
    const auto nextIt = std::next(ctx().players.find(ctx().forehandId));
    setWhoseTurn(nextIt != std::cend(ctx().players) ? nextIt : std::cbegin(ctx().players));
    setForehandId();
}

[[nodiscard]] auto decideTrickWinner() -> Player::Id
{
    const auto winnerId = decideTrickWinner(ctx().trick, ctx().trump);
    const auto winnerName = ctx().playerName(winnerId);
    const auto tricksTaken = ++ctx().player(winnerId).tricksTaken;
    PREF_DI(winnerName, winnerId, tricksTaken);
    ctx().trick.clear();
    return winnerId;
}

[[nodiscard]] constexpr auto makeContractLevel(const std::string_view contract) noexcept -> ContractLevel
{
    using enum ContractLevel;
    if (contract.starts_with(PREF_SIX)) { return Six; }
    if (contract.starts_with(PREF_SEVEN)) { return Seven; }
    if (contract.starts_with(PREF_EIGHT)) { return Eight; }
    if (contract.starts_with(PREF_NINE)) { return Nine; }
    if (contract.starts_with(PREF_TEN)) { return Ten; }
    if (contract.starts_with(PREF_MIS)) { return Miser; }
    std::unreachable();
}

[[nodiscard]] constexpr auto contractPrice(const ContractLevel level) noexcept -> int
{
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
}

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

[[nodiscard]] constexpr auto makeWhistingChoice(const std::string_view choice) noexcept -> WhistingChoice
{
    using enum WhistingChoice;
    if (choice == PREF_WHIST) { return Whist; }
    if (choice == PREF_CATCH) { return Whist; }
    if (choice == PREF_PASS) { return Pass; }
    if (choice == PREF_TRUST) { return Pass; }
    if (choice == PREF_HALF_WHIST) { return HalfWhist; }
    if (choice == PREF_PASS_WHIST) { return PassWhist; }
    if (choice == PREF_PASS_PASS) { return PassPass; }
    std::unreachable();
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

[[nodiscard]] auto isNewPlayer(const Player::Id& playerId) -> bool
{
    return std::empty(playerId) or not ctx().players.contains(playerId);
}

auto joinPlayer(const ChannelPtr& ch, const Player::Id& playerId, PlayerSession& session) -> void
{
    session.playerId = playerId;
    ++session.id;
    assert(session.id == 1);
    PREF_DI(session.playerId, session.playerName, session.id);
    ctx().players.emplace(session.playerId, Player{session.playerId, session.playerName, session.id, ch});
}

auto prepareNewSession(const Player::Id& playerId, PlayerSession& session) -> Awaitable<>
{
    PREF_I("{}, {}, {}{}", PREF_V(playerId), PREF_V(session.playerName), PREF_V(session.id), PREF_M(session.playerId));
    auto& player = ctx().player(playerId);
    session.id = ++player.sessionId;
    session.playerId = playerId;
    session.playerName = player.name; // keep the first connected player's name
    player.conn.cancelReconnectTimer();
    co_await player.conn.closeStream();
}

auto reconnectPlayer(const ChannelPtr& ch, const Player::Id& playerId, PlayerSession& session) -> Awaitable<>
{
    co_await prepareNewSession(playerId, session);
    ctx().player(playerId).conn.replaceChannel(ch);
    PREF_DI(session.playerName, session.playerId, session.id);
    // TODO: send the game state to the reconnected player
}

auto addCardToHand(const Player::Id& playerId, const std::string& card) -> void
{
    assert(not ctx().player(playerId).hand.contains(card) and "card doesn't exists");
    ctx().player(playerId).hand.insert(card);
}

[[nodiscard]] auto decidePlayerTurn(const GameStage stage) -> PlayerTurnData
{
    using enum GameStage;
    auto playerId = ctx().whoseTurnId();
    auto canHalfWhist = false; // default;
    auto talon = CardsNames{};
    if (stage == TALON_PICKING) {
        assert((std::size(ctx().talon.cards) == 2) and "talon is two cards");
        talon = ctx().talon.cards;
        for (const auto& card : talon) { addCardToHand(playerId, card); }
    } else if (stage == WHISTING) {
        if (const auto contractLevel = makeContractLevel(getDeclarer(ctx().players).bid);
            contractLevel == ContractLevel::Six or contractLevel == ContractLevel::Seven) {
            const auto checkHalfWhist = [&](const Player& self, const Player& other) {
                return self.id == playerId and std::empty(self.whistingChoice) and other.whistingChoice == PREF_PASS;
            };
            if (const auto& [w0, w1] = getWhisters(ctx().players); checkHalfWhist(w0, w1) or checkHalfWhist(w1, w0)) {
                canHalfWhist = true;
            }
        }
    }
    return {playerId, stage, std::string{ctx().passGame.minBid()}, canHalfWhist, ctx().passGame.round, talon};
}

auto removeCardFromHand(const Player::Id& playerId, const std::string& card) -> void
{
    assert(ctx().player(playerId).hand.contains(card) and "card exists");
    ctx().player(playerId).playedCards.push_back(card);
    ctx().player(playerId).hand.erase(card);
}

auto dealCards() -> Awaitable<>
{
    const auto suits = std::array{PREF_SPADES, PREF_DIAMONDS, PREF_CLUBS, PREF_HEARTS};
    const auto ranks
        = std::array{PREF_SEVEN, PREF_EIGHT, PREF_NINE, PREF_TEN, PREF_JACK, PREF_QUEEN, PREF_KING, PREF_ACE};
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
    ctx().talon.cards = chunks | rv::drop(NumberOfPlayers) | rv::join | rng::to_vector;
    assert((std::size(ctx().talon.cards) == 2) and "talon is two cards");
    assert((std::size(ctx().players) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (auto&& [playerId, hand] : rv::zip(ctx().players | rv::keys, hands)) {
        ctx().player(playerId).hand = hand | rng::to<Hand>;
    }
    PREF_I("talon: {}", ctx().talon.cards);
    const auto channels = ctx().players
        | rv::values
        | rv::transform([](const Player& player) { return std::pair{player.conn.ch, player.id}; })
        | rng::to_vector;
    assert((std::size(channels) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (auto&& [ch_id, hand] : rv::zip(channels, hands)) {
        const auto& [ch, id] = ch_id;
        co_await sendDealCardsFor(ch, id, hand | rng::to<Hand>);
    }
}

auto removePlayer(const Player::Id& playerId) -> Awaitable<>
{
    assert(ctx().players.contains(playerId) and "player exists");
    PREF_DI(playerId);
    ctx().players.erase(playerId);
    co_await sendPlayerLeft(playerId);
}

auto disconnected(Player::Id playerId) -> Awaitable<>
{
    PREF_DI(playerId);
    auto& player = ctx().player(playerId);
    if (not player.conn.reconnectTimer) { player.conn.reconnectTimer.emplace(co_await net::this_coro::executor); }
    player.conn.reconnectTimer->expires_after(10s);
    if (const auto [error] = co_await player.conn.reconnectTimer->async_wait(net::as_tuple); error) {
        if (error != net::error::operation_aborted) { PREF_DW(error); }
        co_return;
    }
    co_await removePlayer(playerId);
}

using TrickComparator = std::function<bool(int, int)>;
[[nodiscard]] constexpr auto compareTricks(const ContractLevel level) -> TrickComparator
{
    return level == ContractLevel::Miser ? TrickComparator{std::less_equal{}} : TrickComparator{std::greater_equal{}};
}

[[maybe_unused]] auto hasDeclarerFulfilledContract() -> bool
{
    return findDeclarerId(ctx().players)
        .transform([&](const Player::Id& declarerId) {
            const auto& declarer = ctx().players.at(declarerId);
            const auto contractLevel = makeContractLevel(declarer.bid);
            return compareTricks(contractLevel)(declarer.tricksTaken, declarerReqTricks(contractLevel));
        })
        .value_or(false);
}

auto updateScoreSheetForDeal() -> void
{
    findDeclarerId(ctx().players) | OnValue([&](const Player::Id& declarerId) {
        const auto& declarerPlayer = ctx().players.at(declarerId);
        const auto declarer = Declarer{
            .id = declarerId,
            .contractLevel = makeContractLevel(declarerPlayer.bid),
            .tricksTaken = declarerPlayer.tricksTaken};
        auto whisters = std::vector<Whister>{};
        for (const auto& whisterPlayer : getWhisters(ctx().players)) {
            whisters.emplace_back(
                whisterPlayer.get().id,
                makeWhistingChoice(whisterPlayer.get().whistingChoice),
                whisterPlayer.get().tricksTaken);
        }
        for (const auto& [id, entry] : calculateDealScore(declarer, whisters)) {
            ctx().scoreSheet[id].dump.push_back(entry.dump);
            ctx().scoreSheet[id].pool.push_back(entry.pool);
            if (id != declarerId) { ctx().scoreSheet[id].whists[declarerId].push_back(entry.whist); }
        }
    }) | OnNone([] { // PassGame
        const auto players = ctx().players | rv::values;
        const auto minTricksTaken = rng::min(players | rv::transform(&Player::tricksTaken));
        for (const auto& player : players) {
            assert(ctx().passGame.now);
            assert(ctx().passGame.round != 0);
            if (const auto price = progressionTerm(ctx().passGame.round, PassGame::s_progression);
                player.tricksTaken == 0) {
                ctx().scoreSheet[player.id].pool.push_back(price);
            } else {
                ctx().scoreSheet[player.id].dump.push_back((player.tricksTaken - minTricksTaken) * price);
            }
        }
    });
}

auto dealFinished() -> Awaitable<>
{
    ctx().gameDuration = pref::durationInSec(ctx().gameStarted);
    PREF_I("gameId: {} duration: {}", ctx().gameId, formatDuration(ctx().gameDuration));
    updateScoreSheetForDeal();
    const auto finalResult = calculateFinalResult(makeFinalScore(ctx().scoreSheet));
    for (const auto& [playerId, score] : ctx().scoreSheet) {
        PREF_DI(playerId, score.dump, score.pool);
        auto totalWhists = 0;
        for (const auto& [id, whists] : score.whists) {
            PREF_I("whists: {} -> {}", whists, id);
            totalWhists += rng::accumulate(whists, 0);
        }
        addOrUpdateUserGame(
            ctx().gameData,
            playerId,
            makeUserGame(
                ctx().gameId,
                ctx().gameDuration,
                rng::accumulate(score.pool, 0),
                rng::accumulate(score.dump, 0),
                totalWhists,
                finalResult.at(playerId)));
    }
    storeGameData(ctx().gameDataPath, ctx().gameData);
    co_await sendUserGames();
    co_await sendDealFinished();
    ctx().clear();
}

[[nodiscard]] auto stageGame() -> GameStage
{
    const auto bids = ctx().players | rv::values | rv::transform(&Player::bid);
    const auto passCount = rng::count(bids, PREF_PASS);
    const auto activeCount = rng::count_if(bids, [](auto&& bid) { return not std::empty(bid) and bid != PREF_PASS; });
    if (std::cmp_equal(passCount, WhistersCount) and std::cmp_equal(activeCount, DeclarerCount)) {
        const auto& contract = *rng::find_if(bids, notEqualTo(PREF_PASS));
        return contract.contains(PREF_WT) ? GameStage::WITHOUT_TALON : GameStage::TALON_PICKING;
    }
    if (std::cmp_equal(passCount, NumberOfPlayers)) {
        ctx().passGame.update();
        return GameStage::PLAYING;
    }
    return GameStage::BIDDING;
}

auto startGame() -> Awaitable<>
{
    assert(std::size(ctx().players) == NumberOfPlayers);
    // TODO: use UTC on the server and local time zone on the client
    ctx().gameStarted = localTimeSinceEpochInSec();
    ++ctx().gameId;
    PREF_I("gameId: {} started: {} {}", ctx().gameId, formatDate(ctx().gameStarted), formatTime(ctx().gameStarted));
    for (const auto& id : ctx().players | rv::keys) {
        addOrUpdateUserGame(ctx().gameData, id, makeUserGame(ctx().gameId, GameType::RANKED, ctx().gameStarted));
    }
    storeGameData(ctx().gameDataPath, ctx().gameData);
    co_await sendUserGames();
    co_await dealCards();
    resetWhoseTurn();
    setForehandId();
    co_await sendForehand();
    co_await sendPlayerTurn(decidePlayerTurn(GameStage::BIDDING));
}

[[nodiscard]] auto toServerAuthToken(const std::string_view authToken) -> std::string
{
    return hashToken(hex2bytes(authToken));
}

[[nodiscard]] auto generateClientAuthToken() -> std::string
{
    return bytes2hex(generateToken());
}

auto handleLoginRequest(const Message& msg, const ChannelPtr& ch) -> Awaitable<PlayerSession>
{
    auto loginRequest = makeMethod<LoginRequest>(msg);
    if (not loginRequest) { co_return PlayerSession{}; }
    auto session = PlayerSession{};
    const auto& playerName = loginRequest->player_name();
    const auto& password = loginRequest->password();
    if (not verifyPlayerNameAndPassword(ctx().gameData, playerName, password)) {
        const auto error = fmt::format("unknown {} or wrong password", PREF_V(playerName));
        PREF_DW(error);
        co_await sendLoginResponse(ch, error);
        co_return session;
    }
    assert(userPlayerId(ctx().gameData, playerName));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto& playerId = userPlayerId(ctx().gameData, playerName)->get();
    const auto authToken = generateClientAuthToken();
    addAuthToken(ctx().gameData, playerId, toServerAuthToken(authToken));
    storeGameData(ctx().gameDataPath, ctx().gameData);
    PREF_DI(playerName, playerId);
    session.playerName = playerName;
    if (isNewPlayer(playerId)) {
        joinPlayer(ch, playerId, session);
        co_await sendLoginResponse(ch, {}, playerId, authToken);
    } else {
        co_await reconnectPlayer(ch, playerId, session);
        co_await sendLoginResponse(ch, {}, playerId, authToken);
        co_return session;
    }
    co_await sendPlayerJoined(session);
    co_return session;
}

auto handleAuthRequest(const Message& msg, const ChannelPtr& ch) -> Awaitable<PlayerSession>
{
    auto authRequest = makeMethod<AuthRequest>(msg);
    if (not authRequest) { co_return PlayerSession{}; }
    auto session = PlayerSession{};
    const auto& playerId = authRequest->player_id();
    if (not verifyPlayerIdAndAuthToken(ctx().gameData, playerId, toServerAuthToken(authRequest->auth_token()))) {
        const auto error = fmt::format("unknown {} or wrong auth token", PREF_V(playerId));
        PREF_DW(error);
        co_await sendAuthResponse(ch, error);
        co_return session;
    }
    assert(userByPlayerId(ctx().gameData, playerId).has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto& playerName = userByPlayerId(ctx().gameData, playerId)->get().player_name();
    PREF_DI(playerName, playerId);
    session.playerName = playerName;
    if (isNewPlayer(playerId)) {
        joinPlayer(ch, playerId, session);
        co_await sendAuthResponse(ch, {}, playerName);
    } else {
        co_await reconnectPlayer(ch, playerId, session);
        co_await sendAuthResponse(ch, {}, playerName);
        co_return session;
    }
    co_await sendPlayerJoined(session);
    co_return session;
}

auto handleLogout(const Message& msg) -> Awaitable<>
{
    auto logout = makeMethod<Logout>(msg);
    if (not logout) { co_return; }
    const auto& playerId = logout->player_id();
    PREF_DI(playerId);
    revokeAuthToken(ctx().gameData, playerId, toServerAuthToken(logout->auth_token()));
    storeGameData(ctx().gameDataPath, ctx().gameData);
    co_await removePlayer(playerId);
}

auto handleReadyCheck(const Message& msg) -> Awaitable<>
{
    auto readyCheck = makeMethod<ReadyCheck>(msg);
    if (not readyCheck) { co_return; }
    const auto& playerId = readyCheck->player_id();
    const auto& state = readyCheck->state();
    PREF_I("{}, state: {}", PREF_V(playerId), ReadyCheckState_Name(state));
    if (state == ReadyCheckState::REQUESTED) {
        for (auto& readyCheckState : ctx().players | rv::values | rv::transform(&Player::readyCheckState)) {
            readyCheckState = ReadyCheckState::NOT_REQUESTED;
        }
    }
    ctx().player(playerId).readyCheckState = (state == ReadyCheckState::REQUESTED) ? ReadyCheckState::ACCEPTED : state;
    co_await forwardToAllExcept(msg, playerId);
    if (rng::all_of(ctx().players | rv::values, equalTo(ReadyCheckState::ACCEPTED), &Player::readyCheckState)) {
        co_await startGame();
    }
}

auto handleBidding(const Message& msg) -> Awaitable<>
{
    const auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) { co_return; }
    const auto& playerId = bidding->player_id();
    const auto& bid = bidding->bid();
    const auto& playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, bid);
    ctx().player(playerId).bid = bid;
    co_await forwardToAllExcept(msg, playerId);
    const auto stage = stageGame();
    if (ctx().passGame.now) { co_await sendOpenTalon(); }
    advanceWhoseTurn(stage);
    co_await sendPlayerTurn(decidePlayerTurn(stage));
}

auto handleDiscardTalon(const Message& msg) -> Awaitable<>
{
    const auto discardTalon = makeMethod<DiscardTalon>(msg);
    if (not discardTalon) { co_return; }
    const auto& playerId = discardTalon->player_id();
    const auto& bid = discardTalon->bid();
    ctx().player(playerId).bid = bid;
    const auto& discardedCards = discardTalon->cards();
    for (const auto& card : discardedCards) {
        ctx().talon.discardedCards.push_back(card);
        removeCardFromHand(playerId, card);
    }
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, discardedCards, bid);
    ctx().trump = std::string{getTrump(bid)};
    co_await sendBidding(playerId, bid); // final bid
    const auto stage = bid.contains(PREF_SIX) and bid.contains(PREF_SPADE) ? GameStage::PLAYING : GameStage::WHISTING;
    if (stage == GameStage::PLAYING) { // Stalingrad
        const auto whist = std::string{PREF_WHIST};
        for (const auto& w : getWhisters(ctx().players)) { co_await sendWhisting(w.get().id, whist); }
    }
    advanceWhoseTurn();
    co_await sendPlayerTurn(decidePlayerTurn(stage));
}

auto handlePingPong(const Message& msg, const ChannelPtr& ch) -> Awaitable<>
{
    const auto pingPong = makeMethod<PingPong>(msg);
    if (not pingPong) { co_return; }
    co_await sendPingPong(msg, ch);
}

auto finishDeal() -> Awaitable<>
{
    co_await dealFinished();
    co_await sleepFor(3s);
    co_await dealCards();
    setNextDealTurn();
    co_await sendForehand();
    co_await sendPlayerTurn(decidePlayerTurn(GameStage::BIDDING));
}

auto updateDeclarerTakenTricks() -> void
{
    auto& declarer = getDeclarer(ctx().players);
    declarer.tricksTaken = declarerReqTricks(makeContractLevel(declarer.bid));
}

[[nodiscard]] auto playerItByWhistingChoice(Context::Players& players, const WhistingChoice choice)
    -> Context::Players::iterator
{ // clang-format off
    auto it = rng::find_if(players, [&](const Player& player) {
        return not std::empty(player.whistingChoice) and makeWhistingChoice(player.whistingChoice) == choice;
    }, ToPlayer);
    assert(it != rng::end(players) and "player with the given choice exists");
    return it;
} // clang-format on

[[nodiscard]] auto playerByWhistingChoice(Context::Players& players, const WhistingChoice choice) -> Player&
{
    return playerItByWhistingChoice(players, choice)->second;
}

auto openCardsAndLetAnotherWhisterPlay() -> Awaitable<>
{
    const auto& activeWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Whist);
    const auto& passiveWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Pass);
    co_await sendOpenWhistPlay(activeWhister.id, passiveWhister.id);
    co_await sendDealCardsExcept(activeWhister.id, activeWhister.hand);
    co_await sendDealCardsExcept(passiveWhister.id, passiveWhister.hand);
}

auto openCards() -> Awaitable<>
{
    const auto& [w0, w1] = getWhisters(ctx().players);
    co_await sendDealCardsExcept(w0.get().id, w0.get().hand);
    co_await sendDealCardsExcept(w1.get().id, w1.get().hand);
}

auto startPlayingFromForehand() -> Awaitable<>
{
    forehandsTurn();
    co_await sendPlayerTurn(decidePlayerTurn(GameStage::PLAYING));
}

auto handleWhisting(const Message& msg) -> Awaitable<>
{
    using enum WhistingChoice;
    using enum GameStage;
    auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { co_return; }
    const auto& playerId = whisting->player_id();
    const auto& choice = whisting->choice();
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, choice);
    ctx().player(playerId).whistingChoice += choice; // Pass + Whist, Pass + Pass, etc.
    co_await forwardToAllExcept(msg, playerId);
    if (ctx().isHalfWhistAfterPass()) {
        advanceWhoseTurn(); // skip declarer
        advanceWhoseTurn();
        co_return co_await sendPlayerTurn(decidePlayerTurn(WHISTING));
    }
    if (ctx().isWhistAfterHalfWhist()) {
        {
            auto& whister = playerByWhistingChoice(ctx().players, HalfWhist);
            auto pass = std::string{PREF_PASS};
            co_await sendWhisting(whister.id, pass);
            whister.whistingChoice = std::move(pass);
        }
        {
            auto& whister = playerByWhistingChoice(ctx().players, PassWhist);
            whister.whistingChoice = PREF_WHIST;
        }
        co_return co_await sendPlayerTurn(decidePlayerTurn(HOW_TO_PLAY));
    }
    if (ctx().isPassAfterHalfWhist()) {
        playerByWhistingChoice(ctx().players, PassPass).whistingChoice = PREF_PASS;
        updateDeclarerTakenTricks();
        co_return co_await finishDeal();
    }
    if (ctx().areWhistersPass()) {
        updateDeclarerTakenTricks();
        co_return co_await finishDeal();
    }
    const auto& declarer = getDeclarer(ctx().players);
    const auto isMiser = declarer.bid.contains(PREF_MIS);
    const auto oneWhist = ctx().areWhistersPassAndWhist();
    const auto bothWhist = ctx().areWhistersWhist();
    const auto whist = oneWhist or bothWhist;
    if (isMiser and whist) [[unlikely]] {
        if (ctx().forehandId == declarer.id) {
            ctx().isDeclarerFirstMiserTurn = true;
        } else if (ctx().areWhistersWhist()) {
            co_await openCards();
        } else {
            assert(ctx().areWhistersPassAndWhist());
            co_await openCardsAndLetAnotherWhisterPlay();
        }
        co_await sendMiserCards();
        co_return co_await startPlayingFromForehand();
    }
    if (bothWhist) { co_return co_await startPlayingFromForehand(); }
    if (oneWhist) {
        if (choice != PREF_WHIST) { setWhoseTurn(playerItByWhistingChoice(ctx().players, Whist)); }
        co_return co_await sendPlayerTurn(decidePlayerTurn(HOW_TO_PLAY));
    }
    advanceWhoseTurn();
    co_return co_await sendPlayerTurn(decidePlayerTurn(WHISTING));
}

auto handleHowToPlay(const Message& msg) -> Awaitable<>
{
    auto howToPlay = makeMethod<HowToPlay>(msg);
    if (not howToPlay) { co_return; }
    const auto& playerId = howToPlay->player_id();
    const auto& choice = howToPlay->choice();
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, choice);
    auto& player = ctx().player(playerId);
    player.howToPlayChoice = choice;
    co_await forwardToAllExcept(msg, playerId);
    if (choice == PREF_OPENLY) { co_await openCardsAndLetAnotherWhisterPlay(); }
    co_await startPlayingFromForehand();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto handlePlayCard(const Message& msg) -> Awaitable<>
{
    const auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) { co_return; }
    const auto& playerId = playCard->player_id();
    const auto& card = playCard->card();
    const auto playerName = ctx().playerName(playerId);
    removeCardFromHand(playerId, card);
    ctx().trick.emplace_back(playerId, card);
    PREF_DI(playerName, playerId, card);
    co_await forwardToAll(msg);
    if (ctx().isDeclarerFirstMiserTurn) {
        ctx().isDeclarerFirstMiserTurn = false;
        if (ctx().areWhistersWhist()) {
            co_await openCards();
        } else {
            assert(ctx().areWhistersPassAndWhist());
            co_await openCardsAndLetAnotherWhisterPlay();
        }
    }
    if (const auto isNotTrickFinished = (std::size(ctx().trick) != 3); isNotTrickFinished) {
        advanceWhoseTurn();
    } else {
        const auto winnerId = decideTrickWinner();
        co_await sendTrickFinished();
        if (const auto isDealFinished = rng::all_of(ctx().players | rv::values, &Hand::empty, &Player::hand);
            isDealFinished) {
            if (ctx().passGame.round != 0 and hasDeclarerFulfilledContract()) { ctx().passGame.resetRound(); }
            co_return co_await finishDeal();
        }
        if (not ctx().passGame.now) {
            setWhoseTurn(ctx().players.find(winnerId));
        } else {
            ++ctx().talon.open;
            if (ctx().talon.open == 1) {
                co_await sendOpenTalon();
                forehandsTurn();
            } else if (ctx().talon.open == 2) {
                forehandsTurn();
            } else {
                setWhoseTurn(ctx().players.find(winnerId));
            }
        }
    }
    if (const auto declarerId = findDeclarerId(ctx().players); declarerId) {
        const auto isMiser = ctx().player(*declarerId).bid.contains(PREF_MIS);
        if (isMiser and *declarerId == playerId) { co_await sendMiserCards(); }
    }
    co_await sendPlayerTurn(decidePlayerTurn(GameStage::PLAYING));
}

auto handleLog(const Message& msg) -> void
{
    const auto log = makeMethod<Log>(msg);
    if (not log) { return; }
    PREF_I("[client] {}, playerId: {}", log->text(), log->player_id());
}

auto handleSpeechBubble(const Message& msg) -> Awaitable<>
{
    const auto speechBubble = makeMethod<SpeechBubble>(msg);
    if (not speechBubble) { co_return; }
    const auto& playerId = speechBubble->player_id();
    PREF_DI(playerId);
    co_await forwardToAllExcept(msg, playerId);
}

auto dispatchMessage(const ChannelPtr& ch, PlayerSession& session, std::optional<Message> msg) -> Awaitable<>
{ // clang-format off
    if (not msg) { co_return; }
    const auto& method = msg->method();
    if (method == "LoginRequest") { session = co_await handleLoginRequest(*msg, ch); co_return; }
    if (method == "AuthRequest") { session = co_await handleAuthRequest(*msg, ch); co_return; }
    if (session.id == 0) { co_return; }
    if (method == "Logout") { co_await handleLogout(*msg); co_return; }
    if (method == "ReadyCheck") { co_await handleReadyCheck(*msg); co_return; }
    if (method == "Bidding") { co_await handleBidding(*msg); co_return; }
    if (method == "DiscardTalon") { co_await handleDiscardTalon(*msg); co_return; }
    if (method == "Whisting") { co_await handleWhisting(*msg); co_return; }
    if (method == "HowToPlay") { co_await handleHowToPlay(*msg); co_return; }
    if (method == "PlayCard") { co_await handlePlayCard(*msg); co_return; }
    if (method == "SpeechBubble") { co_await handleSpeechBubble(*msg); co_return; }
    if (method == "PingPong") { co_await handlePingPong(*msg, ch); co_return; }
    if (method == "Log") { handleLog(*msg); co_return; }
    PREF_W("error: unknown {}", PREF_V(method));
} // clang-format on

auto launchSession(Stream ws) -> Awaitable<>
{
    PREF_I();
    ws.binary(true);
    ws.set_option(web::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(web::stream_base::decorator([](web::response_type& res) {
        res.set(beast::http::field::server, std::string{BOOST_BEAST_VERSION_STRING} + " preferans-server");
    }));
#ifdef PREF_SSL
    {
        auto error = sys::error_code{};
        co_await ws.next_layer().async_handshake(net::ssl::stream_base::server, net::redirect_error(error));
        if (error) {
            PREF_W("{} (handshake)", PREF_V(error));
            co_return;
        }
    }
#endif // PREF_SSL
    {
        auto error = sys::error_code{};
        co_await ws.async_accept(net::redirect_error(error));
        if (error) {
            PREF_W("{} (accept)", PREF_V(error));
            co_return;
        }
    }
    static constexpr auto channelSize = 128;
    const auto ex = co_await net::this_coro::executor;
    auto ch = std::make_shared<Channel>(ex, channelSize);
    net::co_spawn(ex, payloadSender(ws, ch), Detached("payloadSender"));
    auto session = PlayerSession{};

    while (true) {
        auto buffer = beast::flat_buffer{};
        if (const auto [error, _] = co_await ws.async_read(buffer, net::as_tuple); error) {
            PREF_W("{} (read){}{}", error, PREF_M(session.playerId), PREF_M(session.playerName));
            break;
        }
        co_await dispatchMessage(ch, session, makeMessage(buffer.data().data(), buffer.size()));
        if (session.id == 0) { break; }
    }
    if (session.id == 0) {
        PREF_W("error: session ID is empty");
        ch->close();
        co_return;
    }
    if (not std::empty(session.playerId) and not ctx().players.contains(session.playerId)) {
        PREF_W("{} already left", PREF_V(session.playerId));
        ch->close();
        co_return;
    }
    auto& player = ctx().player(session.playerId);
    if (session.id != player.sessionId) {
        PREF_I("{} reconnected with {} => {}", PREF_V(session.playerId), PREF_V(session.id), player.sessionId);
        co_return;
    }
    net::co_spawn(ex, disconnected(session.playerId), Detached("disconnected"));
    ch->close();
}

} // namespace

Player::Player(Id aId, Name aName, PlayerSession::Id aSessionId, const ChannelPtr& ch)
    : id{std::move(aId)}
    , name{std::move(aName)}
    , sessionId{aSessionId}
    , conn{ch}
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

auto Context::playerName(const Player::Id& playerId) const -> Player::NameView
{
    return player(playerId).name;
}

auto Context::areWhistersPass() const -> bool
{
    return 2 == countWhistingChoice(WhistingChoice::Pass);
}

auto Context::areWhistersWhist() const -> bool
{
    return 2 == countWhistingChoice(WhistingChoice::Whist);
}

auto Context::areWhistersPassAndWhist() const -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::Pass) //
        and 1 == countWhistingChoice(WhistingChoice::Whist);
}

auto Context::isHalfWhistAfterPass() const -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::Pass) //
        and 1 == countWhistingChoice(WhistingChoice::HalfWhist);
}

auto Context::isPassAfterHalfWhist() const -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::PassPass);
}

auto Context::isWhistAfterHalfWhist() const -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::PassWhist);
}

auto Context::countWhistingChoice(const WhistingChoice choice) const -> std::ptrdiff_t
{
    return rng::count_if(
        players
            | rv::values
            | rv::transform(&Player::whistingChoice)
            | rv::filter(rng::not_fn(&std::string::empty))
            | rv::transform(&makeWhistingChoice),
        equalTo(choice));
}

[[nodiscard]] auto beats(const Beat beat) -> bool
{
    const auto [candidate, best, leadSuit, trump] = beat;
    const auto candidateSuit = cardSuit(candidate);
    const auto bestSuit = cardSuit(best);
    if (candidateSuit == bestSuit) { return rankValue(cardRank(candidate)) > rankValue(cardRank(best)); }
    if (not std::empty(trump) and (candidateSuit == trump) and (bestSuit != trump)) { return true; }
    if (not std::empty(trump) and (candidateSuit != trump) and (bestSuit == trump)) { return false; }
    return candidateSuit == leadSuit;
}

[[nodiscard]] auto decideTrickWinner(const std::vector<PlayedCard>& trick, const std::string_view trump) -> Player::Id
{ // clang-format off
    assert(std::size(trick) == NumberOfPlayers and "all players played cards");
    const auto& firstCard = trick.front();
    const auto& leadSuit = cardSuit(not std::empty(ctx().talon.current) ? ctx().talon.current : firstCard.name);
    ctx().talon.current.clear();
    return rng::accumulate(trick, firstCard, [&](const PlayedCard& best, const PlayedCard& candidate) {
        return beats({candidate.name, best.name, leadSuit, trump}) ? candidate : best; // NOLINT(modernize-use-designated-initializers)
    }).playerId;
} // clang-format on

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto calculateDealScore(const Declarer& declarer, const std::vector<Whister>& whisters) -> DealScore
{
    assert(std::size(whisters) == 2);

    const auto& w1 = whisters[0];
    const auto& w2 = whisters[1];
    [[maybe_unused]] static constexpr auto totalTricksPerDeal = 10;

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

    const auto makeDeclarerScore = [&] {
        auto result = DealScoreEntry{};
        if (compareTricks(declarer.contractLevel)(declarer.tricksTaken, declarerReqTricks)) {
            result.pool = contractPrice;
        } else {
            result.dump = declarerFailedTricks * contractPrice;
        }
        return result;
    };

    const auto makeWhisterScore = [&](const Whister& w) {
        auto result = DealScoreEntry{};
        if (declarer.contractLevel == ContractLevel::Miser) { return result; }
        if (w.choice == WhistingChoice::Whist) {
            result.whist += w.tricksTaken * contractPrice;
            if (deficit(twoWhistersReqTricks, whistersTakenTricks) > 0) {
                const auto reqTricks = rng::all_of(whisters, equalTo(WhistingChoice::Whist), &Whister::choice)
                    ? oneWhisterReqTricks(declarer.contractLevel)
                    : twoWhistersReqTricks;
                result.dump += deficit(reqTricks, w.tricksTaken) * contractPrice;
            }
        } else if (w.choice == WhistingChoice::HalfWhist) {
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

auto acceptConnectionAndLaunchSession(
#ifdef PREF_SSL
    net::ssl::context ssl,
#endif // PREF_SSL
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    tcp::endpoint endpoint) -> Awaitable<>
{
    PREF_I();
    const auto ex = co_await net::this_coro::executor;
    auto acceptor = tcp::acceptor{ex, endpoint};
    while (true) {
        auto socket = co_await acceptor.async_accept();
#ifdef PREF_SSL
        auto ws = Stream{std::move(socket), ssl};
#else // PREF_SSL
        auto ws = Stream{std::move(socket)};
#endif // PREF_SSL
        net::co_spawn(ex, launchSession(std::move(ws)), Detached("launchSession"));
    }
}

} // namespace pref
