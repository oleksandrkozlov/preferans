#include "server.hpp"

#include "common/common.hpp"

#include <catch2/catch_test_macros.hpp>

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

} // namespace pref
