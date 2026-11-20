// The streaming bookkeeping: the geometry pool's range allocator and the
// asset table's references and keep list.

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "sr/core/AssetTable.h"
#include "sr/core/RangeAllocator.h"

using sr::core::AssetHandle;
using sr::core::AssetTable;
using sr::core::RangeAllocator;

TEST_CASE("RangeAllocator: best fit, rounding, exhaustion") {
    RangeAllocator a(4096, 256);
    CHECK(a.Capacity() == 4096);
    const auto x = a.Allocate(100); // rounds to 256
    REQUIRE(x);
    CHECK(*x == 0);
    CHECK(a.SizeOf(*x) == 256);
    const auto y = a.Allocate(1000); // 1024
    REQUIRE(y);
    CHECK(*y == 256);
    CHECK(a.Used() == 1280);
    CHECK(a.LargestFree() == 4096 - 1280);
    CHECK_FALSE(a.Allocate(4096));
    CHECK(a.Allocate(4096 - 1280)); // exactly the rest
    CHECK_FALSE(a.Allocate(1));
    CHECK(a.FreeRanges() == 0);
}

TEST_CASE("RangeAllocator: frees merge with both neighbours") {
    RangeAllocator a(1024, 256);
    const auto r0 = *a.Allocate(256), r1 = *a.Allocate(256), r2 = *a.Allocate(256), r3 = *a.Allocate(256);
    CHECK(a.FreeRanges() == 0);
    CHECK(a.Free(r0));
    CHECK(a.Free(r2));
    CHECK(a.FreeRanges() == 2);
    CHECK(a.Free(r1)); // joins r0 and r2
    CHECK(a.FreeRanges() == 1);
    CHECK(a.LargestFree() == 768);
    CHECK_FALSE(a.Free(r1)); // not an allocation any more
    CHECK_FALSE(a.Free(17));
    CHECK(a.Free(r3));
    CHECK(a.FreeRanges() == 1);
    CHECK(a.LargestFree() == 1024);
    CHECK(a.Used() == 0);
}

TEST_CASE("RangeAllocator: best fit picks the smallest hole") {
    RangeAllocator a(8192, 256);
    std::vector<std::uint64_t> r;
    for (int i = 0; i < 8; ++i) r.push_back(*a.Allocate(1024));
    a.Free(r[1]); // a 1024 hole at 1024
    a.Free(r[4]);
    a.Free(r[5]); // a 2048 hole at 4096
    const auto small = a.Allocate(512);
    REQUIRE(small);
    CHECK(*small == 1024); // the 1024 hole, not the 2048 one
    const auto big = a.Allocate(2048);
    REQUIRE(big);
    CHECK(*big == 4096);
}

TEST_CASE("RangeAllocator: random churn keeps the books exact") {
    RangeAllocator a(1 << 20, 256);
    std::mt19937 rng(7);
    std::vector<std::uint64_t> live;
    for (int step = 0; step < 20000; ++step) {
        if (live.empty() || (rng() % 3) != 0) {
            if (const auto r = a.Allocate(1 + rng() % 8000)) live.push_back(*r);
        } else {
            const std::size_t i = rng() % live.size();
            CHECK(a.Free(live[i]));
            live[i] = live.back();
            live.pop_back();
        }
    }
    std::uint64_t used = 0;
    for (std::uint64_t r : live) used += a.SizeOf(r);
    CHECK(used == a.Used());
    for (std::uint64_t r : live) a.Free(r);
    CHECK(a.Used() == 0);
    CHECK(a.FreeRanges() == 1); // everything merged back
    CHECK(a.LargestFree() == a.Capacity());
}

TEST_CASE("AssetTable: one entry per key, reference counted") {
    AssetTable table;
    bool created = false;
    const AssetHandle a = table.Acquire("res\\a.bsr", created);
    CHECK(created);
    const AssetHandle a2 = table.Acquire("res\\a.bsr", created);
    CHECK_FALSE(created);
    CHECK(a == a2);
    CHECK(table.References(a) == 2);
    const AssetHandle b = table.Acquire("res\\b.bsr", created);
    CHECK(created);
    CHECK_FALSE(a == b);
    CHECK(table.Live() == 2);
    CHECK(table.Key(b) == "res\\b.bsr");
    CHECK(table.Find("res\\b.bsr") == b);
    CHECK_FALSE(table.Find("res\\c.bsr"));
}

TEST_CASE("AssetTable: unreferenced entries are kept, oldest first, until removed") {
    AssetTable table;
    bool created = false;
    const AssetHandle a = table.Acquire("a", created);
    const AssetHandle b = table.Acquire("b", created);
    const AssetHandle c = table.Acquire("c", created);
    CHECK(table.Release(b));
    CHECK(table.Release(a));
    CHECK(table.Release(c));
    CHECK(table.Unreferenced() == 3);
    CHECK(table.Live() == 3); // still there: a new reference finds them loaded
    CHECK(table.OldestUnreferenced() == b);
    CHECK(table.OldestUnreferenced(1) == a);
    CHECK(table.OldestUnreferenced(2) == c);
    CHECK_FALSE(table.OldestUnreferenced(3));

    // Used again: out of the keep list, not created again.
    CHECK(table.Acquire("b", created) == b);
    CHECK_FALSE(created);
    CHECK(table.OldestUnreferenced() == a);
    CHECK_FALSE(table.Remove(b)); // referenced: cannot go

    CHECK(table.Remove(a));
    CHECK_FALSE(table.Valid(a));
    CHECK(table.References(a) == 0);
    CHECK_FALSE(table.Release(a)); // stale
    CHECK(table.OldestUnreferenced() == c);
    CHECK(table.Live() == 2);

    // The slot comes back with a new generation; the old handle stays stale.
    const AssetHandle d = table.Acquire("d", created);
    CHECK(created);
    CHECK(d.index == a.index);
    CHECK_FALSE(d == a);
    CHECK_FALSE(table.Valid(a));
    CHECK(table.Valid(d));
    CHECK_FALSE(table.Find("a"));
}

TEST_CASE("AssetTable: a removed key loads again as a new entry") {
    AssetTable table;
    bool created = false;
    const AssetHandle a = table.Acquire("a", created);
    table.Release(a);
    CHECK(table.Remove(a));
    const AssetHandle again = table.Acquire("a", created);
    CHECK(created);
    CHECK_FALSE(again == a);
    CHECK(table.References(again) == 1);
    CHECK(table.Unreferenced() == 0);
}
