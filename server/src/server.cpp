#include "server.hpp"

#include "auth.hpp"
#include "common/common.hpp"
#include "common/time.hpp"
#include "game_data.hpp"

#include <fmt/ranges.h>
#include <range/v3/all.hpp>

#include <array>
#include <cassert>
#include <functional>
#include <gsl/gsl>
#include <iterator>
#include <utility>
#include <variant>

namespace pref {
namespace {

// NOLINTBEGIN(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)

using net::ip::tcp;

auto sendTrickFinished(const Context::Players& players) -> Awaitable<>;
auto dealFinished() -> Awaitable<>;

[[nodiscard]] auto makeMessage(const beast::flat_buffer& buffer) -> std::optional<Message>
{
    if (auto result = Message{}; result.ParseFromArray(buffer.data().data(), gsl::narrow<int>(buffer.size()))) {
        return result;
    }
    PREF_WARN("error: failed to make Message from array");
    return {};
}

auto setWhoseTurn(const auto& it) -> void
{
    ctx().whoseTurnIt = it;
    const auto& [playerId, player] = *(ctx().whoseTurnIt);
    INFO_VAR(player.name, playerId);
}

auto setForehandId() -> void
{
    ctx().forehandId = ctx().whoseTurnId();
    PREF_INFO("playerId: {}", ctx().forehandId);
}

auto resetWhoseTurn() -> void
{
    assert((std::size(ctx().players) == NumberOfPlayers) and "all players joined");
    setWhoseTurn(std::cbegin(ctx().players));
}

auto advanceWhoseTurn() -> void
{
    PREF_INFO();
    assert((std::size(ctx().players) == NumberOfPlayers) and "all players joined");
    if (const auto nextTurnIt = std::next(ctx().whoseTurnIt); nextTurnIt != std::cend(ctx().players)) {
        setWhoseTurn(nextTurnIt);
        return;
    }
    resetWhoseTurn();
}

auto forehandsTurn() -> void
{
    PREF_INFO();
    assert(ctx().players.contains(ctx().forehandId) and "forehand player exists");
    setWhoseTurn(ctx().players.find(ctx().forehandId));
}

auto advanceWhoseTurn(const GameStage stage) -> void
{
    PREF_INFO("stage: {}", GameStage_Name(stage));
    advanceWhoseTurn();
    if (not rng::contains(std::array{GameStage::BIDDING, GameStage::TALON_PICKING}, stage)) { return; }
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

[[nodiscard]] auto finishTrick() -> Player::Id
{
    const auto winnerId = finishTrick(ctx().trick, ctx().trump);
    const auto winnerName = ctx().playerName(winnerId);
    const auto tricksTaken = ++ctx().player(winnerId).tricksTaken;
    INFO_VAR(winnerName, winnerId, tricksTaken);
    ctx().trick.clear();
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

[[nodiscard]] auto findDeclarerId(const Context::Players& players)
{
    const auto values = players | rv::values;
    return pref::findIf(values, notEqualTo(PREF_PASS), &Player::bid, &Player::id);
}

[[nodiscard]] auto getDeclarer(Context::Players& players) -> Player&
{
    const auto declarerId = findDeclarerId(players);
    assert(declarerId and players.contains(*declarerId) and "declarer exists");
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

auto joinPlayer(
    const Player::Id& playerId, const std::shared_ptr<Stream>& ws, Context::Players& players, PlayerSession& session)
    -> void
{
    session.playerId = playerId;
    ++session.id;
    assert(session.id == 1);
    INFO_VAR(session.playerId, session.playerName, session.id);
    players.emplace(session.playerId, Player{session.playerId, session.playerName, session.id, ws});
}

auto prepareNewSession(const Player::Id& playerId, Context::Players& players, PlayerSession& session) -> Awaitable<>
{
    PREF_INFO("{}, {}, {}{}", VAR(playerId), VAR(session.playerName), VAR(session.id), VAR_MB(session.playerId));
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

auto replaceStream(const Player::Id& playerId, const std::shared_ptr<Stream>& ws) -> void
{
    ctx().player(playerId).ws = ws;
}

auto reconnectPlayer(
    const std::shared_ptr<Stream>& ws, const Player::Id& playerId, Context::Players& players, PlayerSession& session)
    -> Awaitable<>
{
    co_await prepareNewSession(playerId, players, session);
    replaceStream(playerId, ws);
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

auto forwardToAll(Message msg) -> Awaitable<>
{
    return sendToAll(std::move(msg), ctx().players);
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

auto forwardToAllExcept(Message msg, const Player::Id& excludedId) -> Awaitable<>
{
    return sendToAllExcept(std::move(msg), ctx().players, excludedId);
}

auto addCardToHand(const Player::Id& playerId, const std::string& card) -> void
{
    INFO_VAR(playerId, card);
    assert(not ctx().player(playerId).hand.contains(card) and "card doesn't exists");
    ctx().player(playerId).hand.insert(card);
}

[[nodiscard]] auto makePlayerJoined(const PlayerSession& session) -> PlayerJoined
{
    INFO_VAR(session.playerId, session.playerName);
    auto result = PlayerJoined{};
    result.set_player_id(session.playerId);
    result.set_player_name(session.playerName);
    return result;
}

[[nodiscard]] auto makeLoginResponse(const std::string& error, const Player::Id& playerId, const std::string& authToken)
    -> LoginResponse
{
    auto result = LoginResponse{};
    if (not std::empty(error)) {
        result.set_error(error);
        return result;
    }
    result.set_player_id(playerId);
    result.set_auth_token(authToken);
    for (const auto& [id, player] : ctx().players) {
        auto p = result.add_players();
        p->set_player_id(id);
        p->set_player_name(player.name);
    }
    return result;
}

[[nodiscard]] auto makeAuthResponse(const std::string& error, const Player::Name& playerName) -> AuthResponse
{
    auto result = AuthResponse{};
    if (not std::empty(error)) {
        result.set_error(error);
        return result;
    }
    result.set_player_name(playerName);
    for (const auto& [id, player] : ctx().players) {
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

[[nodiscard]] auto makeDealCards(const Player::Id& playerId, const Hand& hand) -> DealCards
{
    auto result = DealCards{};
    result.set_player_id(playerId);
    for (const auto& card : hand) { *result.add_cards() = card; }
    return result;
}

[[nodiscard]] auto makePlayerTurn(const pref::GameStage stage) -> PlayerTurn
{
    auto result = PlayerTurn{};
    const auto& playerId = ctx().whoseTurnId();
    result.set_player_id(playerId);
    result.set_stage(stage);
    result.set_can_half_whist(false); // default
    if (stage == GameStage::TALON_PICKING) {
        assert((std::size(ctx().talon.cards) == 2) and "talon is two cards");
        for (const auto& card : ctx().talon.cards) {
            addCardToHand(playerId, card);
            result.add_talon(card);
        }
    } else if (stage == GameStage::WHISTING) {
        if (const auto contractLevel = makeContractLevel(getDeclarer(ctx().players).bid);
            contractLevel == ContractLevel::Six or contractLevel == ContractLevel::Seven) {
            const auto canHalfWhist = [&](const Player& self, const Player& other) {
                return self.id == playerId and std::empty(self.whistingChoice) and other.whistingChoice == PREF_PASS;
            };
            if (const auto& [w0, w1] = getWhisters(ctx().players); canHalfWhist(w0, w1) or canHalfWhist(w1, w0)) {
                result.set_can_half_whist(true);
            }
        }
    }
    const auto playerName = ctx().playerName(playerId);
    PREF_INFO("{}, {}, stage: {}", VAR(playerId), VAR(playerName), GameStage_Name(stage));
    return result;
}

[[nodiscard]] auto makeBidding(const Player::Id& playerId, const std::string& bid) -> Bidding
{
    auto result = Bidding{};
    result.set_player_id(playerId);
    result.set_bid(bid);
    ctx().trump = std::string{getTrump(bid)};
    const auto playerName = ctx().playerName(playerId);
    INFO_VAR(playerName, playerId, bid, ctx().trump);
    return result;
}

[[nodiscard]] auto makeWhisting(const Player::Id& playerId, const std::string& choice) -> Whisting
{
    auto result = Whisting{};
    result.set_player_id(playerId);
    result.set_choice(choice);
    return result;
}

[[nodiscard]] auto makeOpenWhistPlay(const Player::Id& activeWhisterId, const Player::Id& passiveWhisterId)
    -> OpenWhistPlay
{
    INFO_VAR(activeWhisterId, passiveWhisterId);
    auto result = OpenWhistPlay{};
    result.set_active_whister_id(activeWhisterId);
    result.set_passive_whister_id(passiveWhisterId);
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

[[nodiscard]] auto makeOpenTalon(const CardName& card) -> OpenTalon
{
    INFO_VAR(card);
    auto result = OpenTalon{};
    result.set_card(card);
    return result;
}

auto sendPlayerJoined(const PlayerSession& session) -> Awaitable<>
{
    return sendToAllExcept(makeMessage(makePlayerJoined(session)), ctx().players, session.playerId);
}

auto sendLoginResponse(
    const std::shared_ptr<Stream>& ws,
    const std::string& errorMsg,
    const Player::Id& playerId = {},
    const std::string& authToken = {}) -> Awaitable<>
{
    if (const auto error = co_await sendToOne(makeMessage(makeLoginResponse(errorMsg, playerId, authToken)), ws)) {
        PREF_WARN("{}: failed to send LoginResponse", VAR(error));
    }
}

auto sendAuthResponse(
    const std::shared_ptr<Stream>& ws, const std::string& errorMsg, const Player::Name& playerName = {}) -> Awaitable<>
{
    if (const auto error = co_await sendToOne(makeMessage(makeAuthResponse(errorMsg, playerName)), ws)) {
        PREF_WARN("{}: failed to send AuthResponse", VAR(error));
    }
}

auto sendPlayerLeft(const Player::Id& playerId) -> Awaitable<>
{
    return sendToAll(makeMessage(makePlayerLeft(playerId)), ctx().players);
}

auto sendDealCards(const Player::Id& playerId, const Hand& hand) -> Awaitable<>
{
    return sendToAllExcept(makeMessage(makeDealCards(playerId, hand)), ctx().players, playerId);
}

auto sendDealCards(const std::shared_ptr<Stream>& ws, const Player::Id& playerId, const Hand& hand) -> Awaitable<>
{
    if (const auto error = co_await sendToOne(makeMessage(makeDealCards(playerId, hand)), ws)) {
        PREF_WARN("{}: failed to send DealCards to {}", VAR(error), VAR(playerId));
    }
}

auto sendPlayerTurn(const GameStage stage) -> Awaitable<>
{
    return sendToAll(makeMessage(makePlayerTurn(stage)), ctx().players);
}

auto sendBidding(const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    return sendToAllExcept(makeMessage(makeBidding(playerId, bid)), ctx().players, playerId);
}

auto sendWhisting(const Context::Players& players, const Player::Id& playerId, const std::string& choice) -> Awaitable<>
{
    return sendToAll(makeMessage(makeWhisting(playerId, choice)), players);
}

auto sendOpenWhistPlay(const Player::Id& activeWhisterId, const Player::Id& passiveWhisterId) -> Awaitable<>
{
    return sendToAll(makeMessage(makeOpenWhistPlay(activeWhisterId, passiveWhisterId)), ctx().players);
}

auto sendTrickFinished(const Context::Players& players) -> Awaitable<>
{
    return sendToAll(makeMessage(makeTrickFinished(players)), players);
}

auto sendDealFinished() -> Awaitable<>
{
    return sendToAll(makeMessage(makeDealFinished(ctx().scoreSheet)), ctx().players);
}

auto sendPingPong(const Message& msg, const std::shared_ptr<Stream>& ws) -> Awaitable<>
{
    if (const auto error = co_await sendToOne(msg, ws)) { PREF_WARN("{}: failed to send PingPong", VAR(error)); }
}

auto sendUserGames() -> Awaitable<>
{
    for (const auto& player : ctx().players | rv::values) {
        if (const auto error = co_await sendToOne(makeMessage(makeUserGames(ctx().gameData, player.id)), player.ws)) {
            PREF_WARN("{}: failed to send UserGames to {}", VAR(error), VAR(player.id));
        }
    }
}

auto sendOpenTalon() -> Awaitable<>
{
    assert(ctx().talon.open < std::size(ctx().talon.cards));
    ctx().talon.current = ctx().talon.cards[ctx().talon.open];
    return sendToAll(makeMessage(makeOpenTalon(ctx().talon.current)), ctx().players);
}

auto removeCardFromHand(const Player::Id& playerId, const std::string& card) -> void
{
    INFO_VAR(playerId, card);
    assert(ctx().player(playerId).hand.contains(card) and "card exists");
    ctx().player(playerId).hand.erase(card);
}

auto dealCards() -> Awaitable<>
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
    ctx().talon.cards = chunks | rv::drop(NumberOfPlayers) | rv::join | rng::to_vector;
    assert((std::size(ctx().talon.cards) == 2) and "talon is two cards");
    assert((std::size(ctx().players) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (const auto& [playerId, hand] : rv::zip(ctx().players | rv::keys, hands)) {
        ctx().player(playerId).hand = hand | rng::to<Hand>;
    }
    PREF_INFO("talon: {}, {}", ctx().talon.cards, VAR(hands));
    const auto wss = ctx().players
        | rv::values
        | rv::transform([](const Player& player) { return std::pair{player.id, player.ws}; })
        | rng::to_vector;
    assert((std::size(wss) == NumberOfPlayers) and (std::size(hands) == NumberOfPlayers));
    for (const auto& [id_ws, hand] : rv::zip(wss, hands)) {
        const auto& [id, ws] = id_ws;
        co_await sendDealCards(ws, id, hand | rng::to<Hand>);
    }
}

auto removePlayer(const Player::Id playerId) -> Awaitable<>
{
    assert(ctx().players.contains(playerId) and "player exists");
    INFO_VAR(playerId);
    ctx().players.erase(playerId);
    co_await sendPlayerLeft(playerId);
}

auto disconnected(Player::Id playerId) -> Awaitable<>
{
    INFO_VAR(playerId);
    auto& player = ctx().player(playerId);
    if (not player.reconnectTimer) { player.reconnectTimer.emplace(co_await net::this_coro::executor); }
    player.reconnectTimer->expires_after(10s);
    if (const auto [error] = co_await player.reconnectTimer->async_wait(net::as_tuple); error) {
        if (error != net::error::operation_aborted) { WARN_VAR(error); }
        co_return;
    }
    co_await removePlayer(std::move(playerId));
}

auto updateScoreSheetForDeal() -> void
{
    findDeclarerId(ctx().players) | onValue([&](const Player::Id& declarerId) {
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
    }) | onNone([] {
        const auto players = ctx().players | rv::values;
        const auto minTricksTaken = rng::min(players | rv::transform(&Player::tricksTaken));
        for (const auto& player : players) {
            // TODO: consider the level of pass game
            if (const auto price = contractPrice(ContractLevel::Six); player.tricksTaken == 0) {
                ctx().scoreSheet[player.id].pool.push_back(price);
                return;
            } else {
                ctx().scoreSheet[player.id].dump.push_back((player.tricksTaken - minTricksTaken) * price);
            }
        }
    });
}

auto dealFinished() -> Awaitable<>
{
    ctx().gameDuration = pref::durationInSec(ctx().gameStarted);
    PREF_INFO(
        "Game duration: {}, duration: {}, started: {}",
        formatDuration(ctx().gameDuration),
        ctx().gameDuration,
        ctx().gameStarted);
    updateScoreSheetForDeal();
    const auto finalResult = calculateFinalResult(makeFinalScore(ctx().scoreSheet));
    for (const auto& [playerId, score] : ctx().scoreSheet) {
        INFO_VAR(playerId, score.dump, score.pool);
        auto totalWhists = 0;
        for (const auto& [id, whists] : score.whists) {
            PREF_INFO("whists: {} -> {}", whists, id);
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
    PREF_INFO("gameData: {}", formatGameData(ctx().gameData));
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
    INFO_VAR(passCount, activeCount);
    if (passCount == WhistersCount and activeCount == DeclarerCount) {
        const auto& contract = *rng::find_if(bids, notEqualTo(PREF_PASS));
        return contract.contains(PREF_WT) ? GameStage::WHISTING : GameStage::TALON_PICKING;
    }
    if (passCount == NumberOfPlayers) {
        ctx().isPassGame = true;
        return GameStage::PLAYING;
    }
    return GameStage::BIDDING;
}

auto maybeStartGame() -> Awaitable<>
{
    if (std::size(ctx().players) != NumberOfPlayers) { co_return; }
    // TODO: use UTC on the server and local time zone on the client
    ctx().gameStarted = localTimeSinceEpochInSec();
    ++ctx().gameId;
    PREF_INFO("gameId: {} started: {} {}", ctx().gameId, formatDate(ctx().gameStarted), formatTime(ctx().gameStarted));
    for (const auto& id : ctx().players | rv::keys) {
        addOrUpdateUserGame(ctx().gameData, id, makeUserGame(ctx().gameId, GameType::RANKED, ctx().gameStarted));
    }
    storeGameData(ctx().gameDataPath, ctx().gameData);
    PREF_INFO("gameData: {}", formatGameData(ctx().gameData));
    co_await sendUserGames();
    co_await dealCards();
    resetWhoseTurn();
    setForehandId();
    co_await sendPlayerTurn(GameStage::BIDDING);
}

[[nodiscard]] auto toServerAuthToken(const std::string_view authToken) -> std::string
{
    return hashToken(hex2bytes(authToken));
}

[[nodiscard]] auto generateClientAuthToken() -> std::string
{
    return bytes2hex(generateToken());
}

auto handleLoginRequest(const Message msg, const std::shared_ptr<Stream>& ws) -> Awaitable<PlayerSession>
{
    auto loginRequest = makeMethod<LoginRequest>(msg);
    if (not loginRequest) { co_return PlayerSession{}; }
    auto session = PlayerSession{};
    const auto& playerName = loginRequest->player_name();
    const auto& password = loginRequest->password();
    if (not verifyPlayerNameAndPassword(ctx().gameData, playerName, password)) {
        const auto error = fmt::format("unknown {} or wrong password", VAR(playerName));
        WARN_VAR(error);
        co_await sendLoginResponse(ws, error);
        co_return session;
    }
    assert(userPlayerId(ctx().gameData, playerName));
    const auto& playerId = userPlayerId(ctx().gameData, playerName)->get();
    const auto authToken = generateClientAuthToken();
    addAuthToken(ctx().gameData, playerId, toServerAuthToken(authToken));
    storeGameData(ctx().gameDataPath, ctx().gameData);
    INFO_VAR(playerName, playerId);
    session.playerName = playerName;
    if (isNewPlayer(playerId, ctx().players)) {
        joinPlayer(playerId, ws, ctx().players, session);
        co_await sendLoginResponse(ws, {}, playerId, authToken);
    } else {
        co_await reconnectPlayer(ws, playerId, ctx().players, session);
        co_await sendLoginResponse(ws, {}, playerId, authToken);
        co_return session;
    }
    co_await sendPlayerJoined(session);
    co_await maybeStartGame();
    co_return session;
}

auto handleAuthRequest(const Message msg, const std::shared_ptr<Stream>& ws) -> Awaitable<PlayerSession>
{
    auto authRequest = makeMethod<AuthRequest>(msg);
    if (not authRequest) { co_return PlayerSession{}; }
    auto session = PlayerSession{};
    const auto& playerId = authRequest->player_id();
    if (not verifyPlayerIdAndAuthToken(ctx().gameData, playerId, toServerAuthToken(authRequest->auth_token()))) {
        const auto error = fmt::format("unknown {} or wrong auth token", VAR(playerId));
        WARN_VAR(error);
        co_await sendAuthResponse(ws, error);
        co_return session;
    }
    assert(userByPlayerId(ctx().gameData, playerId).has_value());
    const auto& playerName = userByPlayerId(ctx().gameData, playerId)->get().player_name();
    INFO_VAR(playerName, playerId);
    session.playerName = playerName;
    if (isNewPlayer(playerId, ctx().players)) {
        joinPlayer(playerId, ws, ctx().players, session);
        co_await sendAuthResponse(ws, {}, playerName);
    } else {
        co_await reconnectPlayer(ws, playerId, ctx().players, session);
        co_await sendAuthResponse(ws, {}, playerName);
        co_return session;
    }
    co_await sendPlayerJoined(session);
    co_await maybeStartGame();
    co_return session;
}

auto handleLogout(const Message& msg) -> Awaitable<>
{
    auto logout = makeMethod<Logout>(msg);
    if (not logout) { co_return; }
    revokeAuthToken(ctx().gameData, logout->player_id(), toServerAuthToken(logout->auth_token()));
    storeGameData(ctx().gameDataPath, ctx().gameData);
    co_await removePlayer(std::move(*logout->mutable_player_id()));
}

auto handleBidding(Message msg) -> Awaitable<>
{
    const auto bidding = makeMethod<Bidding>(msg);
    if (not bidding) { co_return; }
    const auto& playerId = bidding->player_id();
    const auto& bid = bidding->bid();
    const auto& playerName = ctx().playerName(playerId);
    INFO_VAR(playerName, playerId, bid);
    ctx().player(playerId).bid = bid;
    co_await forwardToAllExcept(std::move(msg), playerId);
    const auto stage = stageGame();
    if (ctx().isPassGame) { co_await sendOpenTalon(); }
    advanceWhoseTurn(stage);
    co_await sendPlayerTurn(stage);
}

auto handleDiscardTalon(const Message& msg) -> Awaitable<>
{
    const auto discardTalon = makeMethod<DiscardTalon>(msg);
    if (not discardTalon) { co_return; }
    const auto& playerId = discardTalon->player_id();
    const auto& bid = discardTalon->bid();
    ctx().player(playerId).bid = bid;
    const auto& discardedCards = discardTalon->cards();
    for (const auto& card : discardedCards) { removeCardFromHand(playerId, card); }
    const auto playerName = ctx().playerName(playerId);
    INFO_VAR(playerName, playerId, discardedCards, bid);
    co_await sendBidding(playerId, bid); // final bid
    const auto gameStage = bid.contains(SIX) and bid.contains(SPADE) ? GameStage::PLAYING : GameStage::WHISTING;
    if (gameStage == GameStage::PLAYING) { // Stalingrad
        const auto whist = std::string{PREF_WHIST};
        for (const auto& w : getWhisters(ctx().players)) { co_await sendWhisting(ctx().players, w.get().id, whist); }
    }
    advanceWhoseTurn();
    co_await sendPlayerTurn(gameStage);
}

auto handlePingPong(Message msg, const std::shared_ptr<Stream>& ws) -> Awaitable<>
{
    const auto pingPong = makeMethod<PingPong>(msg);
    if (not pingPong) { co_return; }
    co_await sendPingPong(msg, ws);
}

auto finishDeal() -> Awaitable<>
{
    co_await dealFinished();
    co_await dealCards();
    setNextDealTurn();
    co_await sendPlayerTurn(GameStage::BIDDING);
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
    }, ToPlayer); // clang-format on
    assert(it != rng::end(players) and "player with the given choice exists");
    return it;
}

[[nodiscard]] auto playerByWhistingChoice(Context::Players& players, const WhistingChoice choice) -> Player&
{
    return playerItByWhistingChoice(players, choice)->second;
}

auto openCardsAndLetAnotherWhisterPlay() -> Awaitable<>
{
    const auto& activeWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Whist);
    const auto& passiveWhister = playerByWhistingChoice(ctx().players, WhistingChoice::Pass);
    co_await sendOpenWhistPlay(activeWhister.id, passiveWhister.id);
    co_await sendDealCards(activeWhister.id, activeWhister.hand);
    co_await sendDealCards(passiveWhister.id, passiveWhister.hand);
}

auto openCards() -> Awaitable<>
{
    const auto& [w0, w1] = getWhisters(ctx().players);
    co_await sendDealCards(w0.get().id, w0.get().hand);
    co_await sendDealCards(w1.get().id, w1.get().hand);
}

auto startPlayingFromForehand() -> Awaitable<>
{
    forehandsTurn();
    co_await sendPlayerTurn(GameStage::PLAYING);
}

auto handleWhisting(Message msg) -> Awaitable<>
{
    using enum WhistingChoice;
    using enum GameStage;
    auto whisting = makeMethod<Whisting>(msg);
    if (not whisting) { co_return; }
    const auto& playerId = whisting->player_id();
    const auto& choice = whisting->choice();
    const auto playerName = ctx().playerName(playerId);
    INFO_VAR(playerName, playerId, choice);
    ctx().player(playerId).whistingChoice += choice; // Pass + Whist, Pass + Pass, etc.
    co_await forwardToAllExcept(std::move(msg), playerId);
    if (ctx().isHalfWhistAfterPass()) {
        advanceWhoseTurn(); // skip declarer
        advanceWhoseTurn();
        co_return co_await sendPlayerTurn(WHISTING);
    }
    if (ctx().isWhistAfterHalfWhist()) {
        {
            auto& whister = playerByWhistingChoice(ctx().players, HalfWhist);
            auto pass = std::string{PREF_PASS};
            co_await sendWhisting(ctx().players, whister.id, pass);
            whister.whistingChoice = std::move(pass);
        }
        {
            auto& whister = playerByWhistingChoice(ctx().players, PassWhist);
            whister.whistingChoice = PREF_WHIST;
        }
        co_return co_await sendPlayerTurn(HOW_TO_PLAY);
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
    const bool isMiser = declarer.bid.contains(PREF_MIS);
    const auto oneWhist = ctx().areWhistersPassAndWhist();
    const auto bothWhist = ctx().areWhistersWhist();
    const bool whist = oneWhist or bothWhist;
    if (isMiser and whist) {
        if (ctx().forehandId == declarer.id) {
            ctx().isDeclarerFirstMiserTurn = true;
        } else if (ctx().areWhistersWhist()) {
            co_await openCards();
        } else {
            assert(ctx().areWhistersPassAndWhist());
            co_await openCardsAndLetAnotherWhisterPlay();
        }
        co_return co_await startPlayingFromForehand();
    }
    if (bothWhist) { co_return co_await startPlayingFromForehand(); }
    if (oneWhist) {
        if (choice != PREF_WHIST) { setWhoseTurn(playerItByWhistingChoice(ctx().players, Whist)); }
        co_return co_await sendPlayerTurn(HOW_TO_PLAY);
    }
    advanceWhoseTurn();
    co_return co_await sendPlayerTurn(WHISTING);
}

auto handleHowToPlay(Message msg) -> Awaitable<>
{
    auto howToPlay = makeMethod<HowToPlay>(msg);
    if (not howToPlay) { co_return; }
    const auto& playerId = howToPlay->player_id();
    const auto& choice = howToPlay->choice();
    const auto playerName = ctx().playerName(playerId);
    INFO_VAR(playerName, playerId, choice);
    auto& player = ctx().player(playerId);
    player.howToPlayChoice = choice;
    co_await forwardToAllExcept(std::move(msg), playerId);
    if (choice == "Openly") { co_await openCardsAndLetAnotherWhisterPlay(); }
    co_await startPlayingFromForehand();
}

auto handlePlayCard(Message msg) -> Awaitable<>
{
    const auto playCard = makeMethod<PlayCard>(msg);
    if (not playCard) { co_return; }
    const auto& playerId = playCard->player_id();
    const auto& card = playCard->card();
    const auto playerName = ctx().playerName(playerId);
    removeCardFromHand(playerId, card);
    ctx().trick.emplace_back(playerId, card);
    INFO_VAR(playerName, playerId, card);
    co_await forwardToAll(std::move(msg));
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
        co_await sendTrickFinished(ctx().players);
        if (const auto isDealFinished = rng::all_of(ctx().players | rv::values, &Hand::empty, &Player::hand);
            isDealFinished) {
            co_return co_await finishDeal();
        }
        if (const auto winnerId = finishTrick(); not ctx().isPassGame) {
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
    co_await sendPlayerTurn(GameStage::PLAYING);
}

auto handleLog(const Message& msg) -> void
{
    const auto log = makeMethod<Log>(msg);
    if (not log) { return; }
    PREF_INFO("[client] {}, playerId: {}", log->text(), log->player_id());
}

auto handleSpeechBubble(Message msg) -> Awaitable<>
{
    const auto speechBubble = makeMethod<SpeechBubble>(msg);
    if (not speechBubble) { co_return; }
    co_await forwardToAllExcept(std::move(msg), speechBubble->player_id());
}

auto dispatchMessage(PlayerSession& session, const std::shared_ptr<Stream>& ws, std::optional<Message> msg)
    -> Awaitable<>
{ // clang-format off
    if (not msg) { co_return; }
    const auto& method = msg->method();
    // TODO: don't dispatch messages if AuthRequest or AuthLogin failed
    if (method == "LoginRequest") { session = co_await handleLoginRequest(*msg, ws); co_return; }
    if (method == "AuthRequest") { session = co_await handleAuthRequest(*msg, ws); co_return; }
    if (method == "Logout") { co_await handleLogout(*msg); co_return; }
    if (method == "Bidding") { co_await handleBidding(std::move(*msg)); co_return; }
    if (method == "DiscardTalon") { co_await handleDiscardTalon(*msg); co_return; }
    if (method == "Whisting") { co_await handleWhisting(std::move(*msg)); co_return; }
    if (method == "HowToPlay") { co_await handleHowToPlay(std::move(*msg)); co_return; }
    if (method == "PlayCard") { co_await handlePlayCard(std::move(*msg)); co_return; }
    if (method == "SpeechBubble") { co_await handleSpeechBubble(std::move(*msg)); co_return; }
    if (method == "PingPong") { co_await handlePingPong(std::move(*msg), ws); co_return; }
    if (method == "Log") { handleLog(*msg); co_return; }
    PREF_WARN("error: unknown {}", VAR(method));
} // clang-format on

auto launchSession(const std::shared_ptr<Stream> ws) -> Awaitable<>
{
    PREF_INFO();
    ws->binary(true);
    ws->set_option(web::stream_base::timeout::suggested(beast::role_type::server));
    ws->set_option(web::stream_base::decorator([](web::response_type& res) {
        res.set(beast::http::field::server, std::string{BOOST_BEAST_VERSION_STRING} + " preferans-server");
    }));
#ifdef PREF_SSL
    {
        auto error = sys::error_code{};
        co_await ws->next_layer().async_handshake(net::ssl::stream_base::server, net::redirect_error(error));
        if (error) {
            PREF_WARN("{} (handshake)", VAR(error));
            co_return;
        }
    }
#endif // PREF_SSL
    {
        auto error = sys::error_code{};
        co_await ws->async_accept(net::redirect_error(error));
        if (error) {
            PREF_WARN("{} (accept)", VAR(error));
            co_return;
        }
    }
    auto session = PlayerSession{};

    while (true) {
        auto buffer = beast::flat_buffer{};
        if (const auto [error, _] = co_await ws->async_read(buffer, net::as_tuple); error) {
            PREF_INFO("{} (read){}{}", error, VAR_MB(session.playerId), VAR_MB(session.playerName));
            break;
        }
        co_await dispatchMessage(session, ws, makeMessage(buffer));
    }
    if (std::empty(session.playerId)) {
        PREF_WARN("error: playerId is empty");
        co_return;
    }
    if (not ctx().players.contains(session.playerId)) {
        PREF_INFO("{} already left", VAR(session.playerId));
        co_return;
    }
    if (auto& player = ctx().player(session.playerId); session.id != player.sessionId) {
        PREF_INFO("{} reconnected with {} => {}", VAR(session.playerId), VAR(session.id), player.sessionId);
        // TODO: send the game state to the reconnected player
        co_return;
    }
    PREF_INFO("disconnected {}, waiting for reconnection", VAR(session.playerId));
    net::co_spawn(co_await net::this_coro::executor, disconnected(session.playerId), Detached("disconnected"));
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

[[nodiscard]] auto finishTrick(const std::vector<PlayedCard>& trick, const std::string_view trump) -> Player::Id
{
    assert(std::size(trick) == NumberOfPlayers and "all players played cards");
    const auto& firstCard = trick.front();
    const auto& leadSuit = cardSuit(not std::empty(ctx().talon.current) ? ctx().talon.current : firstCard.name);
    ctx().talon.current.clear(); // clang-format off
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
// NOLINTEND(readability-function-cognitive-complexity, hicpp-uppercase-literal-suffix)

auto acceptConnectionAndLaunchSession(
#ifdef PREF_SSL
    net::ssl::context ssl,
#endif // PREF_SSL
    const tcp::endpoint endpoint) -> Awaitable<>
{
    PREF_INFO();
    const auto ex = co_await net::this_coro::executor;
    auto acceptor = tcp::acceptor{ex, endpoint};
    while (true) {
        auto socket = co_await acceptor.async_accept();
#ifdef PREF_SSL
        auto ws = std::make_shared<Stream>(std::move(socket), ssl);
#else // PREF_SSL
        auto ws = std::make_shared<Stream>(std::move(socket));
#endif // PREF_SSL
        net::co_spawn(ex, launchSession(std::move(ws)), Detached("launchSession"));
    }
}

} // namespace pref
