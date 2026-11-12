#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// The settings system (Section 16 of the plan).
//
// Every engine and game feature registers its settings here at startup, in
// its own category, with enough metadata that the settings screen, the
// console, persistence and the debug overlay can all be generated from it.
// Adding a setting never needs UI code.
namespace vxe::config {

// When a changed value takes effect (Section 16.2).
enum class ApplyMode : u8 {
    Live,                   // Next frame. The default and the goal.
    RequiresRemesh,         // After resident chunks are remeshed (done automatically).
    RequiresRendererReload, // After the renderer recreates pipelines and targets (done automatically).
    RequiresWorldReload,    // After leaving and re-entering the world.
    RequiresRestart,        // After restarting the program. Avoid whenever possible.
};

// Where a value is stored (persistence arrives in E5.2).
enum class Scope : u8 {
    Global,   // config/settings.toml
    PerWorld, // The world's folder.
};

// Who sees a setting in the settings screen.
enum class Visibility : u8 {
    Normal,
    Advanced,  // Behind an "Advanced" toggle.
    Developer, // Hidden in release builds unless developer mode is on.
};

enum class SettingType : u8 {
    Bool,
    Int,
    Float,
    Enum,
    String,
};

// A value of any setting type, for code that handles settings generically
// (the console, persistence, the generated settings screen). Enums are their
// integer value.
using SettingValue = std::variant<bool, i32, f32, std::string>;

// One choice of an enum setting.
struct EnumOption {
    i32 value = 0;
    std::string name;        // Stable name, used in files and the console ("Vsync").
    std::string displayName; // Shown in the UI ("Vertical sync").
};

// Everything known about a setting except its current value.
struct SettingInfo {
    std::string id;       // Dotted path, for example "Graphics.LOD.MaxDistance".
    std::string category; // The id without its last part: "Graphics.LOD".
    std::string displayName;
    std::string description;
    SettingType type = SettingType::Bool;
    ApplyMode applyMode = ApplyMode::Live;
    Scope scope = Scope::Global;
    Visibility visibility = Visibility::Normal;
    SettingValue defaultValue;
    // Numeric settings only.
    f64 min = 0.0;
    f64 max = 0.0;
    f64 step = 0.0;
    // Whole number settings shown as a list.
    std::vector<i32> choices;
    std::string zeroName;
    // Enum settings only.
    std::vector<EnumOption> options;
    // String settings only.
    usize maxLength = 0;
};

// Fields every description shares. The string views must stay valid only
// during the register call; the registry copies them.
struct CommonDesc {
    std::string_view id;
    std::string_view displayName;
    std::string_view description;
    ApplyMode applyMode = ApplyMode::Live;
    Scope scope = Scope::Global;
    Visibility visibility = Visibility::Normal;
};

struct BoolSettingDesc : CommonDesc {
    bool defaultValue = false;
};

struct IntSettingDesc : CommonDesc {
    i32 defaultValue = 0;
    i32 min = 0;
    i32 max = 100;
    i32 step = 1;
    // The values worth picking, ascending. Menus then offer these as a list
    // instead of a slider; any value in the range stays valid by hand.
    std::span<const i32> choices{};
    std::string_view zeroName{}; // The list's name for 0, for example "Unlimited".
};

struct FloatSettingDesc : CommonDesc {
    f32 defaultValue = 0.0F;
    f32 min = 0.0F;
    f32 max = 1.0F;
    f32 step = 0.01F;
};

template <typename E>
    requires std::is_enum_v<E>
struct EnumSettingDesc : CommonDesc {
    E defaultValue{};
    // Every value the setting may take, with its names.
    struct Option {
        E value;
        std::string_view name;
        std::string_view displayName;
    };
    std::span<const Option> options;
};

struct StringSettingDesc : CommonDesc {
    std::string_view defaultValue;
    usize maxLength = 256;
};

class SettingsRegistry;

namespace detail {
struct SettingEntry;
} // namespace detail

// Keeps a change callback registered for as long as it lives (Section 16.3),
// so a system that shuts down can never be called by accident. Move only.
// The registry must outlive its subscriptions.
class Subscription {
public:
    Subscription() noexcept = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept
        : m_registry(std::exchange(other.m_registry, nullptr))
        , m_id(other.m_id) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            m_registry = std::exchange(other.m_registry, nullptr);
            m_id = other.m_id;
        }
        return *this;
    }
    ~Subscription() { reset(); }

    void reset() noexcept;
    [[nodiscard]] bool isActive() const noexcept { return m_registry != nullptr; }

private:
    friend class SettingsRegistry;
    Subscription(SettingsRegistry* registry, u64 id) noexcept
        : m_registry(registry)
        , m_id(id) {}
    SettingsRegistry* m_registry = nullptr;
    u64 m_id = 0;
};

// The value a Setting<T> handle reads. Enums store their integer value.
template <typename T>
using StorageOf = std::conditional_t<std::is_enum_v<T>, i32, T>;

// A cheap, copyable handle to one registered setting.
//
// get() is a single relaxed atomic load (strings take a shared lock), so any
// thread may read settings every time it needs them. Section 16.5: read the
// setting when you need it, never cache it at startup without a subscription.
// set() goes through the registry, so clamping, validation and change
// callbacks always happen.
template <typename T>
class Setting {
public:
    Setting() = default;

    [[nodiscard]] T get() const;
    Result<void> set(const T& value) const;
    void reset() const;

    [[nodiscard]] const SettingInfo& info() const;
    [[nodiscard]] bool isValid() const noexcept { return m_entry != nullptr; }

    // Registers a function that runs on the main thread (during
    // SettingsRegistry::dispatchChanges) after the value changed.
    [[nodiscard]] Subscription
    onChanged(std::function<void(const T& oldValue, const T& newValue)> callback) const;

private:
    friend class SettingsRegistry;
    Setting(SettingsRegistry* registry, detail::SettingEntry* entry) noexcept
        : m_registry(registry)
        , m_entry(entry) {}
    SettingsRegistry* m_registry = nullptr;
    detail::SettingEntry* m_entry = nullptr;
};

namespace detail {

// One registered setting. Lives in a deque inside the registry, so handles
// can keep a plain pointer to it: a deque never moves its elements.
struct SettingEntry {
    SettingInfo info;
    std::atomic<bool> boolValue{false};
    std::atomic<i32> intValue{0};
    std::atomic<f32> floatValue{0.0F};
    mutable std::shared_mutex stringMutex;
    std::string stringValue;
};

} // namespace detail

// The registry of every setting (Section 16).
//
// Thread safety:
//   - register*() and subscriptions: main thread, at startup or between frames.
//   - get() and set() on handles, and set() by id: any thread.
//   - dispatchChanges(): main thread, once per frame (the engine loop calls it).
// Values are stored immediately on set, so every thread reads the new value
// right away; only the change callbacks wait for dispatchChanges().
class SettingsRegistry {
public:
    SettingsRegistry();
    ~SettingsRegistry();
    SettingsRegistry(const SettingsRegistry&) = delete;
    SettingsRegistry& operator=(const SettingsRegistry&) = delete;
    SettingsRegistry(SettingsRegistry&&) = delete;
    SettingsRegistry& operator=(SettingsRegistry&&) = delete;

    // Registers a setting and returns its handle. Registering an id that
    // already exists with the same type returns the existing handle (the
    // first description wins); a different type is a programmer error.
    [[nodiscard]] Setting<bool> registerBool(const BoolSettingDesc& desc);
    [[nodiscard]] Setting<i32> registerInt(const IntSettingDesc& desc);
    [[nodiscard]] Setting<f32> registerFloat(const FloatSettingDesc& desc);
    [[nodiscard]] Setting<std::string> registerString(const StringSettingDesc& desc);
    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] Setting<E> registerEnum(const EnumSettingDesc<E>& desc) {
        std::vector<EnumOption> options;
        options.reserve(desc.options.size());
        for (const auto& option : desc.options) {
            options.push_back(EnumOption{.value = static_cast<i32>(option.value),
                                         .name = std::string(option.name),
                                         .displayName = std::string(option.displayName)});
        }
        return Setting<E>(this,
                          registerEnumEntry(desc, static_cast<i32>(desc.defaultValue), std::move(options)));
    }

    // Looks a setting up by id. Null if there is none.
    [[nodiscard]] const SettingInfo* find(std::string_view id) const;

    // Every setting, in registration order.
    [[nodiscard]] std::vector<const SettingInfo*> all() const;

    // Every category that has at least one setting, sorted.
    [[nodiscard]] std::vector<std::string> categories() const;

    // The current value of any setting, by id.
    [[nodiscard]] Result<SettingValue> value(std::string_view id) const;

    // Sets any setting by id. The value must have the setting's type (enums
    // take their integer value). Numbers are clamped to the range; NaN,
    // unknown enum values and too long strings are rejected.
    Result<void> set(std::string_view id, const SettingValue& value);

    // Restores defaults. resetCategory takes a category prefix: "Graphics"
    // resets every setting under Graphics, including Graphics.LOD.
    Result<void> reset(std::string_view id);
    usize resetCategory(std::string_view categoryPrefix);

    [[nodiscard]] bool isDefault(std::string_view id) const;

    // Presets (Section 16.2): named sets of values for a group of settings,
    // for example group "Graphics" with presets "Low" to "Ultra". Defining a
    // preset again replaces it. Values are validated when applied.
    void definePreset(std::string_view group, std::string_view presetName,
                      std::vector<std::pair<std::string, SettingValue>> values);
    // Sets every value of the preset. Fails if the group or preset is unknown;
    // values that no longer validate are skipped and reported in the error.
    Result<void> applyPreset(std::string_view group, std::string_view presetName);
    [[nodiscard]] std::vector<std::string> presetNames(std::string_view group) const;
    // The preset whose values all match the current ones, or "Custom".
    [[nodiscard]] std::string currentPreset(std::string_view group) const;

    // How the settings UI shows a category: its page name, and the top group
    // it appears under when that is not its first id part (Accessibility
    // under Interface). Display only: ids and saved values never change.
    // Applies to sub categories too ("Server" moves "Server.Save" along).
    void describeCategory(std::string_view category, std::string displayName,
                          std::string_view shownUnder = {});
    // The page name of a category, or empty when it was never described.
    [[nodiscard]] std::string categoryName(std::string_view category) const;
    // The top group a category shows under: the nearest described ancestor's
    // shownUnder, else the category's first id part.
    [[nodiscard]] std::string groupOf(std::string_view category) const;
    // A setting also shown on its group's first page, for the ones players
    // look for most (render distance on the Graphics page). In call order.
    void featureSetting(std::string_view group, std::string_view id);
    [[nodiscard]] std::vector<std::string> featuredSettings(std::string_view group) const;

    // Keys found in a settings file that no setting claims (written by an
    // older or newer version, or a removed feature). They are kept verbatim,
    // as TOML value text, so saving the file never deletes them.
    void setUnknown(Scope scope, std::string id, std::string tomlValueText);
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> unknown(Scope scope) const;

    // Runs the change callbacks for every setting that changed since the last
    // call. Several changes of one setting are merged into one callback with
    // the oldest old value and the newest new value. Returns the number of
    // callbacks run. Main thread only.
    usize dispatchChanges();

    // Implementation details for Setting<T>. Not for direct use.
    [[nodiscard]] static SettingValue valueOf(const detail::SettingEntry& entry);
    Result<void> setEntry(detail::SettingEntry& entry, const SettingValue& value);
    [[nodiscard]] Subscription
    subscribe(detail::SettingEntry& entry,
              std::function<void(const SettingValue&, const SettingValue&)> callback);

private:
    friend class Subscription;

    struct Subscriber {
        u64 id = 0;
        detail::SettingEntry* entry = nullptr;
        std::function<void(const SettingValue&, const SettingValue&)> callback;
        bool active = true;
    };

    struct PendingChange {
        detail::SettingEntry* entry = nullptr;
        SettingValue oldValue;
        SettingValue newValue;
    };

    [[nodiscard]] detail::SettingEntry* addEntry(SettingInfo info, SettingType expectedType);
    [[nodiscard]] detail::SettingEntry* registerEnumEntry(const CommonDesc& desc, i32 defaultValue,
                                                          std::vector<EnumOption> options);
    [[nodiscard]] detail::SettingEntry* findEntry(std::string_view id) const;
    static void storeValue(detail::SettingEntry& entry, const SettingValue& value);
    void adoptUnknown(detail::SettingEntry& entry);
    void unsubscribe(u64 id) noexcept;
    void assertOwnerThread() const noexcept;

    std::thread::id m_ownerThread;
    std::deque<detail::SettingEntry> m_entries;
    StringMap<detail::SettingEntry*> m_byId;

    std::vector<Subscriber> m_subscribers;
    u64 m_nextSubscriberId = 1;
    bool m_dispatching = false;

    struct Preset {
        std::string group;
        std::string name;
        std::vector<std::pair<std::string, SettingValue>> values;
    };
    std::vector<Preset> m_presets;
    struct CategoryLayout {
        std::string name;
        std::string shownUnder;
    };
    std::vector<std::pair<std::string, CategoryLayout>> m_categoryLayouts;
    std::vector<std::pair<std::string, std::string>> m_featured; // Group, setting id.
    // Sorted by id per scope, so files keep a stable order.
    std::array<std::vector<std::pair<std::string, std::string>>, 2> m_unknown;

    std::mutex m_pendingMutex;
    std::vector<PendingChange> m_pending;
    std::vector<PendingChange> m_dispatchingChanges; // Reused to avoid reallocating.
};

// ---------------------------------------------------------------------------
// Setting<T> implementation

namespace detail {

template <typename T>
[[nodiscard]] SettingValue toSettingValue(const T& value) {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<i32>(value);
    } else {
        return value;
    }
}

template <typename T>
[[nodiscard]] T fromSettingValue(const SettingValue& value) {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(std::get<i32>(value));
    } else {
        return std::get<T>(value);
    }
}

} // namespace detail

template <typename T>
T Setting<T>::get() const {
    VXE_ASSERT(m_entry != nullptr, "Reading an unregistered setting");
    if constexpr (std::is_same_v<T, bool>) {
        return m_entry->boolValue.load(std::memory_order_relaxed);
    } else if constexpr (std::is_same_v<T, f32>) {
        return m_entry->floatValue.load(std::memory_order_relaxed);
    } else if constexpr (std::is_same_v<T, i32>) {
        return m_entry->intValue.load(std::memory_order_relaxed);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(m_entry->intValue.load(std::memory_order_relaxed));
    } else {
        static_assert(std::is_same_v<T, std::string>, "Unsupported setting type");
        const std::shared_lock lock(m_entry->stringMutex);
        return m_entry->stringValue;
    }
}

template <typename T>
Result<void> Setting<T>::set(const T& value) const {
    VXE_ASSERT(m_entry != nullptr, "Writing an unregistered setting");
    return m_registry->setEntry(*m_entry, detail::toSettingValue(value));
}

template <typename T>
void Setting<T>::reset() const {
    VXE_ASSERT(m_entry != nullptr, "Resetting an unregistered setting");
    static_cast<void>(m_registry->setEntry(*m_entry, m_entry->info.defaultValue));
}

template <typename T>
const SettingInfo& Setting<T>::info() const {
    VXE_ASSERT(m_entry != nullptr, "Reading an unregistered setting");
    return m_entry->info;
}

template <typename T>
Subscription Setting<T>::onChanged(std::function<void(const T& oldValue, const T& newValue)> callback) const {
    VXE_ASSERT(m_entry != nullptr, "Subscribing to an unregistered setting");
    return m_registry->subscribe(*m_entry, [callback = std::move(callback)](const SettingValue& oldValue,
                                                                            const SettingValue& newValue) {
        callback(detail::fromSettingValue<T>(oldValue), detail::fromSettingValue<T>(newValue));
    });
}

} // namespace vxe::config
