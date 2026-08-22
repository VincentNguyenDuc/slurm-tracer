#include <catch2/catch_test_macros.hpp>

#include "pair.hpp"

TEST_CASE("Pair sum", "[pair]") {
    const Pair<int> p{3, 5};
    REQUIRE(p.sum() == 8);
}

TEST_CASE("Pair max", "[pair]") {
    const Pair<int> p{3, 5};
    REQUIRE(p.max() == 5);
}

TEST_CASE("Pair first and second", "[pair]") {
    const Pair<double> p{1.5, 2.5};
    REQUIRE(p.first() == 1.5);
    REQUIRE(p.second() == 2.5);
}

TEST_CASE("Pair is_equal", "[pair]") {
    const Pair<int> a{1, 2};
    const Pair<int> b{1, 2};
    const Pair<int> c{3, 4};
    REQUIRE(a.is_equal(b));
    REQUIRE_FALSE(a.is_equal(c));
}
