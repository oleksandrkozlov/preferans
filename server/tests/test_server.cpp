#include "auth.hpp"
#include "common/common.hpp"
#include "server.hpp"

#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

template<>
struct Catch::StringMaker<pref::DealScoreEntry> {
    static auto convert(const pref::DealScoreEntry& entry) -> std::string
    {
        return fmt::format("{}", entry);
    }
};

template<>
struct Catch::StringMaker<std::map<pref::Player::Id, pref::DealScoreEntry>> {
    static auto convert(const std::map<pref::Player::Id, pref::DealScoreEntry>& scoreEntry) -> std::string
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
        REQUIRE_FALSE(beats({.candidate = SEVEN PREF_OF_ HEARTS, .best = EIGHT PREF_OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE_FALSE(beats({.candidate = SEVEN PREF_OF_ HEARTS, .best = SEVEN PREF_OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE_FALSE(beats({.candidate = SEVEN PREF_OF_ HEARTS, .best = SEVEN PREF_OF_ SPADES, .leadSuit = HEARTS, .trump = SPADES}));

        REQUIRE(beats({.candidate = EIGHT PREF_OF_ HEARTS, .best = SEVEN PREF_OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE(beats({.candidate = SEVEN PREF_OF_ HEARTS, .best = SEVEN PREF_OF_ SPADES, .leadSuit = HEARTS, .trump = ""}));
        REQUIRE(beats({.candidate = SEVEN PREF_OF_ SPADES, .best = EIGHT PREF_OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        REQUIRE(beats({.candidate = SEVEN PREF_OF_ SPADES, .best = SEVEN PREF_OF_ HEARTS, .leadSuit = HEARTS, .trump = SPADES}));
        // clang-format on
    }

    // clang-format off
    SECTION("finishTrick")
    {
        SECTION("higher rank wins last")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = SEVEN PREF_OF_ HEARTS},
                                 {.playerId = "2", .name = EIGHT PREF_OF_ HEARTS},
                                 {.playerId = "3", .name = NINE  PREF_OF_ HEARTS}}, SPADES) == "3");
        }

        SECTION("higer rank wins first")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = NINE  PREF_OF_ HEARTS},
                                 {.playerId = "2", .name = EIGHT PREF_OF_ HEARTS},
                                 {.playerId = "3", .name = SEVEN PREF_OF_ HEARTS}}, SPADES) == "1");
        }

        SECTION("trump wins")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = NINE  PREF_OF_ HEARTS},
                                 {.playerId = "2", .name = SEVEN PREF_OF_ SPADES},
                                 {.playerId = "3", .name = SEVEN PREF_OF_ HEARTS}}, SPADES) == "2");
        }

        SECTION ("first lead wins")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = SEVEN PREF_OF_ HEARTS},
                                 {.playerId = "2", .name = SEVEN PREF_OF_ DIAMONDS},
                                 {.playerId = "3", .name = SEVEN PREF_OF_ CLUBS}}, SPADES) == "1");
        }

        SECTION("higher rank wins second without trump")
        {
            REQUIRE(finishTrick({{.playerId = "1", .name = SEVEN PREF_OF_ HEARTS},
                                 {.playerId = "2", .name = EIGHT PREF_OF_ HEARTS},
                                 {.playerId = "3", .name = EIGHT PREF_OF_ CLUBS}}, "") == "2");
        }
        // clang-format on
    }
}

TEST_CASE("calculateDealScore")
{
    using enum ContractLevel;
    using enum WhistingChoice;

    static constexpr auto de = "0-declarer";
    static constexpr auto w1 = "1-whister ";
    static constexpr auto w2 = "2-whister ";

    SECTION("declarer declares miser")
    { // clang-format off
        const auto [tricksDeclarer, tricksW1, dump, pool] = GENERATE(
            table<int, int, int, int>(
                {{ 0, 10,   0, 10},
                 { 1,  9,  10,  0},
                 { 2,  8,  20,  0},
                 { 3,  7,  30,  0},
                 { 4,  6,  40,  0},
                 { 5,  5,  50,  0},
                 { 6,  4,  60,  0},
                 { 7,  3,  70,  0},
                 { 8,  2,  80,  0},
                 { 9,  1,  90,  0},
                 {10,  0, 100,  0}}));
        const auto actual = calculateDealScore(
            { .id = de, .contractLevel = ContractLevel::Miser, .tricksTaken = tricksDeclarer},
            {{.id = w1, .choice = Whist,                       .tricksTaken = tricksW1},
             {.id = w2, .choice = Whist,                       .tricksTaken = 0}});
        const auto expected = DealScore{
            {de, {.dump = dump, .pool = pool, .whist = 0}},
            {w1, {.dump = 0,    .pool = 0,    .whist = 0}},
            {w2, {.dump = 0,    .pool = 0,    .whist = 0}},
        }; // clang-format on
        REQUIRE(actual == expected);
    }

    SECTION("everyone fulfilled what declared")
    { // clang-format off
            const auto [contractLevel, tricksDeclarer, tricksW1, tricksW2, whistW1, whistW2, declarerPool] = GENERATE(
                table<ContractLevel, int,  int, int, int, int, int>({
                     {  Six, 6, 2, 2, 2 *  2, 2 *  2, 2},
                     {Seven, 7, 1, 2, 1 *  4, 2 *  4, 4},
                     {Eight, 8, 1, 1, 1 *  6, 1 *  6, 6},
                     { Nine, 9, 1, 0, 1 *  8, 0 *  8, 8},
            }));
            const auto actual = calculateDealScore(
                {.id = de, .contractLevel = contractLevel, .tricksTaken = tricksDeclarer}, {
                {.id = w1, .choice = Whist,                .tricksTaken = tricksW1},
                {.id = w2, .choice = Whist,                .tricksTaken = tricksW2},
            });
            const auto expected = DealScore{
                {de, {.dump = 0, .pool = declarerPool, .whist = 0}},
                {w1, {.dump = 0, .pool = 0,            .whist = whistW1}},
                {w2, {.dump = 0, .pool = 0,            .whist = whistW2}},
            }; // clang-format on
        REQUIRE(actual == expected);
    }

    SECTION("declarer did not fulfill what declared")
    { // clang-format off
            const auto tricksDeclarer = 5;
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
            const auto actual = calculateDealScore(
                {.id = de, .contractLevel = contractLevel, .tricksTaken = tricksDeclarer}, {
                {.id = w1, .choice = Whist,                .tricksTaken = tricksW1},
                {.id = w2, .choice = Whist,                .tricksTaken = tricksW2},
            });
            const auto expected = DealScore{
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
            const auto actual = calculateDealScore(
                {.id = de, .contractLevel = contractLevel, .tricksTaken = 10}, {
                {.id = w1, .choice = Whist,                .tricksTaken = 0},
                {.id = w2, .choice = Pass,                 .tricksTaken = 0},
            });
            const auto expected = DealScore{
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
            const auto actual = calculateDealScore(
                {.id = de, .contractLevel = contractLevel, .tricksTaken = 10}, {
                {.id = w1, .choice = Whist, .tricksTaken = 0},
                {.id = w2, .choice = Whist, .tricksTaken = 0},
            });
            const auto expected = DealScore{
                {de, {.dump = 0, .pool = declarerPool, .whist = 0}},
                {w1, {.dump = dumpW, .pool = 0, .whist = 0}},
                {w2, {.dump = dumpW, .pool = 0, .whist = 0}},
            }; // clang-format on
        REQUIRE(actual == expected);
    }
}

TEST_CASE("calculateFinalResult")
{
    SECTION("filled")
    {
        const auto result = calculateFinalResult(
            {{"p0", {.dump = 12, .pool = 14, .whists = {{"p1", 12}, {"p2", 0}}}},
             {"p1", {.dump = 26, .pool = 8, .whists = {{"p0", 22}, {"p2", 0}}}},
             {"p2", {.dump = 6, .pool = 0, .whists = {{"p0", 22}, {"p1", 4}}}}});

        REQUIRE(result.at("p0") == 62);
        REQUIRE(result.at("p1") == -101);
        REQUIRE(result.at("p2") == 39);
        REQUIRE(result.at("p0") + result.at("p1") + result.at("p2") == 0);
    }

    SECTION("partially filled")
    {
        const auto result = calculateFinalResult(
            {{"p0", {.dump = 2, .pool = 0, .whists = {}}},
             {"p1", {.dump = 0, .pool = 0, .whists = {{"p0", 4}}}},
             {"p2", {.dump = 0, .pool = 0, .whists = {{"p0", 10}}}}});

        REQUIRE(result.at("p0") == -28);
        REQUIRE(result.at("p1") == 11);
        REQUIRE(result.at("p2") == 17);
        REQUIRE(result.at("p0") + result.at("p1") + result.at("p2") == 0);
    }
    SECTION("partially filled1")
    {
        const auto result = calculateFinalResult(
            {{"p0", {.dump = 0, .pool = 2, .whists = {{"p1", 110}, {"p2", 14}}}},
             {"p1", {.dump = 62, .pool = 0, .whists = {{"p0", 2}, {"p2", 12}}}},
             {"p2", {.dump = 6, .pool = 0, .whists = {{"p0", 4}, {"p1", 70}}}}});

        REQUIRE(result.at("p0") == 359);
        REQUIRE(result.at("p1") == -567);
        REQUIRE(result.at("p2") == 208);
        REQUIRE(result.at("p0") + result.at("p1") + result.at("p2") == 0);
    }

    SECTION("empty")
    {
        REQUIRE_NOTHROW(calculateFinalResult({}));
    }
}

TEST_CASE("makeFinalScore")
{
    SECTION("filled")
    {
        const auto scoreSheet = ScoreSheet{
            {"p0", {.dump = {1, 2, 3}, .pool = {4, 5, 6}, .whists = {{"p1", {7, 8, 9}}, {"p2", {10, 11, 12}}}}},
            {"p1",
             {.dump = {13, 14, 15}, .pool = {16, 17, 18}, .whists = {{"p0", {19, 20, 21}}, {"p2", {22, 23, 24}}}}},
            {"p2",
             {.dump = {25, 26, 27}, .pool = {28, 29, 30}, .whists = {{"p0", {31, 32, 33}}, {"p1", {34, 35, 36}}}}},
        };
        const auto result = makeFinalScore(scoreSheet);
        REQUIRE(result.at("p0").dump == 6);
        REQUIRE(result.at("p0").pool == 15);
        REQUIRE(result.at("p0").whists.at("p1") == 24);
        REQUIRE(result.at("p0").whists.at("p2") == 33);

        REQUIRE(result.at("p1").dump == 42);
        REQUIRE(result.at("p1").pool == 51);
        REQUIRE(result.at("p1").whists.at("p0") == 60);
        REQUIRE(result.at("p1").whists.at("p2") == 69);

        REQUIRE(result.at("p2").dump == 78);
        REQUIRE(result.at("p2").pool == 87);
        REQUIRE(result.at("p2").whists.at("p0") == 96);
        REQUIRE(result.at("p2").whists.at("p1") == 105);
    }

    SECTION("empty values")
    {
        const auto scoreSheet = ScoreSheet{
            {"p0", {.dump = {}, .pool = {}, .whists = {{"p1", {}}, {"p2", {}}}}},
            {"p1", {.dump = {}, .pool = {}, .whists = {{"p0", {}}, {"p2", {}}}}},
            {"p2", {.dump = {}, .pool = {}, .whists = {{"p0", {}}, {"p1", {}}}}},
        };
        const auto result = makeFinalScore(scoreSheet);
        REQUIRE(result.at("p0").dump == 0);
        REQUIRE(result.at("p0").pool == 0);
        REQUIRE(result.at("p0").whists.at("p1") == 0);
        REQUIRE(result.at("p0").whists.at("p2") == 0);

        REQUIRE(result.at("p1").dump == 0);
        REQUIRE(result.at("p1").pool == 0);
        REQUIRE(result.at("p1").whists.at("p0") == 0);
        REQUIRE(result.at("p1").whists.at("p2") == 0);

        REQUIRE(result.at("p2").dump == 0);
        REQUIRE(result.at("p2").pool == 0);
        REQUIRE(result.at("p2").whists.at("p0") == 0);
        REQUIRE(result.at("p2").whists.at("p1") == 0);
    }

    SECTION("empty whists")
    {
        const auto scoreSheet = ScoreSheet{
            {"p0", {.dump = {}, .pool = {}, .whists = {}}},
            {"p1", {.dump = {}, .pool = {}, .whists = {}}},
            {"p2", {.dump = {}, .pool = {}, .whists = {}}},
        };
        const auto result = makeFinalScore(scoreSheet);
        REQUIRE(result.at("p0").dump == 0);
        REQUIRE(result.at("p0").pool == 0);
        REQUIRE_THROWS(result.at("p0").whists.at("p1"));
        REQUIRE_THROWS(result.at("p0").whists.at("p2"));

        REQUIRE(result.at("p1").dump == 0);
        REQUIRE(result.at("p1").pool == 0);
        REQUIRE_THROWS(result.at("p1").whists.at("p0"));
        REQUIRE_THROWS(result.at("p1").whists.at("p2"));

        REQUIRE(result.at("p2").dump == 0);
        REQUIRE(result.at("p2").pool == 0);
        REQUIRE_THROWS(result.at("p2").whists.at("p0"));
        REQUIRE_THROWS(result.at("p2").whists.at("p1"));
    }

    SECTION("empty")
    {
        REQUIRE(std::empty(makeFinalScore({})));
    }
}

TEST_CASE("auth")
{
    const auto hexStr = "b5bb9d8014a0f9b1d61e21e796d78dccdf1352f23cd32812f4850b878ae4944c"s;

    SECTION("hex2bytes & bytes2hex")
    {
        const auto bytes = std::vector<std::uint8_t>{0xB5, 0xBB, 0x9D, 0x80, 0x14, 0xA0, 0xF9, 0xB1, 0xD6, 0x1E, 0x21,
                                                     0xE7, 0x96, 0xD7, 0x8D, 0xCC, 0xDF, 0x13, 0x52, 0xF2, 0x3C, 0xD3,
                                                     0x28, 0x12, 0xF4, 0x85, 0x0B, 0x87, 0x8A, 0xE4, 0x94, 0x4C};
        const auto bytesStr = std::string{std::cbegin(bytes), std::cend(bytes)};
        REQUIRE(hex2bytes(hexStr) == bytesStr);
        REQUIRE(bytes2hex(bytesStr) == hexStr);
    }

    SECTION("generateToken")
    {
        const auto token0 = generateToken();
        const auto token1 = generateToken();
        REQUIRE(std::size(token0) == 32);
        REQUIRE(std::size(token1) == 32);
        REQUIRE(token0 != token1);
    }

    SECTION("hashToken")
    {
        const auto expectedHash = "2dc85e9a540c7016ac1e441d871c6c92b07b74416a5bb6622eeef4f579f46672"s;
        const auto actualHash = hashToken(hexStr);

        REQUIRE(std::size(expectedHash) == std::size(actualHash));
        REQUIRE(expectedHash == actualHash);
    }

    SECTION("generateUuid")
    {
        const auto uuid0 = generateUuid();
        const auto uuid1 = generateUuid();
        REQUIRE(std::size(uuid0) == 36);
        REQUIRE(std::size(uuid1) == 36);
        REQUIRE(uuid0 != uuid1);
    }

    SECTION("password")
    {
        const auto password = "aboba";
        REQUIRE(verifyPassword(password, hashPassword(password)));
    }
}

} // namespace pref
