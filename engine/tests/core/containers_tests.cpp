#include <vxe/core/containers.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/small_vector.hpp>
#include <vxe/core/string_id.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace vxe::literals;
using vxe::u64;
using vxe::usize;

TEST_CASE("XXH3 hashes are stable and seed dependent") {
    // Reference value of XXH3_64bits("") from the xxHash test suite. Saves and
    // caches rely on hashes never changing, so this guards library upgrades.
    CHECK(vxe::hashString("") == 0x2D06800538D394C2ULL);
    CHECK(vxe::hashString("voxel") == vxe::hashString("voxel"));
    CHECK(vxe::hashString("voxel") != vxe::hashString("voxels"));
    CHECK(vxe::hashString("voxel", 1) != vxe::hashString("voxel", 2));

    const std::array<std::byte, 3> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    CHECK(vxe::hashBytes(bytes) == vxe::hashString("abc"));

    struct Packed {
        vxe::i32 x, y, z;
    };
    CHECK(vxe::hashValue(Packed{1, 2, 3}) == vxe::hashValue(Packed{1, 2, 3}));
    CHECK(vxe::hashValue(Packed{1, 2, 3}) != vxe::hashValue(Packed{3, 2, 1}));
}

TEST_CASE("hashCombine depends on order and every input") {
    constexpr u64 kAb = vxe::hashCombine(vxe::hashCombine(0, 1), 2);
    constexpr u64 kBa = vxe::hashCombine(vxe::hashCombine(0, 2), 1);
    static_assert(kAb != kBa);
    CHECK(vxe::hashCombine(0, 1) != vxe::hashCombine(0, 2));
}

TEST_CASE("FNV-1a matches the reference values") {
    static_assert(vxe::fnv1a64("") == 0xcbf29ce484222325ULL);
    static_assert(vxe::fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
    static_assert(vxe::fnv1a64("foobar") == 0x85944171f73967e8ULL);
}

TEST_CASE("StringId compares by hash and works at compile time") {
    constexpr vxe::StringId kJump = "gameplay.jump"_sid;
    static_assert(kJump.value() == vxe::fnv1a64("gameplay.jump"));
    static_assert(kJump == vxe::StringId("gameplay.jump"));
    static_assert(kJump != "gameplay.crouch"_sid);
    static_assert(vxe::StringId().isEmpty());

    // A runtime string that dies right away must still give a usable id.
    const vxe::StringId runtime = [] {
        const std::string temporary = std::string("gameplay.") + "jump";
        return vxe::StringId(temporary);
    }();
    CHECK(runtime == kJump);

#if VXE_STRING_ID_DEBUG_NAMES
    CHECK(kJump.debugName() == "gameplay.jump");
    CHECK(runtime.debugName() == "gameplay.jump"); // Kept alive by the registry.
#else
    CHECK(runtime.debugName().empty());
#endif
}

TEST_CASE("StringId works as a map key") {
    vxe::HashMap<vxe::StringId, int> actions;
    actions[vxe::StringId("move.forward")] = 1;
    actions["move.back"_sid] = 2;
    CHECK(actions.at("move.forward"_sid) == 1);
    CHECK(actions.contains(vxe::StringId("move.back")));
    CHECK_FALSE(actions.contains("move.left"_sid));
}

TEST_CASE("HashMap and StringMap basics") {
    vxe::HashMap<int, std::string> map;
    for (int i = 0; i < 1000; ++i) {
        map.emplace(i, std::to_string(i));
    }
    CHECK(map.size() == 1000);
    CHECK(map.at(500) == "500");
    map.erase(500);
    CHECK_FALSE(map.contains(500));

    vxe::StringMap<int> byName;
    byName.emplace("stone", 1);
    const std::string_view key = "stone";
    // Lookup by string_view without building a std::string.
    REQUIRE(byName.find(key) != byName.end());
    CHECK(byName.find(key)->second == 1);

    const vxe::HashSet<u64> set{1, 2, 3, 2};
    CHECK(set.size() == 3);
}

TEST_CASE("SmallVector stays inline up to N and spills afterwards") {
    vxe::SmallVector<int, 4> values;
    CHECK(values.isInline());
    CHECK(values.capacity() == 4);
    for (int i = 0; i < 4; ++i) {
        values.push_back(i);
    }
    CHECK(values.isInline());

    values.push_back(4);
    CHECK_FALSE(values.isInline());
    CHECK(values.size() == 5);
    CHECK(values.capacity() >= 5);
    for (usize i = 0; i < values.size(); ++i) {
        CHECK(values[i] == static_cast<int>(i));
    }

    values.pop_back();
    CHECK(values.back() == 3);
    values.clear();
    CHECK(values.empty());
}

TEST_CASE("SmallVector push_back of its own element while growing") {
    vxe::SmallVector<std::string, 2> values{"first", "second"};
    values.push_back(values[0]); // Grows; the argument lives in the old storage.
    CHECK(values.size() == 3);
    CHECK(values[2] == "first");
}

TEST_CASE("SmallVector copy, move, resize and erase") {
    vxe::SmallVector<std::unique_ptr<int>, 2> owners;
    owners.push_back(std::make_unique<int>(1));
    vxe::SmallVector<std::unique_ptr<int>, 2> movedInline(std::move(owners));
    CHECK(movedInline.size() == 1);
    CHECK(*movedInline[0] == 1);
    // Checking the moved from state on purpose.
    CHECK(owners.empty()); // NOLINT(bugprone-use-after-move, clang-analyzer-cplusplus.Move)

    vxe::SmallVector<int, 2> small{1, 2, 3, 4};
    CHECK_FALSE(small.isInline());
    const vxe::SmallVector<int, 2> copy = small;
    CHECK(copy == small);

    vxe::SmallVector<int, 2> movedHeap = std::move(small);
    CHECK(movedHeap.size() == 4);
    // Checking the moved from state on purpose.
    CHECK(small.isInline()); // NOLINT(bugprone-use-after-move, clang-analyzer-cplusplus.Move)

    movedHeap.erase(movedHeap.begin() + 1);
    CHECK(movedHeap == vxe::SmallVector<int, 2>{1, 3, 4});

    movedHeap.resize(6);
    CHECK(movedHeap.size() == 6);
    CHECK(movedHeap[5] == 0);
    movedHeap.resize(1);
    CHECK(movedHeap.size() == 1);

    std::vector<int> fromAlgorithm;
    std::ranges::copy(copy, std::back_inserter(fromAlgorithm));
    CHECK(fromAlgorithm == std::vector<int>{1, 2, 3, 4});
}
