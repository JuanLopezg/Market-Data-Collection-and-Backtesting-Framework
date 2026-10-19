// Symbol map: source symbols to venue contracts and units (tests/specs/exchange_gateway.md, Part S).

#include <cmath>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

#include "test_support.h"
#include "venue/symbol_map.h"

using Venue::SymbolMap;
using Venue::SymbolMapping;

namespace {

SymbolMap smallMap()
{
    return SymbolMap("kraken-futures", {{"BTCUSDT", "BTC", "PF_XBTUSD", 1.0},
                                        {"1000PEPEUSDT", "PEPE", "PF_PEPEUSD", 1000.0}});
}

std::string mapFile(const std::string& venue, const std::string& symbols, int schema = 1)
{
    return "{\"schema_version\": " + std::to_string(schema) + ", \"venue\": \"" + venue + "\", \"symbols\": " +
           symbols + "}";
}

} // namespace


TEST(SymbolMap, S1_UnitsOfAThousandPrefixedSymbol)
{
    const SymbolMap map = smallMap();
    const SymbolMapping* pepe = map.bySource("1000PEPEUSDT");
    ASSERT_NE(pepe, nullptr);
    EXPECT_EQ(pepe->venue_symbol, "PF_PEPEUSD");
    EXPECT_EQ(pepe->asset, "PEPE");
    EXPECT_DOUBLE_EQ(pepe->venue_units_per_source_unit, 1000.0);
    // 5 source units = 5,000 PEPE; 0.01 USD per source unit = 0.00001 USD per PEPE; USD unchanged.
    EXPECT_DOUBLE_EQ(5.0 * pepe->venue_units_per_source_unit, 5000.0);
    EXPECT_NEAR(0.01 / pepe->venue_units_per_source_unit, 0.00001, 1e-18);

    const SymbolMapping* back = map.byVenueSymbol("PF_PEPEUSD");
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->source, "1000PEPEUSDT");
}

TEST(SymbolMap, S2_UnmappedSymbolsAreNotGuessed)
{
    const SymbolMap map = smallMap();
    EXPECT_EQ(map.bySource("PEPEUSDT"), nullptr);
    EXPECT_EQ(map.bySource("BTCUSD"), nullptr);
    EXPECT_EQ(map.bySource("ETHUSDT"), nullptr);
    EXPECT_EQ(map.byVenueSymbol("PF_UNKNOWNUSD"), nullptr);
    EXPECT_EQ(map.byVenueSymbol("PF_BTCUSD"), nullptr);
}

TEST(SymbolMap, S3_InvalidTablesAreRefused)
{
    EXPECT_THROW(SymbolMap("", {{"BTCUSDT", "BTC", "PF_XBTUSD", 1.0}}), std::invalid_argument);
    EXPECT_THROW(SymbolMap("kraken-futures", {{"", "BTC", "PF_XBTUSD", 1.0}}), std::invalid_argument);
    EXPECT_THROW(SymbolMap("kraken-futures", {{"BTCUSDT", "", "PF_XBTUSD", 1.0}}), std::invalid_argument);
    EXPECT_THROW(SymbolMap("kraken-futures", {{"BTCUSDT", "BTC", "", 1.0}}), std::invalid_argument);
    for (const double scale : {0.0, -1000.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()})
        EXPECT_THROW(SymbolMap("kraken-futures", {{"BTCUSDT", "BTC", "PF_XBTUSD", scale}}), std::invalid_argument)
            << "scale " << scale;
    EXPECT_THROW(SymbolMap("kraken-futures", {{"BTCUSDT", "BTC", "PF_XBTUSD", 1.0},
                                              {"BTCUSDT", "BTC", "PF_BTCUSD", 1.0}}),
                 std::invalid_argument);
    EXPECT_THROW(SymbolMap("kraken-futures", {{"PEPEUSDT", "PEPE", "PF_PEPEUSD", 1.0},
                                              {"1000PEPEUSDT", "PEPE", "PF_PEPEUSD", 1000.0}}),
                 std::invalid_argument);
}

TEST(SymbolMap, S4_FileForAnotherVenueOrSchemaIsRefused)
{
    test_support::TempDir dir("symbol-map");
    const auto path = dir / "map.json";
    const std::string symbols =
        R"([{"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":1}])";

    test_support::writeFile(path, mapFile("kraken-futures", symbols));
    EXPECT_EQ(SymbolMap::load(path.string(), "kraken-futures").entries().size(), 1u);

    test_support::writeFile(path, mapFile("hyperliquid", symbols));
    EXPECT_THROW(SymbolMap::load(path.string(), "kraken-futures"), std::runtime_error);
    test_support::writeFile(path, mapFile("kraken-futures", symbols, 2));
    EXPECT_THROW(SymbolMap::load(path.string(), "kraken-futures"), std::runtime_error);
    test_support::writeFile(path, mapFile("kraken-futures", "{}"));
    EXPECT_THROW(SymbolMap::load(path.string(), "kraken-futures"), std::runtime_error);
    test_support::writeFile(path, "not json");
    EXPECT_THROW(SymbolMap::load(path.string(), "kraken-futures"), std::runtime_error);
    EXPECT_THROW(SymbolMap::load((dir / "missing.json").string(), "kraken-futures"), std::runtime_error);
}

TEST(SymbolMap, S4_MistypedOrMissingEntryFieldIsRefused)
{
    test_support::TempDir dir("symbol-map");
    const auto path = dir / "map.json";
    test_support::writeFile(path, mapFile("kraken-futures",
        R"([{"source":"BTCUSDT","asset":"BTC","venue_symbol":"PF_XBTUSD","venue_units_per_source_unit":"1"}])"));
    EXPECT_THROW(SymbolMap::load(path.string(), "kraken-futures"), std::runtime_error);
    test_support::writeFile(path, mapFile("kraken-futures",
        R"([{"source":"BTCUSDT","asset":"BTC","venue_units_per_source_unit":1}])"));
    EXPECT_THROW(SymbolMap::load(path.string(), "kraken-futures"), std::runtime_error);
}

TEST(SymbolMap, S4_CommittedKrakenTableLoads)
{
    const SymbolMap map =
        SymbolMap::load(test_support::sourcePath("config/venues/kraken_futures/symbol_map.json").string(),
                        "kraken-futures");
    EXPECT_EQ(map.venue(), "kraken-futures");
    const SymbolMapping* btc = map.bySource("BTCUSDT");
    ASSERT_NE(btc, nullptr);
    EXPECT_EQ(btc->venue_symbol, "PF_XBTUSD");
    EXPECT_DOUBLE_EQ(btc->venue_units_per_source_unit, 1.0);
    const SymbolMapping* pepe = map.bySource("1000PEPEUSDT");
    ASSERT_NE(pepe, nullptr);
    EXPECT_EQ(pepe->venue_symbol, "PF_PEPEUSD");
    EXPECT_DOUBLE_EQ(pepe->venue_units_per_source_unit, 1000.0);
}
