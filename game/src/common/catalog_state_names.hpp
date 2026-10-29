#pragma once

#include "common/block_catalog.hpp"

#include <vxe/voxel/chunk_codec.hpp>

#include <optional>
#include <string_view>

namespace vr {

// The game's side of chunk saving (E9.8): the engine's chunk codec stores
// block state names, and the catalog maps them to this run's state ids and
// back, so a save survives blocks being added or reordered.
//
// Thread safety: the catalog is read only after loading, so save and load
// jobs may use this from any thread. The catalog must outlive it.
class CatalogStateNames final : public vxe::voxel::BlockStateNames {
public:
    explicit CatalogStateNames(const BlockCatalog& catalog) noexcept
        : m_catalog(catalog) {}

    [[nodiscard]] std::string_view nameOf(vxe::voxel::BlockStateId state) const override {
        return m_catalog.stateName(state);
    }
    [[nodiscard]] std::optional<vxe::voxel::BlockStateId> idOf(std::string_view name) const override {
        return m_catalog.stateId(name);
    }

private:
    const BlockCatalog& m_catalog;
};

} // namespace vr
