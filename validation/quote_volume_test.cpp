#include "canonical_market_data_reader.h"
#include "indicator_engine.h"
#include "indicator_calculators.h"
#include "liquidity_universe.h"
#include "market_store.h"
#include "message_json.h"
#include "rolling_market_state.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sqlite3.h>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    require(argc == 2, "Usage: quote_volume_test TEMP_DATABASE");
    const std::filesystem::path database = argv[1];
    require(!std::filesystem::exists(database), "Test database must not exist");
    sqlite3* db = nullptr;
    sqlite3_open(database.c_str(), &db);
    require(sqlite3_exec(db,
        "CREATE TABLE ohlcv_data(pair TEXT,date INTEGER,open REAL,high REAL,low REAL,close REAL,volume REAL,PRIMARY KEY(pair,date));"
        "INSERT INTO ohlcv_data VALUES('BTCUSDT',20260901,100,101,99,100,10);"
        "CREATE TABLE market_volume_rank_daily(date INTEGER,rank INTEGER,pair TEXT,quote_volume REAL,PRIMARY KEY(date,rank),UNIQUE(date,pair));"
        "INSERT INTO market_volume_rank_daily VALUES(20260901,1,'BTCUSDT',900000);", nullptr, nullptr, nullptr) == SQLITE_OK,
        "Create older schema failed");
    sqlite3_close(db);
    {
        CanonicalMarketDataReader oldReader(database);
        const auto older = oldReader.loadWindow(20260901, 1, 1);
        require(std::isnan(older.raw_data.data.at("BTCUSDT").at(20260901).quote_volume),
                "Older schema must retain an unavailable quote value");
    }

    const auto target = std::chrono::year{2026}/9/1;
    TrackedMarketData tracked{target, {{"BTCUSDT", 0}, {"PEPEUSDT", 0}}};
    OHLCVData raw;
    raw.data["BTCUSDT"][20260901] = OHLCV{100, 101, 99, 100, 10, 900000};
    raw.data["PEPEUSDT"][20260901] = OHLCV{1, 2, 1, 1, 1e12, 1000};
    {
        MarketDataStore store(database);
        const auto plan = store.buildDownloadPlan(tracked, target, 365, 1);
        require(plan.size() == 2, "Existing base-only candle must be scheduled for quote backfill");
        store.commitDailyUpdate(tracked, raw, {{"BTCUSDT", 900000}, {"PEPEUSDT", 1000}}, target);
        require(store.buildDownloadPlan(tracked, target, 365, 1).empty(), "Backfill must be idempotent");
        auto invalid = raw;
        invalid.data["BTCUSDT"][20260901].quote_volume = 600000;
        invalid.data["PEPEUSDT"][20260901].quote_volume = -1;
        bool rejected = false;
        try { store.commitDailyUpdate(tracked, invalid, {}, target); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "Invalid quote notional must reject the whole transaction");
    }
    CanonicalMarketDataReader reader(database);
    const auto window = reader.loadWindow(20260901, 1, 2);
    require(window.raw_data.data.at("BTCUSDT").at(20260901).quote_volume == 900000,
            "Reader must preserve actual turnover rather than volume * close");
    const IndicatorSpec base{IndicatorKind::SMA, PriceField::Volume, 1};
    const IndicatorSpec quote{IndicatorKind::SMA, PriceField::QuoteVolume, 1};
    IndicatorEngine indicators;
    indicators.precompute(window.raw_data, {base, quote});
    const auto& bars = window.market_data.at(20260901);
    require(TopNLiquidityUniverse(base, 1).select(bars, 20260901, indicators).contains("PEPEUSDT"),
            "Older base-volume semantics changed");
    require(TopNLiquidityUniverse(quote, 1).select(bars, 20260901, indicators).contains("BTCUSDT"),
            "Quote ranking must prefer actual higher notional");
    std::vector<OHLCV> history(4, OHLCV{1, 1, 1, 1, 1, 10});
    history[0].quote_volume = std::numeric_limits<double>::quiet_NaN();
    const auto sma = calculateSMA(history, PriceField::QuoteVolume, 2);
    require(std::isnan(sma[1]) && sma[2] == 10 && sma[3] == 10,
            "SMA must recover once missing old quote rows leave the window");
    MarketSliceSnapshot slice;
    slice.timestamp = 20260901;
    slice.bars.push_back({"BTCUSDT", raw.data.at("BTCUSDT").at(20260901)});
    const auto decoded = MessageJson::decodeMarketSliceSnapshot(MessageJson::encode(slice));
    require(decoded.bars[0].bar.quote_volume == 900000, "Transport must preserve quote turnover");
    RollingMarketState rolling;
    require(rolling.append(decoded) && !rolling.append(decoded), "Identical quote redelivery must be idempotent");
    slice.bars[0].bar.quote_volume += 1;
    bool conflict = false;
    try { rolling.append(slice); } catch (const std::exception&) { conflict = true; }
    require(conflict, "Conflicting quote redelivery must be rejected");
    slice.bars[0].bar.quote_volume = std::numeric_limits<double>::quiet_NaN();
    const auto oldPayload = MessageJson::encode(slice);
    require(oldPayload.find("quote_volume") == std::string::npos &&
            std::isnan(MessageJson::decodeMarketSliceSnapshot(oldPayload).bars[0].bar.quote_volume),
            "Older base-only wire contract must remain unchanged");
    std::cout << "QUOTE-VOLUME: PASS: older schema migration, actual turnover, ranking, rollback, restart and missing history\n";
}
