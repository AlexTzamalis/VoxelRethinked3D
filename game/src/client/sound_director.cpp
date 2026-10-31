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
    if (has("planks") || has("log")) {
        return StepSound::Wood;
    }
    if (has("sand") || has("clay")) {
        return StepSound::Dry;
    }
    if (has("gravel") || has("cobble") || has("stone") || has("granite") || has("andesite") || has("tuff") ||
        has("calcite") || has("basalt") || has("deepslate") || has("brick")) {
        return StepSound::Gravel;
    }
    return StepSound::Dirt;
}

SoundDirector::SoundDirector(vxe::audio::AudioSystem& audio, std::filesystem::path folder)
    : m_audio(audio)
    , m_folder(std::move(folder)) {
    const std::filesystem::path ambient = m_folder / "ambient";
    m_forest = filesIn(ambient, "nature-forest");
    m_night = filesIn(ambient, "night-ambient");
    m_light = filesIn(ambient, "light-ambient");
    m_music = filesIn(ambient, "ambient-");
    m_impacts = filesIn(m_folder / "block", "ground-impact");
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
    const std::filesystem::path steps = m_folder / "footsteps";
    std::filesystem::path file;
    switch (scene.step) {
    case StepSound::None:
        break;
    case StepSound::Dirt:
        file = steps / "footsteps-dirt.mp3";
        break;
    case StepSound::Gravel:
        file = steps / "footsteps-gravel.mp3";
        break;
    case StepSound::Dry:
        file = steps / "footsteps-dry.mp3";
        break;
    case StepSound::Snow:
        file = steps / "footsteps-in-thin-snow.mp3";
        break;
    case StepSound::Wood:
        file = steps / "footsteps-on-wood.mp3";
        break;
    case StepSound::Metal:
        file = steps / "footsteps-metal.mp3";
        break;
    }
    const bool stepping = scene.walking && !scene.underWater && !file.empty();
    m_audio.setLoop("steps", file.empty() ? steps / "footsteps-dirt.mp3" : file, Bus::Effects,
                    stepping ? 0.55F : 0.0F, kStepFade, scene.sprinting ? 1.3F : 1.0F);
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

void SoundDirector::blockBroken(const vxe::DVec3& position, bool glass) {
    const auto& files = glass && !m_glass.empty() ? m_glass : m_impacts;
    if (!files.empty()) {
        m_audio.playAt(pick(files), position, Bus::Effects, 0.8F, 0.85F + (randomUnit() * 0.3F));
    }
}

void SoundDirector::blockPlaced(const vxe::DVec3& position) {
    if (!m_impacts.empty()) {
        m_audio.playAt(pick(m_impacts), position, Bus::Effects, 0.6F, 1.05F + (randomUnit() * 0.25F));
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
