#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/streaming.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace vxe;
using voxel::BlockStateId;
using voxel::ChunkStage;
using voxel::StreamingManager;

namespace {

constexpr BlockStateId kStone = 1;

// Stone below chunk layer 0, a stone floor with a pillar in layer 0 and air
// above. With `hold` set, jobs wait (watching for cancellation) until it is
// cleared.
class TestSource final : public voxel::ChunkSource {
public:
    std::atomic<u32> produced{0};
    std::atomic<u32> sawCancel{0};
    std::atomic<bool> hold{false};
    std::optional<i32> ground; // What surfaceLayer answers.
    std::mutex orderLock;
    std::vector<ChunkPos> order; // Chunks in the order they were produced.

    [[nodiscard]] std::optional<i32> surfaceLayer(i32 /*chunkX*/, i32 /*chunkZ*/) const override {
        return ground;
    }

    void produce(const voxel::ChunkRequest& request, voxel::ChunkProduct& out) override {
        while (hold.load()) {
            std::this_thread::yield();
        }
        if (request.isCancelled()) {
            ++sawCancel;
            return;
        }
        ++produced;
        const ChunkPos position = request.position();
        {
            const std::lock_guard lock(orderLock);
            order.push_back(position);
        }
        if (position.y < 0) {
            out.blocks.fill(kStone);
            out.light = voxel::ChunkLight(voxel::kDarkLight); // Underground arrives lit (as from a save).
        } else if (position.y > 0) {
            out.blocks.fill(voxel::kAirState);
        } else {
            std::array<BlockStateId, kChunkVolume> states{};
            for (u8 z = 0; z < 16; ++z) {
                for (u8 x = 0; x < 16; ++x) {
                    states.at(LocalPos{.x = x, .y = 0, .z = z}.index()) = kStone;
                }
            }
            for (u8 y = 0; y < 8; ++y) {
                states.at(LocalPos{.x = 8, .y = y, .z = 8}.index()) = kStone;
            }
            out.blocks.assign(states);
        }
        out.surfaceEstimate = 7;
    }
};

// Records what it holds. `budget` meshes are accepted per frame (negative:
// all), the rest are over budget.
class TestSink final : public voxel::ChunkMeshSink {
public:
    HashMap<ChunkPos, u32> uploads; // Accepted uploads per chunk, ever.
    HashSet<ChunkPos> held;         // Chunks with a mesh now.
    u32 removals = 0;
    i32 budget = -1;
    i32 capacity = -1; // Most chunks held; negative: no limit.
    i32 acceptedThisFrame = 0;
    i32 mostInOneFrame = 0;
    f32 scale = 1.0F;

    void newFrame() { acceptedThisFrame = 0; }

    [[nodiscard]] Result upload(const voxel::ChunkMesh& mesh) override {
        if (budget >= 0 && acceptedThisFrame >= static_cast<i32>(static_cast<f32>(budget) * scale)) {
            return Result::OverBudget;
        }
        if (capacity >= 0 && !held.contains(mesh.position) && std::cmp_greater_equal(held.size(), capacity)) {
            return Result::Refused;
        }
        ++acceptedThisFrame;
        mostInOneFrame = std::max(mostInOneFrame, acceptedThisFrame);
        ++uploads[mesh.position];
        held.insert(mesh.position);
        return Result::Accepted;
    }
    void remove(ChunkPos position) override {
        ++removals;
        held.erase(position);
    }
    void setBudgetScale(f32 value) override { scale = value; }
};

std::shared_ptr<voxel::BlockRegistryView> testBlocks() {
    voxel::BlockRegistryBuilder builder;
    voxel::BlockRenderInfo stone{.layer = voxel::RenderLayer::Opaque};
    stone.faceTextureLayer.fill(1);
    REQUIRE(builder.add(stone));
    return builder.build();
}

// Everything a streaming manager needs, in lifetime order rather than
// packed for size: the manager (last) waits for its jobs before anything
// they touch goes.
struct Rig { // NOLINT(clang-analyzer-optin.performance.Padding)
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    voxel::BlockRegistry blocks;
    voxel::VoxelWorld world;
    TestSource source;
    TestSink sink;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 1}};
    std::unique_ptr<StreamingManager> manager;
    voxel::StreamingView view{.position = DVec3(8.0, 8.0, 8.0)};

    explicit Rig(bool withSink = true, i32 minChunkY = -2, i32 maxChunkY = 2) {
        blocks.publish(testBlocks());
        manager = std::make_unique<StreamingManager>(voxel::StreamingDesc{.world = &world,
                                                                          .source = &source,
                                                                          .jobs = &jobs,
                                                                          .settings = &settings,
                                                                          .sink = withSink ? &sink : nullptr,
                                                                          .blocks = &blocks,
                                                                          .stats = &stats,
                                                                          .minChunkY = minChunkY,
                                                                          .maxChunkY = maxChunkY});
        // The manager registers its settings; the first update reads them.
        set("World.Streaming.RenderDistance", 2);
        set("World.Streaming.VerticalRenderDistance", 1);
        set("World.Streaming.UnloadHysteresisChunks", 1);
    }
    ~Rig() { manager.reset(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    void set(std::string_view id, const config::SettingValue& value) {
        REQUIRE(settings.set(id, value).hasValue());
    }

    void step() {
        sink.newFrame();
        manager->update(view);
    }

    // Updates until nothing is left to do (or 10 seconds passed).
    void settle() {
        const Stopwatch clock;
        do {
            step();
            std::this_thread::yield();
        } while (!manager->isSettled() && clock.elapsedMilliseconds() < 10000.0);
        REQUIRE(manager->isSettled());
    }

    void moveTo(ChunkPos chunk) {
        view.position = DVec3((chunk.x * 16) + 8.0, (chunk.y * 16) + 8.0, (chunk.z * 16) + 8.0);
    }

    // Per chunk mesh counts must match the manager's own bookkeeping.
    void checkConsistent() const {
        CHECK(manager->counts().loaded == world.chunkCount());
        CHECK(manager->counts().meshed == sink.held.size());
        for (const ChunkPos& position : sink.held) {
            CHECK(manager->hasMesh(position));
        }
    }
};

i64 horizontalDistance(ChunkPos a, ChunkPos b) {
    return std::max(std::abs(i64{a.x} - b.x), std::abs(i64{a.z} - b.z));
}

} // namespace

TEST_CASE("Streaming loads the range and meshes each chunk once its neighbors are there") {
    Rig rig;
    rig.settle();
    // Loaded: within 4 chunks sideways (render distance + 2, so every lit
    // chunk has its neighbors) and 3 up and down, all 5 world layers.
    CHECK(rig.manager->counts().loaded == 9U * 9U * 5U);
    // Meshed: within 2 sideways and 1 up and down.
    CHECK(rig.manager->counts().meshed == 5U * 5U * 3U);
    rig.checkConsistent();
    for (const auto& [position, count] : rig.sink.uploads) {
        CHECK(count == 1); // Never meshed again when a neighbor arrived.
        CHECK(horizontalDistance(position, {}) <= 2);
        CHECK(std::abs(position.y) <= 1);
    }
    CHECK(rig.manager->isAreaReady(2));
    CHECK(rig.manager->stageOf({.x = 3, .y = 0, .z = 0}) == ChunkStage::Ready);
    CHECK_FALSE(rig.manager->hasMesh({.x = 3, .y = 0, .z = 0}));
    CHECK_FALSE(rig.manager->stageOf({.x = 5, .y = 0, .z = 0}).has_value());
    // Each chunk was produced once, and the generator's estimate arrived.
    CHECK(rig.source.produced.load() == 9U * 9U * 5U);
    REQUIRE(rig.world.findColumn({.x = 0, .z = 0}) != nullptr);
    CHECK(rig.world.findColumn({.x = 0, .z = 0})->surfaceEstimate() == 7);
    // A source's light goes into the world, without one chunks start as open
    // sky, until they are lit: the outer ring (render distance + 2) is not.
    CHECK_FALSE(rig.manager->isLit({.x = 4, .y = 0, .z = 0}));
    CHECK(rig.world.findChunk({.x = 4, .y = -1, .z = 0})->light() == voxel::ChunkLight(voxel::kDarkLight));
    CHECK(rig.world.findChunk({.x = 4, .y = 0, .z = 0})->light() == voxel::ChunkLight());
}

TEST_CASE("Chunks at the top and bottom of the world are meshed without neighbors beyond it") {
    Rig rig(true, -1, 1);
    rig.set("World.Streaming.VerticalRenderDistance", 4);
    rig.settle();
    CHECK(rig.manager->counts().loaded == 9U * 9U * 3U);
    CHECK(rig.manager->counts().meshed == 5U * 5U * 3U);
    CHECK(rig.manager->hasMesh({.x = 0, .y = 1, .z = 0}));
    CHECK(rig.manager->hasMesh({.x = 0, .y = -1, .z = 0}));
}

TEST_CASE("Moving unloads past the hysteresis and loads ahead") {
    Rig rig;
    rig.settle();
    rig.moveTo({.x = 1, .y = 0, .z = 0});
    rig.settle();
    // x = -3 is 4 away now: still within render distance + 2 + hysteresis.
    CHECK(rig.manager->stageOf({.x = -3, .y = 0, .z = 0}).has_value());
    // x = -2 is 3 away: its mesh stays (render distance + hysteresis).
    CHECK(rig.manager->hasMesh({.x = -2, .y = 0, .z = 0}));
    CHECK(rig.manager->hasMesh({.x = 3, .y = 0, .z = 0}));
    rig.checkConsistent();

    rig.moveTo({.x = 4, .y = 0, .z = 0});
    rig.settle();
    CHECK_FALSE(rig.manager->stageOf({.x = -2, .y = 0, .z = 0}).has_value());
    CHECK(rig.world.findChunk({.x = -2, .y = 0, .z = 0}) == nullptr);
    CHECK_FALSE(rig.sink.held.contains({.x = -2, .y = 0, .z = 0}));
    CHECK(rig.sink.removals > 0);
    rig.checkConsistent();
    for (const auto& [position, count] : rig.sink.uploads) {
        CHECK(count == 1); // Moving never remeshed a chunk.
    }
    for (const ChunkPos& position : rig.sink.held) {
        CHECK(horizontalDistance(position, {.x = 4, .y = 0, .z = 0}) <= 3);
    }
}

TEST_CASE("Edits remesh the chunk and the neighbors whose border they touch") {
    Rig rig;
    rig.settle();
    // On the +X border of chunk (0, 0, 0): its neighbor (1, 0, 0) sees it.
    REQUIRE(rig.world.setBlock({.x = 15, .y = 10, .z = 8}, kStone));
    rig.settle();
    for (const auto& [position, count] : rig.sink.uploads) {
        const bool touched = position == ChunkPos{} || position == ChunkPos{.x = 1, .y = 0, .z = 0};
        CHECK(count == (touched ? 2U : 1U));
    }
    CHECK(rig.world.changedChunks().empty());
}

TEST_CASE("An edit is meshed right away, even while the world is still loading") {
    Rig rig;
    rig.settle();
    // Plenty left to load keeps the queues full, as on a long walk at a large
    // render distance.
    rig.set("World.Streaming.RenderDistance", 24);
    rig.set("World.Streaming.VerticalRenderDistance", 2);
    const auto play = [&](int frames) {
        // Real frames: streaming runs once per frame, with time between them.
        for (int i = 0; i < frames; ++i) {
            rig.step();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    play(60);
    REQUIRE_FALSE(rig.manager->isSettled());
    const auto meshes = [&] {
        const auto it = rig.sink.uploads.find(ChunkPos{});
        return it == rig.sink.uploads.end() ? 0U : it->second;
    };
    const auto before = meshes();
    const Stopwatch clock;
    REQUIRE(rig.world.setBlock({.x = 8, .y = 10, .z = 8}, kStone));
    while (meshes() == before && clock.elapsedMilliseconds() < 8000.0) {
        play(1);
    }
    const f64 waited = clock.elapsedMilliseconds();
    MESSAGE("The edit reached its mesh after " << waited << " ms, settled " << rig.manager->isSettled());
    CHECK(meshes() > before);
    if (test::timingIsReliable()) {
        CHECK(waited < 150.0);
    }
}

TEST_CASE("A mesh made from an outdated snapshot is thrown away") {
    Rig rig;
    rig.settle();
    REQUIRE(rig.world.setBlock({.x = 8, .y = 10, .z = 8}, kStone));
    // The edit lights the chunk again first, then snapshots it for a mesh job.
    for (i32 i = 0; i < 10000 && rig.manager->stageOf({}) != ChunkStage::Meshing; ++i) {
        rig.step();
    }
    REQUIRE(rig.manager->stageOf({}) == ChunkStage::Meshing);
    REQUIRE(rig.world.setBlock({.x = 8, .y = 11, .z = 8}, kStone));
    rig.settle();
    // The first mesh came back older than the chunk and never went out.
    CHECK(rig.sink.uploads.at(ChunkPos{}) == 2);
}

TEST_CASE("Chunks leaving the range cancel their load jobs") {
    Rig rig;
    rig.source.hold = true;
    rig.step(); // Starts load jobs, which wait.
    REQUIRE(rig.manager->counts().at(ChunkStage::Loading) > 0);
    rig.moveTo({.x = 100, .y = 0, .z = 0});
    rig.step(); // The old chunks are out of range: their jobs are cancelled.
    rig.source.hold = false;
    rig.settle();
    CHECK(rig.source.sawCancel.load() > 0);
    rig.world.forEachChunk(
        [](const voxel::Chunk& chunk) { CHECK(horizontalDistance(chunk.position(), {.x = 100}) <= 4); });
    CHECK(rig.manager->counts().loaded == 9U * 9U * 5U);
    rig.checkConsistent();
}

TEST_CASE("Uploads over the sink's budget wait for the next frame") {
    Rig rig;
    // Nothing goes out until at least 3 meshes wait, so the budget is what
    // limits the next frame, not how fast the mesh jobs happen to finish. The
    // main thread budget stays out of the way: a slow frame (sanitizers, a
    // busy machine) would otherwise stop uploads before the sink's budget does.
    REQUIRE(rig.settings.set("World.Streaming.MainThreadBudgetMs", config::SettingValue{8.0F}).hasValue());
    rig.sink.budget = 0;
    const Stopwatch clock;
    while (rig.manager->counts().at(ChunkStage::Uploading) < 3 && clock.elapsedMilliseconds() < 10000.0) {
        rig.step();
        std::this_thread::yield();
    }
    REQUIRE(rig.manager->counts().at(ChunkStage::Uploading) >= 3);
    CHECK(rig.sink.mostInOneFrame == 0);
    rig.sink.budget = 3;
    rig.settle();
    CHECK(rig.sink.mostInOneFrame == 3);
    CHECK(rig.manager->counts().meshed == 5U * 5U * 3U);
    rig.checkConsistent();
}

TEST_CASE("Out of renderer memory, the nearest chunks keep their meshes and edits still show") {
    Rig rig;
    rig.set("World.Streaming.RenderDistance", 6);
    // Room for a radius of 4 (9 x 9 x 3 layers) and a few more, not for 6.
    rig.sink.capacity = 300;
    rig.settle();
    const i32 reach = rig.manager->meshReach();
    MESSAGE("Meshes reach " << reach << " of 6 chunks, " << rig.sink.held.size() << " held");
    CHECK(reach < 6);
    CHECK(reach >= 4);
    CHECK(std::cmp_less_equal(rig.sink.held.size(), rig.sink.capacity));
    rig.checkConsistent();
    // Nothing near is missing, and nothing beyond the reach took its place.
    for (i32 z = -reach; z <= reach; ++z) {
        for (i32 x = -reach; x <= reach; ++x) {
            for (i32 y = -1; y <= 1; ++y) {
                CHECK(rig.sink.held.contains({.x = x, .y = y, .z = z}));
            }
        }
    }
    // An edit replaces its chunk's mesh even with memory full.
    const ChunkPos edited{.x = 1, .y = 0, .z = 1};
    const u32 before = rig.sink.uploads[edited];
    REQUIRE(rig.world.setBlock({.x = 20, .y = 10, .z = 20}, kStone));
    rig.settle();
    CHECK(rig.sink.uploads[edited] > before);
    // A new render distance tries again.
    rig.sink.capacity = -1;
    rig.set("World.Streaming.RenderDistance", 5);
    rig.settle();
    CHECK(rig.manager->meshReach() == 5);
    rig.checkConsistent();
}

TEST_CASE("The ground at the edge of the view loads before sky and rock near the camera") {
    Rig rig(true, -24, 24);
    rig.source.ground = 0;
    rig.set("World.Streaming.RenderDistance", 4);
    rig.set("World.Streaming.VerticalRenderDistance", 16);
    rig.moveTo({.x = 0, .y = 15, .z = 0}); // High above the ground.
    rig.settle();
    // Jobs finish out of order, so a few ground chunks may land after the first
    // sky chunk, but nearly all of them must come before it.
    usize ground = 0;
    usize groundBeforeSky = 0;
    bool skySeen = false;
    const usize total = rig.source.order.size();
    for (const ChunkPos& at : rig.source.order) {
        skySeen = skySeen || (at.y >= 5 && at.y <= 8); // Beside neither the ground nor the camera.
        if (std::abs(at.y) <= 4) {
            ++ground;
            groundBeforeSky += skySeen ? 0U : 1U;
        }
    }
    MESSAGE(groundBeforeSky << " of " << ground << " ground chunks came before the first sky chunk");
    CHECK(groundBeforeSky * 10 > ground * 9);
    // And in the end nothing is missing.
    CHECK(rig.manager->counts().loaded == total);
}

TEST_CASE("Jobs in flight stay within their limits, and burst mode raises them") {
    Rig rig;
    rig.set("World.Streaming.MaxLoadJobsInFlight", 2);
    rig.set("World.Streaming.MaxMeshJobsInFlight", 1);
    rig.set("World.Streaming.BurstSeconds", 30.0F);
    usize mostLoading = 0;
    usize mostMeshing = 0;
    const auto track = [&] {
        const voxel::StreamingCounts& counts = rig.manager->counts();
        mostLoading = std::max(mostLoading, counts.at(ChunkStage::Loading));
        mostMeshing =
            std::max(mostMeshing, counts.at(ChunkStage::Meshing) + counts.at(ChunkStage::Uploading));
    };
    for (i32 i = 0; i < 20 && !rig.manager->isSettled(); ++i) {
        rig.step();
        track();
    }
    CHECK(mostLoading <= 2);
    CHECK(mostMeshing <= 1);

    rig.moveTo({.x = 20});
    rig.manager->startBurst();
    CHECK(rig.manager->isBursting());
    CHECK(rig.sink.scale == doctest::Approx(4.0F));
    while (rig.manager->isBursting()) {
        rig.step();
        track();
    }
    // Ended because everything was done, long before 30 seconds.
    CHECK(rig.manager->isSettled());
    CHECK(rig.sink.scale == doctest::Approx(1.0F));
    CHECK(mostLoading > 2);
    CHECK(mostLoading <= 8);
    CHECK(mostMeshing <= 4);
}

TEST_CASE("Changing a mesh setting remeshes every meshed chunk") {
    Rig rig;
    rig.settle();
    REQUIRE(rig.settings.set("Graphics.Terrain.GreedyMerge", config::SettingValue{false}).hasValue());
    rig.settle();
    for (const auto& [position, count] : rig.sink.uploads) {
        CHECK(count == 2);
    }
    rig.checkConsistent();
}

TEST_CASE("A smaller render distance unloads, a larger one loads more") {
    Rig rig;
    rig.set("World.Streaming.RenderDistance", 3);
    rig.settle();
    CHECK(rig.manager->counts().loaded == 11U * 11U * 5U);
    rig.set("World.Streaming.RenderDistance", 2);
    rig.settle();
    // Kept within 2 + 2 + hysteresis 1 = 5: nothing unloads yet; meshes
    // stay within 2 + 1.
    CHECK(rig.manager->counts().loaded == 11U * 11U * 5U);
    for (const ChunkPos& position : rig.sink.held) {
        CHECK(horizontalDistance(position, {}) <= 3);
    }
    rig.set("World.Streaming.UnloadHysteresisChunks", 0);
    rig.settle();
    CHECK(rig.manager->counts().loaded == 9U * 9U * 5U);
    CHECK(rig.manager->counts().meshed == 5U * 5U * 3U);
    rig.set("World.Streaming.RenderDistance", 4);
    rig.settle();
    CHECK(rig.manager->counts().loaded == 13U * 13U * 5U);
    CHECK(rig.manager->counts().meshed == 9U * 9U * 3U);
    rig.checkConsistent();
}

TEST_CASE("Without a sink chunks load but are never meshed") {
    Rig rig(false);
    rig.settle();
    CHECK(rig.manager->counts().loaded == 9U * 9U * 5U);
    CHECK(rig.manager->counts().meshed == 0);
    CHECK(rig.manager->isAreaReady(2));
    CHECK(rig.sink.uploads.empty());
}

TEST_CASE("Unloading everything takes the meshes back and empties the world") {
    Rig rig;
    rig.settle();
    rig.manager->unloadAll();
    CHECK(rig.world.chunkCount() == 0);
    CHECK(rig.sink.held.empty());
    CHECK(rig.manager->counts().loaded == 0);
    CHECK(rig.manager->counts().meshed == 0);
    rig.settle(); // And it streams in again.
    CHECK(rig.manager->counts().meshed == 5U * 5U * 3U);
}

TEST_CASE("Stats show the pipeline") {
    Rig rig;
    rig.settle();
    rig.stats.endFrame(1.0 / 60.0);
    CHECK(rig.stats.value(rig.stats.find("Voxel.Streaming.Loaded")) == doctest::Approx(9.0 * 9.0 * 5.0));
    CHECK(rig.stats.value(rig.stats.find("Voxel.Streaming.Meshed")) == doctest::Approx(5.0 * 5.0 * 3.0));
    CHECK(rig.stats.histogram(rig.stats.find("Voxel.Streaming.Latency")).samples == 5U * 5U * 3U);
    CHECK(rig.stats.find("Voxel.Streaming.UpdateTime").isValid());
    CHECK(rig.manager->summary().find("75 meshed") != std::string::npos);
}

TEST_CASE("Fast flight keeps every update near the main thread budget") {
    Rig rig(true, -4, 4);
    rig.set("World.Streaming.RenderDistance", 8);
    rig.set("World.Streaming.VerticalRenderDistance", 4);
    rig.settle();
    rig.manager->resetUpdateTimes();
    // Two chunks per update: 32 blocks a frame, far faster than streaming
    // can follow, so every update has more work than its budget allows.
    for (i32 frame = 0; frame < 400; ++frame) {
        rig.moveTo({.x = frame * 2, .y = 0, .z = frame});
        rig.step();
    }
    const StreamingManager::UpdateTimes& after = rig.manager->updateTimes();
    CHECK(after.updates == 400);
    MESSAGE("Fast flight: max update ", after.maxMilliseconds, " ms, ", after.overOneMillisecond,
            " updates over 1 ms");
#if defined(NDEBUG) && !defined(VXE_SANITIZER_ACTIVE)
    if (test::timingIsReliable()) {
        CHECK(after.maxMilliseconds < 1.0);
    }
#endif
    rig.checkConsistent();
}
