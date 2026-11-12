#include "ui/screens/resource_packs_screen.hpp"

#include <doctest/doctest.h>

using namespace vr;

TEST_CASE("Pack lists drop blanks, repeats and the default pack") {
    CHECK(parsePackList("").empty());
    CHECK(parsePackList(" , ,").empty());
    CHECK(parsePackList("Faithful, default , Faithful,Sharp ") ==
          std::vector<std::string>{"Faithful", "Sharp"});
    CHECK(joinPackList({"Faithful", "Sharp"}) == "Faithful, Sharp");
    CHECK(joinPackList({}).empty());
    CHECK(parsePackList(joinPackList({"A pack", "B"})) == std::vector<std::string>{"A pack", "B"});
}
