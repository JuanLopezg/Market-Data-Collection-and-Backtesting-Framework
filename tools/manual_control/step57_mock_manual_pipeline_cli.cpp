#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "manual_trading.h"

using namespace ManualControl;
using namespace MockVenue;

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::cerr << "usage: step57_mock_manual_pipeline_cli <state_dir> <asset> <weight_pct> <reference_price>\n";
        return 2;
    }

    const std::string asset = argv[2];
    const double weight_pct = std::stod(argv[3]);
    const double price = std::stod(argv[4]);
    if (weight_pct < 0.0 || weight_pct > 100.0 || price <= 0.0)
        return 2;

    MockChaosConfig chaos;
    chaos.submit_limit = 100U;
    chaos.reconcile_limit = 100U;
    MockExchange adapter(argv[1], chaos);
    ManualTrading pipeline(adapter);

    ManualTargetIntent intent;
    intent.request_id = "cli-request";
    intent.correlation_id = "cli-correlation";
    intent.actor = "CLI / OPERATOR";
    intent.request_hash = "sha256:cli";
    intent.decision_timestamp = 100U;
    intent.execution_timestamp = 101U;
    if (weight_pct > 0.0)
        intent.asset_weights.emplace(asset, weight_pct / 100.0);
    intent.cash_weight = 1.0 - weight_pct / 100.0;

    ExecutionReferencePrices closes;
    closes.set(asset, price);
    ExecutionReferencePrices opens;
    opens.set(asset, price);

    const auto result = pipeline.route(intent, closes, opens);
    std::cout << "status=" << static_cast<int>(result.status)
              << " submits=" << result.submit_count
              << " cancels=" << result.cancel_count
              << " reason=" << result.reason << "\n";
    return result.status == ManualRouteStatus::Submitted ||
           result.status == ManualRouteStatus::Noop ? 0 : 1;
}
