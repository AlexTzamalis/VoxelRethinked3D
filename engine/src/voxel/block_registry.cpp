#include "vxe/voxel/block_registry.hpp"

#include <vxe/core/assert.hpp>

#include <format>

namespace vxe::voxel {

BlockRegistryBuilder::BlockRegistryBuilder() {
    m_infos.push_back(airRenderInfo());
}

Result<BlockStateId> BlockRegistryBuilder::add(const BlockRenderInfo& info) {
    if (info.emittedLight > kMaxLightLevel || info.lightOpacity > kMaxLightLevel) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("Block state {}: light values go from 0 to {} (emitted {}, opacity {})",
                                 m_infos.size(), kMaxLightLevel, info.emittedLight, info.lightOpacity));
    }
    if (m_infos.size() >= kMaxBlockStates) {
        return Error(ErrorCode::OutOfMemory,
                     std::format("All {} block state ids are in use", kMaxBlockStates));
    }
    m_infos.push_back(info);
    return static_cast<BlockStateId>(m_infos.size() - 1);
}

std::shared_ptr<BlockRegistryView> BlockRegistryBuilder::build() const {
    auto view = std::make_shared<BlockRegistryView>();
    view->m_infos = m_infos;
    return view;
}

BlockRegistry::BlockRegistry(stats::StatRegistry* stats)
    : m_stats(stats) {
    if (m_stats != nullptr) {
        auto id = m_stats->registerStat({.name = "Voxel.BlockRegistry.States",
                                         .description = "Block states in the current block registry.",
                                         .type = stats::StatType::Gauge});
        m_statesStat = id ? id.value() : stats::StatId{};
    }
    publish(BlockRegistryBuilder().build());
}

std::shared_ptr<const BlockRegistryView> BlockRegistry::current() const {
    const std::scoped_lock lock(m_mutex);
    return m_current;
}

void BlockRegistry::publish(std::shared_ptr<BlockRegistryView> view) {
    VXE_ASSERT(view != nullptr && view->m_generation == 0, "Publish a freshly built view, once");
    const usize states = view->size();
    {
        const std::scoped_lock lock(m_mutex);
        view->m_generation = ++m_generation;
        m_current = std::move(view);
    }
    if (m_stats != nullptr) {
        m_stats->set(m_statesStat, static_cast<f64>(states));
    }
}

u64 BlockRegistry::generation() const {
    const std::scoped_lock lock(m_mutex);
    return m_generation;
}

} // namespace vxe::voxel
