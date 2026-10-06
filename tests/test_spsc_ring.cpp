#include "midipad/SpscRing.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace midipad;

TEST_CASE("the ring keeps order and refuses to overflow") {
    SpscRing<int, 4> ring;
    CHECK_FALSE(ring.pop());
    for (int i = 0; i < 4; ++i)
        CHECK(ring.push(i));
    CHECK_FALSE(ring.push(4));
    CHECK(ring.pop() == 0);
    CHECK(ring.push(4));
    for (int i = 1; i <= 4; ++i)
        CHECK(ring.pop() == i);
    CHECK_FALSE(ring.pop());
}

TEST_CASE("one producer and one consumer exchange every value in order") {
    SpscRing<int, 64> ring;
    constexpr int kCount = 200000;
    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i)
            while (!ring.push(i))
                std::this_thread::yield();
    });
    int expected = 0;
    bool ordered = true;
    while (expected < kCount) {
        if (auto v = ring.pop()) {
            ordered = ordered && *v == expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(ordered);
}
