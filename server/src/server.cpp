#include "common/common.hpp"
#include "common/logger.hpp"
#include "proto/pref.pb.h"

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <docopt/docopt.h>
#include <fmt/ranges.h>
#include <fmt/std.h>
#include <range/v3/all.hpp>

#include <array>
#include <cstdlib>
#include <exception>
#include <gsl/gsl>
#include <iterator>
#include <memory>
#include <set>

namespace sys = boost::system;
namespace net = boost::asio;
namespace beast = boost::beast;
namespace web = beast::websocket;
namespace rng = ranges;
namespace rv = rng::views;

using namespace std::literals;

namespace boost::system {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunneeded-internal-declaration"
// NOLINTNEXTLINE(readability-identifier-naming, misc-use-anonymous-namespace)
[[nodiscard]] static auto format_as(const error_code& error) -> std::string
{
    return error.message();
}
#pragma GCC diagnostic pop
} // namespace boost::system

namespace pref {
namespace {

// NOLINTBEGIN(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)

constexpr auto NumberOfPlayers = 3UZ;

using SteadyTimer = net::steady_timer;
using TcpAcceptor = net::ip::tcp::acceptor;
using Stream = web::stream<beast::tcp_stream>;

template<typename T = void>
using Awaitable = net::awaitable<T>;

template<typename Callable>
[[maybe_unused]] [[nodiscard]] auto unpack(Callable callable)
{
    return [cb = std::move(callable)](const auto& pair) { return cb(pair.first, pair.second); };
}

constexpr auto Detached = [](const std::string_view func) { // NOLINT(fuchsia-statically-constructed-objects)
    return [func](const std::exception_ptr& eptr) {
        if (not eptr) {
            return;
        }
        try {
            std::rethrow_exception(eptr);
        } catch (const sys::system_error& error) {
            if (error.code() != net::error::operation_aborted) {
                WARN("[{}][Detached] error: {}", func, error);
            }
        } catch (const std::exception& error) {
            WARN("[{}][Detached] error: {}", func, error);
        } catch (...) {
            WARN("[{}][Detached] error: unknown", func);
        }
    };
};

[[nodiscard]] auto makeMessage(const beast::flat_buffer& buffer) -> std::optional<Message>
{
    auto result = Message{};
    if (not result.ParseFromArray(buffer.data().data(), gsl::narrow<int>(buffer.size()))) {
        WARN("error: failed to make Message");
        return {};
    }
    return result;
}

using CardName = std::string;
using Hand = std::set<CardName>;

struct PlayerSession {
    using Id = std::uint64_t;

    Id id{};
    std::string playerId;
    std::string playerName;
};

struct Player {
    using Id = std::string;
    using Name = std::string;

    Player() = default;

    Player(Id aId, Name aName, PlayerSession::Id aSessionId, const std::shared_ptr<Stream>& aWs)
        : id{std::move(aId)}
        , name{std::move(aName)}
        , sessionId{aSessionId}
        , ws{aWs}
    {
    }

    Id id;
    Name name;
    PlayerSession::Id sessionId{};
    std::shared_ptr<Stream> ws;
    std::optional<SteadyTimer> reconnectTimer;
    Hand hand;
    std::string bid;
    int tricksTaken{};
};

struct PlayedCard {
    Player::Id byPlayerId;
    CardName name;
};

[[maybe_unused]] auto generateUuid() -> Player::Id
{
    return boost::uuids::to_string(std::invoke(boost::uuids::random_generator{}));
}

struct Context {
    using Players = std::map<Player::Id, Player>;

    [[nodiscard]] auto whoseTurnId() const -> const Player::Id&
    {
        return whoseTurnIt->first;
    }

    [[nodiscard]] auto player(const Player::Id& playerId) const -> Player&
    {
        assert(players.contains(playerId) and "player exists");
        return players[playerId];
    }

    [[nodiscard]] auto playerName(const Player::Id& playerId) const -> const std::string&
    {
        return player(playerId).name;
    }

    mutable Players players;
    Players::const_iterator whoseTurnIt;
    std::vector<CardName> talon;
    std::vector<PlayedCard> trick;
    std::string trump;
    Player::Id forehandId;
};

auto resetWhoseTurn(Context& ctx) -> void
{
    assert((std::size(ctx.players) == NumberOfPlayers) and "all players joined");
    ctx.whoseTurnIt = std::cbegin(ctx.players);
}

auto advanceWhoseTurn(Context& ctx) -> void
{
    INFO();
    assert((std::size(ctx.players) == NumberOfPlayers) and "all players joined");
    if (++ctx.whoseTurnIt == std::cend(ctx.players)) {
        resetWhoseTurn(ctx);
    }
}

auto forehandsTurn(Context& ctx) -> void
{
    assert(ctx.players.contains(ctx.forehandId) and "forehand player exists");
    ctx.whoseTurnIt = ctx.players.find(ctx.forehandId);
}

auto advanceWhoseTurn(Context& ctx, const std::string_view stage) -> void
{
    INFO_VAR(stage);
    advanceWhoseTurn(ctx);
    if (stage != "Bidding") {
        return;
    }
    while (ctx.player(ctx.whoseTurnId()).bid == "PASS") {
        advanceWhoseTurn(ctx);
    }
}

[[nodiscard]] auto rankOf(const std::string_view card) -> std::string
{
    return std::string{card.substr(0, card.find("_of_"))};
}

[[nodiscard]] auto rankValue(const std::string_view rank) -> int
{
    static const auto rankMap = std::map<std::string_view, int>{
        {"ace", 8}, {"king", 7}, {"queen", 6}, {"jack", 5}, {"10", 4}, {"9", 3}, {"8", 2}, {"7", 1}};
    return rankMap.at(rank);
}

[[nodiscard]] auto beats(
    const std::string_view candidate,
    const std::string_view best,
    const std::string_view leadSuit,
    const std::string_view trump) -> bool
{
    const auto candidateSuit = suitOf(candidate);
    const auto bestSuit = suitOf(best);
    if (candidateSuit == bestSuit) {
        return rankValue(rankOf(candidate)) > rankValue(rankOf(best));
    }
    if (not std::empty(trump) and (candidateSuit == trump) and (bestSuit != trump)) {
        return true;
    }
    if (not std::empty(trump) and (candidateSuit != trump) and (bestSuit == trump)) {
        return false;
    }
    return candidateSuit == leadSuit;
}

[[nodiscard]] auto finishTrick(Context& ctx) -> Player::Id
{
    assert(std::size(ctx.trick) == NumberOfPlayers and "all players played cards");
    const auto& firstPlayedCard = ctx.trick.front();
    const auto leadSuit = suitOf(firstPlayedCard.name);
    auto winnerId = firstPlayedCard.byPlayerId;
    auto winnerCard = firstPlayedCard.name;
    for (const auto& playedCard : ctx.trick) {
        if (beats(playedCard.name, winnerCard, leadSuit, ctx.trump)) {
            winnerId = playedCard.byPlayerId;
            winnerCard = playedCard.name;
        }
    }
    const auto& winnerName = ctx.playerName(winnerId);
    const auto tricksTaken = ++ctx.player(winnerId).tricksTaken;
    INFO_VAR(winnerId, winnerName, leadSuit, winnerCard, tricksTaken);
    ctx.trick.clear();
    return winnerId;
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
            WARN("{}: failed to send message", VAR(error));
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
        | rv::filter(std::bind_front(std::not_equal_to{}, excludedId), GetPlayerId) //
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
        WARN("{}: failed to close ws", VAR(error));
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

auto joined(Context& ctx, Message msg, const std::shared_ptr<Stream> ws) -> Awaitable<PlayerSession>
{
    auto joinRequest = JoinRequest{};
    if (not joinRequest.ParseFromString(msg.payload())) {
        WARN("error: failed to parse JoinRequest");
        co_return PlayerSession{};
    }
    auto session = PlayerSession{};
    const auto& playerId = joinRequest.player_id();
    session.playerName = joinRequest.player_name();
    const auto address = ws->next_layer().socket().remote_endpoint().address().to_string();
    INFO_VAR(playerId, session.playerName, address); // TODO: hide address
    if (isNewPlayer(playerId, ctx.players)) {
        joinPlayer(ws, ctx.players, session);
    } else {
        co_await reconnectPlayer(ws, playerId, ctx.players, session);
    }
    auto joinResponse = JoinResponse{};
    joinResponse.set_player_id(session.playerId);
    for (const auto& [id, player] : ctx.players) {
        auto p = joinResponse.add_players();
        p->set_player_id(id);
        p->set_player_name(player.name);
    }
    msg = Message{};
    msg.set_method("JoinResponse");
    msg.set_payload(joinResponse.SerializeAsString());
    if (const auto error = co_await sendToOne(msg, ws)) {
        WARN("{}: failed to send JoinResponse", VAR(error));
    }
    if (playerId == session.playerId) {
        co_return session;
    }
    auto playerJoined = PlayerJoined{};
    playerJoined.set_player_id(session.playerId);
    playerJoined.set_player_name(session.playerName);
    msg = Message{};
    msg.set_method("PlayerJoined");
    msg.set_payload(playerJoined.SerializeAsString());
    co_await sendToAllExcept(msg, ctx.players, session.playerId);
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

auto playerTurn(Context& ctx, const std::string_view stage) -> Awaitable<>
{
    auto playerTurn = PlayerTurn{};
    const auto& playerId = ctx.whoseTurnId();
    playerTurn.set_player_id(playerId);
    playerTurn.set_stage(std::string{stage});
    if (stage == "TalonPicking") {
        assert((std::size(ctx.talon) == 2) and "talon is two cards");
        for (const auto& card : ctx.talon) {
            addCardToHand(ctx, playerId, card);
            playerTurn.add_talon(card);
        }
    }
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerId, playerName, stage);
    auto msg = Message{};
    msg.set_method("PlayerTurn");
    msg.set_payload(playerTurn.SerializeAsString());
    co_await sendToAll(msg, ctx.players);
}

auto cardPlayed(Context& ctx, const Message& msg) -> Awaitable<>
{
    auto playCard = PlayCard{};
    if (not playCard.ParseFromString(msg.payload())) {
        WARN("error: failed to parse PlayCard");
        co_return;
    }
    const auto& playerId = playCard.player_id();
    const auto& card = playCard.card();
    const auto& playerName = ctx.playerName(playerId);
    removeCardFromHand(ctx, playerId, card);
    ctx.trick.emplace_back(playerId, card);
    INFO_VAR(playerName, playerId, card);
    co_await sendToAllExcept(msg, ctx.players, playerId);
    if (std::size(ctx.trick) == NumberOfPlayers) {
        const auto winnerId = finishTrick(ctx);
        const auto& winnerName = ctx.playerName(winnerId);
        INFO_VAR(winnerId, winnerName);
        // TODO: Notify all players
        ctx.whoseTurnIt = ctx.players.find(winnerId);
        if (rng::all_of(ctx.players | rv::values, &Hand::empty, &Player::hand)) {
            INFO("End of the deal");
            // TODO: Start the new deal instead of playTurn()
            co_await playerTurn(ctx, "Playing");
        }
    } else {
        advanceWhoseTurn(ctx);
    }
    co_await playerTurn(ctx, "Playing");
}

[[nodiscard]] auto talonDiscarded(Context& ctx, const Message& msg) -> std::pair<Player::Id, std::string>
{
    auto discardTalon = DiscardTalon{};
    if (not discardTalon.ParseFromString(msg.payload())) {
        WARN("error: failed to parse DiscardTalon");
        // TODO: throw?
        return {};
    }
    const auto& playerId = discardTalon.player_id();
    const auto& bid = discardTalon.bid();
    const auto& discardedCards = discardTalon.cards();
    for (const auto& card : discardedCards) {
        removeCardFromHand(ctx, playerId, card);
    }
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, discardedCards, bid);
    return {playerId, bid};
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
    INFO("removed {} after timeout", VAR(playerId));
    ctx.players.erase(playerId);
    auto playerLeft = PlayerLeft{};
    playerLeft.set_player_id(playerId);
    auto msg = Message{};
    msg.set_method("PlayerLeft");
    msg.set_payload(playerLeft.SerializeAsString());
    co_await sendToAll(msg, ctx.players);
}

auto dealCards(Context& ctx) -> Awaitable<>
{
    const auto suits = std::array{"spades", "diamonds", "clubs", "hearts"};
    const auto ranks = std::array{"7", "8", "9", "10", "jack", "queen", "king", "ace"};
    const auto toCard = [](const auto& card) {
        const auto& [rank, suit] = card;
        return fmt::format("{}_of_{}", rank, suit);
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
        auto dealCards = DealCards{};
        for (const auto& card : hand) {
            *dealCards.add_cards() = card;
        }
        auto msg = Message{};
        msg.set_method("DealCards");
        msg.set_payload(dealCards.SerializeAsString());
        co_await sendToOne(msg, ws);
    }
}

auto bid(Context& ctx, const Message& msg) -> Awaitable<>
{
    auto bidding = Bidding{};
    if (not bidding.ParseFromString(msg.payload())) {
        WARN("error: failed to parse Bidding");
        co_return;
    }
    const auto& playerId = bidding.player_id();
    const auto& bid = bidding.bid();
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, bid);
    ctx.player(playerId).bid = bid;
    co_await sendToAllExcept(msg, ctx.players, playerId);
}

auto finalBid(Context& ctx, const Player::Id& playerId, const std::string& bid) -> Awaitable<>
{
    auto bidding = Bidding{};
    bidding.set_player_id(playerId);
    bidding.set_bid(bid);
    ctx.trump = std::string{getTrump(bid)};
    const auto& playerName = ctx.playerName(playerId);
    INFO_VAR(playerName, playerId, bid, ctx.trump);
    auto msg = Message{};
    msg.set_method("Bidding");
    msg.set_payload(bidding.SerializeAsString());
    co_await sendToAllExcept(msg, ctx.players, playerId);
}

[[nodiscard]] auto stageGame(Context& ctx) -> std::string_view
{
    const auto bids = ctx.players | rv::values | rv::transform(&Player::bid);
    if (std::size(bids) == NumberOfPlayers) {
        if (rng::count(bids, "PASS") == NumberOfPlayers) {
            // TODO: implement Pass Game
            ERROR("The Pass Game is not implemented");
            return "Playing";
        }
        if ((rng::count_if(bids, std::bind_front(std::not_equal_to{}, "PASS")) == 1)
            and (rng::count(bids, "PASS") == 2)) {
            return "TalonPicking";
        }
    }
    return "Bidding";
}

auto launchSession(Context& ctx, const std::shared_ptr<Stream> ws) -> Awaitable<>
{
    INFO();
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
                INFO("{}: disconnected: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else if (error == net::error::operation_aborted) {
                WARN("{}: read: {}, {}", VAR(error), VAR(session.playerName), VAR(session.playerId));
            } else {
                ERROR_VAR(session.playerName, session.playerId, error);
                // TODO(olkozlo): maybe throw to not handle a disconnection?
            }
            break;
        }
        const auto maybeMsg = makeMessage(buffer);
        if (not maybeMsg) {
            continue;
        }
        if (const auto& msg = *maybeMsg; msg.method() == "JoinRequest") {
            session = co_await joined(ctx, msg, ws);
            if (std::size(ctx.players) == NumberOfPlayers) {
                co_await dealCards(ctx);
                resetWhoseTurn(ctx);
                ctx.forehandId = ctx.whoseTurnId();
                co_await playerTurn(ctx, "Bidding");
            }
        } else if (msg.method() == "Bidding") {
            co_await bid(ctx, msg);
            const auto stage = stageGame(ctx);
            advanceWhoseTurn(ctx, stage);
            co_await playerTurn(ctx, stage);
        } else if (msg.method() == "PlayCard") {
            co_await cardPlayed(ctx, msg);
        } else if (msg.method() == "DiscardTalon") {
            const auto [playerId, bid] = talonDiscarded(ctx, msg);
            co_await finalBid(ctx, playerId, bid);
            forehandsTurn(ctx);
            co_await playerTurn(ctx, "Playing");
        } else {
            WARN("error: unknown method: {}", msg.method());
            continue;
        }
    }
    if (std::empty(session.playerId) or not ctx.players.contains(session.playerId)) {
        // TODO: Can `playerId` ever be set but not present in `players`? If not, replace with `assert()`.
        WARN("error: empty or unknown {}", VAR(session.playerId));
        co_return;
    }
    auto& player = ctx.player(session.playerId);
    if (session.id != player.sessionId) {
        INFO("{} reconnected with {} => {}", VAR(session.playerId), VAR(session.id), player.sessionId);
        // TODO: send the game state to the reconnected player
        co_return;
    }
    WARN("disconnected {}, waiting for reconnection", VAR(session.playerId));
    net::co_spawn(co_await net::this_coro::executor, disconnected(ctx, session.playerId), Detached("disconnected"));
}

auto acceptConnectionAndLaunchSession(Context& ctx, const net::ip::tcp::endpoint endpoint) -> Awaitable<>
{
    INFO();
    const auto ex = co_await net::this_coro::executor;
    auto acceptor = TcpAcceptor{ex, endpoint};
    while (true) {
        net::co_spawn(
            ex,
            launchSession(ctx, std::make_shared<Stream>(co_await acceptor.async_accept())),
            Detached("launchSession"));
    }
}

auto handleSignals() -> Awaitable<>
{
    const auto ex = co_await net::this_coro::executor;
    auto signals = net::signal_set{ex, SIGINT, SIGTERM};
    const auto signal = co_await signals.async_wait();
    INFO_VAR(signal);
    static_cast<net::io_context&>(ex.context()).stop(); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
}

constexpr auto usage = R"(
Usage:
    preferans-server <address> <port>

Options:
    -h --help     Show this screen.
)";

// NOLINTEND(performance-unnecessary-value-param, cppcoreguidelines-avoid-reference-coroutine-parameters)
} // namespace
} // namespace pref

int main(const int argc, const char* const argv[])
{
    try {
        const auto args = docopt::docopt(pref::usage, {std::next(argv), std::next(argv, argc)});
        spdlog::set_pattern("[%^%l%$][%!] %v");
        auto const address = net::ip::make_address(args.at("<address>").asString());
        auto const port = gsl::narrow<std::uint16_t>(args.at("<port>").asLong());
        auto loop = net::io_context{};
        auto ctx = pref::Context{};
        net::co_spawn(loop, pref::handleSignals(), pref::Detached("handleStop"));
        net::co_spawn(
            loop,
            pref::acceptConnectionAndLaunchSession(ctx, {address, port}),
            pref::Detached("acceptConnectionAndLaunchSession"));
        loop.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        ERROR("{}", VAR(error));
        return EXIT_FAILURE;
    }
}
