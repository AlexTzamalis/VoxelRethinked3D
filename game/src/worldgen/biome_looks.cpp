#include "biome_looks.hpp"

#include <vxe/core/error.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>

// The recipe tables stay readable as short lists, and clang needs the empty
// initializers to accept partly filled recipes.
// NOLINTBEGIN(modernize-use-designated-initializers, readability-redundant-member-init)

namespace vr {
namespace {

using std::string_view;
using vxe::u16;
using vxe::voxel::BlockStateId;

struct Weighted {
    string_view name;
    u16 weight = 1;
};

// One biome, by block names. G3.7 moves these into data files.
struct Recipe {
    Biome biome = Biome::Plains;
    string_view top = "grass_block";
    string_view under = "dirt";
    string_view floor = "sand";
    string_view deep{};
    string_view patch{};
    string_view patch2{};
    float patchShare = 0.0F;
    float patch2Share = 0.0F;
    vxe::u8 underDepth = 3;
    vxe::u8 deepDepth = 0;
    bool banded = false;
    bool dry = false;
    TreeKind waterTree = TreeKind::Oak;
    float trees = 0.0F;
    float groves = 0.0F;
    std::array<TreeChoice, 6> treeKinds{};
    float grass = 0.0F;
    std::array<Weighted, 5> grassKinds{};
    float flowers = 0.0F;
    std::array<Weighted, 7> flowerKinds{};
    string_view boulder{};
    string_view boulderMix{};
    float boulders = 0.0F;
    float fallenLogs = 0.0F;
    TreeKind logKind = TreeKind::Oak;
    vxe::u8 snowDepth = 0;
};

constexpr std::array<Weighted, 5> kMeadowGrass = {{{"short_grass", 8}, {"tall_grass", 3}, {"fern", 1}}};
constexpr std::array<Weighted, 5> kForestGrass = {
    {{"short_grass", 6}, {"fern", 3}, {"tall_grass", 2}, {"large_fern", 1}}};
constexpr std::array<Weighted, 5> kColdGrass = {
    {{"fern", 5}, {"short_grass", 3}, {"large_fern", 2}, {"sweet_berry_bush", 1}}};
constexpr std::array<Weighted, 5> kDryGrass = {{{"tall_dry_grass", 5}, {"short_grass", 2}}};
constexpr std::array<Weighted, 7> kPlainsFlowers = {
    {{"poppy", 4}, {"azure_bluet", 3}, {"cornflower", 2}, {"white_tulip", 1}, {"red_tulip", 1}}};
constexpr std::array<Weighted, 7> kAllFlowers = {{{"allium", 3},
                                                  {"pink_tulip", 3},
                                                  {"lily_of_the_valley", 3},
                                                  {"peony", 2},
                                                  {"lilac", 2},
                                                  {"rose_bush", 2},
                                                  {"torchflower", 1}}};
constexpr std::array<Weighted, 7> kMeadowFlowers = {
    {{"cornflower", 4}, {"allium", 3}, {"azure_bluet", 3}, {"poppy", 2}, {"lilac", 1}, {"peony", 1}}};

constexpr std::array<Weighted, 7> kWaterside = {{{"tall_grass", 6},
                                                 {"fern", 6},
                                                 {"short_grass", 5},
                                                 {"large_fern", 3},
                                                 {"blue_orchid", 1},
                                                 {"lily_of_the_valley", 1},
                                                 {"firefly_bush", 1}}};

constexpr std::array<Recipe, kBiomeCount> kRecipes = {{
    {.biome = Biome::DeepOcean,
     .top = "sand",
     .under = "sand",
     .floor = "gravel",
     .patch = "clay",
     .patchShare = 0.1F},
    {.biome = Biome::Ocean,
     .top = "sand",
     .under = "sand",
     .floor = "sand",
     .patch = "gravel",
     .patchShare = 0.25F},
    {.biome = Biome::WarmOcean,
     .top = "sand",
     .under = "sand",
     .floor = "sand",
     .patch = "clay",
     .patchShare = 0.08F},
    {.biome = Biome::FrozenOcean, .top = "snow_block", .under = "gravel", .floor = "gravel", .snowDepth = 1},
    {.biome = Biome::Beach,
     .top = "sand",
     .under = "sand",
     .deep = "sandstone",
     .patch = "gravel",
     .patchShare = 0.05F,
     .underDepth = 4,
     .deepDepth = 9,
     .waterTree = TreeKind::Palm,
     .trees = 0.05F,
     .groves = 0.8F,
     .treeKinds = {{{TreeKind::Palm, 1}}},
     .grass = 0.018F,
     .grassKinds = kDryGrass,
     .boulder = "sandstone",
     .boulderMix = "cobblestone",
     .boulders = 0.05F,
     .fallenLogs = 0.12F,
     .logKind = TreeKind::Oak},
    {.biome = Biome::SnowyBeach,
     .top = "snow_block",
     .under = "sand",
     .floor = "gravel",
     .patch = "gravel",
     .patchShare = 0.18F,
     .boulder = "stone",
     .boulderMix = "cobblestone",
     .boulders = 0.12F,
     .fallenLogs = 0.08F,
     .logKind = TreeKind::Spruce,
     .snowDepth = 1},
    {.biome = Biome::StonyShore,
     .top = "stone",
     .under = "stone",
     .floor = "gravel",
     .patch = "gravel",
     .patch2 = "cobblestone",
     .patchShare = 0.3F,
     .patch2Share = 0.15F,
     .boulder = "stone",
     .boulderMix = "mossy_cobblestone",
     .boulders = 0.4F},
    {.biome = Biome::Swamp,
     .floor = "mud",
     .deep = "clay",
     .patch = "mud",
     .patch2 = "muddy_mangrove_roots",
     .patchShare = 0.4F,
     .patch2Share = 0.08F,
     .underDepth = 2,
     .deepDepth = 6,
     .trees = 0.14F,
     .groves = 0.5F,
     .treeKinds = {{{TreeKind::DarkOak, 2}, {TreeKind::Oak, 2}, {TreeKind::Bush, 3}}},
     .grass = 0.270F,
     .grassKinds = {{{"fern", 4}, {"tall_grass", 3}, {"firefly_bush", 1}, {"large_fern", 2}}},
     .flowers = 0.044F,
     .flowerKinds = {{{"blue_orchid", 5}, {"allium", 1}}},
     .fallenLogs = 0.2F,
     .logKind = TreeKind::DarkOak},
    {.biome = Biome::SnowyPeaks,
     .top = "snow_block",
     .under = "snow_block",
     .floor = "gravel",
     .patch = "calcite",
     .patch2 = "packed_ice",
     .patchShare = 0.04F,
     .patch2Share = 0.07F,
     .underDepth = 2,
     .boulder = "calcite",
     .boulderMix = "stone",
     .boulders = 0.06F,
     .snowDepth = 4},
    {.biome = Biome::RockyPeaks,
     .top = "stone",
     .under = "stone",
     .floor = "gravel",
     .patch = "gravel",
     .patch2 = "grass_block",
     .patchShare = 0.14F,
     .patch2Share = 0.16F,
     .trees = 0.04F,
     .groves = 0.8F,
     .treeKinds = {{{TreeKind::Pine, 3}, {TreeKind::Spruce, 1}, {TreeKind::Bush, 1}}},
     .grass = 0.048F,
     .grassKinds = kColdGrass,
     .boulder = "andesite",
     .boulderMix = "tuff",
     .boulders = 0.45F},
    {.biome = Biome::WoodedSlopes,
     .patch = "podzol",
     .patch2 = "coarse_dirt",
     .patchShare = 0.3F,
     .patch2Share = 0.12F,
     .trees = 0.425F,
     .groves = 0.5F,
     .treeKinds =
         {{{TreeKind::TallSpruce, 5}, {TreeKind::Spruce, 5}, {TreeKind::Pine, 3}, {TreeKind::Birch, 1}}},
     .grass = 0.180F,
     .grassKinds = kColdGrass,
     .boulder = "stone",
     .boulderMix = "mossy_cobblestone",
     .boulders = 0.25F,
     .fallenLogs = 0.2F,
     .logKind = TreeKind::Spruce},
    {.biome = Biome::Meadow,
     .patch = "coarse_dirt",
     .patchShare = 0.03F,
     .trees = 0.05F,
     .groves = 0.9F,
     .treeKinds = {{{TreeKind::Spruce, 3}, {TreeKind::Birch, 2}, {TreeKind::Poplar, 2}, {TreeKind::Bush, 2}}},
     .grass = 0.372F,
     .grassKinds = kMeadowGrass,
     .flowers = 0.165F,
     .flowerKinds = kMeadowFlowers,
     .boulder = "stone",
     .boulderMix = "andesite",
     .boulders = 0.22F},
    {.biome = Biome::SnowyPlains,
     .top = "snowy_grass",
     .floor = "gravel",
     .patch = "snow_block",
     .patchShare = 0.3F,
     .trees = 0.02F,
     .groves = 0.9F,
     .treeKinds = {{{TreeKind::Spruce, 3}, {TreeKind::Pine, 1}}},
     .boulder = "stone",
     .boulderMix = "gravel",
     .boulders = 0.08F,
     .snowDepth = 2},
    {.biome = Biome::SnowyTaiga,
     .top = "snowy_grass",
     .floor = "gravel",
     .patch = "snow_block",
     .patch2 = "podzol",
     .patchShare = 0.2F,
     .patch2Share = 0.1F,
     .trees = 0.425F,
     .groves = 0.4F,
     .treeKinds = {{{TreeKind::Spruce, 7}, {TreeKind::Pine, 3}}},
     .grass = 0.060F,
     .grassKinds = {{{"sweet_berry_bush", 2}, {"fern", 3}}},
     .boulder = "cobblestone",
     .boulderMix = "mossy_cobblestone",
     .boulders = 0.12F,
     .fallenLogs = 0.15F,
     .logKind = TreeKind::Spruce,
     .snowDepth = 3},
    {.biome = Biome::Taiga,
     .floor = "gravel",
     .patch = "podzol",
     .patch2 = "coarse_dirt",
     .patchShare = 0.32F,
     .patch2Share = 0.12F,
     .trees = 0.468F,
     .groves = 0.4F,
     .treeKinds = {{{TreeKind::Spruce, 7}, {TreeKind::Pine, 4}, {TreeKind::Hemlock, 3}, {TreeKind::Bush, 1}}},
     .grass = 0.252F,
     .grassKinds = kColdGrass,
     .boulder = "mossy_cobblestone",
     .boulderMix = "cobblestone",
     .boulders = 0.18F,
     .fallenLogs = 0.3F,
     .logKind = TreeKind::Spruce},
    {.biome = Biome::Shrubland,
     .patch = "coarse_dirt",
     .patch2 = "rooted_dirt",
     .patchShare = 0.3F,
     .patch2Share = 0.06F,
     .dry = true,
     .waterTree = TreeKind::Olive,
     .trees = 0.08F,
     .groves = 0.7F,
     .treeKinds = {{{TreeKind::Bush, 6}, {TreeKind::Olive, 3}, {TreeKind::Oak, 1}}},
     .grass = 0.204F,
     .grassKinds = kDryGrass,
     .boulder = "granite",
     .boulderMix = "stone",
     .boulders = 0.14F},
    {.biome = Biome::Plains,
     .floor = "dirt",
     .patch = "coarse_dirt",
     .patchShare = 0.01F,
     .trees = 0.022F,
     .groves = 0.95F,
     .treeKinds = {{{TreeKind::Oak, 5},
                    {TreeKind::BigOak, 2},
                    {TreeKind::Poplar, 3},
                    {TreeKind::Birch, 1},
                    {TreeKind::Bush, 2}}},
     .grass = 0.420F,
     .grassKinds = {{{"short_grass", 12}, {"tall_grass", 3}}},
     .flowers = 0.055F,
     .flowerKinds = kPlainsFlowers,
     .boulder = "stone",
     .boulderMix = "cobblestone",
     .boulders = 0.03F},
    {.biome = Biome::FlowerPlains,
     .floor = "dirt",
     .trees = 0.015F,
     .groves = 0.95F,
     .treeKinds = {{{TreeKind::Oak, 3}, {TreeKind::Azalea, 2}, {TreeKind::Poplar, 2}, {TreeKind::Bush, 1}}},
     .grass = 0.360F,
     .grassKinds = kMeadowGrass,
     .flowers = 0.303F,
     .flowerKinds = kAllFlowers},
    {.biome = Biome::Forest,
     .patch = "rooted_dirt",
     .patchShare = 0.05F,
     .trees = 0.527F,
     .groves = 0.3F,
     .treeKinds = {{{TreeKind::Oak, 6}, {TreeKind::BigOak, 3}, {TreeKind::Birch, 3}, {TreeKind::Bush, 2}}},
     .grass = 0.300F,
     .grassKinds = kForestGrass,
     .flowers = 0.044F,
     .flowerKinds = {{{"poppy", 2}, {"lily_of_the_valley", 2}, {"azure_bluet", 1}}},
     .boulder = "mossy_cobblestone",
     .boulderMix = "cobblestone",
     .boulders = 0.1F,
     .fallenLogs = 0.3F,
     .logKind = TreeKind::Oak},
    {.biome = Biome::BirchForest,
     .patch = "coarse_dirt",
     .patchShare = 0.03F,
     .trees = 0.510F,
     .groves = 0.25F,
     .treeKinds = {{{TreeKind::Birch, 6},
                    {TreeKind::TallBirch, 5},
                    {TreeKind::ForkedBirch, 4},
                    {TreeKind::WeepingBirch, 3},
                    {TreeKind::Oak, 1}}},
     .grass = 0.330F,
     .grassKinds = kMeadowGrass,
     .flowers = 0.055F,
     .flowerKinds = {{{"allium", 2}, {"lily_of_the_valley", 3}, {"pink_tulip", 1}}},
     .fallenLogs = 0.3F,
     .logKind = TreeKind::Birch},
    {.biome = Biome::FlowerForest,
     .trees = 0.357F,
     .groves = 0.5F,
     .treeKinds = {{{TreeKind::Azalea, 5},
                    {TreeKind::Oak, 3},
                    {TreeKind::Birch, 3},
                    {TreeKind::WeepingBirch, 2},
                    {TreeKind::BigOak, 1}}},
     .grass = 0.300F,
     .grassKinds = kMeadowGrass,
     .flowers = 0.248F,
     .flowerKinds = kAllFlowers,
     .boulder = "mossy_cobblestone",
     .boulderMix = "cobblestone",
     .boulders = 0.05F,
     .fallenLogs = 0.12F,
     .logKind = TreeKind::Birch},
    {.biome = Biome::DarkForest,
     .patch = "podzol",
     .patch2 = "rooted_dirt",
     .patchShare = 0.25F,
     .patch2Share = 0.1F,
     .trees = 0.697F,
     .groves = 0.15F,
     .treeKinds = {{{TreeKind::DarkOak, 7}, {TreeKind::BigOak, 2}, {TreeKind::Bush, 2}}},
     .grass = 0.210F,
     .grassKinds = {{{"fern", 5}, {"large_fern", 2}, {"firefly_bush", 1}, {"short_grass", 2}}},
     .flowers = 0.022F,
     .flowerKinds = {{{"lily_of_the_valley", 3}, {"poppy", 1}}},
     .boulder = "mossy_stone_bricks",
     .boulderMix = "mossy_cobblestone",
     .boulders = 0.06F,
     .fallenLogs = 0.4F,
     .logKind = TreeKind::DarkOak},
    {.biome = Biome::PaleForest,
     .patch = "coarse_dirt",
     .patch2 = "rooted_dirt",
     .patchShare = 0.2F,
     .patch2Share = 0.12F,
     .trees = 0.595F,
     .groves = 0.2F,
     .treeKinds = {{{TreeKind::PaleOak, 8}, {TreeKind::Bush, 1}}},
     .grass = 0.144F,
     .grassKinds = {{{"short_grass", 3}, {"fern", 2}, {"firefly_bush", 1}}},
     .flowers = 0.017F,
     .flowerKinds = {{{"white_tulip", 2}, {"lily_of_the_valley", 2}}},
     .boulder = "tuff",
     .boulderMix = "stone",
     .boulders = 0.08F,
     .fallenLogs = 0.35F,
     .logKind = TreeKind::PaleOak},
    {.biome = Biome::Savanna,
     .patch = "coarse_dirt",
     .patchShare = 0.16F,
     .dry = true,
     .waterTree = TreeKind::Palm,
     .trees = 0.09F,
     .groves = 0.6F,
     .treeKinds = {{{TreeKind::Acacia, 8}, {TreeKind::Olive, 2}, {TreeKind::Bush, 1}}},
     .grass = 0.372F,
     .grassKinds = {{{"tall_grass", 5}, {"tall_dry_grass", 5}, {"short_grass", 6}}},
     .flowers = 0.011F,
     .flowerKinds = {{{"allium", 1}, {"poppy", 1}}},
     .boulder = "granite",
     .boulderMix = "red_sandstone",
     .boulders = 0.1F},
    {.biome = Biome::Desert,
     .top = "sand",
     .under = "sand",
     .deep = "sandstone",
     .patch = "red_sand",
     .patchShare = 0.03F,
     .underDepth = 4,
     .deepDepth = 22,
     .dry = true,
     .waterTree = TreeKind::Palm,
     .grass = 0.009F,
     .grassKinds = kDryGrass,
     .boulder = "sandstone",
     .boulderMix = "red_sandstone",
     .boulders = 0.1F},
    {.biome = Biome::Badlands,
     .top = "red_sand",
     .under = "red_sandstone",
     .patch = "coarse_dirt",
     .patchShare = 0.05F,
     .underDepth = 2,
     .banded = true,
     .dry = true,
     .waterTree = TreeKind::Olive,
     .grass = 0.012F,
     .grassKinds = kDryGrass,
     .boulder = "red_sandstone",
     .boulderMix = "packed_mud",
     .boulders = 0.14F},
    {.biome = Biome::DeepSpruceForest,
     .floor = "gravel",
     .patch = "podzol",
     .patch2 = "coarse_dirt",
     .patchShare = 0.42F,
     .patch2Share = 0.1F,
     .trees = 0.74F,
     .groves = 0.12F,
     .treeKinds =
         {{{TreeKind::TallSpruce, 8}, {TreeKind::Hemlock, 5}, {TreeKind::Spruce, 4}, {TreeKind::Pine, 2}}},
     .grass = 0.2F,
     .grassKinds = kColdGrass,
     .boulder = "mossy_cobblestone",
     .boulderMix = "cobblestone",
     .boulders = 0.14F,
     .fallenLogs = 0.4F,
     .logKind = TreeKind::Spruce},
    {.biome = Biome::Jungle,
     .floor = "mud",
     .patch = "podzol",
     .patch2 = "mud",
     .patchShare = 0.18F,
     .patch2Share = 0.06F,
     .trees = 0.680F,
     .groves = 0.2F,
     .treeKinds =
         {{{TreeKind::GiantJungle, 3}, {TreeKind::Jungle, 6}, {TreeKind::Palm, 2}, {TreeKind::Bush, 3}}},
     .grass = 0.432F,
     .grassKinds = {{{"fern", 6}, {"large_fern", 4}, {"tall_grass", 4}, {"short_grass", 2}}},
     .flowers = 0.028F,
     .flowerKinds = {{{"blue_orchid", 2}, {"torchflower", 1}, {"rose_bush", 1}}},
     .boulder = "mossy_cobblestone",
     .boulderMix = "mossy_stone_bricks",
     .boulders = 0.08F,
     .fallenLogs = 0.3F,
     .logKind = TreeKind::Jungle},
    {.biome = Biome::MapleForest,
     .top = "autumn_grass_block",
     .floor = "gravel",
     .patch = "podzol",
     .patchShare = 0.12F,
     .trees = 0.52F,
     .groves = 0.5F,
     .treeKinds = {{{TreeKind::Maple, 6},
                    {TreeKind::GoldMaple, 5},
                    {TreeKind::ForkedBirch, 1},
                    {TreeKind::Poplar, 1}}},
     .grass = 0.24F,
     .grassKinds = {{{"autumn_short_grass", 10}, {"fern", 2}, {"large_fern", 1}}},
     .flowers = 0.01F,
     .flowerKinds = {{{"lily_of_the_valley", 2}, {"allium", 1}}},
     .boulder = "mossy_cobblestone",
     .boulderMix = "stone",
     .boulders = 0.08F,
     .fallenLogs = 0.1F,
     .logKind = TreeKind::Oak},
}};

struct Species {
    TreeKind kind = TreeKind::Oak;
    string_view wood;
    string_view leaves;
    string_view mixed;
    float mixedShare = 0.0F;
};

constexpr std::array<Species, kTreeKindCount> kSpecies = {{
    {TreeKind::Oak, "oak", "oak_leaves", "birch_leaves", 0.12F},
    {TreeKind::BigOak, "oak", "oak_leaves", "dark_oak_leaves", 0.25F},
    {TreeKind::Birch, "birch", "birch_leaves", "oak_leaves", 0.1F},
    {TreeKind::TallBirch, "birch", "birch_leaves", "flowering_azalea_leaves", 0.05F},
    {TreeKind::Spruce, "spruce", "spruce_leaves", "", 0.0F},
    {TreeKind::Pine, "spruce", "spruce_leaves", "dark_oak_leaves", 0.2F},
    {TreeKind::DarkOak, "dark_oak", "dark_oak_leaves", "oak_leaves", 0.15F},
    {TreeKind::PaleOak, "pale_oak", "pale_oak_leaves", "", 0.0F},
    {TreeKind::Acacia, "acacia", "acacia_leaves", "", 0.0F},
    {TreeKind::Jungle, "jungle", "jungle_leaves", "oak_leaves", 0.15F},
    {TreeKind::GiantJungle, "jungle", "jungle_leaves", "dark_oak_leaves", 0.2F},
    {TreeKind::Azalea, "oak", "flowering_azalea_leaves", "oak_leaves", 0.45F},
    {TreeKind::Bush, "oak", "oak_leaves", "jungle_leaves", 0.3F},
    {TreeKind::TallSpruce, "spruce", "spruce_leaves", "dark_oak_leaves", 0.08F},
    {TreeKind::Palm, "palm", "palm_leaves", "", 0.0F},
    {TreeKind::Olive, "olive", "olive_leaves", "", 0.0F},
    {TreeKind::Maple, "oak", "maple_leaves", "golden_maple_leaves", 0.2F},
    {TreeKind::GoldMaple, "oak", "golden_maple_leaves", "maple_leaves", 0.2F},
    {TreeKind::Hemlock, "spruce", "hemlock_leaves", "", 0.0F},
    {TreeKind::Poplar, "pale_oak", "poplar_leaves", "", 0.0F},
    {TreeKind::WeepingBirch, "birch", "birch_leaves", "", 0.0F},
    {TreeKind::ForkedBirch, "birch", "birch_leaves", "flowering_azalea_leaves", 0.04F},
}};

// Finds states by name and remembers the first name that was missing.
class Lookup {
public:
    explicit Lookup(const BlockCatalog& catalog) noexcept
        : m_catalog(catalog) {}

    [[nodiscard]] BlockStateId operator()(string_view name) {
        if (name.empty()) {
            return vxe::voxel::kAirState;
        }
        const auto state = m_catalog.stateId(name);
        if (!state) {
            if (m_missing.empty()) {
                m_missing = name;
            }
            return vxe::voxel::kAirState;
        }
        return *state;
    }

    [[nodiscard]] const std::string& missing() const noexcept { return m_missing; }
    [[nodiscard]] const BlockCatalog& catalog() const noexcept { return m_catalog; }

private:
    const BlockCatalog& m_catalog;
    std::string m_missing;
};

template <std::size_t N, std::size_t M>
vxe::u8 resolvePlants(const std::array<Weighted, N>& from, std::array<PlantChoice, M>& to, Lookup& state) {
    vxe::u8 count = 0;
    for (const Weighted& plant : from) {
        if (!plant.name.empty()) {
            const BlockStateId id = state(plant.name);
            to.at(count) = {.state = id, .upper = state.catalog().tallUpper(id), .weight = plant.weight};
            ++count;
        }
    }
    return count;
}

} // namespace

vxe::Result<TerrainBlocks> TerrainBlocks::fromCatalog(const BlockCatalog& catalog) {
    Lookup state(catalog);
    TerrainBlocks blocks;
    blocks.stone = state("stone");
    blocks.granite = state("granite");
    blocks.andesite = state("andesite");
    blocks.tuff = state("tuff");
    blocks.calcite = state("calcite");
    blocks.basalt = state("basalt");
    blocks.gravel = state("gravel");
    blocks.sand = state("sand");
    blocks.clay = state("clay");
    blocks.sandstone = state("sandstone");
    blocks.redSandstone = state("red_sandstone");
    blocks.packedMud = state("packed_mud");
    blocks.water = state("water");
    blocks.coal = state("coal_ore");
    blocks.iron = state("iron_ore");
    blocks.gold = state("gold_ore");
    blocks.copper = state("copper_ore");
    blocks.diamond = state("diamond_ore");
    blocks.emerald = state("emerald_ore");
    blocks.floor = state("foundation_stone");
    blocks.soils = {
        state("grass_block"),       state("dirt"), state("coarse_dirt"), state("podzol"),
        state("rooted_dirt"),       state("mud"),  state("snowy_grass"), state("muddy_mangrove_roots"),
        state("autumn_grass_block")};
    blocks.watersideCount = resolvePlants(kWaterside, blocks.waterside, state);
    for (const Species& species : kSpecies) {
        const std::string wood(species.wood);
        blocks.trees.at(static_cast<std::size_t>(species.kind)) = {
            .log = state(std::format("{}_log[axis=y]", wood)),
            .logX = state(std::format("{}_log[axis=x]", wood)),
            .logZ = state(std::format("{}_log[axis=z]", wood)),
            .leaves = state(species.leaves),
            .mixedLeaves = state(species.mixed),
            .mixedShare = species.mixedShare};
    }
    for (const Recipe& recipe : kRecipes) {
        BiomeLook& look = blocks.biomes.at(static_cast<std::size_t>(recipe.biome));
        look.top = state(recipe.top);
        look.under = state(recipe.under);
        look.floor = state(recipe.floor);
        look.deep = state(recipe.deep);
        look.patch = state(recipe.patch);
        look.patch2 = state(recipe.patch2);
        look.patchShare = recipe.patchShare;
        look.patch2Share = recipe.patch2Share;
        look.underDepth = recipe.underDepth;
        look.deepDepth = recipe.deepDepth;
        look.banded = recipe.banded;
        look.dry = recipe.dry;
        look.waterTree = recipe.waterTree;
        look.trees = recipe.trees;
        look.groves = recipe.groves;
        look.treeKinds = recipe.treeKinds;
        for (const TreeChoice& choice : recipe.treeKinds) {
            look.treeCount = static_cast<vxe::u8>(look.treeCount + (choice.weight > 0 ? 1 : 0));
        }
        look.grass = recipe.grass;
        look.grassCount = resolvePlants(recipe.grassKinds, look.grassKinds, state);
        look.flowers = recipe.flowers;
        look.flowerCount = resolvePlants(recipe.flowerKinds, look.flowerKinds, state);
        look.boulder = state(recipe.boulder);
        look.boulderMix = state(recipe.boulderMix);
        look.boulders = recipe.boulders;
        look.fallenLogs = recipe.fallenLogs;
        look.logKind = recipe.logKind;
        look.snowDepth = recipe.snowDepth;
    }
    for (vxe::u8 layers = 1; layers <= blocks.snowLayers.size(); ++layers) {
        blocks.snowLayers.at(layers - 1U) = state(std::format("snow[layers={}]", layers));
    }
    blocks.ice = state("ice");
    blocks.packedIce = state("packed_ice");
    blocks.snowRest.assign(catalog.stateCount(), SnowRest::None);
    for (vxe::usize id = 0; id < catalog.stateCount(); ++id) {
        const auto stateId = static_cast<vxe::voxel::BlockStateId>(id);
        const vxe::voxel::BlockRenderInfo& info = catalog.renderInfo(stateId);
        if (info.layer == vxe::voxel::RenderLayer::Plant) {
            blocks.snowRest.at(id) = SnowRest::Buries;
        } else if (catalog.isSolid(stateId) && info.height == 0 &&
                   (info.layer == vxe::voxel::RenderLayer::Opaque ||
                    info.layer == vxe::voxel::RenderLayer::Cutout)) {
            blocks.snowRest.at(id) = SnowRest::Holds;
        }
    }
    if (!state.missing().empty()) {
        return vxe::Error(vxe::ErrorCode::NotFound,
                          std::format("World generation needs the block '{}'", state.missing()));
    }
    return blocks;
}

} // namespace vr
// NOLINTEND(modernize-use-designated-initializers, readability-redundant-member-init)
