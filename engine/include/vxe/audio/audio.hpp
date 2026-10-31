#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace vxe::audio {

// What a sound belongs to. Each has its own volume setting.
enum class Bus : u8 {
    Music,
    Ambient,
    Effects,
    Interface
};

struct AudioDesc {
    config::SettingsRegistry* settings = nullptr;
    // False mixes silently without a sound card: tests and headless servers.
    bool device = true;
};

// Sound output: music, ambience that fades, and one shot effects.
//
// Files load and decode on miniaudio's own threads, so playing a sound never
// waits for the disk. Long files are streamed; short ones are decoded once and
// shared. A computer without sound simply stays silent: nothing here can fail
// in a way the game has to handle, it only logs.
//
// Thread safety: main thread only. Mixing runs on miniaudio's audio thread.
class AudioSystem {
public:
    explicit AudioSystem(const AudioDesc& desc);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    [[nodiscard]] bool isAvailable() const noexcept;

    // Moves fades on and frees what has finished. Call once per frame.
    void update(f64 deltaSeconds);

    // Where the listener is, for positioned sounds.
    void setListener(const DVec3& position, const Vec3& forward);

    // Plays a short sound once, everywhere or from a place in the world.
    void play(const std::filesystem::path& file, Bus bus, f32 volume = 1.0F, f32 pitch = 1.0F);
    void playAt(const std::filesystem::path& file, const DVec3& position, Bus bus, f32 volume = 1.0F,
                f32 pitch = 1.0F);

    // A named looping sound that fades to `volume` over `fadeSeconds`. Calling
    // it again changes the target; another file crossfades to that file. A
    // volume of 0 fades it out and frees it. `pitch` changes at once.
    void setLoop(std::string_view name, const std::filesystem::path& file, Bus bus, f32 volume,
                 f32 fadeSeconds, f32 pitch = 1.0F);

    // A music track, played once. The previous one fades out.
    void playMusic(const std::filesystem::path& file, f32 fadeSeconds);
    void stopMusic(f32 fadeSeconds);
    [[nodiscard]] bool isMusicPlaying() const noexcept;

    // Fades every loop and the music out, for leaving a world.
    void fadeAll(f32 fadeSeconds);

    // Sounds alive right now, for tests and the overlay.
    [[nodiscard]] usize activeSounds() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vxe::audio
