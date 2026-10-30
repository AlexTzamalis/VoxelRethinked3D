#pragma once

#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/widget.hpp>

#include <memory>
#include <vector>

namespace vxe::ui {

class ScreenManager;

// How a screen sits on the stack (Section 19.2).
struct ScreenOptions {
    // The screen below stays visible (inert) behind this one: a pause menu
    // over another menu, a dialog like screen. The world under the UI is the
    // game's business and always draws.
    bool drawBelow = false;
    // Dims what is behind the screen with the theme's backdrop color.
    bool backdrop = false;
    // The screen needs the mouse cursor (menus); false for screens shown
    // while playing (the HUD).
    bool cursor = true;
};

// One screen of the game UI: the main menu, the world list, the pause menu.
// A screen builds its widgets once, into a full window root the manager
// gives it, and keeps them while it is on the stack, also while covered,
// so text fields and scroll positions survive a trip to another screen.
//
// Thread safety: main thread only.
class Screen {
public:
    Screen() = default;
    virtual ~Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    Screen(Screen&&) = delete;
    Screen& operator=(Screen&&) = delete;

    [[nodiscard]] virtual ScreenOptions options() const { return {}; }
    // Builds the widgets into `root`, which covers the window. Once, when pushed.
    virtual void build(Widget& root) = 0;
    // The screen became the top one (pushed, or the one above was popped).
    virtual void onEnter() {}
    // Another screen covers it, or it leaves the stack.
    virtual void onLeave() {}
    // Back (Escape) with no dialog open. Return true when handled; false
    // pops the screen, unless it is the last one.
    virtual bool onBack() { return false; }
    // Every frame while on the stack, top or not.
    virtual void update(f32 /*deltaSeconds*/) {}

    // The manager the screen is on (set while pushed), for screens that
    // open the next screen from a button.
    [[nodiscard]] ScreenManager* manager() const noexcept { return m_manager; }

private:
    friend class ScreenManager;
    ScreenManager* m_manager = nullptr;
};

// The screen stack (Section 19.2) on top of a UiContext's root. Changes
// (push, pop, replace, clear) wait until the next update, so a button may
// replace its own screen from its click. Only the top screen takes the
// pointer and the keyboard; screens below it are hidden, or drawn but inert
// while the screens above them ask for drawBelow.
//
// Stats: Ui.Screens (screens on the stack).
// Thread safety: main thread only.
class ScreenManager {
public:
    explicit ScreenManager(UiContext& context, stats::StatRegistry* stats = nullptr);
    ~ScreenManager();
    ScreenManager(const ScreenManager&) = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;
    ScreenManager(ScreenManager&&) = delete;
    ScreenManager& operator=(ScreenManager&&) = delete;

    void push(std::unique_ptr<Screen> screen);
    void pop();
    // Pops the top screen and pushes this one in its place.
    void replace(std::unique_ptr<Screen> screen);
    // Removes every screen (leaving the menus for the world).
    void clear();

    // Applies the waiting changes, hands the last frame's Back to the top
    // screen, and updates every screen. Call before UiContext::update, so a
    // screen pushed by a click is laid out and drawn in the next frame.
    void update(f32 deltaSeconds);

    [[nodiscard]] Screen* top() const noexcept {
        return m_stack.empty() ? nullptr : m_stack.back().screen.get();
    }
    [[nodiscard]] usize size() const noexcept { return m_stack.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_stack.empty(); }
    // True when a visible screen needs the mouse cursor.
    [[nodiscard]] bool wantsCursor() const noexcept;

private:
    struct Entry {
        std::unique_ptr<Screen> screen;
        Widget* root = nullptr; // Owned by the context's root.
        bool entered = false;   // Between its onEnter and onLeave.
    };
    enum class Change : u8 {
        Push,
        Pop,
        Clear
    };
    struct Pending {
        Change change = Change::Push;
        std::unique_ptr<Screen> screen;
    };

    void apply();
    void popNow();
    void refreshVisibility();

    UiContext& m_context;
    stats::StatRegistry* m_stats;
    stats::StatId m_screensStat;
    std::vector<Entry> m_stack;
    std::vector<Pending> m_pending;
};

} // namespace vxe::ui
