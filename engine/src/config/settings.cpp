#include "vxe/config/settings.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace vxe::config {
namespace {

std::string categoryOf(std::string_view id) {
    const usize dot = id.rfind('.');
    return dot == std::string_view::npos ? std::string() : std::string(id.substr(0, dot));
}

SettingInfo makeInfo(const CommonDesc& desc, SettingType type, SettingValue defaultValue) {
    return SettingInfo{.id = std::string(desc.id),
                       .category = categoryOf(desc.id),
                       .displayName = std::string(desc.displayName),
                       .description = std::string(desc.description),
                       .type = type,
                       .applyMode = desc.applyMode,
                       .scope = desc.scope,
                       .visibility = desc.visibility,
                       .defaultValue = std::move(defaultValue),
                       .min = 0.0,
                       .max = 0.0,
                       .step = 0.0,
                       .options = {},
                       .maxLength = 0};
}

const char* typeName(SettingType type) noexcept {
    switch (type) {
    case SettingType::Bool:
        return "bool";
    case SettingType::Int:
        return "int";
    case SettingType::Float:
        return "float";
    case SettingType::Enum:
        return "enum";
    case SettingType::String:
        return "string";
    }
    return "unknown";
}

// Checks a value against a setting's type and limits. Numbers are clamped
// into range (a slider dragged too far is not an error); values that cannot
// mean anything (NaN, an unknown enum value, a wrong type) are rejected.
Result<SettingValue> validate(const SettingInfo& info, const SettingValue& value) {
    const auto wrongType = [&info] {
        return Error(ErrorCode::InvalidArgument,
                     std::format("Setting '{}' is a {} setting", info.id, typeName(info.type)));
    };
    switch (info.type) {
    case SettingType::Bool:
        if (!std::holds_alternative<bool>(value)) {
            return wrongType();
        }
        return value;
    case SettingType::Int: {
        if (!std::holds_alternative<i32>(value)) {
            return wrongType();
        }
        const i32 clamped =
            std::clamp(std::get<i32>(value), static_cast<i32>(info.min), static_cast<i32>(info.max));
        return SettingValue(clamped);
    }
    case SettingType::Float: {
        if (!std::holds_alternative<f32>(value)) {
            return wrongType();
        }
        const f32 raw = std::get<f32>(value);
        if (!std::isfinite(raw)) {
            return Error(ErrorCode::InvalidArgument,
                         std::format("Setting '{}' needs a finite number", info.id));
        }
        return SettingValue(std::clamp(raw, static_cast<f32>(info.min), static_cast<f32>(info.max)));
    }
    case SettingType::Enum: {
        if (!std::holds_alternative<i32>(value)) {
            return wrongType();
        }
        const i32 raw = std::get<i32>(value);
        const bool known = std::ranges::any_of(
            info.options, [raw](const EnumOption& option) { return option.value == raw; });
        if (!known) {
            return Error(ErrorCode::InvalidArgument,
                         std::format("Setting '{}' has no option {}", info.id, raw));
        }
        return value;
    }
    case SettingType::String:
        if (!std::holds_alternative<std::string>(value)) {
            return wrongType();
        }
        if (std::get<std::string>(value).size() > info.maxLength) {
            return Error(ErrorCode::InvalidArgument,
                         std::format("Setting '{}' allows at most {} characters", info.id, info.maxLength));
        }
        return value;
    }
    return wrongType();
}

} // namespace

void Subscription::reset() noexcept {
    if (m_registry != nullptr) {
        m_registry->unsubscribe(m_id);
        m_registry = nullptr;
    }
}

SettingsRegistry::SettingsRegistry()
    : m_ownerThread(std::this_thread::get_id()) {}

SettingsRegistry::~SettingsRegistry() = default;

void SettingsRegistry::assertOwnerThread() const noexcept {
    VXE_ASSERT(
        std::this_thread::get_id() == m_ownerThread,
        "Settings registration, subscriptions and dispatch belong to the thread that created the registry");
}

detail::SettingEntry* SettingsRegistry::addEntry(SettingInfo info, SettingType expectedType) {
    assertOwnerThread();
    VXE_ASSERT(!info.id.empty(), "A setting needs an id");
    if (detail::SettingEntry* existing = findEntry(info.id)) {
        VXE_VERIFY(existing->info.type == expectedType, "Setting registered twice with different types");
        return existing;
    }
    detail::SettingEntry& entry = m_entries.emplace_back();
    entry.info = std::move(info);
    storeValue(entry, entry.info.defaultValue);
    m_byId.emplace(entry.info.id, &entry);
    return &entry;
}

Setting<bool> SettingsRegistry::registerBool(const BoolSettingDesc& desc) {
    return Setting<bool>(this,
                         addEntry(makeInfo(desc, SettingType::Bool, desc.defaultValue), SettingType::Bool));
}

Setting<i32> SettingsRegistry::registerInt(const IntSettingDesc& desc) {
    VXE_ASSERT(desc.min <= desc.max && desc.defaultValue >= desc.min && desc.defaultValue <= desc.max,
               "Int setting default outside its range");
    SettingInfo info = makeInfo(desc, SettingType::Int, desc.defaultValue);
    info.min = desc.min;
    info.max = desc.max;
    info.step = desc.step;
    return Setting<i32>(this, addEntry(std::move(info), SettingType::Int));
}

Setting<f32> SettingsRegistry::registerFloat(const FloatSettingDesc& desc) {
    VXE_ASSERT(desc.min <= desc.max && desc.defaultValue >= desc.min && desc.defaultValue <= desc.max,
               "Float setting default outside its range");
    SettingInfo info = makeInfo(desc, SettingType::Float, desc.defaultValue);
    info.min = static_cast<f64>(desc.min);
    info.max = static_cast<f64>(desc.max);
    info.step = static_cast<f64>(desc.step);
    return Setting<f32>(this, addEntry(std::move(info), SettingType::Float));
}

Setting<std::string> SettingsRegistry::registerString(const StringSettingDesc& desc) {
    VXE_ASSERT(desc.defaultValue.size() <= desc.maxLength, "String setting default longer than its limit");
    SettingInfo info = makeInfo(desc, SettingType::String, std::string(desc.defaultValue));
    info.maxLength = desc.maxLength;
    return Setting<std::string>(this, addEntry(std::move(info), SettingType::String));
}

detail::SettingEntry* SettingsRegistry::registerEnumEntry(const CommonDesc& desc, i32 defaultValue,
                                                          std::vector<EnumOption> options) {
    VXE_ASSERT(!options.empty(), "An enum setting needs options");
    VXE_ASSERT(
        std::ranges::any_of(options, [defaultValue](const EnumOption& o) { return o.value == defaultValue; }),
        "Enum setting default is not one of its options");
    SettingInfo info = makeInfo(desc, SettingType::Enum, defaultValue);
    info.options = std::move(options);
    return addEntry(std::move(info), SettingType::Enum);
}

detail::SettingEntry* SettingsRegistry::findEntry(std::string_view id) const {
    const auto found = m_byId.find(id);
    return found == m_byId.end() ? nullptr : found->second;
}

const SettingInfo* SettingsRegistry::find(std::string_view id) const {
    const detail::SettingEntry* entry = findEntry(id);
    return entry != nullptr ? &entry->info : nullptr;
}

std::vector<const SettingInfo*> SettingsRegistry::all() const {
    std::vector<const SettingInfo*> result;
    result.reserve(m_entries.size());
    for (const detail::SettingEntry& entry : m_entries) {
        result.push_back(&entry.info);
    }
    return result;
}

std::vector<std::string> SettingsRegistry::categories() const {
    std::set<std::string> unique;
    for (const detail::SettingEntry& entry : m_entries) {
        unique.insert(entry.info.category);
    }
    return {unique.begin(), unique.end()};
}

SettingValue SettingsRegistry::valueOf(const detail::SettingEntry& entry) {
    switch (entry.info.type) {
    case SettingType::Bool:
        return entry.boolValue.load(std::memory_order_relaxed);
    case SettingType::Int:
    case SettingType::Enum:
        return entry.intValue.load(std::memory_order_relaxed);
    case SettingType::Float:
        return entry.floatValue.load(std::memory_order_relaxed);
    case SettingType::String: {
        const std::shared_lock lock(entry.stringMutex);
        return entry.stringValue;
    }
    }
    return false;
}

Result<SettingValue> SettingsRegistry::value(std::string_view id) const {
    const detail::SettingEntry* entry = findEntry(id);
    if (entry == nullptr) {
        return Error(ErrorCode::NotFound, std::format("No setting named '{}'", id));
    }
    return valueOf(*entry);
}

void SettingsRegistry::storeValue(detail::SettingEntry& entry, const SettingValue& value) {
    switch (entry.info.type) {
    case SettingType::Bool:
        entry.boolValue.store(std::get<bool>(value), std::memory_order_relaxed);
        return;
    case SettingType::Int:
    case SettingType::Enum:
        entry.intValue.store(std::get<i32>(value), std::memory_order_relaxed);
        return;
    case SettingType::Float:
        entry.floatValue.store(std::get<f32>(value), std::memory_order_relaxed);
        return;
    case SettingType::String: {
        const std::unique_lock lock(entry.stringMutex);
        entry.stringValue = std::get<std::string>(value);
        return;
    }
    }
}

Result<void> SettingsRegistry::setEntry(detail::SettingEntry& entry, const SettingValue& value) {
    Result<SettingValue> valid = validate(entry.info, value);
    if (!valid) {
        return std::move(valid).error();
    }
    // The pending lock also orders concurrent writers of the same setting, so
    // the old value recorded for each change is exact.
    const std::scoped_lock lock(m_pendingMutex);
    SettingValue oldValue = valueOf(entry);
    if (oldValue == valid.value()) {
        return {}; // No change, no callback.
    }
    storeValue(entry, valid.value());
    m_pending.push_back(
        PendingChange{.entry = &entry, .oldValue = std::move(oldValue), .newValue = valid.value()});
    return {};
}

Result<void> SettingsRegistry::set(std::string_view id, const SettingValue& value) {
    detail::SettingEntry* entry = findEntry(id);
    if (entry == nullptr) {
        return Error(ErrorCode::NotFound, std::format("No setting named '{}'", id));
    }
    return setEntry(*entry, value);
}

Result<void> SettingsRegistry::reset(std::string_view id) {
    detail::SettingEntry* entry = findEntry(id);
    if (entry == nullptr) {
        return Error(ErrorCode::NotFound, std::format("No setting named '{}'", id));
    }
    return setEntry(*entry, entry->info.defaultValue);
}

usize SettingsRegistry::resetCategory(std::string_view categoryPrefix) {
    usize count = 0;
    for (detail::SettingEntry& entry : m_entries) {
        const std::string_view category = entry.info.category;
        const bool matches = category == categoryPrefix || (category.starts_with(categoryPrefix) &&
                                                            category.size() > categoryPrefix.size() &&
                                                            category[categoryPrefix.size()] == '.');
        if (matches && setEntry(entry, entry.info.defaultValue).hasValue()) {
            ++count;
        }
    }
    return count;
}

bool SettingsRegistry::isDefault(std::string_view id) const {
    const detail::SettingEntry* entry = findEntry(id);
    return entry != nullptr && valueOf(*entry) == entry->info.defaultValue;
}

Subscription
SettingsRegistry::subscribe(detail::SettingEntry& entry,
                            std::function<void(const SettingValue&, const SettingValue&)> callback) {
    assertOwnerThread();
    const u64 id = m_nextSubscriberId++;
    m_subscribers.push_back(
        Subscriber{.id = id, .entry = &entry, .callback = std::move(callback), .active = true});
    return Subscription(this, id);
}

void SettingsRegistry::unsubscribe(u64 id) noexcept {
    assertOwnerThread();
    const auto found = std::ranges::find_if(m_subscribers, [id](const Subscriber& s) { return s.id == id; });
    if (found == m_subscribers.end()) {
        return;
    }
    if (m_dispatching) {
        found->active = false; // Removed after the dispatch loop.
    } else {
        m_subscribers.erase(found);
    }
}

void SettingsRegistry::definePreset(std::string_view group, std::string_view presetName,
                                    std::vector<std::pair<std::string, SettingValue>> values) {
    assertOwnerThread();
    const auto existing = std::ranges::find_if(
        m_presets, [&](const Preset& preset) { return preset.group == group && preset.name == presetName; });
    if (existing != m_presets.end()) {
        existing->values = std::move(values);
        return;
    }
    m_presets.push_back(
        Preset{.group = std::string(group), .name = std::string(presetName), .values = std::move(values)});
}

Result<void> SettingsRegistry::applyPreset(std::string_view group, std::string_view presetName) {
    const auto preset = std::ranges::find_if(m_presets, [&](const Preset& candidate) {
        return candidate.group == group && candidate.name == presetName;
    });
    if (preset == m_presets.end()) {
        return Error(ErrorCode::NotFound, std::format("No preset '{}' in group '{}'", presetName, group));
    }
    std::string problems;
    for (const auto& [id, presetValue] : preset->values) {
        if (Result<void> result = set(id, presetValue); !result) {
            problems += problems.empty() ? "" : "; ";
            problems += result.error().message();
        }
    }
    if (!problems.empty()) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("Preset '{}' partly applied: {}", presetName, problems));
    }
    return {};
}

std::vector<std::string> SettingsRegistry::presetNames(std::string_view group) const {
    std::vector<std::string> names;
    for (const Preset& preset : m_presets) {
        if (preset.group == group) {
            names.push_back(preset.name);
        }
    }
    return names;
}

std::string SettingsRegistry::currentPreset(std::string_view group) const {
    for (const Preset& preset : m_presets) {
        if (preset.group != group) {
            continue;
        }
        const bool matches = std::ranges::all_of(preset.values, [this](const auto& pair) {
            const Result<SettingValue> current = value(pair.first);
            return current.hasValue() && current.value() == pair.second;
        });
        if (matches) {
            return preset.name;
        }
    }
    return "Custom";
}

void SettingsRegistry::setUnknown(Scope scope, std::string id, std::string tomlValueText) {
    auto& list = m_unknown[static_cast<usize>(scope)];
    const auto position = std::ranges::lower_bound(list, id, {}, &std::pair<std::string, std::string>::first);
    if (position != list.end() && position->first == id) {
        position->second = std::move(tomlValueText);
    } else {
        list.insert(position, {std::move(id), std::move(tomlValueText)});
    }
}

std::vector<std::pair<std::string, std::string>> SettingsRegistry::unknown(Scope scope) const {
    return m_unknown[static_cast<usize>(scope)];
}

usize SettingsRegistry::dispatchChanges() {
    VXE_PROFILE_FUNCTION();
    assertOwnerThread();
    {
        const std::scoped_lock lock(m_pendingMutex);
        m_dispatchingChanges.swap(m_pending);
    }
    if (m_dispatchingChanges.empty()) {
        return 0;
    }

    // Merge several changes of one setting into one: the oldest old value and
    // the newest new value. A change that ends where it started is dropped.
    std::vector<PendingChange> merged;
    merged.reserve(m_dispatchingChanges.size());
    for (PendingChange& change : m_dispatchingChanges) {
        const auto existing = std::ranges::find_if(
            merged, [&change](const PendingChange& m) { return m.entry == change.entry; });
        if (existing == merged.end()) {
            merged.push_back(std::move(change));
        } else {
            existing->newValue = std::move(change.newValue);
        }
    }
    m_dispatchingChanges.clear();

    usize callbacks = 0;
    m_dispatching = true;
    // Index loop: a callback may subscribe, which appends to the vector.
    for (const PendingChange& change : merged) {
        if (change.oldValue == change.newValue) {
            continue;
        }
        const usize count = m_subscribers.size();
        for (usize i = 0; i < count; ++i) {
            if (m_subscribers[i].active && m_subscribers[i].entry == change.entry) {
                // Copied, because the callback may subscribe and reallocate the vector.
                const auto callback = m_subscribers[i].callback;
                callback(change.oldValue, change.newValue);
                ++callbacks;
            }
        }
    }
    m_dispatching = false;
    std::erase_if(m_subscribers, [](const Subscriber& s) { return !s.active; });
    return callbacks;
}

} // namespace vxe::config
