#pragma once

#include "menu_services.hpp"

#include <vxe/render/texture_pack.hpp>
#include <vxe/ui/screen.hpp>
#include <vxe/ui/widgets.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vr {

// The enabled packs of Graphics.Textures.Packs ("MyPack, OtherPack"), highest
// priority first, without blanks, repeats or "default" (always last anyway).
[[nodiscard]] std::vector<std::string> parsePackList(std::string_view text);
[[nodiscard]] std::string joinPackList(const std::vector<std::string>& packs);

// Resource Packs (Section 19.3, G4.4): the packs found in the packs folder on
// the left, the enabled ones on the right with the highest priority on top.
// Selecting a pack shows what it is; Enable, Disable, Move up and Move down
// change Graphics.Textures.Packs, which loads the textures again at once.
// The default pack stays at the bottom of the enabled list, so every texture
// always has a fallback. Open Packs Folder shows the folder; Reload looks for
// packs again after copying one in.
class ResourcePacksScreen final : public vxe::ui::Screen {
public:
    explicit ResourcePacksScreen(const MenuServices& services)
        : m_services(services) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;

private:
    void refresh();
    void rebuildLists();
    void select(const std::string& id);
    void updateButtons();
    void setEnabled(std::vector<std::string> enabled);
    [[nodiscard]] const vxe::render::PackManifest* manifest(std::string_view id) const;

    MenuServices m_services;
    std::vector<vxe::render::PackManifest> m_packs; // Every pack found, without "default".
    vxe::render::PackManifest m_default;            // Shown last in the enabled list.
    std::vector<std::string> m_enabled;             // Highest priority first.
    std::string m_selected;
    vxe::ui::Widget* m_availableList = nullptr;
    vxe::ui::Widget* m_enabledList = nullptr;
    vxe::ui::Label* m_details = nullptr;
    vxe::ui::Button* m_toggle = nullptr;
    vxe::ui::Button* m_up = nullptr;
    vxe::ui::Button* m_down = nullptr;
    std::vector<std::pair<std::string, vxe::ui::Button*>> m_rows;
};

} // namespace vr
