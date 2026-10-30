#include "vxe/ui/screen.hpp"

#include <algorithm>
#include <utility>

namespace vxe::ui {
namespace {

// The full window root of one screen: dims what is behind it when the
// screen asks for a backdrop.
class ScreenRoot : public Widget {
public:
    explicit ScreenRoot(bool backdrop)
        : m_backdrop(backdrop) {}

    void draw(DrawContext& context) const override {
        if (m_backdrop) {
            context.fillRect(rect(), context.theme().colors.backdrop);
        }
    }
    // A screen covers the window: clicks on its empty parts never reach the
    // world behind it.
    [[nodiscard]] bool blocksPointer() const noexcept override { return m_backdrop; }

private:
    bool m_backdrop;
};

} // namespace

ScreenManager::ScreenManager(UiContext& context, stats::StatRegistry* stats)
    : m_context(context)
    , m_stats(stats) {
    m_context.root().style().direction = Direction::Stack;
    if (m_stats != nullptr) {
        auto id = m_stats->registerStat({.name = "Ui.Screens",
                                         .description = "Screens on the UI stack.",
                                         .type = stats::StatType::Gauge});
        m_screensStat = id ? id.value() : stats::StatId{};
    }
}

ScreenManager::~ScreenManager() {
    while (!m_stack.empty()) {
        popNow();
    }
}

void ScreenManager::push(std::unique_ptr<Screen> screen) {
    m_pending.push_back({.change = Change::Push, .screen = std::move(screen)});
}

void ScreenManager::pop() {
    m_pending.push_back({.change = Change::Pop, .screen = nullptr});
}

void ScreenManager::replace(std::unique_ptr<Screen> screen) {
    pop();
    push(std::move(screen));
}

void ScreenManager::clear() {
    m_pending.push_back({.change = Change::Clear, .screen = nullptr});
}

void ScreenManager::popNow() {
    const Entry entry = std::move(m_stack.back());
    m_stack.pop_back();
    if (entry.entered) {
        entry.screen->onLeave();
    }
    entry.screen->m_manager = nullptr;
    m_context.root().removeChild(*entry.root);
}

void ScreenManager::apply() {
    // Taken out first: screens may queue more changes from onEnter.
    std::vector<Pending> pending = std::exchange(m_pending, {});
    if (pending.empty()) {
        return;
    }
    for (Pending& change : pending) {
        switch (change.change) {
        case Change::Push: {
            if (!m_stack.empty() && m_stack.back().entered) {
                m_stack.back().entered = false;
                m_stack.back().screen->onLeave();
            }
            const ScreenOptions options = change.screen->options();
            Widget& root = m_context.root().add<ScreenRoot>(options.backdrop);
            change.screen->m_manager = this;
            change.screen->build(root);
            m_stack.push_back({.screen = std::move(change.screen), .root = &root});
            break;
        }
        case Change::Pop:
            if (!m_stack.empty()) {
                popNow();
            }
            break;
        case Change::Clear:
            while (!m_stack.empty()) {
                popNow();
            }
            break;
        }
    }
    refreshVisibility();
    // Exactly one onEnter per onLeave, however many changes came at once.
    if (!m_stack.empty() && !m_stack.back().entered) {
        m_stack.back().entered = true;
        m_stack.back().screen->onEnter();
    }
}

// The top screen is visible and interactive; going down, screens stay
// visible (but inert) while the screen above them draws what is below.
void ScreenManager::refreshVisibility() {
    bool shown = true;
    for (usize index = m_stack.size(); index-- > 0;) {
        Entry& entry = m_stack[index];
        const bool isTop = index + 1 == m_stack.size();
        entry.root->setVisible(shown);
        entry.root->setInteractive(isTop);
        shown = shown && entry.screen->options().drawBelow;
    }
}

void ScreenManager::update(f32 deltaSeconds) {
    apply();
    if (m_context.takeBack() && !m_stack.empty()) {
        if (!top()->onBack() && m_stack.size() > 1) {
            pop();
        }
        apply();
    }
    for (const Entry& entry : m_stack) {
        entry.screen->update(deltaSeconds);
    }
    if (m_stats != nullptr && m_screensStat.isValid()) {
        m_stats->set(m_screensStat, static_cast<f64>(m_stack.size()));
    }
}

bool ScreenManager::wantsCursor() const noexcept {
    return std::ranges::any_of(
        m_stack, [](const Entry& entry) { return entry.root->visible() && entry.screen->options().cursor; });
}

} // namespace vxe::ui
