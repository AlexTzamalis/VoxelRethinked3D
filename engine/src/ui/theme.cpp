#include "vxe/ui/theme.hpp"

#include <toml++/toml.hpp>

#include <array>
#include <charconv>
#include <format>
#include <utility>

namespace vxe::ui {
namespace {

// The keys of a theme file and the fields they set. One table each, so
// adding a field is one line here.
const std::array<std::pair<std::string_view, u32 Theme::Colors::*>, 15> kColorKeys = {{
    {"text", &Theme::Colors::text},
    {"text_muted", &Theme::Colors::textMuted},
    {"text_disabled", &Theme::Colors::textDisabled},
    {"title", &Theme::Colors::title},
    {"panel", &Theme::Colors::panel},
    {"panel_border", &Theme::Colors::panelBorder},
    {"button", &Theme::Colors::button},
    {"button_hover", &Theme::Colors::buttonHover},
    {"button_pressed", &Theme::Colors::buttonPressed},
    {"button_disabled", &Theme::Colors::buttonDisabled},
    {"button_border", &Theme::Colors::buttonBorder},
    {"button_highlight", &Theme::Colors::buttonHighlight},
    {"button_shadow", &Theme::Colors::buttonShadow},
    {"focus", &Theme::Colors::focus},
    {"backdrop", &Theme::Colors::backdrop},
}};

const std::array<std::pair<std::string_view, f32 Theme::Metrics::*>, 6> kMetricKeys = {{
    {"button_height", &Theme::Metrics::buttonHeight},
    {"button_min_width", &Theme::Metrics::buttonMinWidth},
    {"button_padding", &Theme::Metrics::buttonPadding},
    {"panel_padding", &Theme::Metrics::panelPadding},
    {"gap", &Theme::Metrics::gap},
    {"border", &Theme::Metrics::border},
}};

constexpr f64 kMaxMetric = 512.0;

void readColors(const toml::table& table, Theme::Colors& colors, std::vector<std::string>& warnings) {
    for (const auto& [key, node] : table) {
        const auto found =
            std::ranges::find(kColorKeys, key.str(), [](const auto& entry) { return entry.first; });
        if (found == kColorKeys.end()) {
            warnings.push_back(std::format("Theme: unknown color '{}'", key.str()));
            continue;
        }
        const auto* text = node.as_string();
        u32 value = 0;
        if (text == nullptr || !parseColor(text->get(), value)) {
            warnings.push_back(
                std::format(R"(Theme: color '{}' is not "#RRGGBB" or "#RRGGBBAA")", key.str()));
            continue;
        }
        colors.*(found->second) = value;
    }
}

void readMetrics(const toml::table& table, Theme::Metrics& metrics, std::vector<std::string>& warnings) {
    for (const auto& [key, node] : table) {
        const auto found =
            std::ranges::find(kMetricKeys, key.str(), [](const auto& entry) { return entry.first; });
        if (found == kMetricKeys.end()) {
            warnings.push_back(std::format("Theme: unknown metric '{}'", key.str()));
            continue;
        }
        const std::optional<f64> value = node.value<f64>();
        if (!value || *value < 0.0 || *value > kMaxMetric) {
            warnings.push_back(
                std::format("Theme: metric '{}' must be a number from 0 to {}", key.str(), kMaxMetric));
            continue;
        }
        metrics.*(found->second) = static_cast<f32>(*value);
    }
}

} // namespace

bool parseColor(std::string_view text, u32& out) noexcept {
    if (text.size() != 7 && text.size() != 9) {
        return false;
    }
    if (text.front() != '#') {
        return false;
    }
    u32 value = 0;
    const char* begin = text.data() + 1;
    const char* end = text.data() + text.size();
    const auto [last, error] = std::from_chars(begin, end, value, 16);
    if (error != std::errc() || last != end) {
        return false;
    }
    if (text.size() == 7) {
        value = (value << 8U) | 0xFFU; // No alpha: opaque.
    }
    // The text is RRGGBBAA; ui::rgba keeps red in the lowest byte.
    out = ((value >> 24U) & 0xFFU) | (((value >> 16U) & 0xFFU) << 8U) | (((value >> 8U) & 0xFFU) << 16U) |
          ((value & 0xFFU) << 24U);
    return true;
}

Result<Theme> Theme::parse(std::string_view text, std::vector<std::string>& warnings) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& error) {
        return Error(ErrorCode::ParseError, std::format("Theme: {}", error.description()));
    }
    Theme theme;
    for (const auto& [key, node] : root) {
        const auto* table = node.as_table();
        if (key.str() == "colors" && table != nullptr) {
            readColors(*table, theme.colors, warnings);
        } else if (key.str() == "metrics" && table != nullptr) {
            readMetrics(*table, theme.metrics, warnings);
        } else {
            warnings.push_back(
                std::format("Theme: unknown entry '{}' (expected [colors] and [metrics])", key.str()));
        }
    }
    return theme;
}

} // namespace vxe::ui
