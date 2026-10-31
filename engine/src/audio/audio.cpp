#include "vxe/audio/audio.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#define STB_VORBIS_HEADER_ONLY
#include <miniaudio.h>
#include <stb_vorbis.c>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vxe::audio {
namespace {

constexpr usize kBusCount = 4;
constexpr f32 kNearDistance = 4.0F; // Blocks within which a sound plays at full volume.
constexpr f32 kFarDistance = 64.0F; // Blocks at which it has faded to nearly nothing.
constexpr usize kMaxOneShots = 48;  // More at once is noise, and the oldest would cut off anyway.

// A sound the system holds. ma_sound must not move once started.
struct Sound {
    ma_sound sound{};
};

struct Loop {
    std::string name;
    std::string file;
    std::unique_ptr<Sound> sound;
    f32 volume = 0.0F;
    f32 target = 0.0F;
    f32 fadePerSecond = 1.0F;
};

ma_result openSound(ma_engine& engine, const std::filesystem::path& file, ma_uint32 flags,
                    ma_sound_group* group, ma_sound& sound) {
#if defined(_WIN32)
    return ma_sound_init_from_file_w(&engine, file.c_str(), flags, group, nullptr, &sound);
#else
    return ma_sound_init_from_file(&engine, file.c_str(), flags, group, nullptr, &sound);
#endif
}

} // namespace

struct AudioSystem::Impl {
    ma_engine engine{};
    std::array<ma_sound_group, kBusCount> groups{};
    bool available = false;

    config::Setting<f32> master;
    std::array<config::Setting<f32>, kBusCount> volumes;

    std::vector<std::unique_ptr<Sound>> shots;
    std::vector<Loop> loops;
    u32 fadingCount = 0; // Numbers the loops that lost their name to a newer one.
};

AudioSystem::AudioSystem(const AudioDesc& desc)
    : m_impl(std::make_unique<Impl>()) {
    VXE_ASSERT(desc.settings != nullptr, "AudioSystem needs settings");
    const auto volume = [&](std::string_view id, std::string_view name, std::string_view text, f32 value) {
        config::FloatSettingDesc setting{{.id = id, .displayName = name, .description = text}};
        setting.defaultValue = value;
        setting.min = 0.0F;
        setting.max = 1.0F;
        setting.step = 0.05F;
        return desc.settings->registerFloat(setting);
    };
    m_impl->master = volume("Audio.Master", "Master volume", "The volume of everything.", 1.0F);
    m_impl->volumes[static_cast<usize>(Bus::Music)] =
        volume("Audio.Music", "Music", "The volume of music.", 0.6F);
    m_impl->volumes[static_cast<usize>(Bus::Ambient)] =
        volume("Audio.Ambient", "Ambience", "Birds, wind, water and other sounds of the world.", 0.8F);
    m_impl->volumes[static_cast<usize>(Bus::Effects)] =
        volume("Audio.Effects", "Effects", "Blocks, footsteps and splashes.", 0.8F);
    m_impl->volumes[static_cast<usize>(Bus::Interface)] =
        volume("Audio.Interface", "Interface", "Menu sounds.", 0.7F);

    ma_engine_config config = ma_engine_config_init();
    config.noDevice = desc.device ? MA_FALSE : MA_TRUE;
    config.channels = 2;
    config.sampleRate = 48000;
    if (const ma_result result = ma_engine_init(&config, &m_impl->engine); result != MA_SUCCESS) {
        VXE_LOG_WARN(log::core, "No sound: {}", ma_result_description(result));
        return;
    }
    for (ma_sound_group& group : m_impl->groups) {
        if (ma_sound_group_init(&m_impl->engine, 0, nullptr, &group) != MA_SUCCESS) {
            VXE_LOG_WARN(log::core, "No sound: could not create a mixing group");
            ma_engine_uninit(&m_impl->engine);
            return;
        }
    }
    m_impl->available = true;
    VXE_LOG_INFO(log::core, "Sound ready at {} Hz", ma_engine_get_sample_rate(&m_impl->engine));
}

AudioSystem::~AudioSystem() {
    if (!m_impl->available) {
        return;
    }
    // Sounds first, as they feed the groups, which feed the engine.
    for (const std::unique_ptr<Sound>& shot : m_impl->shots) {
        ma_sound_uninit(&shot->sound);
    }
    for (const Loop& loop : m_impl->loops) {
        ma_sound_uninit(&loop.sound->sound);
    }
    for (ma_sound_group& group : m_impl->groups) {
        ma_sound_group_uninit(&group);
    }
    ma_engine_uninit(&m_impl->engine);
}

bool AudioSystem::isAvailable() const noexcept {
    return m_impl->available;
}

usize AudioSystem::activeSounds() const noexcept {
    return m_impl->shots.size() + m_impl->loops.size();
}

void AudioSystem::update(f64 deltaSeconds) {
    VXE_PROFILE_FUNCTION();
    if (!m_impl->available) {
        return;
    }
    // Volumes are read every frame, so settings apply live.
    ma_engine_set_volume(&m_impl->engine, m_impl->master.get());
    for (usize bus = 0; bus < kBusCount; ++bus) {
        ma_sound_group_set_volume(&m_impl->groups.at(bus), m_impl->volumes.at(bus).get());
    }
    std::erase_if(m_impl->shots, [](const std::unique_ptr<Sound>& shot) {
        if (ma_sound_at_end(&shot->sound) == MA_FALSE) {
            return false;
        }
        ma_sound_uninit(&shot->sound);
        return true;
    });
    const auto step = static_cast<f32>(deltaSeconds);
    std::erase_if(m_impl->loops, [&](Loop& loop) {
        const f32 change = loop.fadePerSecond * step;
        loop.volume = loop.volume < loop.target ? std::min(loop.volume + change, loop.target)
                                                : std::max(loop.volume - change, loop.target);
        ma_sound_set_volume(&loop.sound->sound, loop.volume);
        const bool ended = ma_sound_at_end(&loop.sound->sound) == MA_TRUE;
        if ((loop.volume > 0.0F || loop.target > 0.0F) && !ended) {
            return false;
        }
        ma_sound_uninit(&loop.sound->sound);
        return true;
    });
}

void AudioSystem::setListener(const DVec3& position, const Vec3& forward) {
    if (!m_impl->available) {
        return;
    }
    ma_engine_listener_set_position(&m_impl->engine, 0, static_cast<f32>(position.x),
                                    static_cast<f32>(position.y), static_cast<f32>(position.z));
    ma_engine_listener_set_direction(&m_impl->engine, 0, forward.x, forward.y, forward.z);
}

void AudioSystem::play(const std::filesystem::path& file, Bus bus, f32 volume, f32 pitch) {
    if (!m_impl->available || m_impl->shots.size() >= kMaxOneShots) {
        return;
    }
    auto shot = std::make_unique<Sound>();
    constexpr ma_uint32 kFlags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (openSound(m_impl->engine, file, kFlags, &m_impl->groups.at(static_cast<usize>(bus)), shot->sound) !=
        MA_SUCCESS) {
        VXE_LOG_WARN(log::core, "Could not play {}", file.string());
        return;
    }
    ma_sound_set_volume(&shot->sound, volume);
    ma_sound_set_pitch(&shot->sound, pitch);
    ma_sound_start(&shot->sound);
    m_impl->shots.push_back(std::move(shot));
}

void AudioSystem::playAt(const std::filesystem::path& file, const DVec3& position, Bus bus, f32 volume,
                         f32 pitch) {
    if (!m_impl->available || m_impl->shots.size() >= kMaxOneShots) {
        return;
    }
    auto shot = std::make_unique<Sound>();
    constexpr ma_uint32 kFlags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_ASYNC;
    if (openSound(m_impl->engine, file, kFlags, &m_impl->groups.at(static_cast<usize>(bus)), shot->sound) !=
        MA_SUCCESS) {
        VXE_LOG_WARN(log::core, "Could not play {}", file.string());
        return;
    }
    ma_sound_set_volume(&shot->sound, volume);
    ma_sound_set_pitch(&shot->sound, pitch);
    ma_sound_set_position(&shot->sound, static_cast<f32>(position.x), static_cast<f32>(position.y),
                          static_cast<f32>(position.z));
    ma_sound_set_min_distance(&shot->sound, kNearDistance);
    ma_sound_set_max_distance(&shot->sound, kFarDistance);
    ma_sound_start(&shot->sound);
    m_impl->shots.push_back(std::move(shot));
}

void AudioSystem::setLoop(std::string_view name, const std::filesystem::path& file, Bus bus, f32 volume,
                          f32 fadeSeconds, f32 pitch) {
    if (!m_impl->available) {
        return;
    }
    const f32 fade = 1.0F / std::max(fadeSeconds, 0.01F);
    const std::string path = file.string();
    const auto found =
        std::ranges::find_if(m_impl->loops, [&](const Loop& loop) { return loop.name == name; });
    if (found != m_impl->loops.end()) {
        found->fadePerSecond = fade;
        if (found->file == path) {
            found->target = volume;
            ma_sound_set_pitch(&found->sound->sound, pitch);
            return;
        }
        // Another file: the old one keeps fading out under a name nobody asks for.
        found->name = "fading " + std::to_string(m_impl->fadingCount++);
        found->target = 0.0F;
    }
    if (volume <= 0.0F) {
        return;
    }
    auto sound = std::make_unique<Sound>();
    constexpr ma_uint32 kFlags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (openSound(m_impl->engine, file, kFlags, &m_impl->groups.at(static_cast<usize>(bus)), sound->sound) !=
        MA_SUCCESS) {
        VXE_LOG_WARN(log::core, "Could not play {}", path);
        return;
    }
    ma_sound_set_looping(&sound->sound, MA_TRUE);
    ma_sound_set_volume(&sound->sound, 0.0F);
    ma_sound_set_pitch(&sound->sound, pitch);
    ma_sound_start(&sound->sound);
    m_impl->loops.push_back({.name = std::string(name),
                             .file = path,
                             .sound = std::move(sound),
                             .volume = 0.0F,
                             .target = volume,
                             .fadePerSecond = fade});
}

void AudioSystem::playMusic(const std::filesystem::path& file, f32 fadeSeconds) {
    if (!m_impl->available) {
        return;
    }
    setLoop("music", file, Bus::Music, 1.0F, fadeSeconds);
    const auto found =
        std::ranges::find_if(m_impl->loops, [](const Loop& loop) { return loop.name == "music"; });
    if (found != m_impl->loops.end()) {
        ma_sound_set_looping(&found->sound->sound, MA_FALSE); // A track plays once; quiet follows.
    }
}

void AudioSystem::stopMusic(f32 fadeSeconds) {
    for (Loop& loop : m_impl->loops) {
        if (loop.name == "music") {
            loop.target = 0.0F;
            loop.fadePerSecond = 1.0F / std::max(fadeSeconds, 0.01F);
        }
    }
}

bool AudioSystem::isMusicPlaying() const noexcept {
    return std::ranges::any_of(m_impl->loops,
                               [](const Loop& loop) { return loop.name == "music" && loop.target > 0.0F; });
}

void AudioSystem::fadeAll(f32 fadeSeconds) {
    const f32 fade = 1.0F / std::max(fadeSeconds, 0.01F);
    for (Loop& loop : m_impl->loops) {
        loop.target = 0.0F;
        loop.fadePerSecond = fade;
    }
}

} // namespace vxe::audio
