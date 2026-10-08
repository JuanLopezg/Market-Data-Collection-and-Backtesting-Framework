// Loads and validates the market-data service JSON configuration.

#include "config.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

void requirePositive(const char* name, long long value)
{
    if (value <= 0)
        throw std::runtime_error(std::string("'") + name + "' must be positive");
}

} // namespace

MarketDataConfig loadMarketDataConfig(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open market-data config: " + path.string());

    json root;
    input >> root;

    MarketDataConfig config;

    if (root.contains("main_exchange"))
        config.main_exchange = root.at("main_exchange").get<std::string>();
    if (root.contains("database_path"))
        config.database_path = root.at("database_path").get<std::string>();
    if (root.contains("binance_base_url"))
        config.binance_base_url = root.at("binance_base_url").get<std::string>();
    if (root.contains("nats_url"))
        config.nats_url = root.at("nats_url").get<std::string>();
    if (root.contains("stream"))
        config.stream = root.at("stream").get<std::string>();
    if (root.contains("publish_paper_execution_prices"))
        config.publish_paper_execution_prices = root.at("publish_paper_execution_prices").get<bool>();
    if (root.contains("ranking_size"))
        config.ranking_size = root.at("ranking_size").get<std::size_t>();
    if (root.contains("active_top_n"))
        config.active_top_n = root.at("active_top_n").get<std::size_t>();
    if (root.contains("retain_after_top_n_days"))
        config.retain_after_top_n_days = root.at("retain_after_top_n_days").get<int>();
    if (root.contains("minimum_history_days"))
        config.minimum_history_days = root.at("minimum_history_days").get<int>();
    if (root.contains("max_parallel_requests"))
        config.max_parallel_requests = root.at("max_parallel_requests").get<std::size_t>();
    if (root.contains("midnight_delay_seconds"))
        config.midnight_delay_seconds = root.at("midnight_delay_seconds").get<int>();
    if (root.contains("retry_after_failure_seconds"))
        config.retry_after_failure_seconds = root.at("retry_after_failure_seconds").get<int>();

    if (config.main_exchange != "binance")
        throw std::runtime_error("Market-data ingestion supports main_exchange='binance' only");
    if (config.database_path.empty())
        throw std::runtime_error("'database_path' cannot be empty");
    if (config.binance_base_url.empty())
        throw std::runtime_error("'binance_base_url' cannot be empty");
    if (config.nats_url.empty())
        throw std::runtime_error("'nats_url' cannot be empty");
    if (config.stream.empty())
        throw std::runtime_error("'stream' cannot be empty");

    requirePositive("ranking_size", static_cast<long long>(config.ranking_size));
    requirePositive("active_top_n", static_cast<long long>(config.active_top_n));
    requirePositive("retain_after_top_n_days", config.retain_after_top_n_days);
    requirePositive("minimum_history_days", config.minimum_history_days);
    requirePositive("max_parallel_requests", static_cast<long long>(config.max_parallel_requests));
    requirePositive("retry_after_failure_seconds", config.retry_after_failure_seconds);

    if (config.active_top_n > config.ranking_size)
        throw std::runtime_error("'active_top_n' cannot be greater than 'ranking_size'");
    if (config.ranking_size > 1000)
        throw std::runtime_error("'ranking_size' is unreasonably large");
    if (config.minimum_history_days > 1500)
        throw std::runtime_error("'minimum_history_days' cannot exceed 1500");
    if (config.midnight_delay_seconds < 0 || config.midnight_delay_seconds > 300)
        throw std::runtime_error("'midnight_delay_seconds' must be between 0 and 300");

    if (!config.binance_base_url.empty() && config.binance_base_url.back() == '/')
        config.binance_base_url.pop_back();

    return config;
}
