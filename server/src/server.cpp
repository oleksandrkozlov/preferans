// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2025 Oleksandr Kozlov

#include "server.hpp"

#include "auth.hpp"
#include "common/common.hpp"
#include "common/time.hpp"
#include "game_data.hpp"
#include "proto/pref.pb.h"
#include "send_msg.hpp"
#include "serialization.hpp"
#include "transport.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/beast.hpp>
#include <exec/async_scope.hpp>
#include <exec/scope.hpp>
#include <range/v3/all.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <ranges>
#include <tuple>
#include <utility>

namespace pref {
namespace {

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
    ctx().lastTrick = ctx().trick | rv::transform(&PlayedCard::name) | rng::to_vector;
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
    assert(choice != PREF_CATCH and choice != PREF_TRUST);
    if (choice == PREF_WHIST) { return Whist; }
    if (choice == PREF_PASS) { return Pass; }
    if (choice == PREF_HALF_WHIST) { return HalfWhist; }
    if (choice == PREF_PASS_WHIST) { return PassWhist; }
    if (choice == PREF_PASS_PASS) { return PassPass; }
    std::unreachable();
}

[[nodiscard]] auto findPasserIds() -> std::vector<Player::Id>
{
    return players() | rv::filter(equalTo(PREF_PASS), &Player::bid) | rv::transform(&Player::id) | rng::to_vector;
}

[[nodiscard]] auto getTwoPassers() -> std::array<std::reference_wrapper<Player>, 2>
{
    const auto whisterIds = findPasserIds();
    assert(std::size(whisterIds) == 2 and "there are two passers");
    const auto& w0 = whisterIds[0];
    const auto& w1 = whisterIds[1];
    assert(ctx().players.contains(w0) and ctx().players.contains(w1) and "whisters exist");
    return {ctx().players.at(w0), ctx().players.at(w1)};
}

[[nodiscard]] auto getOneOrTwoWhisters() -> std::vector<std::reference_wrapper<Player>>
{
    const auto passers = getTwoPassers();
    auto result = passers // clang-format off
        | rv::filter([](const std::string_view choice) {
            return std::ranges::contains_subrange(choice, std::string_view{PREF_WHIST});
        }, &Player::whistingChoice)
        | rng::to_vector; // clang-format on
    assert(not std::empty(result) and std::size(result) <= 2);
    return result;
}

[[nodiscard]] auto isNewPlayer(const Player::IdView playerId) -> bool
{
    return std::empty(playerId) or not ctx().players.contains(playerId);
}

auto joinPlayer(const ChannelPtr& ch, const Player::IdView playerId, PlayerSession& session) -> void
{
    session.playerId = playerId;
    ++session.id;
    assert(session.id == 1);
    PREF_DI(session.playerId, session.playerName, session.id);
    ctx().players.emplace(session.playerId, Player{session.playerId, session.playerName, session.id, ch});
}

auto prepareNewSession(const Player::IdView playerId, PlayerSession& session) -> task<>
{
    PREF_I("{}, {}, {}{}", PREF_V(playerId), PREF_V(session.playerName), PREF_V(session.id), PREF_M(session.playerId));
    auto& player = ctx().player(playerId);
    session.id = ++player.sessionId;
    session.playerId = playerId;
    session.playerName = player.name; // keep the first connected player's name
    if (player.conn.reconnectTimer) { player.conn.cancelReconnectTimer(); }
    // the channel might be already close
    if (player.conn.ch->is_open()) { co_await player.conn.closeStream(); }
}

auto addCardToHand(const Player::IdView playerId, const CardName& card) -> void
{
    assert(not ctx().player(playerId).hand.contains(card) and "card doesn't exists");
    ctx().player(playerId).hand.insert(card);
}

[[nodiscard]] auto makePlayerTurnData() -> PlayerTurnData
{
    using enum GameStage;
    const auto playerId = ctx().whoseTurnId();
    auto canHalfWhist = false; // default;
    auto talon = CardsNames{};
    if (ctx().stage == TALON_PICKING) {
        assert((std::size(ctx().talon.cards) == 2) and "talon is two cards");
        talon = ctx().talon.cards;
    } else if (ctx().stage == WHISTING) {
        if (const auto contractLevel = makeContractLevel(getDeclarer().bid);
            contractLevel == ContractLevel::Six or contractLevel == ContractLevel::Seven) {
            const auto checkHalfWhist = [&](const Player& self, const Player& other) {
                return self.id == playerId and std::empty(self.whistingChoice) and other.whistingChoice == PREF_PASS;
            };
            if (const auto& [p0, p1] = getTwoPassers(); checkHalfWhist(p0, p1) or checkHalfWhist(p1, p0)) {
                canHalfWhist = true;
            }
        }
    }
    return {
        std::string{playerId},
        ctx().stage,
        std::string{ctx().passGame.minBid()},
        canHalfWhist,
        ctx().passGame.round,
        talon};
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

auto openCardsAndLetAnotherWhisterPlay() -> task<>
{
    const auto& activeWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Whist);
    const auto& passiveWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Pass);
    co_await sendOpenWhistPlay(activeWhister.id, passiveWhister.id);
    co_await sendDealCardsExcept(activeWhister.id, activeWhister.hand);
    co_await sendDealCardsExcept(passiveWhister.id, passiveWhister.hand);
}

auto reconnectPlayer(const ChannelPtr& ch, const Player::IdView playerId, PlayerSession& session) -> task<>
{
    co_await prepareNewSession(playerId, session);
    auto& player = ctx().player(playerId);
    player.conn.replaceChannel(ch);
    PREF_DI(session.playerName, session.playerId, session.id);
    const auto players = pref::players();
    if (ctx().stage == GameStage::UNKNOWN) {
        const auto readyChecks = players
            | rv::filter(notEqualTo(ReadyCheckState::NOT_REQUESTED), &Player::readyCheckState)
            | rv::transform([](const Player& p) { return std::pair{p.id, p.readyCheckState}; })
            | rng::to_vector;
        for (auto&& [id, check] : readyChecks) { co_await sendReadyCheckToOne(ch, id, check); }
        co_return;
    }
    // TODO: combine all the messages in a batch
    // TODO: send SpeechBubble after reconnection
    // TODO: send Offer after reconnection
    co_await sendUserGames(player);
    co_await sendDealCardsFor(ch, playerId, player.hand | rng::to<Hand>);
    co_await sendForehand();
    co_await sendPlayerTurn(makePlayerTurnData());
    co_await sendPlayedCards(ch);
    const auto bids = players
        | rv::filter(rng::not_fn(rng::empty), &Player::bid)
        | rv::transform([](const Player& p) { return std::pair{p.id, p.bid}; })
        | rng::to_vector;
    const auto choices = players
        | rv::filter(rng::not_fn(rng::empty), &Player::whistingChoice)
        | rv::transform([](const Player& p) { return std::pair{p.id, p.whistingChoice}; })
        | rng::to_vector;

    const auto howToPlay = players
        | rv::filter(rng::not_fn(rng::empty), &Player::howToPlayChoice)
        | rv::transform([](const Player& p) { return std::pair{p.id, p.howToPlayChoice}; })
        | rng::to_vector;
    for (auto&& [id, bid] : bids) { co_await sendBiddingToOne(ch, id, bid); }
    for (auto&& [id, whist] : choices) { co_await sendWhistingToOne(ch, id, whist); }
    for (auto&& [id, play] : howToPlay) { co_await sendHowToPlayToOne(ch, id, play); }
    if (rng::any_of(players, equalTo(PREF_OPENLY), &Player::howToPlayChoice)) {
        const auto& activeWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Whist);
        const auto& passiveWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Pass);
        co_await sendOpenWhistPlayToOne(ch, activeWhister.id, passiveWhister.id);
        if (playerId == activeWhister.id) {
            co_await sendDealCardsFor(ch, passiveWhister.id, passiveWhister.hand);
        } else if (playerId == passiveWhister.id) {
            co_await sendDealCardsFor(ch, activeWhister.id, activeWhister.hand);
        } else {
            co_await sendDealCardsFor(ch, passiveWhister.id, passiveWhister.hand);
            co_await sendDealCardsFor(ch, activeWhister.id, activeWhister.hand);
        }
    }
    if (ctx().passGame.now and (ctx().talon.open < std::size(ctx().talon.cards))) { co_await sendOpenTalonToOne(ch); }
    if (ctx().stage == GameStage::PLAYING) {
        if (const auto declarerId = findDeclarerId(); declarerId) {
            const auto isMiser = ctx().player((*declarerId).get()).bid.contains(PREF_MIS);
            if (isMiser) { co_await sendMiserCards(); }
        }
    }
    co_await sendGameState(ch);
}

auto maybeAddTalonToHand() -> void
{
    if (ctx().stage != GameStage::TALON_PICKING) { return; }
    assert((std::size(ctx().talon.cards) == 2) and "talon is two cards");
    for (const auto& card : ctx().talon.cards) { addCardToHand(ctx().whoseTurnId(), card); }
}

[[nodiscard]] auto decidePlayerTurn() -> PlayerTurnData
{
    maybeAddTalonToHand();
    return makePlayerTurnData();
}

auto removeCardFromHand(const Player::IdView playerId, CardName card) -> void
{
    assert(ctx().player(playerId).hand.contains(card) and "card exists");
    ctx().player(playerId).hand.erase(card);
    ctx().player(playerId).playedCards.push_back(std::move(card));
}

auto dealCards() -> task<>
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
    const auto channels = players()
        | rv::transform([](const Player& player) { return std::pair{player.conn.ch, player.id}; })
        | rng::to_vector;
    assert((std::size(channels) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (auto&& [ch_id, hand] : rv::zip(channels, hands)) {
        const auto& [ch, id] = ch_id;
        co_await sendDealCardsFor(ch, id, hand | rng::to<Hand>);
    }
}

auto removePlayer(Player::Id playerId) -> task<>
{
    assert(ctx().players.contains(playerId) and "player exists");
    PREF_DI(playerId);
    ctx().players.erase(playerId);
    co_await sendPlayerLeft(std::move(playerId));
}

auto disconnected(Player::Id playerId) -> task<>
{
    PREF_DI(playerId);
    auto& player = ctx().player(playerId);
    if (not player.conn.reconnectTimer) { player.conn.reconnectTimer.emplace(player.conn.ch->get_executor()); }
    player.conn.reconnectTimer->expires_after(10s);
    if (const auto [error] = co_await player.conn.reconnectTimer->async_wait(); error) {
        if (error != net::error::operation_aborted) { PREF_DW(error); }
        co_return;
    }
    co_await removePlayer(std::move(playerId));
}

using TrickComparator = std::function<bool(int, int)>;
[[nodiscard]] constexpr auto compareTricks(const ContractLevel level) -> TrickComparator
{
    return level == ContractLevel::Miser ? TrickComparator{std::less_equal{}} : TrickComparator{std::greater_equal{}};
}

[[maybe_unused]] auto hasDeclarerFulfilledContract() -> bool
{
    return findDeclarerId()
        .transform([&](const Player::Id& declarerId) {
            const auto& declarer = ctx().players.at(declarerId);
            const auto contractLevel = makeContractLevel(declarer.bid);
            return compareTricks(contractLevel)(declarer.tricksTaken, declarerReqTricks(contractLevel));
        })
        .value_or(false);
}

auto updateScoreSheetForDeal() -> void
{
    findDeclarerId() | OnValue([&](const Player::Id& declarerId) {
        const auto& declarerPlayer = ctx().players.at(declarerId);
        const auto declarer = Declarer{
            .id = declarerId,
            .contractLevel = makeContractLevel(declarerPlayer.bid),
            .tricksTaken = declarerPlayer.tricksTaken};
        auto whisters = std::vector<Whister>{};
        for (const auto& passerPlayer : getTwoPassers()) {
            whisters.emplace_back(
                passerPlayer.get().id,
                makeWhistingChoice(passerPlayer.get().whistingChoice),
                passerPlayer.get().tricksTaken);
        }
        for (const auto& [id, entry] : calculateDealScore(declarer, whisters)) {
            ctx().scoreSheet[id].dump.push_back(entry.dump);
            ctx().scoreSheet[id].pool.push_back(entry.pool);
            if (id != declarerId) { ctx().scoreSheet[id].whists[declarerId].push_back(entry.whist); }
        }
    }) | OnNone([] { // PassGame
        const auto players = pref::players();
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

auto dealFinished() -> task<bool>
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
    PREF_DI(finalResult);
    storeGameData(ctx().gameDataPath, ctx().gameData);
    co_await sendUserGames();
    const auto pools = ctx().scoreSheet
        | rv::values
        | rv::transform(&Score::pool)
        | rv::transform([](const auto& pool) { return rng::accumulate(pool, 0); });
    const auto isGameOver = rng::all_of(pools, [](const std::int32_t pool) { return pool >= ScoreTarget; });
    PREF_DI(isGameOver, pools);
    co_await sendDealFinished(isGameOver);
    ctx().clear();
    co_return isGameOver;
}

auto updateStageGame() -> void
{
    const auto bids = players() | rv::transform(&Player::bid);
    const auto passCount = rng::count(bids, std::string_view{PREF_PASS});
    const auto activeCount = rng::count_if(bids, [](auto&& bid) { return not std::empty(bid) and bid != PREF_PASS; });
    if (std::cmp_equal(passCount, WhistersCount) and std::cmp_equal(activeCount, DeclarerCount)) {
        const auto& contract = *rng::find_if(bids, notEqualTo(PREF_PASS));
        ctx().stage = contract.contains(PREF_WT) ? GameStage::WITHOUT_TALON : GameStage::TALON_PICKING;
        return;
    }
    if (std::cmp_equal(passCount, NumberOfPlayers)) {
        ctx().passGame.update();
        ctx().stage = GameStage::PLAYING;
        return;
    }
    ctx().stage = GameStage::BIDDING;
}

auto startGame() -> task<>
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
    co_await dealCards();
    resetWhoseTurn();
    setForehandId();
    co_await sendForehand();
    ctx().stage = GameStage::BIDDING;
    co_await sendPlayerTurn(decidePlayerTurn());
}

[[nodiscard]] auto toServerAuthToken(const std::string_view authToken) -> std::string
{
    return hashToken(hex2bytes(authToken));
}

[[nodiscard]] auto generateClientAuthToken() -> std::string
{
    return bytes2hex(generateToken());
}

auto handleLoginRequest(const Message& msg, const ChannelPtr& ch) -> task<PlayerSession>
{
    auto loginRequest = makeMethod<LoginRequest>(msg);
    if (not loginRequest) { co_return PlayerSession{}; }
    auto session = PlayerSession{};
    auto& playerName = *loginRequest->mutable_player_name();
    const auto password = loginRequest->password();
    if (not verifyPlayerNameAndPassword(ctx().gameData, playerName, password)) {
        auto error = fmt::format("unknown {} or wrong password", PREF_V(playerName));
        PREF_DW(error);
        co_await sendLoginResponse(ch, std::move(error));
        co_return session;
    }
    assert(userPlayerId(ctx().gameData, playerName));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto playerId = *userPlayerId(ctx().gameData, playerName);
    auto authToken = generateClientAuthToken();
    addAuthToken(ctx().gameData, playerId, toServerAuthToken(authToken));
    storeGameData(ctx().gameDataPath, ctx().gameData);
    PREF_DI(playerName, playerId);
    session.playerName = std::move(playerName);
    if (isNewPlayer(playerId)) {
        joinPlayer(ch, playerId, session);
        co_await sendLoginResponse(ch, {}, playerId, std::move(authToken));
    } else {
        co_await sendLoginResponse(ch, {}, playerId, std::move(authToken));
        co_await reconnectPlayer(ch, playerId, session);
        co_return session;
    }
    co_await sendPlayerJoined(session);
    co_await sendUserGames();
    co_return session;
}

auto handleAuthRequest(const Message& msg, const ChannelPtr& ch) -> task<PlayerSession>
{
    const auto authRequest = makeMethod<AuthRequest>(msg);
    if (not authRequest) { co_return PlayerSession{}; }
    auto session = PlayerSession{};
    const auto playerId = authRequest->player_id();
    if (not verifyPlayerIdAndAuthToken(ctx().gameData, playerId, toServerAuthToken(authRequest->auth_token()))) {
        auto error = fmt::format("unknown {} or wrong auth token", PREF_V(playerId));
        PREF_DW(error);
        co_await sendAuthResponse(ch, std::move(error));
        co_return session;
    }
    assert(userByPlayerId(ctx().gameData, playerId));
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto playerName = userByPlayerId(ctx().gameData, playerId)->get().player_name();
    PREF_DI(playerName, playerId);
    session.playerName = playerName;
    if (isNewPlayer(playerId)) {
        joinPlayer(ch, playerId, session);
        co_await sendAuthResponse(ch, {}, playerName);
    } else {
        co_await sendAuthResponse(ch, {}, playerName);
        co_await reconnectPlayer(ch, playerId, session);
        co_return session;
    }
    co_await sendPlayerJoined(session);
    co_await sendUserGames();
    co_return session;
}

auto handleLogout(const Message& msg) -> task<>
{
    auto logout = makeMethod<Logout>(msg);
    if (not logout) { co_return; }
    auto& playerId = *logout->mutable_player_id();
    PREF_DI(playerId);
    revokeAuthToken(ctx().gameData, playerId, toServerAuthToken(logout->auth_token()));
    storeGameData(ctx().gameDataPath, ctx().gameData);
    co_await removePlayer(std::move(playerId));
}

auto handleReadyCheck(const Message& msg) -> task<>
{
    const auto readyCheck = makeMethod<ReadyCheck>(msg);
    if (not readyCheck) { co_return; }
    const auto playerId = readyCheck->player_id();
    const auto state = readyCheck->state();
    PREF_I("{}, state: {}", PREF_V(playerId), ReadyCheckState_Name(state));
    if (state == ReadyCheckState::REQUESTED) {
        for (auto& readyCheckState : players() | rv::transform(&Player::readyCheckState)) {
            readyCheckState = ReadyCheckState::NOT_REQUESTED;
        }
    }
    ctx().player(playerId).readyCheckState = (state == ReadyCheckState::REQUESTED) ? ReadyCheckState::ACCEPTED : state;
    co_await forwardToAllExcept(msg, playerId);
    if (rng::all_of(players(), equalTo(ReadyCheckState::ACCEPTED), &Player::readyCheckState)) { co_await startGame(); }
}

auto handleBidding(const Message& msg) -> task<>
{
    auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) { co_return; }
    const auto playerId = bidding->player_id();
    auto& bid = *bidding->mutable_bid();
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, bid);
    ctx().player(playerId).bid = std::move(bid);
    co_await forwardToAllExcept(msg, playerId);
    updateStageGame();
    if (ctx().passGame.now) { co_await sendOpenTalon(); }
    advanceWhoseTurn(ctx().stage);
    co_await sendPlayerTurn(decidePlayerTurn());
}

auto startPlayingFromForehand() -> task<>;

auto handleDiscardTalon(const Message& msg) -> task<>
{
    auto discardTalon = makeMethod<DiscardTalon>(msg);
    if (not discardTalon) { co_return; }
    const auto playerId = discardTalon->player_id();
    ctx().player(playerId).bid = std::move(*discardTalon->mutable_bid());
    const auto& bid = ctx().player(playerId).bid;
    for (auto& card : *discardTalon->mutable_cards()) {
        ctx().talon.discardedCards.push_back(card);
        removeCardFromHand(playerId, std::move(card));
    }
    auto& discardedCards = ctx().talon.discardedCards;
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, discardedCards, bid);
    ctx().trump = getTrump(bid);
    co_await sendBidding(playerId, bid); // final bid
    ctx().stage = bid.contains(PREF_SIX) and bid.contains(PREF_SPADE) ? GameStage::PLAYING : GameStage::WHISTING;
    if (ctx().stage == GameStage::PLAYING) { // Stalingrad
        for (auto& p : getTwoPassers()) {
            p.get().whistingChoice = PREF_WHIST;
            co_await sendWhisting(p.get().id, p.get().whistingChoice);
        }
        co_await startPlayingFromForehand();
        co_return;
    }
    advanceWhoseTurn();
    co_await sendPlayerTurn(decidePlayerTurn());
}

auto handlePingPong(const Message& msg, const ChannelPtr& ch) -> task<>
{
    const auto pingPong = makeMethod<PingPong>(msg);
    if (not pingPong) { co_return; }
    co_await sendPingPong(msg, ch);
}

auto finishDeal() -> task<>
{
    const auto isGameOver = co_await dealFinished();
    PREF_DI(isGameOver);
    if (isGameOver) {
        ctx().clear();
        ctx().stage = GameStage::UNKNOWN;
        ctx().forehandId = {};
        ctx().scoreSheet = {};
        ctx().gameStarted = {};
        ctx().gameDuration = {};
        co_return;
    }
    co_await sleepFor(3s, ctx().ex);
    co_await dealCards();
    setNextDealTurn();
    co_await sendForehand();
    ctx().stage = GameStage::BIDDING;
    co_await sendPlayerTurn(decidePlayerTurn());
}

auto updateDeclarerTakenTricks() -> void
{
    auto& declarer = getDeclarer();
    declarer.tricksTaken = declarerReqTricks(makeContractLevel(declarer.bid));
}

auto openCards() -> task<>
{
    const auto& [p0, p1] = getTwoPassers();
    co_await sendDealCardsExcept(p0.get().id, p0.get().hand);
    co_await sendDealCardsExcept(p1.get().id, p1.get().hand);
}

auto startPlayingFromForehand() -> task<>
{
    forehandsTurn();
    ctx().stage = GameStage::PLAYING;
    co_await sendPlayerTurn(decidePlayerTurn());
}

auto handleWhisting(const Message& msg) -> task<>
{
    using enum WhistingChoice;
    using enum GameStage;
    const auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { co_return; }
    const auto playerId = whisting->player_id();
    const auto choice = whisting->choice();
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, choice);
    ctx().player(playerId).whistingChoice += choice; // Pass + Whist, Pass + Pass, etc.
    co_await forwardToAllExcept(msg, playerId);
    if (Context::isHalfWhistAfterPass()) {
        advanceWhoseTurn(); // skip declarer
        advanceWhoseTurn();
        ctx().stage = WHISTING;
        co_return co_await sendPlayerTurn(decidePlayerTurn());
    }
    if (Context::isWhistAfterHalfWhist()) {
        {
            auto& whister = playerByWhistingChoice(ctx().players, HalfWhist);
            whister.whistingChoice = PREF_PASS;
            co_await sendWhisting(whister.id, whister.whistingChoice);
        }
        {
            auto& whister = playerByWhistingChoice(ctx().players, PassWhist);
            whister.whistingChoice = PREF_WHIST;
        }
        ctx().stage = HOW_TO_PLAY;
        co_return co_await sendPlayerTurn(decidePlayerTurn());
    }
    if (Context::isPassAfterHalfWhist()) {
        playerByWhistingChoice(ctx().players, PassPass).whistingChoice = PREF_PASS;
        updateDeclarerTakenTricks();
        co_return co_await finishDeal();
    }
    if (Context::areWhistersPass()) {
        updateDeclarerTakenTricks();
        co_return co_await finishDeal();
    }
    const auto& declarer = getDeclarer();
    const auto isMiser = declarer.bid.contains(PREF_MIS);
    const auto oneWhist = Context::areWhistersPassAndWhist();
    const auto bothWhist = Context::areWhistersWhist();
    const auto oneOrBothWhist = oneWhist or bothWhist;
    if (isMiser and oneOrBothWhist) [[unlikely]] {
        if (ctx().forehandId == declarer.id) {
            ctx().isDeclarerFirstMiserTurn = true;
        } else if (Context::areWhistersWhist()) {
            co_await openCards();
            co_await sendMiserCards();
        } else {
            assert(ctx().areWhistersPassAndWhist());
            co_await openCardsAndLetAnotherWhisterPlay();
            co_await sendMiserCards();
        }
        co_return co_await startPlayingFromForehand();
    }
    if (bothWhist) { co_return co_await startPlayingFromForehand(); }
    if (oneWhist) {
        if (choice != PREF_WHIST) { setWhoseTurn(playerItByWhistingChoice(ctx().players, Whist)); }
        ctx().stage = HOW_TO_PLAY;
        co_return co_await sendPlayerTurn(decidePlayerTurn());
    }
    advanceWhoseTurn();
    ctx().stage = WHISTING;
    co_return co_await sendPlayerTurn(decidePlayerTurn());
}

auto handleHowToPlay(const Message& msg) -> task<>
{
    auto howToPlay = makeMethod<HowToPlay>(msg);
    if (not howToPlay) { co_return; }
    const auto playerId = howToPlay->player_id();
    auto& choice = *howToPlay->mutable_choice();
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, choice);
    auto& player = ctx().player(playerId);
    player.howToPlayChoice = std::move(choice);
    co_await forwardToAllExcept(msg, playerId);
    if (player.howToPlayChoice == PREF_OPENLY) { co_await openCardsAndLetAnotherWhisterPlay(); }
    co_await startPlayingFromForehand();
}

auto resetPassGameIfNeeded() -> void
{
    if (ctx().passGame.round != 0 and hasDeclarerFulfilledContract()) { ctx().passGame.resetRound(); }
}

auto handleMakeOffer(const Message& msg) -> task<>
{
    const auto makeOffer = makeMethod<MakeOffer>(msg);
    if (not makeOffer) { co_return; }
    const auto playerId = makeOffer->player_id();
    const auto offerRequest = makeOffer->offer();
    auto& player = ctx().player(playerId);
    auto& declarer = getDeclarer();
    const auto isMiser = declarer.bid.contains(PREF_MIS);
    if (offerRequest == Offer::OFFER_REQUESTED) {
        if (player.offer != Offer::OFFER_REQUESTED) {
            co_await sendDealCardsExcept(player.id, player.hand);
            if (isMiser) { co_await sendMiserCards(); }
        }
        for (auto& offer : players() | rv::filter(notEqualTo(playerId), &Player::id) | rv::transform(&Player::offer)) {
            offer = Offer::NO_OFFER;
        }
    }
    co_await forwardToAllExcept(msg, playerId);
    player.offer = offerRequest;
    if (rng::count_if(players(), equalTo(Offer::OFFER_ACCEPTED), &Player::offer) == std::ssize(getOneOrTwoWhisters())) {
        auto& whomAddTricks = std::invoke([&] -> Player& {
            if (isMiser) { return playerByWhistingChoice(ctx().players, WhistingChoice::Whist); }
            return declarer;
        });
        whomAddTricks.tricksTaken += static_cast<int>(std::size(declarer.hand));
        resetPassGameIfNeeded();
        // TODO: create GameState once
        // TODO: send only taken tricks
        for (const auto& p : players()) { co_await sendGameState(p.conn.ch); }
        co_await finishDeal();
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto handlePlayCard(const Message& msg) -> task<>
{
    auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) { co_return; }
    const auto playerId = playCard->player_id();
    auto& card = *playCard->mutable_card();
    const auto playerName = ctx().playerName(playerId);
    PREF_DI(playerName, playerId, card);
    ctx().trick.emplace_back(std::string{playerId}, card);
    removeCardFromHand(playerId, std::move(card));
    co_await forwardToAll(msg);
    if (ctx().isDeclarerFirstMiserTurn) {
        ctx().isDeclarerFirstMiserTurn = false;
        if (Context::areWhistersWhist()) {
            co_await openCards();
        } else {
            assert(Context::areWhistersPassAndWhist());
            co_await openCardsAndLetAnotherWhisterPlay();
        }
    }
    if (const auto isNotTrickFinished = (std::size(ctx().trick) != 3); isNotTrickFinished) {
        advanceWhoseTurn();
    } else {
        const auto winnerId = decideTrickWinner();
        co_await sendTrickFinished();
        if (const auto isDealFinished = rng::all_of(players(), &Hand::empty, &Player::hand); isDealFinished) {
            resetPassGameIfNeeded();
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
    if (const auto declarerId = findDeclarerId(); declarerId) {
        const auto isMiser = ctx().player((*declarerId).get()).bid.contains(PREF_MIS);
        if (isMiser and (*declarerId).get() == playerId) { co_await sendMiserCards(); }
    }
    ctx().stage = GameStage::PLAYING;
    co_await sendPlayerTurn(decidePlayerTurn());
}

auto handleLog(const Message& msg) -> void
{
    const auto log = makeMethod<Log>(msg);
    if (not log) { return; }
    PREF_I("[client] {}, playerId: {}", log->text(), log->player_id());
}

auto handleSpeechBubble(const Message& msg) -> task<>
{
    const auto speechBubble = makeMethod<SpeechBubble>(msg);
    if (not speechBubble) { co_return; }
    const auto playerId = speechBubble->player_id();
    PREF_DI(playerId);
    co_await forwardToAllExcept(msg, playerId);
}

auto handleAudioSignal(const Message& msg) -> task<>
{
    const auto audioSignal = makeMethod<AudioSignal>(msg);
    if (not audioSignal) { co_return; }
    const auto fromPlayerId = audioSignal->from_player_id();
    const auto toPlayerId = audioSignal->to_player_id();
    PREF_DI(fromPlayerId, toPlayerId);
    co_await forwardToOne(toPlayerId, msg);
}

auto dispatchMessage(const ChannelPtr& ch, PlayerSession& session, std::optional<Message> msg) -> task<>
{ // clang-format off
    if (not msg) { co_return; }
    const auto method = msg->method();
    if (method == "LoginRequest") { session = co_await handleLoginRequest(*msg, ch); co_return; }
    if (method == "AuthRequest") { session = co_await handleAuthRequest(*msg, ch); co_return; }
    if (session.id == 0) { co_return; }
    if (method == "Logout") { co_await handleLogout(*msg); co_return; }
    if (method == "ReadyCheck") { co_await handleReadyCheck(*msg); co_return; }
    if (method == "Bidding") { co_await handleBidding(*msg); co_return; }
    if (method == "DiscardTalon") { co_await handleDiscardTalon(*msg); co_return; }
    if (method == "Whisting") { co_await handleWhisting(*msg); co_return; }
    if (method == "HowToPlay") { co_await handleHowToPlay(*msg); co_return; }
    if (method == "MakeOffer") { co_await handleMakeOffer(*msg); co_return; }
    if (method == "PlayCard") { co_await handlePlayCard(*msg); co_return; }
    if (method == "SpeechBubble") { co_await handleSpeechBubble(*msg); co_return; }
    if (method == "PingPong") { co_await handlePingPong(*msg, ch); co_return; }
    if (method == "Log") { handleLog(*msg); co_return; }
    if (method == "AudioSignal") { co_await handleAudioSignal(*msg); co_return; }
    PREF_W("error: unknown {}", PREF_V(method));
} // clang-format on

auto launchSession(Stream ws) -> task<>
{
    ws.binary(true);
    ws.set_option(web::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(web::stream_base::decorator([](web::response_type& res) {
        res.set(beast::http::field::server, std::string{BOOST_BEAST_VERSION_STRING} + " preferans-server");
    }));
    auto scp = ex::async_scope{};
    auto buf = beast::flat_buffer{};
    auto chn = std::shared_ptr<Channel>{};
    auto sch = co_await stdx::get_scheduler();
    auto ssn = PlayerSession{};
    co_await (
#ifdef PREF_SSL
        ws.next_layer().async_handshake(net::ssl::stream_base::server, netx::use_sender)
        | stdx::let_value([&] { return ws.async_accept(netx::use_sender); })
#else // PREF_SSL
        ws.async_accept(netx::use_sender)
#endif // PREF_SSL
        | stdx::then([&] {
              static constexpr auto channelSize = 128;
              chn = std::make_shared<Channel>(ws.get_executor(), channelSize);
              scp.spawn(stdx::starts_on(sch, payloadSender(ws, *chn)));
          })
        | stdx::let_value([&] {
              return ex::repeat_effect_until(
                  ws.async_read(buf, netx::use_sender)
                  | stdx::let_value([&](const std::uint64_t bytes) {
                        assert(bytes == buf.size());
                        auto _ = ex::scope_guard{[&] noexcept { buf.consume(buf.size()); }};
                        return dispatchMessage(chn, ssn, makeMessage(buf.data().data(), buf.size()));
                    })
                  | stdx::then([&] { return ssn.id == 0; })
                  | stdx::upon_stopped([] {
                        PREF_I("[launchSession] Stream canceled");
                        return true;
                    })
                  | stdx::upon_error([](const std::exception_ptr& error) {
                        PrintError("launchSession", error);
                        return true;
                    }));
          })
        | stdx::then([&] {
              if (chn) {
                  assert(chn->is_open());
                  scp.request_stop();
              }
              const auto& playerId = ssn.playerId;
              if (std::empty(playerId)
                  or not ctx().players.contains(playerId)
                  or ssn.id == 0
                  or ssn.id != ctx().player(playerId).sessionId) {
                  return;
              }
              stdx::start_detached(
                  stdx::starts_on(sch, disconnected(playerId) | stdx::upon_error(Detached("disconnected"))));
          })
        | stdx::upon_error([](const std::exception_ptr& error) { PrintError("launchSession", error); }));
    co_await scp.on_empty();
}

} // namespace

Player::Player(Id aId, Name aName, PlayerSession::Id aSessionId, const ChannelPtr& ch)
    : id{std::move(aId)}
    , name{std::move(aName)}
    , sessionId{aSessionId}
    , conn{ch}
{
}

auto Context::whoseTurnId() const -> Player::IdView
{
    return whoseTurnIt->first;
}

auto Context::player(const Player::IdView playerId) const -> Player&
{
    assert(players.contains(playerId) and "player exists");
    return players.find(playerId)->second;
}

auto Context::playerName(const Player::IdView playerId) const -> Player::NameView
{
    return player(playerId).name;
}

auto Context::areWhistersPass() -> bool
{
    return 2 == countWhistingChoice(WhistingChoice::Pass);
}

auto Context::areWhistersWhist() -> bool
{
    return 2 == countWhistingChoice(WhistingChoice::Whist);
}

auto Context::areWhistersPassAndWhist() -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::Pass) //
        and 1 == countWhistingChoice(WhistingChoice::Whist);
}

auto Context::isHalfWhistAfterPass() -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::Pass) //
        and 1 == countWhistingChoice(WhistingChoice::HalfWhist);
}

auto Context::isPassAfterHalfWhist() -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::PassPass);
}

auto Context::isWhistAfterHalfWhist() -> bool
{
    return 1 == countWhistingChoice(WhistingChoice::PassWhist);
}

auto Context::countWhistingChoice(const WhistingChoice choice) -> std::ptrdiff_t
{
    const auto values = pref::players();
    return rng::count_if(
        values
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

auto createAcceptor(
#ifdef PREF_SSL
    net::ssl::context ssl,
#endif // PREF_SSL
    tcp::endpoint endpoint,
    net::any_io_executor ex) -> task<>
{
    PREF_I();
    auto acceptor = Acceptor{ex, endpoint};
    auto sch = co_await stdx::get_scheduler();
    co_await ex::repeat_effect_until(
        acceptor.async_accept()
        | stdx::then([&](auto socket) {
#ifdef PREF_SSL
              auto ws = Stream{std::move(socket), ssl};
#else // PREF_SSL
              auto ws = Stream{std::move(socket)};
#endif // PREF_SSL
              stdx::start_detached(stdx::starts_on(sch, launchSession(std::move(ws))));
              return false;
          })
        | stdx::upon_error([](const std::exception_ptr& error) {
              PrintError("createAcceptor", error);
              return true;
          }));
}

} // namespace pref
