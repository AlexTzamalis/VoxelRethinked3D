#include "sound_director.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace vr {

using vxe::f32;
using vxe::f64;
using vxe::u64;
using vxe::usize;
using vxe::audio::Bus;

namespace {

constexpr f32 kBedFade = 2.5F;   // Seconds for ambience to follow the player.
constexpr f32 kStepFade = 0.12F; // Footsteps stop as soon as the feet do.
constexpr f32 kMusicFade = 4.0F;
constexpr f64 kShortestGap = 150.0; // Seconds of quiet between music tracks.
constexpr f64 kLongestGap = 360.0;
constexpr f32 kCaveCover = 14.0F; // Blocks of ground overhead that make a cave.

// The files of a folder whose names start with `prefix`, in a fixed order, so
// that one seed of the random sequence always means the same tracks.
std::vector<std::filesystem::path> filesIn(const std::filesystem::path& folder, std::string_view prefix) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        const std::string name = entry.path().filename().string();
        const std::string extension = entry.path().extension().string();
        if (name.starts_with(prefix) && (extension == ".ogg" || extension == ".mp3" || extension == ".wav")) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

f32 pitchOf(StepSound material) noexcept {
    switch (material) {
    case StepSound::Grass:
        return 1.15F;
    case StepSound::Leaves:
        return 1.4F;
    case StepSound::Stone:
        return 0.8F;
    default:
        return 1.0F;
    }
}

f32 volumeOf(StepSound material) noexcept {
    return material == StepSound::Leaves ? 0.45F : 0.85F;
}

bool isForest(Biome biome) noexcept {
    switch (biome) {
    case Biome::Forest:
    case Biome::BirchForest:
    case Biome::FlowerForest:
    case Biome::DarkForest:
    case Biome::PaleForest:
    case Biome::Jungle:
    case Biome::Swamp:
    case Biome::Taiga:
    case Biome::DeepSpruceForest:
    case Biome::WoodedSlopes:
    case Biome::SnowyTaiga:
        return true;
    default:
        return false;
    }
}

} // namespace

StepSound stepSoundOf(std::string_view name) noexcept {
    const auto has = [&](std::string_view part) { return name.find(part) != std::string_view::npos; };
    if (has("snow")) {
        return StepSound::Snow;
    }
    if (has("leaves")) {
        return StepSound::Leaves;
    }
    if (has("planks") || has("log")) {
        return StepSound::Wood;
    }
    // Polished and shining blocks ring; the rest of the rock crunches.
    if (has("polished") || has("lumen") || has("glass") || has("foundation")) {
        return StepSound::Metal;
    }
    if (has("gravel") || has("cobble")) {
        return StepSound::Gravel;
    }
    if (has("stone") || has("granite") || has("andesite") || has("tuff") || has("calcite") || has("basalt") ||
        has("deepslate") || has("brick") || has("ore")) {
        return StepSound::Stone;
    }
    if (has("sand") || has("clay")) {
        return StepSound::Dry;
    }
    if (has("coarse") || has("rooted") || has("mud") || has("podzol") || has("roots")) {
        return StepSound::Rough;
    }
    return has("grass") ? StepSound::Grass : StepSound::Dirt;
}

SoundDirector::SoundDirector(vxe::audio::AudioSystem& audio, std::filesystem::path folder)
    : m_audio(audio)
    , m_folder(std::move(folder)) {
    const std::filesystem::path ambient = m_folder / "ambient";
    m_forest = filesIn(ambient, "nature-forest");
    m_night = filesIn(ambient, "night-ambient");
    m_light = filesIn(ambient, "light-ambient");
    m_music = filesIn(ambient, "ambient-");
    const std::filesystem::path block = m_folder / "block";
    const auto digs = [&](StepSound material, std::string_view prefix) {
        m_digs.at(static_cast<usize>(material)) = filesIn(block, prefix);
    };
    digs(StepSound::Dirt, "dig-dirt");
    digs(StepSound::Grass, "dig-dirt");
    digs(StepSound::Leaves, "dig-dirt");
    digs(StepSound::Rough, "dig-stone");
    digs(StepSound::Stone, "dig-stone");
    digs(StepSound::Gravel, "dig-gravel");
    digs(StepSound::Dry, "dig-sand");
    digs(StepSound::Snow, "dig-snow");
    digs(StepSound::Wood, "dig-wood");
    digs(StepSound::Metal, "dig-metal");
    m_glass = filesIn(m_folder / "block", "glass-break");
    m_clicks = filesIn(m_folder / "ui", "mouse-click");
    m_whooshes = filesIn(m_folder / "ui", "whoosh");
    m_lightTrack = pick(m_light);
    m_nightTrack = pick(m_night);
}

f32 SoundDirector::randomUnit() noexcept {
    // Variety only; nothing about the world depends on it.
    m_random ^= m_random << 13U;
    m_random ^= m_random >> 7U;
    m_random ^= m_random << 17U;
    return static_cast<f32>(m_random >> 40U) / 16777216.0F;
}

std::filesystem::path SoundDirector::pick(const std::vector<std::filesystem::path>& files) {
    if (files.empty()) {
        return {};
    }
    return files[static_cast<usize>(randomUnit() * static_cast<f32>(files.size())) % files.size()];
}

void SoundDirector::update(const SoundScene& scene, f64 deltaSeconds) {
    if (!scene.inWorld) {
        updateMusic(deltaSeconds);
        return;
    }
    updateAmbience(scene);
    updateSteps(scene);
    updateMusic(deltaSeconds);
}

void SoundDirector::updateAmbience(const SoundScene& scene) {
    const bool night = scene.daylight < 0.3F;
    if (night && !m_wasNight) {
        m_nightTrack = pick(m_night); // Each night sounds a little different.
    }
    m_wasNight = night;
    // Rock overhead shuts out the world above.
    const f32 outdoors = 1.0F - std::clamp((scene.cover - 4.0F) / 10.0F, 0.0F, 1.0F);
    const f32 muffled = outdoors * (scene.underWater ? 0.2F : 1.0F);
    const f32 day = std::clamp((scene.daylight - 0.2F) / 0.5F, 0.0F, 1.0F);

    const f32 forest = isForest(scene.biome) ? day * muffled * 0.85F : 0.0F;
    if (!m_forest.empty()) {
        m_audio.setLoop("forest", m_forest.front(), Bus::Ambient, forest, kBedFade);
    }
    if (!m_nightTrack.empty()) {
        m_audio.setLoop("night", m_nightTrack, Bus::Ambient, (1.0F - day) * muffled * 0.7F, kBedFade * 2.0F);
    }
    // Wind grows with height; lowlands keep only a soft bed under the rest.
    const f32 height = std::clamp(scene.altitude / 180.0F, 0.0F, 1.0F);
    if (!m_lightTrack.empty()) {
        m_audio.setLoop("air", m_lightTrack, Bus::Ambient, (0.18F + (0.55F * height)) * muffled, kBedFade);
    }
    const f32 water = std::max(scene.nearWater, scene.sea * 0.8F);
    m_audio.setLoop("water", m_folder / "water" / "water-stream.mp3", Bus::Ambient, water * outdoors * 0.9F,
                    kBedFade * 0.6F, scene.sea > scene.nearWater ? 0.8F : 1.0F);
    const f32 cave = scene.cover >= kCaveCover ? 0.75F : 0.0F;
    m_audio.setLoop("cave", m_folder / "water" / "dripping-water-in-cave.mp3", Bus::Ambient, cave, kBedFade);
}

void SoundDirector::updateSteps(const SoundScene& scene) {
    // Fewer recordings than kinds of ground, so some share one at another pitch:
    // the same steps sound lighter on grass and harder on stone.
    struct Step {
        std::string_view file;
        f32 pitch = 1.0F;
        f32 volume = 0.55F;
    };
    Step step;
    switch (scene.step) {
    case StepSound::None:
    case StepSound::Dirt:
        step = {.file = "footsteps-dirt.mp3"};
        break;
    case StepSound::Grass:
        step = {.file = "footsteps-dirt.mp3", .pitch = 1.14F, .volume = 0.42F};
        break;
    case StepSound::Leaves:
        step = {.file = "footsteps-dirt.mp3", .pitch = 1.32F, .volume = 0.3F};
        break;
    case StepSound::Rough:
        step = {.file = "footsteps-dirt-gravel.mp3"};
        break;
    case StepSound::Stone:
        step = {.file = "footsteps-dirt-gravel.mp3", .pitch = 0.82F, .volume = 0.6F};
        break;
    case StepSound::Gravel:
        step = {.file = "footsteps-gravel.mp3", .volume = 0.6F};
        break;
    case StepSound::Dry:
        step = {.file = "footsteps-dry.mp3"};
        break;
    case StepSound::Snow:
        step = {.file = "footsteps-in-thin-snow.mp3"};
        break;
    case StepSound::Wood:
        step = {.file = "footsteps-on-wood.mp3"};
        break;
    case StepSound::Metal:
        step = {.file = "footsteps-metal.mp3", .volume = 0.4F};
        break;
    }
    const bool stepping = scene.walking && !scene.underWater && scene.step != StepSound::None;
    m_audio.setLoop("steps", m_folder / "footsteps" / step.file, Bus::Effects, stepping ? step.volume : 0.0F,
                    kStepFade, step.pitch * (scene.sprinting ? 1.3F : 1.0F));
}

void SoundDirector::updateMusic(f64 deltaSeconds) {
    if (m_audio.isMusicPlaying() || m_music.empty()) {
        return;
    }
    m_musicWait -= deltaSeconds;
    if (m_musicWait <= 0.0) {
        m_audio.playMusic(pick(m_music), kMusicFade);
        m_musicWait = kShortestGap + (static_cast<f64>(randomUnit()) * (kLongestGap - kShortestGap));
    }
}

// A block's impact in the file's own voice, lighter things higher up. Breaking
// sits a little lower than placing, and a small random pitch keeps a row of
// the same block from sounding like one sample.
void SoundDirector::blockBroken(const vxe::DVec3& position, StepSound material, bool glass) {
    if (glass && !m_glass.empty()) {
        m_audio.playAt(pick(m_glass), position, Bus::Effects, 0.8F, 0.9F + (randomUnit() * 0.2F));
        return;
    }
    const auto& files = m_digs.at(static_cast<usize>(material));
    if (!files.empty()) {
        m_audio.playAt(pick(files), position, Bus::Effects, volumeOf(material),
                       pitchOf(material) * (0.88F + (randomUnit() * 0.12F)));
    }
}

void SoundDirector::blockPlaced(const vxe::DVec3& position, StepSound material) {
    const auto& files = m_digs.at(static_cast<usize>(material));
    if (!files.empty()) {
        m_audio.playAt(pick(files), position, Bus::Effects, volumeOf(material) * 0.75F,
                       pitchOf(material) * (1.04F + (randomUnit() * 0.12F)));
    }
}

void SoundDirector::splash(const vxe::DVec3& position) {
    m_audio.playAt(m_folder / "water" / "water-splash.mp3", position, Bus::Effects, 0.8F,
                   0.9F + (randomUnit() * 0.2F));
}

void SoundDirector::click() {
    if (!m_clicks.empty()) {
        m_audio.play(pick(m_clicks), Bus::Interface, 0.7F);
    }
}

void SoundDirector::screenChanged() {
    if (!m_whooshes.empty()) {
        m_audio.play(pick(m_whooshes), Bus::Interface, 0.35F);
    }
}

void SoundDirector::enterMenu() {
    m_audio.fadeAll(1.5F);
    if (!m_music.empty()) {
        m_audio.playMusic(pick(m_music), kMusicFade);
    }
}

void SoundDirector::leaveMenu() {
    m_audio.stopMusic(2.0F);
    m_musicWait = 40.0;
}

} // namespace vr
