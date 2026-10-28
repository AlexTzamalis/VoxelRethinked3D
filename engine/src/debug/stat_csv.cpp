#include "vxe/debug/overlay.hpp"

#include <vxe/io/file_io.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <mutex>
#include <numeric>
#include <thread>

namespace vxe::debug {

FrameTimeSummary summarizeFrameTimes(std::span<const f64> milliseconds, std::vector<f64>& scratch) {
    FrameTimeSummary summary;
    if (milliseconds.empty()) {
        return summary;
    }
    scratch.assign(milliseconds.begin(), milliseconds.end());
    std::ranges::sort(scratch);
    const usize count = scratch.size();
    const auto percentile = [&scratch, count](f64 fraction) {
        const auto rank = static_cast<usize>(std::ceil(fraction * static_cast<f64>(count)));
        return scratch[std::clamp<usize>(rank, 1, count) - 1];
    };
    // The slowest `fraction` of frames, as a frame rate.
    const auto lowFps = [&scratch, count](f64 fraction) {
        const usize worst =
            std::max<usize>(1, static_cast<usize>(std::ceil(fraction * static_cast<f64>(count))));
        const f64 sum =
            std::accumulate(scratch.end() - static_cast<std::ptrdiff_t>(worst), scratch.end(), 0.0);
        const f64 average = sum / static_cast<f64>(worst);
        return average > 0.0 ? 1000.0 / average : 0.0;
    };
    summary.averageMs = std::accumulate(scratch.begin(), scratch.end(), 0.0) / static_cast<f64>(count);
    summary.averageFps = summary.averageMs > 0.0 ? 1000.0 / summary.averageMs : 0.0;
    summary.p50Ms = percentile(0.50);
    summary.p90Ms = percentile(0.90);
    summary.p99Ms = percentile(0.99);
    summary.p999Ms = percentile(0.999);
    summary.low1Fps = lowFps(0.01);
    summary.low01Fps = lowFps(0.001);
    return summary;
}

std::vector<const config::SettingInfo*> changedSettings(const config::SettingsRegistry& settings) {
    std::vector<const config::SettingInfo*> changed;
    for (const config::SettingInfo* info : settings.all()) {
        if (!settings.isDefault(info->id)) {
            changed.push_back(info);
        }
    }
    return changed;
}

// The background write of a finished capture. The thread owns the rows; the
// result text is shared with the main thread under a mutex.
struct StatCsvRecorder::Writer {
    mutable std::mutex mutex;
    std::string result;
    std::jthread thread;
};

StatCsvRecorder::StatCsvRecorder() = default;

StatCsvRecorder::~StatCsvRecorder() {
    waitForWrite();
}

void StatCsvRecorder::waitForWrite() {
    if (m_writer && m_writer->thread.joinable()) {
        m_writer->thread.join();
    }
}

std::string StatCsvRecorder::lastResult() const {
    if (!m_writer) {
        return {};
    }
    const std::scoped_lock lock(m_writer->mutex);
    return m_writer->result;
}

void StatCsvRecorder::start(const stats::StatRegistry& registry, std::filesystem::path file, f64 seconds) {
    waitForWrite(); // One capture at a time.
    m_ids.clear();
    m_rows.clear();
    m_rows += "frame,seconds";
    for (u32 i = 0; i < registry.statCount(); ++i) {
        m_ids.push_back(stats::StatId{i});
        // Stat names are dotted words, so they never need CSV quoting.
        m_rows += ',';
        m_rows += registry.info(stats::StatId{i}).name;
    }
    m_rows += '\n';
    m_file = std::move(file);
    m_duration = seconds;
    m_elapsed = 0.0;
    m_frame = 0;
    m_recording = true;
}

void StatCsvRecorder::recordFrame(const stats::StatRegistry& registry, f64 frameSeconds) {
    if (!m_recording) {
        return;
    }
    m_elapsed += frameSeconds;
    std::format_to(std::back_inserter(m_rows), "{},{:.6f}", m_frame++, m_elapsed);
    for (const stats::StatId id : m_ids) {
        std::format_to(std::back_inserter(m_rows), ",{}", registry.value(id));
    }
    m_rows += '\n';
    if (m_elapsed >= m_duration) {
        finish();
    }
}

void StatCsvRecorder::finish() {
    m_recording = false;
    if (!m_writer) {
        m_writer = std::make_unique<Writer>();
    }
    m_writer->thread = std::jthread([writer = m_writer.get(), file = m_file, rows = std::move(m_rows)] {
        const auto written = io::writeFile(file, std::as_bytes(std::span(rows)));
        const std::scoped_lock lock(writer->mutex);
        writer->result =
            written ? std::format("Saved {}", file.generic_string())
                    : std::format("Could not save {}: {}", file.generic_string(), written.error().describe());
    });
    m_rows = {};
}

} // namespace vxe::debug
