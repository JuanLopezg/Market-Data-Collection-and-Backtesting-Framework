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
    std::size_t max_parallel_requests = 8;

    int midnight_delay_seconds = 5;
    int retry_after_failure_seconds = 300;
};

MarketDataConfig loadMarketDataConfig(const std::filesystem::path& path);
