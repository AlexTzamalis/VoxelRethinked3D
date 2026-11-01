#include "client/sound_director.hpp"

#include <doctest/doctest.h>

using vr::StepSound;
using vr::stepSoundOf;

TEST_CASE("Blocks sound like what they are made of") {
    CHECK(stepSoundOf("grass_block") == StepSound::Grass);
    CHECK(stepSoundOf("dirt") == StepSound::Dirt);
    CHECK(stepSoundOf("coarse_dirt") == StepSound::Rough);
    CHECK(stepSoundOf("podzol") == StepSound::Rough);
    CHECK(stepSoundOf("gravel") == StepSound::Gravel);
    CHECK(stepSoundOf("cobblestone") == StepSound::Gravel);
    CHECK(stepSoundOf("stone") == StepSound::Stone);
    CHECK(stepSoundOf("sandstone") == StepSound::Stone);
    CHECK(stepSoundOf("iron_ore") == StepSound::Stone);
    CHECK(stepSoundOf("sand") == StepSound::Dry);
    CHECK(stepSoundOf("snow_block") == StepSound::Snow);
    CHECK(stepSoundOf("snowy_grass") == StepSound::Snow);
    CHECK(stepSoundOf("oak_log[axis=y]") == StepSound::Wood);
    CHECK(stepSoundOf("spruce_planks") == StepSound::Wood);
    CHECK(stepSoundOf("polished_granite") == StepSound::Metal);
    CHECK(stepSoundOf("oak_leaves") == StepSound::Leaves);
}
