#include "vxe/core/string_id.hpp"

#include <vxe/core/containers.hpp>

#include <deque>
#include <mutex>
#include <string>

namespace vxe::detail {
namespace {

// The registry of debug names. A deque never moves its elements, so the
// views handed out stay valid while more names are added.
struct NameRegistry {
    std::mutex mutex;
    std::deque<std::string> storage;
    HashMap<u64, std::string_view> byHash;
};

NameRegistry& registry() {
    static NameRegistry instance;
    return instance;
}

} // namespace

std::string_view registerStringIdName(u64 hash, std::string_view text) {
    NameRegistry& reg = registry();
    const std::scoped_lock lock(reg.mutex);
    if (const auto found = reg.byHash.find(hash); found != reg.byHash.end()) {
        // Two different strings with one hash would make their ids equal and
        // silently merge two settings or actions. Stop right away instead.
        VXE_VERIFY(found->second == text, "StringId hash collision between two different strings");
        return found->second;
    }
    const std::string_view stored = reg.storage.emplace_back(text);
    reg.byHash.emplace(hash, stored);
    return stored;
}

} // namespace vxe::detail
