#include "vxe/debug/overlay.hpp"

#include <vxe/config/console.hpp>
#include <vxe/core/memory.hpp>
#include <vxe/debug/imgui_renderer.hpp>
#include <vxe/platform/platform.hpp>

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <ranges>
#include <variant>

namespace vxe::debug {
namespace {

constexpr u16 kScancodeF3 = 60;
constexpr u16 kScancodeRight = 79;
constexpr u16 kScancodeLeft = 80;
constexpr usize kMaxHitches = 32;
constexpr usize kMaxRecentEvents = 32;
constexpr u32 kFrameHistory = 1000; // Section 18.2: the frame graph shows the last 1000 frames.
constexpr f64 kBytesPerMiB = 1024.0 * 1024.0;

void text(const std::string& line) {
    ImGui::TextUnformatted(line.c_str());
}

std::string describeEvent(const platform::PlatformEvent& event) {
    if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
        return std::format("Key {} ({}) {}{}", key->scancode, platform::Platform::scancodeName(key->scancode),
                           key->down ? "down" : "up", key->repeat ? " (repeat)" : "");
    }
    if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
        return std::format("Mouse button {} {} at {:.0f}, {:.0f}", static_cast<int>(button->button),
                           button->down ? "down" : "up", button->x, button->y);
    }
    if (const auto* wheel = std::get_if<platform::MouseWheelEvent>(&event)) {
        return std::format("Wheel {:.1f}, {:.1f}", wheel->x, wheel->y);
    }
    if (const auto* typed = std::get_if<platform::TextInputEvent>(&event)) {
        return std::format("Text \"{}\"", typed->text);
    }
    if (const auto* focus = std::get_if<platform::WindowFocusChanged>(&event)) {
        return focus->focused ? "Window focused" : "Window lost focus";
    }
    return {}; // Motion and window events would flood the list.
}

std::string_view typeName(stats::StatType type) {
    switch (type) {
    case stats::StatType::Counter:
        return "counter";
    case stats::StatType::Gauge:
        return "gauge";
    case stats::StatType::Timer:
        return "timer";
    case stats::StatType::Histogram:
        return "histogram";
    }
    return "?";
}

std::string formatValue(f64 value, stats::StatUnit unit) {
    switch (unit) {
    case stats::StatUnit::Bytes:
        return std::format("{:.2f} MiB", value / kBytesPerMiB);
    case stats::StatUnit::Milliseconds:
        return std::format("{:.3f} ms", value);
    case stats::StatUnit::Percent:
        return std::format("{:.1f} %", value);
    case stats::StatUnit::Count:
        break;
    }
    return std::format("{:.0f}", value);
}

// A capture file name that sorts by time: captures/stats-20261028-031512.csv.
std::filesystem::path captureFileName(const std::filesystem::path& folder) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return folder / std::format("stats-{:04}{:02}{:02}-{:02}{:02}{:02}.csv", local.tm_year + 1900,
                                local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

} // namespace

DebugOverlay::DebugOverlay(const OverlayDesc& desc)
    : m_settings(*desc.settings)
    , m_input(*desc.input)
    , m_stats(*desc.stats)
    , m_device(desc.device)
    , m_captureFolder(desc.captureFolder)
    , m_lastAllocationCount(memory::globalAllocationCount()) {
    using config::Visibility;
    m_visible =
        m_settings.registerBool({{.id = "Debug.Overlay.Visible",
                                  .displayName = "Show performance overlay",
                                  .description = "The F3 overlay with frame times, GPU, memory and input."},
                                 false});
    m_page =
        m_settings.registerInt({{.id = "Debug.Overlay.Page",
                                 .displayName = "Overlay page",
                                 .description = "The page shown next to the Overview (F3+Left, F3+Right).",
                                 .visibility = Visibility::Advanced},
                                0,
                                0,
                                31,
                                1});
    m_opacity = m_settings.registerFloat({{.id = "Debug.Overlay.Opacity",
                                           .displayName = "Overlay opacity",
                                           .description = "Background opacity of the overlay windows.",
                                           .visibility = Visibility::Advanced},
                                          0.85F,
                                          0.2F,
                                          1.0F,
                                          0.05F});
    m_scale = m_settings.registerFloat({{.id = "Debug.Overlay.Scale",
                                         .displayName = "Overlay scale",
                                         .description = "Text size of the developer tools.",
                                         .visibility = Visibility::Advanced},
                                        1.0F,
                                        0.5F,
                                        3.0F,
                                        0.05F});
    m_updateRate =
        m_settings.registerInt({{.id = "Debug.Overlay.UpdateRate",
                                 .displayName = "Overlay text update rate",
                                 .description = "Times per second the Overview text changes, so it stays "
                                                "readable. Graphs update every frame.",
                                 .visibility = Visibility::Advanced},
                                10,
                                1,
                                60,
                                1});
    m_csvSeconds =
        m_settings.registerInt({{.id = "Debug.Overlay.CsvSeconds",
                                 .displayName = "CSV capture length",
                                 .description = "Seconds of stats one CSV capture records into captures/.",
                                 .visibility = Visibility::Advanced},
                                10,
                                1,
                                600,
                                1});
    m_hitchFactor =
        m_settings.registerFloat({{.id = "Debug.Overlay.HitchFactor",
                                   .displayName = "Hitch threshold",
                                   .description = "A frame slower than this many times the median frame "
                                                  "is logged as a hitch.",
                                   .visibility = Visibility::Advanced},
                                  2.0F,
                                  1.5F,
                                  10.0F,
                                  0.1F});

    // The Debug context is always active: F3 works in menus and while the
    // debug UI itself has the input.
    m_context = m_input.registerContext({.name = "Debug", .alwaysActive = true});
    m_input.pushContext(m_context);
    using input::Binding;
    m_toggle = m_input.registerAction({.id = "debug.overlay.toggle",
                                       .displayName = "Toggle debug overlay",
                                       .category = "Debug",
                                       .context = m_context,
                                       .defaults = {Binding::key(kScancodeF3)}});
    m_nextPage = m_input.registerAction({.id = "debug.overlay.next_page",
                                         .displayName = "Next overlay page",
                                         .category = "Debug",
                                         .context = m_context,
                                         .defaults = {Binding::chord(kScancodeF3, kScancodeRight)}});
    m_previousPage = m_input.registerAction({.id = "debug.overlay.previous_page",
                                             .displayName = "Previous overlay page",
                                             .category = "Debug",
                                             .context = m_context,
                                             .defaults = {Binding::chord(kScancodeF3, kScancodeLeft)}});

    using stats::StatType;
    using stats::StatUnit;
    const auto stat = [this](const stats::StatDesc& statDesc) {
        return m_stats.registerStat(statDesc).value();
    };
    m_cpuTime = stat({.name = "Frame.CpuTime",
                      .description = "Wall clock time of the whole frame on the CPU.",
                      .type = StatType::Gauge,
                      .unit = StatUnit::Milliseconds,
                      .historyFrames = kFrameHistory});
    m_gpuTime = stat({.name = "Frame.GpuTime",
                      .description = "GPU time of the frame, from timestamp queries (a few frames old).",
                      .type = StatType::Gauge,
                      .unit = StatUnit::Milliseconds,
                      .historyFrames = kFrameHistory});
    m_allocations =
        stat({.name = "Memory.Allocations",
              .description = "Global operator new calls in the frame. Should be near zero in game.",
              .type = StatType::Counter});
    m_drawTime = stat({.name = "Debug.Overlay.DrawTime",
                       .description = "CPU time the overlay spends per frame.",
                       .type = StatType::Timer});
    m_gpuDrawCalls = stat({.name = "Gpu.DrawCalls", .description = "Draws sent to the GPU this frame."});
    m_gpuStateChanges =
        stat({.name = "Gpu.StateChanges", .description = "Fixed function state calls this frame."});
    m_gpuBufferBytes = stat({.name = "Gpu.BufferMemory",
                             .description = "Memory of every live GPU buffer.",
                             .type = StatType::Gauge,
                             .unit = StatUnit::Bytes});
    m_gpuTextureBytes = stat({.name = "Gpu.TextureMemory",
                              .description = "Memory of every live GPU texture.",
                              .type = StatType::Gauge,
                              .unit = StatUnit::Bytes});
    if (m_device != nullptr) {
        m_previousDevice = m_device->stats();
    }

    // Page 0 is the Overview, which is always drawn while the overlay is on.
    m_pages.push_back({.name = "Overview", .draw = [] {}});
    m_pages.push_back({.name = "Frame", .draw = [this] { drawFramePage(); }});
    m_pages.push_back({.name = "GPU", .draw = [this] { drawGpuPage(); }});
    m_pages.push_back({.name = "Streaming and Jobs", .draw = [this] { drawStreamingPage(); }});
    m_pages.push_back({.name = "LOD", .draw = [this] { drawLodPage(); }});
    m_pages.push_back({.name = "Memory", .draw = [this] { drawMemoryPage(); }});
    m_pages.push_back({.name = "Input", .draw = [this] { drawInputPage(); }});
    m_pages.push_back({.name = "Settings diff", .draw = [this] { drawSettingsPage(); }});
    m_pages.push_back({.name = "All stats", .draw = [this] { drawStatsPage(); }});

    m_cpuHistory.reserve(kFrameHistory);
    m_gpuHistory.reserve(kFrameHistory);
    m_scratch.reserve(kFrameHistory);
}

DebugOverlay::~DebugOverlay() {
    m_input.popContext(m_context);
}

void DebugOverlay::addPage(std::string name, std::function<void()> draw) {
    m_pages.push_back({.name = std::move(name), .draw = std::move(draw)});
}

void DebugOverlay::addOverviewLine(std::function<std::string()> line) {
    m_overviewLines.push_back(std::move(line));
}

bool DebugOverlay::isVisible() const {
    return m_visible.get();
}

void DebugOverlay::setVisible(bool visible) {
    static_cast<void>(m_visible.set(visible));
}

usize DebugOverlay::currentPage() const {
    return std::min(static_cast<usize>(std::max(m_page.get(), 0)), m_pages.size() - 1);
}

void DebugOverlay::showPage(usize index) {
    static_cast<void>(m_page.set(static_cast<i32>(index % m_pages.size())));
}

void DebugOverlay::startCsvCapture() {
    m_csv.start(m_stats, captureFileName(m_captureFolder), static_cast<f64>(m_csvSeconds.get()));
}

void DebugOverlay::processEvents(std::span<const platform::PlatformEvent> events) {
    for (const platform::PlatformEvent& event : events) {
        std::string description = describeEvent(event);
        if (description.empty()) {
            continue;
        }
        m_recentEvents.push_front(std::move(description));
        if (m_recentEvents.size() > kMaxRecentEvents) {
            m_recentEvents.pop_back();
        }
    }
}

void DebugOverlay::recordFrame(f64 cpuMilliseconds, std::optional<f64> gpuMilliseconds) {
    m_stats.set(m_cpuTime, cpuMilliseconds);
    m_stats.set(m_gpuTime, gpuMilliseconds.value_or(0.0));
    const u64 allocations = memory::globalAllocationCount();
    m_stats.add(m_allocations, static_cast<i64>(allocations - m_lastAllocationCount));
    m_lastAllocationCount = allocations;
    if (m_device != nullptr) {
        const rhi::DeviceStats device = m_device->stats();
        m_stats.add(m_gpuDrawCalls, static_cast<i64>(device.drawCalls - m_previousDevice.drawCalls));
        m_stats.add(m_gpuStateChanges, static_cast<i64>(device.stateChanges - m_previousDevice.stateChanges));
        m_stats.set(m_gpuBufferBytes, static_cast<f64>(device.bufferBytes));
        m_stats.set(m_gpuTextureBytes, static_cast<f64>(device.textureBytes));
        m_previousDevice = device;
    }
}

// F3 alone toggles on release, so holding F3 for F3+Right does not also hide
// the overlay (the same feel as other games' F3 menus).
void DebugOverlay::handleKeys() {
    if (m_input.isPressed(m_toggle)) {
        m_chordUsed = false;
    }
    const bool next = m_input.isPressed(m_nextPage);
    const bool previous = m_input.isPressed(m_previousPage);
    if (next || previous) {
        m_chordUsed = true;
        const usize count = m_pages.size();
        showPage(next ? (currentPage() + 1) % count : (currentPage() + count - 1) % count);
        setVisible(true);
    }
    if (m_input.isReleased(m_toggle) && !m_chordUsed) {
        setVisible(!isVisible());
    }
}

void DebugOverlay::update(f64 frameSeconds) {
    const stats::ScopedStatTimer timer(m_stats, m_drawTime);
    handleKeys();
    m_csv.recordFrame(m_stats, frameSeconds);

    // Hitches are logged even while the overlay is hidden, so they can be
    // looked at after they happened.
    const f64 cpu = m_stats.value(m_cpuTime);
    if (m_summary.p50Ms > 0.0 && cpu > m_summary.p50Ms * static_cast<f64>(m_hitchFactor.get())) {
        m_hitches.push_front(
            {.frame = m_stats.framesClosed(), .milliseconds = cpu, .timesMedian = cpu / m_summary.p50Ms});
        if (m_hitches.size() > kMaxHitches) {
            m_hitches.pop_back();
        }
    }
    m_sinceRefresh += frameSeconds;
    if (m_sinceRefresh >= 1.0 / static_cast<f64>(std::max(m_updateRate.get(), 1))) {
        refreshOverview();
    }
    if (!isVisible()) {
        return;
    }
    ImGui::GetStyle().FontScaleMain = m_scale.get();
    drawOverview();
    const usize page = currentPage();
    if (page > 0) {
        m_pages[page].draw();
    }
}

FrameTimeSummary DebugOverlay::recentFrames() {
    m_stats.copyHistory(m_cpuTime, m_cpuHistory);
    return summarizeFrameTimes(m_cpuHistory, m_scratch);
}

// Rebuilds the Overview text at Debug.Overlay.UpdateRate: numbers that change
// every frame are unreadable, and formatting them less often is cheaper.
void DebugOverlay::refreshOverview() {
    m_sinceRefresh = 0.0;
    m_stats.copyHistory(m_cpuTime, m_cpuHistory);
    m_summary = summarizeFrameTimes(m_cpuHistory, m_scratch);
    m_overviewText.clear();
    m_overviewText.push_back(std::format("{:.0f} FPS   1% low {:.0f}   0.1% low {:.0f}", m_summary.averageFps,
                                         m_summary.low1Fps, m_summary.low01Fps));
    m_overviewText.push_back(std::format("CPU {:.3f} ms   GPU {:.3f} ms", m_stats.average(m_cpuTime, 60),
                                         m_stats.average(m_gpuTime, 60)));
    std::string memoryLine =
        std::format("Tracked memory {:.1f} MiB", static_cast<f64>(memory::totalBytes()) / kBytesPerMiB);
    if (memory::globalAllocationCountAvailable()) {
        memoryLine += std::format("   {:.0f} allocations per frame", m_stats.average(m_allocations, 60));
    }
    m_overviewText.push_back(std::move(memoryLine));
    if (m_device != nullptr) {
        m_overviewText.push_back(std::format(
            "{:.0f} draw calls per frame   GPU memory {:.1f} MiB", m_stats.average(m_gpuDrawCalls, 60),
            (m_stats.value(m_gpuBufferBytes) + m_stats.value(m_gpuTextureBytes)) / kBytesPerMiB));
    }
    for (const auto& line : m_overviewLines) {
        m_overviewText.push_back(line());
    }
}

void DebugOverlay::drawOverview() {
    ImGui::SetNextWindowPos({8.0F, 8.0F}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(m_opacity.get());
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##Overview", nullptr, kFlags)) {
        for (const std::string& line : m_overviewText) {
            text(line);
        }
        const usize page = currentPage();
        text(std::format("Page {}/{}: {}   (F3+Left, F3+Right)", page + 1, m_pages.size(),
                         m_pages[page].name));
        if (m_csv.isRecording()) {
            text("Recording stats to CSV...");
        }
    }
    ImGui::End();
}

void DebugOverlay::beginPageWindow(const char* title) {
    ImGui::SetNextWindowPos({8.0F, 150.0F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({560.0F, 440.0F}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(m_opacity.get());
    ImGui::Begin(title);
}

void DebugOverlay::drawFramePage() {
    beginPageWindow("Frame");
    m_stats.copyHistory(m_cpuTime, m_cpuHistory);
    m_stats.copyHistory(m_gpuTime, m_gpuHistory);
    const FrameTimeSummary cpu = summarizeFrameTimes(m_cpuHistory, m_scratch);
    if (ImPlot::BeginPlot("Frame time", {-1.0F, 180.0F})) {
        ImPlot::SetupAxes("frame", "ms", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::PlotLine("CPU", m_cpuHistory.data(), static_cast<int>(m_cpuHistory.size()));
        ImPlot::PlotLine("GPU", m_gpuHistory.data(), static_cast<int>(m_gpuHistory.size()));
        ImPlot::EndPlot();
    }
    if (ImGui::BeginTable("Percentiles", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
        for (const char* header : {"average", "50%", "90%", "99%", "99.9%", "1% low"}) {
            ImGui::TableSetupColumn(header);
        }
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (const f64 value : {cpu.averageMs, cpu.p50Ms, cpu.p90Ms, cpu.p99Ms, cpu.p999Ms}) {
            ImGui::TableNextColumn();
            text(std::format("{:.3f} ms", value));
        }
        ImGui::TableNextColumn();
        text(std::format("{:.0f} FPS", cpu.low1Fps));
        ImGui::EndTable();
    }
    if (ImPlot::BeginPlot("Frame time histogram", {-1.0F, 140.0F})) {
        ImPlot::SetupAxes("ms", "frames", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::PlotHistogram("CPU", m_cpuHistory.data(), static_cast<int>(m_cpuHistory.size()), 50);
        ImPlot::EndPlot();
    }
    text(std::format("Hitches (frames over {:.1f}x the median):", m_hitchFactor.get()));
    if (m_hitches.empty()) {
        text("  none");
    }
    for (const Hitch& hitch : m_hitches) {
        text(std::format("  frame {}: {:.2f} ms ({:.1f}x)", hitch.frame, hitch.milliseconds,
                         hitch.timesMedian));
    }
    ImGui::End();
}

void DebugOverlay::drawGpuPage() {
    beginPageWindow("GPU");
    if (m_device == nullptr) {
        text("No graphics device.");
        ImGui::End();
        return;
    }
    const rhi::DeviceCapabilities& caps = m_device->capabilities();
    const rhi::DeviceStats stats = m_device->stats();
    const auto yesNo = [](bool value) { return value ? "yes" : "no"; };
    text(std::format("{} ({})", caps.rendererName, caps.vendorName));
    text(std::format("API {}.{}, driver {}", caps.apiVersionMajor, caps.apiVersionMinor, caps.driverVersion));
    text(std::format("Indirect count {}, bindless {}, SPIR-V {}, timestamps {}, debug output {}",
                     yesNo(caps.multiDrawIndirectCount), yesNo(caps.bindlessTextures),
                     yesNo(caps.spirvShaders), yesNo(caps.timestampQueries), yesNo(caps.debugOutput)));
    text(std::format("Max texture {} px, {} array layers, video memory {:.0f} MiB", caps.maxTextureSize,
                     caps.maxTextureArrayLayers, static_cast<f64>(caps.videoMemoryBytes) / kBytesPerMiB));
    ImGui::Separator();
    text(std::format("GPU frame {:.3f} ms (average of 60)", m_stats.average(m_gpuTime, 60)));
    text(std::format("Draw calls {:.0f} per frame, state changes {:.0f} per frame",
                     m_stats.value(m_gpuDrawCalls), m_stats.value(m_gpuStateChanges)));
    text(std::format("Buffers {} ({:.2f} MiB), textures {} ({:.2f} MiB)", stats.buffers,
                     static_cast<f64>(stats.bufferBytes) / kBytesPerMiB, stats.textures,
                     static_cast<f64>(stats.textureBytes) / kBytesPerMiB));
    text(
        std::format("Samplers {}, shaders {}, pipelines {}", stats.samplers, stats.shaders, stats.pipelines));
    text(std::format("Pipeline binds {} ({} skipped), invalid commands {}, waiting to be freed {}",
                     stats.pipelineBinds, stats.pipelineBindsSkipped, stats.invalidCommands,
                     stats.pendingDestroys));
    text(std::format("Driver errors {}, warnings {}", stats.apiErrors, stats.apiWarnings));
    drawPassTimings();
    ImGui::End();
}

// Per pass GPU times (Section 18.2). The renderer's frame graph publishes
// them as Gpu.Pass.<name> stats, so this page reads the stat registry and
// needs no dependency on the render module or on a particular renderer.
void DebugOverlay::drawPassTimings() {
    constexpr std::string_view kPrefix = "Gpu.Pass.";
    constexpr u32 kAverageFrames = 60;
    bool tableOpen = false;
    f64 total = 0.0;
    for (u32 i = 0; i < m_stats.statCount(); ++i) {
        const stats::StatId id{i};
        const auto& info = m_stats.info(id);
        if (!info.name.starts_with(kPrefix)) {
            continue;
        }
        if (!tableOpen) {
            ImGui::Separator();
            tableOpen = ImGui::BeginTable("Passes", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
            if (!tableOpen) {
                return;
            }
            for (const char* header : {"Pass", "GPU ms", "Max of 60"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
        }
        const f64 average = m_stats.average(id, kAverageFrames);
        total += average;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        text(info.name.substr(kPrefix.size()));
        ImGui::TableNextColumn();
        text(std::format("{:.3f}", average));
        ImGui::TableNextColumn();
        text(std::format("{:.3f}", m_stats.maximum(id, kAverageFrames)));
    }
    if (tableOpen) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        text("All passes");
        ImGui::TableNextColumn();
        text(std::format("{:.3f}", total));
        ImGui::EndTable();
    }
}

void DebugOverlay::setJobSystem(jobs::JobSystem* jobs) {
    m_jobs = jobs;
    m_jobSnapshot = {};
    m_workerBusy.clear();
}

// Streaming (Section 18.2 page 4) reads the Voxel.Streaming stats by name,
// since debug sits below voxel in the module layers; the job system is
// queried directly, at most Debug.Overlay.UpdateRate times per second and
// only while this page shows.
// Far terrain (Section 18.2 page 6), all from stats: Voxel.Lod.* from the
// LOD manager, Render.Lod.* from the LOD renderer; the coverage map preview
// when the main loop handed over its texture.
void DebugOverlay::drawLodPage() {
    beginPageWindow("LOD");
    const auto statValue = [this](std::string_view name) {
        const stats::StatId id = m_stats.find(name);
        return id.isValid() ? m_stats.value(id) : 0.0;
    };
    if (!m_stats.find("Voxel.Lod.Tiles").isValid()) {
        text("No far terrain.");
        ImGui::End();
        return;
    }
    text(std::format(
        "Reach {:.0f} blocks   tiles {:.0f} ready, {:.0f} queued, {:.0f} in work, {:.0f} retiring",
        statValue("Voxel.Lod.ReachBlocks"), statValue("Voxel.Lod.Tiles"), statValue("Voxel.Lod.Queued"),
        statValue("Voxel.Lod.Running"), statValue("Voxel.Lod.Retiring")));
    if (ImGui::BeginTable("Levels", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* header : {"Level", "Cell (blocks)", "Wanted", "Ready"}) {
            ImGui::TableSetupColumn(header);
        }
        ImGui::TableHeadersRow();
        for (u32 level = 1; level <= 16; ++level) {
            const f64 wanted = statValue(std::format("Voxel.Lod.Level{}.Wanted", level));
            const f64 ready = statValue(std::format("Voxel.Lod.Level{}.Ready", level));
            if (wanted == 0.0 && ready == 0.0) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(std::format("{}", level));
            ImGui::TableNextColumn();
            text(std::format("{}", u64{1} << level));
            ImGui::TableNextColumn();
            text(std::format("{:.0f}", wanted));
            ImGui::TableNextColumn();
            text(std::format("{:.0f}", ready));
        }
        ImGui::EndTable();
    }
    const auto perSecond = [this](std::string_view name) {
        const stats::StatId id = m_stats.find(name);
        return id.isValid() ? m_stats.perSecond(id) : 0.0;
    };
    text(std::format("Sampled {:.0f}/s ({:.2f} ms each)   from cache {:.0f}/s   edits {:.0f}/s",
                     perSecond("Voxel.Lod.Sampled"),
                     m_stats.find("Voxel.Lod.SampleTime").isValid()
                         ? m_stats.average(m_stats.find("Voxel.Lod.SampleTime"), 240)
                         : 0.0,
                     perSecond("Voxel.Lod.FromCache"), perSecond("Voxel.Lod.Edits")));
    text(std::format("Cache: {:.0f} hits, {:.0f} misses, {:.0f} stale, {:.0f} damaged (per second)",
                     perSecond("Voxel.Lod.Cache.Hits"), perSecond("Voxel.Lod.Cache.Misses"),
                     perSecond("Voxel.Lod.Cache.Stale"), perSecond("Voxel.Lod.Cache.Damaged")));
    if (const f64 total = statValue("Voxel.Lod.Premap.Total"); total > 0.0) {
        text(std::format("Premap: {:.0f} of {:.0f} tiles ({:.0f} already cached)",
                         statValue("Voxel.Lod.Premap.Done"), total, statValue("Voxel.Lod.Premap.Skipped")));
    }
    ImGui::Separator();
    const f64 drawnQuads = statValue("Render.Lod.DrawnQuads");
    text(std::format("GPU: {:.0f} tiles, {:.0f} drawn, {:.0f} under real chunks",
                     statValue("Render.Lod.Tiles"), statValue("Render.Lod.DrawnTiles"),
                     statValue("Render.Lod.CoveredTiles")));
    text(std::format("Triangles drawn {:.0f} of {:.0f}   memory {:.1f} MB tiles, {:.1f} MB GPU",
                     drawnQuads * 2.0, statValue("Render.Lod.Quads") * 2.0,
                     statValue("Voxel.Lod.MemoryBytes") / (1024.0 * 1024.0),
                     statValue("Render.Lod.ArenaBytes") / (1024.0 * 1024.0)));
    if (m_lodCoverage.isValid()) {
        text("Coverage map (red: chunk columns real chunks draw; wraps every 128 columns):");
        ImGui::Image(ImGuiRenderer::textureId(m_lodCoverage), ImVec2(256.0F, 256.0F));
    }
    ImGui::End();
}

void DebugOverlay::drawStreamingPage() {
    beginPageWindow("Streaming and Jobs");
    const auto statValue = [this](std::string_view name) {
        const stats::StatId id = m_stats.find(name);
        return id.isValid() ? m_stats.value(id) : 0.0;
    };
    const stats::StatId loaded = m_stats.find("Voxel.Streaming.Loaded");
    if (!loaded.isValid()) {
        text("Nothing is streaming.");
    } else {
        text(std::format("Chunks: {:.0f} loaded, {:.0f} with a mesh{}", m_stats.value(loaded),
                         statValue("Voxel.Streaming.Meshed"),
                         statValue("Voxel.Streaming.Burst") > 0.0 ? "   (burst mode)" : ""));
        constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kStages = {{
            {"Pending", "Voxel.Streaming.Pending"},
            {"Loading", "Voxel.Streaming.Loading"},
            {"Ready", "Voxel.Streaming.Ready"},
            {"Lighting", "Voxel.Streaming.Lighting"},
            {"Mesh queued", "Voxel.Streaming.MeshQueued"},
            {"Meshing", "Voxel.Streaming.Meshing"},
            {"Uploading", "Voxel.Streaming.Uploading"},
        }};
        constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kRates = {{
            {"Loaded", "Voxel.Streaming.ChunksLoaded"},
            {"Lit by a job", "Voxel.Light.ChunksLit"},
            {"Meshed", "Voxel.Streaming.ChunksMeshed"},
            {"Uploaded", "Voxel.Streaming.ChunksUploaded"},
            {"Unloaded", "Voxel.Streaming.ChunksUnloaded"},
            {"Cancelled", "Voxel.Streaming.Cancelled"},
            {"Stale meshes", "Voxel.Streaming.StaleMeshes"},
        }};
        if (ImGui::BeginTable("Pipeline", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            for (const char* header : {"Stage", "Chunks", "Done", "Per second"}) {
                ImGui::TableSetupColumn(header);
            }
            ImGui::TableHeadersRow();
            for (usize row = 0; row < kStages.size(); ++row) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                text(std::string(kStages.at(row).first));
                ImGui::TableNextColumn();
                text(std::format("{:.0f}", statValue(kStages.at(row).second)));
                ImGui::TableNextColumn();
                text(std::string(kRates.at(row).first));
                ImGui::TableNextColumn();
                const stats::StatId rate = m_stats.find(kRates.at(row).second);
                text(std::format("{:.0f}", rate.isValid() ? m_stats.perSecond(rate) : 0.0));
            }
            ImGui::EndTable();
        }
        if (const stats::StatId time = m_stats.find("Voxel.Streaming.UpdateTime"); time.isValid()) {
            text(std::format("Main thread: {:.3f} ms average, {:.3f} ms max (240 frames)",
                             m_stats.average(time, 60), m_stats.maximum(time, 240)));
        }
        if (const stats::StatId latency = m_stats.find("Voxel.Streaming.Latency"); latency.isValid()) {
            const stats::HistogramSnapshot histogram = m_stats.histogram(latency);
            if (histogram.samples > 0) {
                text(std::format("Request to GPU: {:.0f} ms median, 90% {:.0f} ms, 99% {:.0f} ms",
                                 histogram.percentile(0.5), histogram.percentile(0.9),
                                 histogram.percentile(0.99)));
            }
        }
        const stats::StatId upload = m_stats.find("Render.Chunks.UploadBytes");
        const Result<config::SettingValue> budget = m_settings.value("World.Streaming.MaxUploadKBPerFrame");
        if (upload.isValid() && budget && std::holds_alternative<i32>(budget.value())) {
            text(std::format("Uploads: {:.0f} KB now, {:.0f} KB average, budget {} KB per frame",
                             m_stats.value(upload) / 1024.0, m_stats.average(upload, 60) / 1024.0,
                             std::get<i32>(budget.value())));
        }
    }

    ImGui::Separator();
    if (m_jobs == nullptr) {
        text("No job system.");
        ImGui::End();
        return;
    }
    const f64 refreshSeconds = 1.0 / static_cast<f64>(std::max(m_updateRate.get(), 1));
    const f64 elapsedSeconds = m_jobClock.elapsedMilliseconds() / 1000.0;
    if (m_workerBusy.empty() || elapsedSeconds >= refreshSeconds) {
        m_jobClock.restart();
        jobs::JobSystemStats current = m_jobs->stats();
        m_workerBusy.assign(current.workers.size(), 0.0F);
        if (m_jobSnapshot.workers.size() == current.workers.size() && elapsedSeconds > 0.0) {
            for (usize i = 0; i < current.workers.size(); ++i) {
                const f64 busy =
                    toMilliseconds(current.workers[i].busy - m_jobSnapshot.workers[i].busy) / 1000.0;
                m_workerBusy[i] = static_cast<f32>(std::clamp(busy / elapsedSeconds, 0.0, 1.0));
            }
        }
        m_jobSnapshot = std::move(current);
    }
    const jobs::JobSystemStats& jobStats = m_jobSnapshot;
    u64 queued = 0;
    for (const u64 count : jobStats.queued) {
        queued += count;
    }
    text(std::format("Jobs: {} workers, {} IO threads, {:.0f} per second, {} cancelled",
                     jobStats.workers.size(), jobStats.ioThreads.size(), jobStats.jobsPerSecond,
                     jobStats.jobsCancelled));
    text(std::format("Queued {}: critical {}, high {}, normal {}, low {}, background {}; {} waiting", queued,
                     jobStats.queued[0], jobStats.queued[1], jobStats.queued[2], jobStats.queued[3],
                     jobStats.queued[4], jobStats.waitingForDependencies));
    for (usize i = 0; i < m_workerBusy.size(); ++i) {
        const std::string label = std::format("Worker {}: {:.0f}% busy", i, m_workerBusy[i] * 100.0F);
        ImGui::ProgressBar(m_workerBusy[i], ImVec2(-1.0F, 0.0F), label.c_str());
    }
    ImGui::End();
}

void DebugOverlay::drawMemoryPage() {
    beginPageWindow("Memory");
    if (memory::globalAllocationCountAvailable()) {
        text(std::format("Allocations this frame: {:.0f} (average {:.1f})", m_stats.value(m_allocations),
                         m_stats.average(m_allocations, 120)));
    } else {
        text("Allocation counting is off in sanitizer builds.");
    }
    if (ImGui::BeginTable("Tags", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* header : {"Tag", "Current", "Peak", "Allocations"}) {
            ImGui::TableSetupColumn(header);
        }
        ImGui::TableHeadersRow();
        for (usize i = 0; i < kMemoryTagCount; ++i) {
            const auto tag = static_cast<MemoryTag>(i);
            const memory::TagStats tagStats = memory::stats(tag);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(std::string(memory::nameOf(tag)));
            ImGui::TableNextColumn();
            text(std::format("{:.2f} MiB", static_cast<f64>(tagStats.currentBytes) / kBytesPerMiB));
            ImGui::TableNextColumn();
            text(std::format("{:.2f} MiB", static_cast<f64>(tagStats.peakBytes) / kBytesPerMiB));
            ImGui::TableNextColumn();
            text(std::format("{}", tagStats.allocations));
        }
        ImGui::EndTable();
    }
    text(std::format("Tracked total: {:.2f} MiB", static_cast<f64>(memory::totalBytes()) / kBytesPerMiB));
    ImGui::End();
}

void DebugOverlay::drawInputPage() {
    beginPageWindow("Input");
    text("Context stack (top first):");
    const auto stack = m_input.contextStack();
    for (const input::InputContextId context : std::views::reverse(stack)) {
        text(std::format("  {}", m_input.contextName(context)));
    }
    text("Active actions:");
    for (u32 i = 0; i < m_input.actionCount(); ++i) {
        const input::ActionId action{i};
        if (m_input.isHeld(action) || m_input.isToggledOn(action)) {
            text(std::format("  {}{}", m_input.action(action).id,
                             m_input.isPressed(action) ? " (pressed)" : ""));
        }
    }
    text("Recent input:");
    for (const std::string& event : m_recentEvents) {
        text(std::format("  {}", event));
    }
    ImGui::End();
}

void DebugOverlay::drawSettingsPage() {
    beginPageWindow("Settings diff");
    const auto changed = changedSettings(m_settings);
    if (changed.empty()) {
        text("Every setting has its default value.");
    }
    if (!changed.empty() &&
        ImGui::BeginTable("Changed", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* header : {"Setting", "Value", "Default", ""}) {
            ImGui::TableSetupColumn(header);
        }
        ImGui::TableHeadersRow();
        for (const config::SettingInfo* info : changed) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(info->id);
            ImGui::TableNextColumn();
            if (const auto value = m_settings.value(info->id)) {
                text(config::displaySettingValue(*info, value.value()));
            }
            ImGui::TableNextColumn();
            text(config::displaySettingValue(*info, info->defaultValue));
            ImGui::TableNextColumn();
            ImGui::PushID(info->id.c_str());
            if (ImGui::SmallButton("Reset")) {
                static_cast<void>(m_settings.reset(info->id));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void DebugOverlay::drawStatsPage() {
    beginPageWindow("All stats");
    if (m_csv.isRecording()) {
        text("Recording a CSV capture...");
    } else if (ImGui::Button("Record CSV")) {
        startCsvCapture();
    }
    if (const std::string result = m_csv.lastResult(); !result.empty()) {
        ImGui::SameLine();
        text(result);
    }
    ImGui::InputTextWithHint("##filter", "Filter", m_statFilter.data(), m_statFilter.size());
    const std::string_view filter(m_statFilter.data());
    if (ImGui::BeginTable("Stats", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          {0.0F, -1.0F})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* header : {"Stat", "Type", "This frame", "Average of 60"}) {
            ImGui::TableSetupColumn(header);
        }
        ImGui::TableHeadersRow();
        for (u32 i = 0; i < m_stats.statCount(); ++i) {
            const stats::StatId id{i};
            const auto& info = m_stats.info(id);
            if (!filter.empty() && info.name.find(filter) == std::string::npos) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(info.name);
            if (ImGui::IsItemHovered() && !info.description.empty()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(info.description.c_str());
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            text(std::string(typeName(info.type)));
            ImGui::TableNextColumn();
            text(formatValue(m_stats.value(id), info.unit));
            ImGui::TableNextColumn();
            text(formatValue(m_stats.average(id, 60), info.unit));
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace vxe::debug
