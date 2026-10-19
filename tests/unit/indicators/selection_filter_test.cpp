#include <gtest/gtest.h>

#include "alphabetical_ranker.h"
#include "benchmark_above_sma_filter.h"
#include "indicator_engine.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace {

constexpr Timestamp selectionDate = 20240102;
constexpr IndicatorSpec quoteSma{IndicatorKind::SMA, PriceField::QuoteVolume, 2};
constexpr IndicatorSpec volumeSma{IndicatorKind::SMA, PriceField::Volume, 2};
constexpr IndicatorSpec closeRoc{IndicatorKind::ROC, PriceField::Close, 1};

void addAsset(OHLCVData& raw, const Coin& coin, const std::vector<double>& closes,
              const std::vector<double>& quotes = {}, double volume = 1)
{
    if (!quotes.empty() && quotes.size() != closes.size())
        throw std::invalid_argument("Fixture close and quote series must align");
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double close = closes[i];
        const double quote = quotes.empty() ? std::numeric_limits<double>::quiet_NaN() : quotes[i];
        raw.data[coin][20240101u + static_cast<Timestamp>(i)] =
            {close, close + 1, close - 1, close, volume, quote};
    }
}

CoinBarMap candidates(const OHLCVData& raw, Timestamp date, std::initializer_list<Coin> coins)
{
    const auto market = buildMarketData(raw);
    CoinBarMap result;
    for (const auto& coin : coins) result.emplace(coin, market.at(date).at(coin));
    return result;
}

std::set<Coin> symbols(const CoinBarMap& bars)
{
    std::set<Coin> result;
    for (const auto& [coin, bar] : bars) {
        (void)bar;
        result.insert(coin);
    }
    return result;
}

std::vector<Coin> symbols(const RankedUniverse& ranked)
{
    std::vector<Coin> result;
    for (const auto& entry : ranked) result.push_back(entry.coin);
    return result;
}

class LiquiditySelection : public ::testing::Test {
protected:
    OHLCVData raw;
    IndicatorEngine engine;
    CoinBarMap bars;

    void SetUp() override
    {
        addAsset(raw, "A", {10, 12}, {100, 300}, 1000);
        addAsset(raw, "B", {20, 22}, {300, 500}, 1);
        addAsset(raw, "C", {30, 32}, {200, 400}, 20);
        addAsset(raw, "OUTSIDE", {40, 42}, {9000, 11000}, 5);
        addAsset(raw, "D", {50, 52}); // Legacy/missing quote turnover, not zero turnover.
        engine.precompute(raw, {quoteSma, volumeSma});
        bars = candidates(raw, selectionDate, {"A", "B", "C", "D"});
    }
};

TEST_F(LiquiditySelection, R11_TopNUsesQuoteSmaOnlyWithinCurrentCandidates)
{
    const TopNLiquidityUniverse selector(quoteSma, 2);
    engine.precompute(raw, selector.requiredIndicators());
    const auto selected = selector.select(bars, selectionDate, engine);
    EXPECT_EQ(symbols(selected), (std::set<Coin>{"B", "C"}));
    EXPECT_FALSE(selected.contains("OUTSIDE"));
}

TEST_F(LiquiditySelection, R11_QuoteAndBaseVolumeRemainDifferentLiquidityPolicies)
{
    const TopNLiquidityUniverse quoteSelector(quoteSma, 2);
    const TopNLiquidityUniverse baseSelector(volumeSma, 2);
    EXPECT_EQ(symbols(quoteSelector.select(bars, selectionDate, engine)),
              (std::set<Coin>{"B", "C"}));
    EXPECT_EQ(symbols(baseSelector.select(bars, selectionDate, engine)),
              (std::set<Coin>{"A", "C"}));
}

TEST_F(LiquiditySelection, R11_AscendingSelectionKeepsTheLowestFiniteScores)
{
    const TopNLiquidityUniverse selector(quoteSma, 2, false);
    EXPECT_EQ(symbols(selector.select(bars, selectionDate, engine)), (std::set<Coin>{"A", "C"}));
}

TEST_F(LiquiditySelection, R11_SelectedBarsKeepTheirCurrentFinancialObservations)
{
    const TopNLiquidityUniverse selector(quoteSma, 2);
    const auto selected = selector.select(bars, selectionDate, engine);
    ASSERT_TRUE(selected.contains("B"));
    EXPECT_DOUBLE_EQ(selected.at("B").close, 22);
    EXPECT_DOUBLE_EQ(selected.at("B").open, 22);
    EXPECT_DOUBLE_EQ(selected.at("B").volume, 1);
    EXPECT_DOUBLE_EQ(selected.at("B").quote_volume, 500);
    EXPECT_DOUBLE_EQ(bars.at("B").quote_volume, 500);
}

TEST_F(LiquiditySelection, R11_ZeroEmptyAndOversizedLimitsHaveBoundedMembership)
{
    const TopNLiquidityUniverse none(quoteSma, 0);
    const TopNLiquidityUniverse oversized(quoteSma, 100);
    EXPECT_TRUE(none.select(bars, selectionDate, engine).empty());
    EXPECT_TRUE(oversized.select({}, selectionDate, engine).empty());
    EXPECT_EQ(symbols(oversized.select(bars, selectionDate, engine)),
              (std::set<Coin>{"A", "B", "C"}));
}

TEST_F(LiquiditySelection, R12_MissingQuoteHistoryCannotBeEstimatedIntoEligibility)
{
    const TopNLiquidityUniverse selector(quoteSma, 2);
    const auto restricted = candidates(raw, selectionDate, {"A", "D"});
    EXPECT_EQ(symbols(selector.select(restricted, selectionDate, engine)), (std::set<Coin>{"A"}));
}

TEST_F(LiquiditySelection, R12_ZeroLiquidityIsEligibleWhileUnavailableLiquidityIsSkipped)
{
    addAsset(raw, "ZERO", {10, 12}, {0, 0});
    engine.precompute(raw, {quoteSma});
    const TopNLiquidityUniverse selector(quoteSma, 2);
    const auto restricted = candidates(raw, selectionDate, {"ZERO", "D"});
    EXPECT_EQ(symbols(selector.select(restricted, selectionDate, engine)), (std::set<Coin>{"ZERO"}));
}

TEST_F(LiquiditySelection, R12_EqualLiquidityUsesAlphabeticalBoundaryRegardlessOfInsertionOrder)
{
    addAsset(raw, "B", {20, 22}, {100, 300});
    engine.precompute(raw, {quoteSma});
    bars = candidates(raw, selectionDate, {"A", "B"});
    const TopNLiquidityUniverse selector(quoteSma, 1);
    for (const auto& order : {std::vector<Coin>{"A", "B"}, std::vector<Coin>{"B", "A"}}) {
        CoinBarMap tied;
        for (const auto& coin : order) tied.emplace(coin, bars.at(coin));
        EXPECT_EQ(symbols(selector.select(tied, selectionDate, engine)), (std::set<Coin>{"A"}));
    }
}

TEST_F(LiquiditySelection, R12_RetainedUnavailableScoresFollowFiniteLiquidity)
{
    const auto restricted = candidates(raw, selectionDate, {"A", "D"});
    const TopNLiquidityUniverse first(quoteSma, 1, true, false);
    const TopNLiquidityUniverse both(quoteSma, 2, true, false);
    EXPECT_EQ(symbols(first.select(restricted, selectionDate, engine)), (std::set<Coin>{"A"}));
    EXPECT_EQ(symbols(both.select(restricted, selectionDate, engine)), (std::set<Coin>{"A", "D"}));
}

TEST_F(LiquiditySelection, R12_UnavailableTiesStillHaveADeterministicAlphabeticalBoundary)
{
    const IndicatorEngine uncomputed;
    const TopNLiquidityUniverse selector(quoteSma, 1, true, false);
    for (const auto& order : {std::vector<Coin>{"C", "B", "A"}, std::vector<Coin>{"A", "B", "C"}}) {
        CoinBarMap tied;
        for (const auto& coin : order) tied.emplace(coin, bars.at(coin));
        EXPECT_EQ(symbols(selector.select(tied, selectionDate, uncomputed)), (std::set<Coin>{"A"}));
    }
}

TEST_F(LiquiditySelection, R12_RecomputeWithFutureTurnoverSpikeCannotChangeTodaysSelection)
{
    const TopNLiquidityUniverse selector(quoteSma, 2);
    EXPECT_EQ(symbols(selector.select(bars, selectionDate, engine)), (std::set<Coin>{"B", "C"}));
    raw.data["A"][20240103] = {1000, 1001, 999, 1000, 1000, 1000000000};
    IndicatorEngine restarted;
    restarted.precompute(raw, selector.requiredIndicators());
    EXPECT_EQ(symbols(selector.select(bars, selectionDate, restarted)), (std::set<Coin>{"B", "C"}));
}

TEST_F(LiquiditySelection, R10_DefaultSelectionExcludesAnInfiniteLiquidityScore)
{
    // Defensive corrupt-input case: an infinite source value must not win top-N.
    addAsset(raw, "D", {50, 52}, {10, std::numeric_limits<double>::infinity()});
    const IndicatorSpec turnover{IndicatorKind::Highest, PriceField::QuoteVolume, 1};
    const TopNLiquidityUniverse selector(turnover, 2);
    engine.precompute(raw, selector.requiredIndicators());
    const auto current = candidates(raw, selectionDate, {"A", "B", "C", "D"});
    EXPECT_EQ(symbols(selector.select(current, selectionDate, engine)), (std::set<Coin>{"B", "C"}));
}

class EntryRanking : public ::testing::Test {
protected:
    OHLCVData raw;
    IndicatorEngine engine;
    CoinBarMap bars;

    void SetUp() override
    {
        addAsset(raw, "A", {100, 110});
        addAsset(raw, "B", {100, 80});
        addAsset(raw, "C", {100, 100});
        addAsset(raw, "OUTSIDE", {100, 1000});
        raw.data["D"][selectionDate] = {50, 51, 49, 50, 1};
        engine.precompute(raw, {closeRoc});
        bars = candidates(raw, selectionDate, {"A", "B", "C", "D"});
    }
};

TEST_F(EntryRanking, R13_DescendingRanksUseFractionalScoresAndConsecutivePositions)
{
    const IndicatorRanker ranker(closeRoc);
    engine.precompute(raw, ranker.requiredIndicators());
    const auto ranked = ranker.rank(bars, selectionDate, engine);
    ASSERT_EQ(symbols(ranked), (std::vector<Coin>{"A", "C", "B"}));
    EXPECT_NEAR(ranked[0].score, 0.1, 1e-12);
    EXPECT_DOUBLE_EQ(ranked[1].score, 0);
    EXPECT_NEAR(ranked[2].score, -0.2, 1e-12);
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        EXPECT_EQ(ranked[i].rank, static_cast<unsigned>(i + 1));
        ASSERT_NE(ranked[i].bar, nullptr);
    }
    EXPECT_DOUBLE_EQ(ranked[2].bar->close, 80);
}

TEST_F(EntryRanking, R13_AscendingRanksPreferTheLowestScore)
{
    const IndicatorRanker ranker(closeRoc, false);
    const auto ranked = ranker.rank(bars, selectionDate, engine);
    EXPECT_EQ(symbols(ranked), (std::vector<Coin>{"B", "C", "A"}));
}

TEST_F(EntryRanking, R13_EqualScoresHaveAlphabeticalOrderIndependentOfInsertion)
{
    addAsset(raw, "B", {100, 110});
    engine.precompute(raw, {closeRoc});
    bars = candidates(raw, selectionDate, {"A", "B"});
    const IndicatorRanker ranker(closeRoc);
    for (const auto& order : {std::vector<Coin>{"A", "B"}, std::vector<Coin>{"B", "A"}}) {
        CoinBarMap tied;
        for (const auto& coin : order) tied.emplace(coin, bars.at(coin));
        EXPECT_EQ(symbols(ranker.rank(tied, selectionDate, engine)), (std::vector<Coin>{"A", "B"}));
    }
}

TEST_F(EntryRanking, R13_CachedAssetsOutsideCandidatesCannotGainARank)
{
    const IndicatorRanker ranker(closeRoc);
    const auto restricted = candidates(raw, selectionDate, {"B", "C"});
    EXPECT_EQ(symbols(ranker.rank(restricted, selectionDate, engine)), (std::vector<Coin>{"C", "B"}));
}

TEST_F(EntryRanking, R13_MissingScoresAreSkippedWhileZeroReturnRemainsRanked)
{
    const IndicatorRanker ranker(closeRoc);
    const auto restricted = candidates(raw, selectionDate, {"C", "D"});
    const auto ranked = ranker.rank(restricted, selectionDate, engine);
    ASSERT_EQ(symbols(ranked), (std::vector<Coin>{"C"}));
    EXPECT_DOUBLE_EQ(ranked[0].score, 0);
    EXPECT_EQ(ranked[0].rank, 1u);
}

TEST_F(EntryRanking, R10_DefaultRankingExcludesAnInfiniteScore)
{
    raw.data["D"][selectionDate].high = std::numeric_limits<double>::infinity();
    const IndicatorRanker ranker({IndicatorKind::Highest, PriceField::High, 1});
    engine.precompute(raw, ranker.requiredIndicators());
    const auto current = candidates(raw, selectionDate, {"A", "B", "C", "D"});
    EXPECT_EQ(symbols(ranker.rank(current, selectionDate, engine)),
              (std::vector<Coin>{"A", "C", "B"}));
}

TEST_F(EntryRanking, R13_RetainedUnavailableScoresHaveDeterministicOrderAfterFiniteScores)
{
    const IndicatorRanker ranker(closeRoc, true, false);
    // Only C's valid zero return is precomputed; A/B remain unavailable.
    OHLCVData onlyC;
    onlyC.data["C"] = raw.data.at("C");
    engine.precompute(onlyC, {closeRoc});
    for (const auto& order : {std::vector<Coin>{"C", "B", "A"}, std::vector<Coin>{"A", "B", "C"}}) {
        CoinBarMap mixed;
        for (const auto& coin : order) mixed.emplace(coin, bars.at(coin));
        EXPECT_EQ(symbols(ranker.rank(mixed, selectionDate, engine)),
                  (std::vector<Coin>{"C", "A", "B"}));
    }
}

TEST_F(EntryRanking, R14_AlphabeticalRankingIsDeterministicAndAssignsOneBasedRanks)
{
    const AlphabeticalRanker ranker;
    const IndicatorEngine uncomputed;
    for (const auto& order : {std::vector<Coin>{"C", "A", "B"}, std::vector<Coin>{"B", "C", "A"}}) {
        CoinBarMap unordered;
        for (const auto& coin : order) unordered.emplace(coin, bars.at(coin));
        const auto ranked = ranker.rank(unordered, selectionDate, uncomputed);
        ASSERT_EQ(symbols(ranked), (std::vector<Coin>{"A", "B", "C"}));
        for (std::size_t i = 0; i < ranked.size(); ++i) {
            EXPECT_EQ(ranked[i].rank, static_cast<unsigned>(i + 1));
            ASSERT_NE(ranked[i].bar, nullptr);
        }
        EXPECT_DOUBLE_EQ(ranked[1].bar->close, 80);
    }
}

OHLCVData benchmarkHistory(const std::vector<double>& closes)
{
    OHLCVData raw;
    addAsset(raw, "BTC", closes);
    return raw;
}

TEST(BenchmarkGate, R15_CloseStrictlyAboveItsSameDateSmaAllowsEntries)
{
    const auto raw = benchmarkHistory({10, 20, 30});
    const BenchmarkAboveSMAFilter filter("BTC", 3);
    IndicatorEngine engine;
    engine.precompute(raw, filter.requiredIndicators());
    EXPECT_TRUE(filter.passes(buildMarketData(raw), 20240103, engine));
}

TEST(BenchmarkGate, R15_CloseBelowOrEqualToSmaBlocksEntries)
{
    const BenchmarkAboveSMAFilter filter("BTC", 3);
    for (const auto& closes : {std::vector<double>{30, 20, 10}, std::vector<double>{20, 20, 20}}) {
        const auto raw = benchmarkHistory(closes);
        IndicatorEngine engine;
        engine.precompute(raw, filter.requiredIndicators());
        EXPECT_FALSE(filter.passes(buildMarketData(raw), 20240103, engine));
    }
}

TEST(BenchmarkGate, R15_MissingDateBenchmarkWarmupOrUncomputedSmaFailsClosed)
{
    const auto raw = benchmarkHistory({10, 20, 30});
    auto market = buildMarketData(raw);
    const BenchmarkAboveSMAFilter filter("BTC", 3);
    IndicatorEngine engine;
    engine.precompute(raw, filter.requiredIndicators());
    EXPECT_FALSE(filter.passes(market, 20240104, engine));
    EXPECT_FALSE(filter.passes(market, 20240101, engine));
    const IndicatorEngine uncomputed;
    EXPECT_FALSE(filter.passes(market, 20240103, uncomputed));
    market.at(20240103).erase("BTC");
    EXPECT_FALSE(filter.passes(market, 20240103, engine));
}

TEST(BenchmarkGate, R15_ConfiguredSourceControlsTheAverageButComparisonUsesClose)
{
    auto raw = benchmarkHistory({10, 20, 30});
    for (auto& [date, bar] : raw.data.at("BTC")) {
        (void)date;
        bar.open = 40;
        bar.high = 41;
    }
    const BenchmarkAboveSMAFilter filter("BTC", 3, PriceField::Open);
    IndicatorEngine engine;
    engine.precompute(raw, filter.requiredIndicators());
    EXPECT_FALSE(filter.passes(buildMarketData(raw), 20240103, engine));
}

TEST(BenchmarkGate, R15_NonfiniteCurrentBenchmarkCloseCannotAllowEntries)
{
    const auto raw = benchmarkHistory({10, 20, 30});
    auto market = buildMarketData(raw);
    const BenchmarkAboveSMAFilter filter("BTC", 3);
    IndicatorEngine engine;
    engine.precompute(raw, filter.requiredIndicators());
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        market.at(20240103).at("BTC").close = invalid;
        EXPECT_FALSE(filter.passes(market, 20240103, engine));
    }
}

TEST(BenchmarkGate, R15_LaterCrashCannotChangeAnEarlierBenchmarkDecision)
{
    auto raw = benchmarkHistory({10, 20, 30});
    const BenchmarkAboveSMAFilter filter("BTC", 3);
    IndicatorEngine engine;
    engine.precompute(raw, filter.requiredIndicators());
    EXPECT_TRUE(filter.passes(buildMarketData(raw), 20240103, engine));
    raw.data["BTC"][20240104] = {1, 2, 0.5, 1, 1};
    IndicatorEngine restarted;
    restarted.precompute(raw, filter.requiredIndicators());
    EXPECT_TRUE(filter.passes(buildMarketData(raw), 20240103, restarted));
}

TEST(SelectionPipeline, R16_QuoteLiquidityPrecedesRsiRankingAndSameDateBenchmarkGate)
{
    OHLCVData raw;
    addAsset(raw, "A", {100, 120, 100}, {100, 100, 100});
    addAsset(raw, "B", {100, 110, 100}, {400, 400, 400});
    addAsset(raw, "C", {100, 90, 110}, {300, 300, 300});
    addAsset(raw, "OUTSIDE", {100, 200, 400}, {10000, 10000, 10000});
    addAsset(raw, "BTC", {10, 20, 30});
    const TopNLiquidityUniverse selector(quoteSma, 2);
    const IndicatorRanker ranker({IndicatorKind::RSI, PriceField::Close, 2});
    const BenchmarkAboveSMAFilter filter("BTC", 3);
    auto specs = selector.requiredIndicators();
    const auto rankSpecs = ranker.requiredIndicators();
    const auto filterSpecs = filter.requiredIndicators();
    specs.insert(specs.end(), rankSpecs.begin(), rankSpecs.end());
    specs.insert(specs.end(), filterSpecs.begin(), filterSpecs.end());
    IndicatorEngine engine;
    engine.precompute(raw, specs);
    const auto market = buildMarketData(raw);
    const auto current = candidates(raw, 20240103, {"A", "B", "C"});
    for (unsigned evaluation = 0; evaluation < 2; ++evaluation) {
        EXPECT_TRUE(filter.passes(market, 20240103, engine));
        const auto eligible = selector.select(current, 20240103, engine);
        ASSERT_EQ(symbols(eligible), (std::set<Coin>{"B", "C"}));
        const auto ranked = ranker.rank(eligible, 20240103, engine);
        ASSERT_EQ(symbols(ranked), (std::vector<Coin>{"C", "B"}));
        EXPECT_NEAR(ranked[0].score, 200.0 / 3.0, 1e-10);
        EXPECT_DOUBLE_EQ(ranked[1].score, 50);
        EXPECT_EQ(ranked[0].rank, 1u);
        EXPECT_EQ(ranked[1].rank, 2u);
    }
    EXPECT_DOUBLE_EQ(current.at("C").close, 110);
    EXPECT_DOUBLE_EQ(current.at("C").quote_volume, 300);
    EXPECT_DOUBLE_EQ(raw.data.at("B").at(20240103).close, 100);
}

} // namespace
