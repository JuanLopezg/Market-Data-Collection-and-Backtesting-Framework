// Defines the market-data service configuration values read from JSON.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

// All user-configurable inputs for the live market-data service.
struct MarketDataConfig {
    std::string main_exchange = "binance";
    std::filesystem::path database_path = "storage/databases/database.db";
    std::string binance_base_url = "https://fapi.binance.com";

    std::string nats_url = "nats://127.0.0.1:4222";
    std::string stream = "ALGOTRADING_RUNTIME";
    // Opt-in simulated execution only; the ordinary LIVE deployment stays pre-exchange.
    bool publish_paper_execution_prices = false;

    std::size_t ranking_size = 100;
    std::size_t active_top_n = 50;
    int retain_after_top_n_days = 365;
    int minimum_history_days = 100;
    std::size_t max_parallel_requests = 64; // the schema maximum; see the weight note in ingestion.cpp

    // The daily run starts this long after UTC midnight; the ingestor proves each day closed
    // from Binance itself (the next day's candle, or Binance's clock) before storing it.
    int midnight_delay_ms = 0;
    int retry_after_failure_seconds = 300;

    // After each published day, download Binance funding settlements of every stored pair
    // into funding_rate_events (research/backtests only; no live decision reads them).
    bool download_funding_rates = true;
};

MarketDataConfig loadMarketDataConfig(const std::filesystem::path& path);
