#include "server.hpp"

#include "common/common.hpp"

#include <catch2/catch_all.hpp>

#include <gsl/gsl>

template<>
struct Catch::StringMaker<pref::ScoreEntry> {
    static auto convert(const pref::ScoreEntry& entry) -> std::string
    {
        return fmt::format("{}", entry);
    }
};

template<>
struct Catch::StringMaker<std::map<pref::Player::Id, pref::ScoreEntry>> {
    static auto convert(const std::map<pref::Player::Id, pref::ScoreEntry>& scoreEntry) -> std::string
    {
        return fmt::format("{}", scoreEntry);
    }
};

namespace pref {

TEST_CASE("server")
{
    SECTION("beats")
    {
        // clang-format off
        REQUIRE_FALSE(beats({.candidate = SEVEN _OF_ HEARTS, .best = EIGHT _OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE_FALSE(beats({.candidate = SEVEN _OF_ HEARTS, .best = SEVEN _OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE_FALSE(beats({.candidate = SEVEN _OF_ HEARTS, .best = SEVEN _OF_ SPADES, .leadSuit = HEARTS, .trump = SPADES}));

        REQUIRE(beats({.candidate = EIGHT _OF_ HEARTS, .best = SEVEN _OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE(beats({.candidate = SEVEN _OF_ HEARTS, .best = SEVEN _OF_ SPADES, .leadSuit = HEARTS, .trump = ""}));
        REQUIRE(beats({.candidate = SEVEN _OF_ SPADES, .best = EIGHT _OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE(beats({.candidate = SEVEN _OF_ SPADES, .best = SEVEN _OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        // clang-format on
    }

    // clang-format off
    SECTION("finishTrick")
    {
        SECTION("higher rank wins last")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = SEVEN _OF_ HEARTS},
                                 {.playerId = "2", .name = EIGHT _OF_ HEARTS},
                                 {.playerId = "3", .name = NINE  _OF_ HEARTS}}, SPADES) == "3");
        }

        SECTION("higer rank wins first")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = NINE  _OF_ HEARTS},
                                 {.playerId = "2", .name = EIGHT _OF_ HEARTS},
                                 {.playerId = "3", .name = SEVEN _OF_ HEARTS}}, SPADES) == "1");
        }

        SECTION("trump wins")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = NINE  _OF_ HEARTS},
                                 {.playerId = "2", .name = SEVEN _OF_ SPADES},
                                 {.playerId = "3", .name = SEVEN _OF_ HEARTS}}, SPADES) == "2");
        }

        SECTION ("first lead wins")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = SEVEN _OF_ HEARTS},
                                 {.playerId = "2", .name = SEVEN _OF_ DIAMONDS},
                                 {.playerId = "3", .name = SEVEN _OF_ CLUBS}}, SPADES) == "1");
        }

        SECTION("higher rank wins second without trump")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = SEVEN _OF_ HEARTS},
                                 {.playerId = "2", .name = EIGHT _OF_ HEARTS},
                                 {.playerId = "3", .name = EIGHT _OF_ CLUBS}}, "") == "2");
        }
        // clang-format on
    }
}

TEST_CASE("calculate score")
{
        using enum ContractLevel;
        using enum WhistChoise;

        static constexpr auto de = "0-declarer";
        static constexpr auto w1 = "1-whister ";
        static constexpr auto w2 = "2-whister ";

        SECTION("everyone fulfilled what declared")
        { // clang-format off
            const auto [contractLevel, tricksW1, tricksW2, whistW1, whistW2, declarerPool] = GENERATE(
                table<ContractLevel, int, int, int, int, int>({
                     {  Six, 2, 2, 2 *  2, 2 *  2, 2},
                     {Seven, 1, 2, 1 *  4, 2 *  4, 4},
                     {Eight, 1, 1, 1 *  6, 1 *  6, 6},
                     { Nine, 1, 0, 1 *  8, 0 *  8, 8},
            }));
            const auto actual = calculateScoreEntry(
                {.id = de, .contractLevel = contractLevel}, {
                {.id = w1, .choise = Whist, .tricksTaken = tricksW1},
                {.id = w2, .choise = Whist, .tricksTaken = tricksW2},
            });
            const auto expected = std::map<Player::Id, ScoreEntry>{
                {de, {.dump = 0, .pool = declarerPool, .whist = 0}},
                {w1, {.dump = 0, .pool = 0,            .whist = whistW1}},
                {w2, {.dump = 0, .pool = 0,            .whist = whistW2}},
            }; // clang-format on
            REQUIRE(actual == expected);
        }

        SECTION("declarer did not fulfill what declared")
        { // clang-format off
            const auto tricksW1 = 3;
            const auto tricksW2 = 2;
            const auto [contractLevel, dump, whistW1, whistW2] = GENERATE(
                table<ContractLevel, int, int, int>(
                    {{  Six, 1 *  2, (tricksW1 *  2) + (1 *  2), (tricksW2 *  2) + (1 *  2)},
                     {Seven, 2 *  4, (tricksW1 *  4) + (2 *  4), (tricksW2 *  4) + (2 *  4)},
                     {Eight, 3 *  6, (tricksW1 *  6) + (3 *  6), (tricksW2 *  6) + (3 *  6)},
                     { Nine, 4 *  8, (tricksW1 *  8) + (4 *  8), (tricksW2 *  8) + (4 *  8)},
                     {  Ten, 5 * 10, (tricksW1 * 10) + (5 * 10), (tricksW2 * 10) + (5 * 10)},
            }));
            const auto actual = calculateScoreEntry(
                {.id = de, .contractLevel = contractLevel}, {
                {.id = w1, .choise = Whist, .tricksTaken = tricksW1},
                {.id = w2, .choise = Whist, .tricksTaken = tricksW2},
            });
            const auto expected = std::map<Player::Id, ScoreEntry>{
                {de, {.dump = dump, .pool = 0, .whist = 0}},
                {w1, {.dump = 0,    .pool = 0, .whist = whistW1}},
                {w2, {.dump = 0,    .pool = 0, .whist = whistW2}},
            }; // clang-format on
            REQUIRE(actual == expected);
        }

        SECTION("one whister did not fulfill what declared")
        { // clang-format off
            const auto [contractLevel, declarerPool, dumpW1] = GENERATE(
                table<ContractLevel, int, int>({
                    {  Six,  2, 4 *  2},
                    {Seven,  4, 2 *  4},
                    {Eight,  6, 1 *  6},
                    { Nine,  8, 1 *  8},
                    {  Ten, 10, 1 * 10},
            }));
            const auto actual = calculateScoreEntry(
                {.id = de, .contractLevel = contractLevel}, {
                {.id = w1, .choise = Whist, .tricksTaken = 0},
                {.id = w2, .choise = Pass,  .tricksTaken = 0},
            });
            const auto expected = std::map<Player::Id, ScoreEntry>{
                {de, {.dump = 0,      .pool = declarerPool, .whist = 0}},
                {w1, {.dump = dumpW1, .pool = 0,            .whist = 0}},
                {w2, {.dump = 0,      .pool = 0,            .whist = 0}},
            }; // clang-format on
            REQUIRE(actual == expected);
        }

        SECTION("both whisters did not fulfill what declared")
        { // clang-format off
            const auto [contractLevel, declarerPool, dumpW] = GENERATE(
                table<ContractLevel, int, int>({
                    {  Six, 2, 2 *  2},
                    {Seven, 4, 1 *  4},
                    {Eight, 6, 1 *  6},
                    { Nine, 8, 1 *  8},
                    { Ten, 10, 1 * 10},
            }));
            const auto actual = calculateScoreEntry(
                {.id = de, .contractLevel = contractLevel}, {
                {.id = w1, .choise = Whist, .tricksTaken = 0},
                {.id = w2, .choise = Whist, .tricksTaken = 0},
            });
            const auto expected = std::map<Player::Id, ScoreEntry>{
                {de, {.dump = 0, .pool = declarerPool, .whist = 0}},
                {w1, {.dump = dumpW, .pool = 0, .whist = 0}},
                {w2, {.dump = dumpW, .pool = 0, .whist = 0}},
            }; // clang-format on
            REQUIRE(actual == expected);
        }
}

} // namespace pref
