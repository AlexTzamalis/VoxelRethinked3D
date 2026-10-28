#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/platform/events.hpp>
#include <vxe/rhi/device.hpp>

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The performance debug overlay (Section 18, E7.3).
namespace vxe::debug {

// Frame time figures for the Overview and Frame pages.
struct FrameTimeSummary {
    f64 averageMs = 0.0;
    f64 p50Ms = 0.0;
    f64 p90Ms = 0.0;
    f64 p99Ms = 0.0;
    f64 p999Ms = 0.0;
    // "1% low" and "0.1% low" FPS: the frame rate of the slowest 1 and 0.1
    // percent of frames, which shows stutter that the average hides.
    f64 low1Fps = 0.0;
    f64 low01Fps = 0.0;
    f64 averageFps = 0.0;
};

// Summarizes frame times in milliseconds. `scratch` is reused so callers
// that summarize every frame do not allocate.
[[nodiscard]] FrameTimeSummary summarizeFrameTimes(std::span<const f64> milliseconds,
                                                   std::vector<f64>& scratch);

// Every setting whose value differs from its default, for the Settings diff
// page (and the console's "changed" listing later).
[[nodiscard]] std::vector<const config::SettingInfo*>
changedSettings(const config::SettingsRegistry& settings);

// Records every stat once per frame into a CSV file (Section 18.3,
// Debug.Overlay.ExportCsv). Rows collect in memory while recording; the file
// is written on a background thread at the end, so the main thread never
// waits for the disk.
class StatCsvRecorder {
public:
    StatCsvRecorder();  // Defined in the .cpp, where Writer is a complete type.
    ~StatCsvRecorder(); // Waits for a file still being written.
    StatCsvRecorder(const StatCsvRecorder&) = delete;
    StatCsvRecorder& operator=(const StatCsvRecorder&) = delete;
    StatCsvRecorder(StatCsvRecorder&&) = delete;
    StatCsvRecorder& operator=(StatCsvRecorder&&) = delete;

    // Starts recording every stat registered right now for `seconds`.
    void start(const stats::StatRegistry& registry, std::filesystem::path file, f64 seconds);
    // Adds the frame just closed by StatRegistry::endFrame. Writes the file
    // once the duration is reached.
    void recordFrame(const stats::StatRegistry& registry, f64 frameSeconds);
    [[nodiscard]] bool isRecording() const noexcept { return m_recording; }
    // The last finished file and whether writing it worked, for the overlay.
    // A copy, because the writer thread sets it.
    [[nodiscard]] std::string lastResult() const;
    // Blocks until a file being written is on disk (tests, shutdown).
    void waitForWrite();

private:
    void finish();

    std::vector<stats::StatId> m_ids;
    std::string m_rows;
    std::filesystem::path m_file;
    f64 m_duration = 0.0;
    f64 m_elapsed = 0.0;
    u64 m_frame = 0;
    bool m_recording = false;
    struct Writer;
    std::unique_ptr<Writer> m_writer;
};

struct OverlayDesc {
    config::SettingsRegistry* settings = nullptr;
    input::InputSystem* input = nullptr;
    stats::StatRegistry* stats = nullptr;
    rhi::Device* device = nullptr; // Optional: the GPU page needs it.
    std::filesystem::path captureFolder = "captures";
};

// The F3 overlay: a compact Overview and full pages for frame times, the
// GPU, memory, input, changed settings and every registered stat (Section
// 18.2). Each page is an ImGui window, so it can be moved and docked.
//
// Everything it shows comes from the stat registry, the device, the input
// system and the settings; systems add their own pages with addPage().
// Its own settings live in Debug.Overlay, its keys in the always active
// "Debug" input context: F3 toggles (on release, unless F3 was held for a
// page switch), F3+Right and F3+Left switch pages.
//
// Per frame, in this order:
//   recordFrame(cpu, gpu)          frame times of the frame just finished
//   stats.endFrame(seconds)        the application closes the stat frame
//   update(seconds)                reads the keys, records CSV, draws
// update() draws with ImGui, so it runs between DebugUi::beginFrame and
// DebugUi::endFrame.
//
// Thread safety: main thread only.
class DebugOverlay {
public:
    explicit DebugOverlay(const OverlayDesc& desc);
    ~DebugOverlay();
    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;
    DebugOverlay(DebugOverlay&&) = delete;
    DebugOverlay& operator=(DebugOverlay&&) = delete;

    // Adds a page after the built in ones. `draw` fills the page's window.
    void addPage(std::string name, std::function<void()> draw);
    // Adds a line to the Overview, for example "Chunk: 12 3 -4" from the game.
    void addOverviewLine(std::function<std::string()> line);

    // Remembers raw events for the Input page.
    void processEvents(std::span<const platform::PlatformEvent> events);
    void recordFrame(f64 cpuMilliseconds, std::optional<f64> gpuMilliseconds);
    void update(f64 frameSeconds);

    [[nodiscard]] bool isVisible() const;
    void setVisible(bool visible);
    [[nodiscard]] usize currentPage() const;
    void showPage(usize index);
    [[nodiscard]] usize pageCount() const noexcept { return m_pages.size(); }
    [[nodiscard]] const std::string& pageName(usize index) const { return m_pages.at(index).name; }

    // Starts a CSV capture of every stat for Debug.Overlay.CsvSeconds.
    void startCsvCapture();
    [[nodiscard]] const StatCsvRecorder& csv() const noexcept { return m_csv; }
    [[nodiscard]] StatCsvRecorder& csv() noexcept { return m_csv; }

    [[nodiscard]] input::InputContextId inputContext() const noexcept { return m_context; }

private:
    struct Page {
        std::string name;
        std::function<void()> draw;
    };

    void handleKeys();
    void refreshOverview();
    void drawOverview();
    void drawFramePage();
    void drawGpuPage();
    void drawPassTimings();
    void drawMemoryPage();
    void drawInputPage();
    void drawSettingsPage();
    void drawStatsPage();
    void beginPageWindow(const char* title);

    config::SettingsRegistry& m_settings;
    input::InputSystem& m_input;
    stats::StatRegistry& m_stats;
    rhi::Device* m_device;
    std::filesystem::path m_captureFolder;

    config::Setting<bool> m_visible;
    config::Setting<i32> m_page;
    config::Setting<f32> m_opacity;
    config::Setting<f32> m_scale;
    config::Setting<i32> m_updateRate;
    config::Setting<i32> m_csvSeconds;
    config::Setting<f32> m_hitchFactor;

    input::InputContextId m_context;
    input::ActionId m_toggle;
    input::ActionId m_nextPage;
    input::ActionId m_previousPage;
    bool m_chordUsed = false; // F3 was held for a page switch, so its release does not toggle.

    stats::StatId m_cpuTime;
    stats::StatId m_gpuTime;
    stats::StatId m_allocations;
    stats::StatId m_drawTime;
    u64 m_lastAllocationCount = 0;
    // Per frame GPU figures, derived from the device's running totals so they
    // also show on the Stats page and in CSV captures.
    stats::StatId m_gpuDrawCalls;
    stats::StatId m_gpuStateChanges;
    stats::StatId m_gpuBufferBytes;
    stats::StatId m_gpuTextureBytes;
    rhi::DeviceStats m_previousDevice{};

    std::vector<Page> m_pages;
    std::vector<std::function<std::string()>> m_overviewLines;
    std::vector<std::string> m_overviewText; // Refreshed at Debug.Overlay.UpdateRate.
    f64 m_sinceRefresh = 1.0e9;

    struct Hitch {
        u64 frame = 0;
        f64 milliseconds = 0.0;
        f64 timesMedian = 0.0;
    };
    std::deque<Hitch> m_hitches;
    std::deque<std::string> m_recentEvents;
    StatCsvRecorder m_csv;
    std::array<char, 64> m_statFilter{};

    // Reused every frame, so drawing does not allocate once warmed up.
    std::vector<f64> m_cpuHistory;
    std::vector<f64> m_gpuHistory;
    std::vector<f64> m_scratch;
    FrameTimeSummary m_summary;
};

} // namespace vxe::debug
