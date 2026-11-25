#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "../src/loot_generator.h"

using namespace std::literals;

namespace {
using loot_gen::LootGenerator;
}  // namespace

TEST_CASE("Loot generator", "[loot_generator]") {
    constexpr LootGenerator::TimeInterval kBaseInterval = 1s;

    SECTION("nothing is generated when there are no looters") {
        LootGenerator gen{kBaseInterval, 1.0};
        CHECK(gen.Generate(kBaseInterval, 0, 0) == 0);
        CHECK(gen.Generate(kBaseInterval, 5, 0) == 0);
    }

    SECTION("nothing is generated when loot is not less than looters") {
        LootGenerator gen{kBaseInterval, 1.0};
        CHECK(gen.Generate(kBaseInterval, 3, 3) == 0);
        CHECK(gen.Generate(kBaseInterval, 5, 3) == 0);
    }

    SECTION("generated loot fills the shortage with probability 1") {
        LootGenerator gen{kBaseInterval, 1.0};
        CHECK(gen.Generate(kBaseInterval, 0, 4) == 4);
        CHECK(gen.Generate(kBaseInterval, 1, 4) == 3);
    }

    SECTION("zero probability never generates loot") {
        LootGenerator gen{kBaseInterval, 0.0};
        for (int i = 0; i < 10; ++i) {
            CHECK(gen.Generate(10s, 0, 10) == 0);
        }
    }

    SECTION("zero time delta does not generate loot") {
        LootGenerator gen{kBaseInterval, 1.0};
        CHECK(gen.Generate(0ms, 0, 10) == 0);
    }

    SECTION("loot never exceeds the number of looters") {
        LootGenerator gen{kBaseInterval, 1.0};
        unsigned loot = 0;
        constexpr unsigned kLooters = 7;
        for (int i = 0; i < 20; ++i) {
            loot += gen.Generate(kBaseInterval, loot, kLooters);
            CHECK(loot <= kLooters);
        }
        CHECK(loot == kLooters);
    }

    SECTION("probability scales with elapsed time") {
        LootGenerator one_sec{kBaseInterval, 0.5, [] { return 1.0; }};
        CHECK(one_sec.Generate(1s, 0, 4) == 2);

        LootGenerator two_sec{kBaseInterval, 0.5, [] { return 1.0; }};
        CHECK(two_sec.Generate(2s, 0, 4) == 3);
    }

    SECTION("time without loot is accumulated between calls") {
        LootGenerator gen{kBaseInterval, 0.5, [] { return 1.0; }};
        CHECK(gen.Generate(500ms, 0, 1) == 0); 
        CHECK(gen.Generate(500ms, 0, 1) == 1);  
    }

    SECTION("time counter is reset after loot appears") {
        LootGenerator gen{kBaseInterval, 0.5, [] { return 1.0; }};
        CHECK(gen.Generate(1s, 0, 2) == 1);   
        CHECK(gen.Generate(100ms, 1, 2) == 0); 
    }

    SECTION("random generator value scales the probability") {
        LootGenerator gen{kBaseInterval, 1.0, [] { return 0.5; }};
        CHECK(gen.Generate(kBaseInterval, 0, 10) == 5);
    }
}
