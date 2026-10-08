// Host tests for src/core/RingBuffer.h, which already lived in src/core/ before
// the host target existed.

#include "core/RingBuffer.h"

#include <doctest/doctest.h>

#include <vector>

using Huginn::RingBuffer;

TEST_CASE("ring buffer: starts empty")
{
    RingBuffer<int, 4> rb;
    CHECK(rb.empty());
    CHECK(rb.size() == 0);
    CHECK(rb.capacity() == 4);
    CHECK(rb.begin() == rb.end());
}

TEST_CASE("ring buffer: keeps insertion order until full")
{
    RingBuffer<int, 4> rb;
    rb.push_back(1);
    rb.push_back(2);
    rb.push_back(3);
    CHECK(rb.size() == 3);
    CHECK(rb.front() == 1);
    CHECK(rb.back() == 3);
    CHECK(rb[1] == 2);
}

TEST_CASE("ring buffer: overwrites the oldest when full")
{
    RingBuffer<int, 3> rb;
    for (int i = 1; i <= 5; ++i) {
        rb.push_back(i);
    }
    CHECK(rb.size() == 3);
    std::vector<int> seen(rb.begin(), rb.end());
    CHECK(seen == std::vector<int>{ 3, 4, 5 });
    CHECK(rb.front() == 3);
    CHECK(rb.back() == 5);
}

TEST_CASE("ring buffer: pop_front drops the oldest; on empty it is a no-op")
{
    RingBuffer<int, 3> rb;
    rb.pop_front();
    CHECK(rb.empty());
    for (int i = 1; i <= 4; ++i) {
        rb.push_back(i);   // holds 2,3,4
    }
    rb.pop_front();
    CHECK(rb.size() == 2);
    CHECK(rb.front() == 3);
    rb.push_back(5);
    rb.push_back(6);       // wraps: 4,5,6
    std::vector<int> seen(rb.begin(), rb.end());
    CHECK(seen == std::vector<int>{ 4, 5, 6 });
}

TEST_CASE("ring buffer: clear empties it")
{
    RingBuffer<int, 2> rb;
    rb.push_back(7);
    rb.push_back(8);
    rb.clear();
    CHECK(rb.empty());
    rb.push_back(9);
    CHECK(rb.front() == 9);
    CHECK(rb.back() == 9);
}
