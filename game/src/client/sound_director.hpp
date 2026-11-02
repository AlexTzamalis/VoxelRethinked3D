#pragma once

#include "worldgen/climate.hpp"

#include <vxe/audio/audio.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vr {

// What walking on a block sounds like.
enum class StepSound : vxe::u8 {
    None,
    Dirt,
    Grass,
    Rough, // Coarse ground: mud, roots, podzol.
    Gravel,
    Stone,
    Dry, // Sand and clay.
    Snow,
    Wood,
    Metal, // Hard, smooth blocks.
    Leaves,
};
inline constexpr vxe::usize kStepSoundCount = 11;

// What the player's surroundings are like right now.
struct SoundScene {
    Biome biome = Biome::Plains;
    bool inWorld = false;
    bool walking = false; // On the ground and moving.
    bool sprinting = false;
    StepSound step = StepSound::None;
    bool underWater = false;
    vxe::f32 nearWater = 0.0F; // 0 is none in earshot, 1 is standing beside it.
    vxe::f32 sea = 0.0F;       // The same for open sea.
    vxe::f32 altitude = 0.0F;  // Blocks above the sea.
    vxe::f32 cover = 0.0F;     // Blocks of ground above the player, for caves.
    vxe::f32 daylight = 1.0F;  // 0 at night, 1 by day.
    vxe::f32 rain = 0.0F;      // 0 to 1: how hard it rains around the player.
};

// Picks what plays from where the player is (SoundDirector::update is told the
// scene, so it can be driven by tests and never looks at the world itself):
//
//   Forests     the birds of ambient/nature-forest-sound-*, by day
//   Night       ambient/night-ambient-*, one per night
//   Open land   ambient/light-ambient-*, quiet, more of it up high
//   Water       water/water-stream, as loud as the water is near
//   Caves       water/dripping-water-in-cave under enough ground
//   Rain        weather/calming-rain while it rains, with thunder now and then in a downpour
//   Footsteps   footsteps/*, by the block underfoot, faster when sprinting
//   Blocks      block/dig-*, short impacts by material, for breaking and placing
//   Music       ambient/ambient-*, a track now and then with quiet between
//
// Every category is read from the sounds folder at startup, so adding a file
// adds it to the game, and a missing file only means less sound.
//
// Thread safety: main thread only.
class SoundDirector {
public:
    SoundDirector(vxe::audio::AudioSystem& audio, std::filesystem::path folder);

    void update(const SoundScene& scene, vxe::f64 deltaSeconds);

    // One shots from what happens in the world.
    void blockBroken(const vxe::DVec3& position, StepSound material, bool glass);
    void blockPlaced(const vxe::DVec3& position, StepSound material);
    void splash(const vxe::DVec3& position);

    // The interface.
    void click();
    void screenChanged();

    // Music for the main menu, and silence when leaving it.
    void enterMenu();
    void leaveMenu();

    // How many seconds until the next piece of music, for tests.
    [[nodiscard]] vxe::f64 secondsUntilMusic() const noexcept { return m_musicWait; }

private:
    [[nodiscard]] std::filesystem::path pick(const std::vector<std::filesystem::path>& files);
    [[nodiscard]] vxe::f32 randomUnit() noexcept;
    void updateMusic(vxe::f64 deltaSeconds);
    void updateAmbience(const SoundScene& scene);
    void updateWeather(const SoundScene& scene, vxe::f64 deltaSeconds);
    void updateSteps(const SoundScene& scene);

    vxe::audio::AudioSystem& m_audio;
    std::filesystem::path m_folder;

    std::vector<std::filesystem::path> m_forest;
    std::vector<std::filesystem::path> m_night;
    std::vector<std::filesystem::path> m_light;
    std::vector<std::filesystem::path> m_music;
    std::array<std::vector<std::filesystem::path>, kStepSoundCount> m_digs; // By StepSound.
    std::vector<std::filesystem::path> m_glass;
    std::vector<std::filesystem::path> m_clicks;
    std::vector<std::filesystem::path> m_whooshes;
    std::vector<std::filesystem::path> m_thunder;
    std::filesystem::path m_rain;
    vxe::f64 m_untilThunder = 40.0;

    std::filesystem::path m_nightTrack;
    std::filesystem::path m_lightTrack;
    bool m_wasNight = false;
    vxe::f64 m_musicWait = 40.0; // The first track comes soon after arriving.
    vxe::u64 m_random = 0x9E3779B97F4A7C15ULL;
};

// Which step sound a block makes, from its name.
[[nodiscard]] StepSound stepSoundOf(std::string_view blockName) noexcept;

} // namespace vr
