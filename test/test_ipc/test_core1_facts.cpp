// The Core 1 fact ring, single-threaded: order, full, wrap, reset. The
// barriers between the cores are not tested here.

#include "doctest.h"

#include "../../src/rp2350/ipc/core1_facts.cpp"

TEST_CASE("facts pop in the order they were pushed") {
    core1FactsReset();
    CHECK(core1FactPop() == FACT_NONE);
    CHECK(core1FactPush(FACT_ESTOP_SWEPT));
    CHECK(core1FactPush(FACT_RING_DRAINED));
    CHECK(core1FactPop() == FACT_ESTOP_SWEPT);
    CHECK(core1FactPop() == FACT_RING_DRAINED);
    CHECK(core1FactPop() == FACT_NONE);
    CHECK_FALSE(core1FactsLost());
}

TEST_CASE("a push into a full ring is dropped and sets lost") {
    core1FactsReset();
    for (unsigned i = 0; i < FACT_SLOTS; i++) CHECK(core1FactPush(FACT_RING_DRAINED));
    CHECK_FALSE(core1FactsLost());
    CHECK_FALSE(core1FactPush(FACT_ESTOP_SWEPT));
    CHECK(core1FactsLost());
    for (unsigned i = 0; i < FACT_SLOTS; i++) CHECK(core1FactPop() == FACT_RING_DRAINED);
    CHECK(core1FactPop() == FACT_NONE);
    core1FactsLostClear();
    CHECK_FALSE(core1FactsLost());
}

TEST_CASE("the indices wrap") {
    core1FactsReset();
    for (unsigned i = 0; i < 300; i++) {
        const Core1Fact f = (i & 1) ? FACT_ESTOP_SWEPT : FACT_RING_DRAINED;
        REQUIRE(core1FactPush(f));
        REQUIRE(core1FactPush(FACT_RING_DRAINED));
        REQUIRE(core1FactPop() == f);
        REQUIRE(core1FactPop() == FACT_RING_DRAINED);
    }
    CHECK(core1FactPop() == FACT_NONE);
}

TEST_CASE("reset empties the ring and clears lost") {
    core1FactsReset();
    for (unsigned i = 0; i <= FACT_SLOTS; i++) core1FactPush(FACT_RING_DRAINED);
    CHECK(core1FactsLost());
    core1FactsReset();
    CHECK(core1FactPop() == FACT_NONE);
    CHECK_FALSE(core1FactsLost());
}
