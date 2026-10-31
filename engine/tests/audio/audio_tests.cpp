#include <vxe/audio/audio.hpp>
#include <vxe/config/settings.hpp>

#include <doctest/doctest.h>

using namespace vxe;
using namespace vxe::audio;

namespace {

// Without a sound card, as on the build machines: everything mixes silently.
struct Rig {
    config::SettingsRegistry settings;
    AudioSystem system{{.settings = &settings, .device = false}};
};

} // namespace

TEST_CASE("Sound registers a volume for each kind of sound") {
    Rig rig;
    CHECK(rig.system.isAvailable());
    for (const auto* id :
         {"Audio.Master", "Audio.Music", "Audio.Ambient", "Audio.Effects", "Audio.Interface"}) {
        CHECK(rig.settings.find(id) != nullptr);
    }
}

TEST_CASE("A file that does not exist plays nothing and breaks nothing") {
    Rig rig;
    rig.system.play("no/such/sound.ogg", Bus::Effects);
    rig.system.playAt("no/such/sound.ogg", {1.0, 2.0, 3.0}, Bus::Effects);
    rig.system.setLoop("wind", "no/such/sound.ogg", Bus::Ambient, 1.0F, 1.0F);
    rig.system.playMusic("no/such/sound.ogg", 1.0F);
    rig.system.update(0.016);
    CHECK(rig.system.activeSounds() == 0);
    CHECK_FALSE(rig.system.isMusicPlaying());
}

TEST_CASE("A loop fades in, crossfades to another file and fades away") {
    Rig rig;
    const char* first = VXE_TEST_SOUND;
    rig.system.setLoop("birds", first, Bus::Ambient, 0.5F, 1.0F);
    CHECK(rig.system.activeSounds() == 1);
    rig.system.update(0.5);
    // The same file again only changes the target.
    rig.system.setLoop("birds", first, Bus::Ambient, 1.0F, 1.0F);
    CHECK(rig.system.activeSounds() == 1);
    // Fading to nothing frees it once it got there.
    rig.system.setLoop("birds", first, Bus::Ambient, 0.0F, 0.1F);
    rig.system.update(1.0);
    CHECK(rig.system.activeSounds() == 0);
}

TEST_CASE("Music plays through and leaving fades everything") {
    Rig rig;
    rig.system.playMusic(VXE_TEST_SOUND, 0.1F);
    CHECK(rig.system.isMusicPlaying());
    rig.system.fadeAll(0.1F);
    rig.system.update(1.0);
    CHECK_FALSE(rig.system.isMusicPlaying());
    CHECK(rig.system.activeSounds() == 0);
}

TEST_CASE("One shot sounds are freed when done") {
    Rig rig;
    rig.system.play(VXE_TEST_SOUND, Bus::Interface, 0.5F, 1.2F);
    CHECK(rig.system.activeSounds() == 1);
    rig.system.setListener({0.0, 64.0, 0.0}, {0.0F, 0.0F, -1.0F});
    rig.system.update(0.016);
}
