#pragma once

#include "background_task.hpp"
#include "menu_services.hpp"
#include "server/world_store.hpp"

#include <vxe/ui/controls.hpp>
#include <vxe/ui/screen.hpp>
#include <vxe/ui/widgets.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vr {

// Starts playing a world, once it exists on disk.
using PlayWorld = std::function<void(const WorldInfo&)>;

// The world list (Section 19.3, G5.1): saved worlds, last played first, with
// name, last played time, world type and size, a search box, and Play, Create
// New World, Edit, and Delete with a confirmation. Double clicking plays.
// Everything on disk happens on IO threads through BackgroundTask.
class WorldSelectScreen final : public vxe::ui::Screen {
public:
    WorldSelectScreen(const MenuServices& services, PlayWorld play)
        : m_services(services)
        , m_play(std::move(play)) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;
    void update(vxe::f32 deltaSeconds) override;

private:
    void load();
    void removeWorld(const std::string& folder);
    void rebuildList();
    void select(const std::string& folder);
    void updateButtons();
    [[nodiscard]] const WorldInfo* selected() const;

    MenuServices m_services;
    PlayWorld m_play;
    BackgroundTask m_task;
    std::shared_ptr<std::vector<WorldInfo>> m_loaded;
    std::vector<WorldInfo> m_worlds;
    std::string m_selected; // Folder of the selected world.
    vxe::f32 m_sinceClick = 0.0F;
    bool m_reloadOnReturn = false;
    vxe::ui::Widget* m_list = nullptr;
    vxe::ui::TextField* m_search = nullptr;
    vxe::ui::Button* m_playButton = nullptr;
    vxe::ui::Button* m_editButton = nullptr;
    vxe::ui::Button* m_deleteButton = nullptr;
    vxe::ui::Button* m_recreateButton = nullptr;
    std::vector<std::pair<std::string, vxe::ui::Button*>> m_rows;
};

// Create World (Section 19.3, G5.2): tabs for the name, the seed and world,
// far terrain, and more. Creating writes the world and starts it. Given a
// world (Re-create, G5.5), every field starts from that world's options; the
// new world gets a folder of its own.
class CreateWorldScreen final : public vxe::ui::Screen {
public:
    CreateWorldScreen(const MenuServices& services, PlayWorld play, const WorldInfo* from = nullptr);

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;
    void update(vxe::f32 deltaSeconds) override;

private:
    enum class Tab : vxe::u8 {
        Game,
        World,
        Lod,
        More
    };

    void showTab(Tab tab);
    void buildPage();
    void create();

    MenuServices m_services;
    PlayWorld m_play;
    BackgroundTask m_task;
    std::shared_ptr<WorldInfo> m_created;
    std::shared_ptr<std::string> m_error;
    Tab m_tab = Tab::Game;
    std::string m_name = "New World";
    std::string m_seed;
    std::string m_minY = "-1024";
    std::string m_maxY = "3072";
    bool m_structures = true;
    bool m_recreating = false;
    vxe::usize m_type = 0;
    vxe::usize m_height = 0;
    vxe::usize m_premap = 5; // The whole horizon.
    vxe::usize m_quality = 2;
    vxe::usize m_preset = 0;
    vxe::ui::Widget* m_page = nullptr;
    vxe::ui::Button* m_createButton = nullptr;
    std::vector<std::pair<Tab, vxe::ui::Button*>> m_tabs;
};

// Edit World: rename a world, change how far its far terrain is premapped and
// its far terrain quality, and read what it was made with. The folder and seed
// do not change.
class EditWorldScreen final : public vxe::ui::Screen {
public:
    EditWorldScreen(const MenuServices& services, WorldInfo info)
        : m_services(services)
        , m_info(std::move(info)) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;
    void update(vxe::f32 deltaSeconds) override;

private:
    MenuServices m_services;
    WorldInfo m_info;
    BackgroundTask m_task;
    vxe::ui::TextField* m_name = nullptr;
    vxe::usize m_premap = 0;
    vxe::usize m_quality = 0;
};

} // namespace vr
