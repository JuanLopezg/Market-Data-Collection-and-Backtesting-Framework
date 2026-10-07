#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()

FILES = {
    "risk": ROOT / "live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp",
    "planner": ROOT / "live_trading/order_planner_service/src/order_planner_service_main.cpp",
    "execution": ROOT / "live_trading/execution_state_service/src/execution_state_service_main.cpp",
}

HELPER_BUSINESS = r'''
std::optional<Timestamp> configuredReplayBootstrapCompletedUtcDate(const TimeHandlerConfig& config)
{
    const char* simulatedReference = std::getenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    if (!simulatedReference || *simulatedReference == '\0')
        return std::nullopt;

    const auto referenceUtcDate = getCurrentUtcDate(config.simulated_reference_utc);
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(referenceUtcDate)));
}

'''

def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)

def patch_risk(path):
    text = path.read_text()
    if "configuredReplayBootstrapCompletedUtcDate" in text:
        print(f"SKIP already patched: {path}")
        return

    text = replace_once(
        text,
        '''Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}

''',
        '''Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}

''' + HELPER_BUSINESS,
        "risk helper"
    )

    text = replace_once(
        text,
        '''    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    JetStreamBus bus_;
''',
        '''    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> replay_bootstrap_completed_date_;
    JetStreamBus bus_;
''',
        "risk member"
    )

    text = replace_once(
        text,
        '''          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          bus_(options_.nats_url),
''',
        '''          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          replay_bootstrap_completed_date_(configuredReplayBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
''',
        "risk ctor"
    )

    text = replace_once(
        text,
        '''            if (snapshot.timestamp < newestCompleted) {
                LG_INFO(
                    "service=portfolio-risk event=stale_account_snapshot_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    snapshot.timestamp,
                    newestCompleted);
                return DurableMessageDisposition::Ack;
            }
''',
        '''            if (replay_bootstrap_completed_date_.has_value() &&
                snapshot.timestamp < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=portfolio-risk event=pre_bootstrap_account_snapshot_skipped timestamp={} bootstrap_completed_date={} disposition=ack",
                    snapshot.timestamp,
                    *replay_bootstrap_completed_date_);
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                snapshot.timestamp < newestCompleted) {
                LG_INFO(
                    "service=portfolio-risk event=stale_account_snapshot_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    snapshot.timestamp,
                    newestCompleted);
                return DurableMessageDisposition::Ack;
            }
''',
        "risk account stale gate"
    )

    text = replace_once(
        text,
        '''            // LIVE only acts on the latest fully completed UTC day. Old durable messages
            // are drained/ACKed but never turned into late economic decisions.
            if (target < newestCompleted) {
                LG_WARN(
                    "service=portfolio-risk event=stale_strategy_intents timestamp={} newest_completed_utc={} disposition=ack",
                    target,
                    newestCompleted);
                return DurableMessageDisposition::Ack;
            }
''',
        '''            // Identity LIVE mode still acts only on the latest fully completed UTC day.
            // Historical replay with an explicit shared simulated reference is event-time
            // driven: a durable intent at/after the immutable bootstrap day remains valid
            // even if processing latency means TimeHandler has advanced to a later day.
            if (replay_bootstrap_completed_date_.has_value() &&
                target < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=portfolio-risk event=pre_bootstrap_strategy_intents_skipped timestamp={} bootstrap_completed_date={} disposition=ack",
                    target,
                    *replay_bootstrap_completed_date_);
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() && target < newestCompleted) {
                LG_WARN(
                    "service=portfolio-risk event=stale_strategy_intents timestamp={} newest_completed_utc={} disposition=ack",
                    target,
                    newestCompleted);
                return DurableMessageDisposition::Ack;
            }
''',
        "risk intents stale gate"
    )

    path.write_text(text)
    print(f"PATCHED: {path}")

def patch_planner(path):
    text = path.read_text()
    if "configuredReplayBootstrapCompletedUtcDate" in text:
        print(f"SKIP already patched: {path}")
        return

    text = replace_once(
        text,
        '''Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}

''',
        '''Timestamp newestCompletedBusinessUtcDate(const TimeHandler& timeHandler)
{
    const auto todayUtc = getCurrentUtcDate(timeHandler.getTime());
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(todayUtc)));
}

''' + HELPER_BUSINESS,
        "planner helper"
    )

    text = replace_once(
        text,
        '''    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    JetStreamBus bus_;
''',
        '''    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> replay_bootstrap_completed_date_;
    JetStreamBus bus_;
''',
        "planner member"
    )

    text = replace_once(
        text,
        '''          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          bus_(options_.nats_url),
''',
        '''          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          replay_bootstrap_completed_date_(configuredReplayBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
''',
        "planner ctor"
    )

    text = replace_once(
        text,
        '''            if (request.decision_timestamp < newestCompleted) {
                LG_INFO(
                    "service=order-planner event=stale_notional_request_skipped decision_timestamp={} newest_completed_utc={} disposition=ack",
                    request.decision_timestamp,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }
''',
        '''            if (replay_bootstrap_completed_date_.has_value() &&
                request.decision_timestamp < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=order-planner event=pre_bootstrap_notional_request_skipped decision_timestamp={} bootstrap_completed_date={} disposition=ack",
                    request.decision_timestamp,
                    *replay_bootstrap_completed_date_
                );
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                request.decision_timestamp < newestCompleted) {
                LG_INFO(
                    "service=order-planner event=stale_notional_request_skipped decision_timestamp={} newest_completed_utc={} disposition=ack",
                    request.decision_timestamp,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }
''',
        "planner stale gate"
    )

    path.write_text(text)
    print(f"PATCHED: {path}")

def patch_execution(path):
    text = path.read_text()
    if "configuredReplayBootstrapCompletedUtcDate" in text:
        print(f"SKIP already patched: {path}")
        return

    helper = r'''std::optional<Timestamp> configuredReplayBootstrapCompletedUtcDate(const TimeHandlerConfig& config)
{
    const char* simulatedReference = std::getenv(TimeHandlerFactory::SIMULATED_REFERENCE_ENV);
    if (!simulatedReference || *simulatedReference == '\0')
        return std::nullopt;

    const auto referenceUtcDate = getCurrentUtcDate(config.simulated_reference_utc);
    return static_cast<Timestamp>(toYYYYMMDD(getPreviousDayDate(referenceUtcDate)));
}

'''

    text = replace_once(
        text,
        '''Timestamp newestCompletedUtcDate(const TimeHandler& timeHandler)
{
    const auto todaySys = std::chrono::floor<std::chrono::days>(timeHandler.getTime());
    const auto previousSys = todaySys - std::chrono::days{1};
    const std::chrono::year_month_day ymd{std::chrono::sys_days{previousSys}};
    const int value = int(ymd.year()) * 10000 +
        static_cast<int>(unsigned(ymd.month())) * 100 +
        static_cast<int>(unsigned(ymd.day()));
    return static_cast<Timestamp>(value);
}


''',
        '''Timestamp newestCompletedUtcDate(const TimeHandler& timeHandler)
{
    const auto todaySys = std::chrono::floor<std::chrono::days>(timeHandler.getTime());
    const auto previousSys = todaySys - std::chrono::days{1};
    const std::chrono::year_month_day ymd{std::chrono::sys_days{previousSys}};
    const int value = int(ymd.year()) * 10000 +
        static_cast<int>(unsigned(ymd.month())) * 100 +
        static_cast<int>(unsigned(ymd.day()));
    return static_cast<Timestamp>(value);
}

''' + helper + '\n',
        "execution helper"
    )

    text = replace_once(
        text,
        '''    const Options options_;
    TimeHandler time_handler_;
    std::vector<StrategyID> strategy_ids_;
''',
        '''    const Options options_;
    const TimeHandlerConfig time_config_;
    const TimeHandler time_handler_;
    const std::optional<Timestamp> replay_bootstrap_completed_date_;
    std::vector<StrategyID> strategy_ids_;
''',
        "execution members"
    )

    text = replace_once(
        text,
        '''        : options_(std::move(options)),
          time_handler_(TimeHandlerFactory::createFromEnvironment()),
          bus_(options_.nats_url),
''',
        '''        : options_(std::move(options)),
          time_config_(TimeHandlerFactory::loadConfigFromEnvironment()),
          time_handler_(TimeHandlerFactory::create(time_config_)),
          replay_bootstrap_completed_date_(configuredReplayBootstrapCompletedUtcDate(time_config_)),
          bus_(options_.nats_url),
''',
        "execution ctor"
    )

    text = replace_once(
        text,
        '''        const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
        if (pending_decision_->decision_timestamp != newestCompleted)
            return;
''',
        '''        const Timestamp newestCompleted = newestCompletedUtcDate(time_handler_);
        if (pending_decision_->decision_timestamp > newestCompleted)
            return;
        if (!replay_bootstrap_completed_date_.has_value() &&
            pending_decision_->decision_timestamp != newestCompleted)
            return;
''',
        "execution pending handoff gate"
    )

    text = replace_once(
        text,
        '''            if (update.completed_through < newestCompleted) {
                LG_INFO(
                    "service=execution-state event=stale_market_data_account_snapshot_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    update.completed_through,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }
''',
        '''            if (replay_bootstrap_completed_date_.has_value() &&
                update.completed_through < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=execution-state event=pre_bootstrap_market_data_account_snapshot_skipped timestamp={} bootstrap_completed_date={} disposition=ack",
                    update.completed_through,
                    *replay_bootstrap_completed_date_
                );
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                update.completed_through < newestCompleted) {
                LG_INFO(
                    "service=execution-state event=stale_market_data_account_snapshot_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    update.completed_through,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }
''',
        "execution market snapshot stale gate"
    )

    text = replace_once(
        text,
        '''            if (batch.decision_timestamp < newestCompleted) {
                LG_INFO(
                    "service=execution-state event=stale_decision_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    batch.decision_timestamp,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }
''',
        '''            if (replay_bootstrap_completed_date_.has_value() &&
                batch.decision_timestamp < *replay_bootstrap_completed_date_) {
                LG_INFO(
                    "service=execution-state event=pre_bootstrap_decision_skipped timestamp={} bootstrap_completed_date={} disposition=ack",
                    batch.decision_timestamp,
                    *replay_bootstrap_completed_date_
                );
                return DurableMessageDisposition::Ack;
            }
            if (!replay_bootstrap_completed_date_.has_value() &&
                batch.decision_timestamp < newestCompleted) {
                LG_INFO(
                    "service=execution-state event=stale_decision_skipped timestamp={} newest_completed_utc={} disposition=ack",
                    batch.decision_timestamp,
                    newestCompleted
                );
                return DurableMessageDisposition::Ack;
            }
''',
        "execution decision stale gate"
    )

    path.write_text(text)
    print(f"PATCHED: {path}")

def main():
    for p in FILES.values():
        if not p.exists():
            raise SystemExit(f"missing expected source file: {p}")

    backups = ROOT / "deploy/historical_replay/run/t18_event_time_patch_backup"
    backups.mkdir(parents=True, exist_ok=True)
    for key, p in FILES.items():
        shutil.copy2(p, backups / f"{key}.cpp.pre_t18_event_time")

    try:
        patch_risk(FILES["risk"])
        patch_planner(FILES["planner"])
        patch_execution(FILES["execution"])
    except Exception as exc:
        for key, p in FILES.items():
            backup = backups / f"{key}.cpp.pre_t18_event_time"
            if backup.exists():
                shutil.copy2(backup, p)
        raise SystemExit(f"FAIL: no partial patch retained; restored backups: {exc}")

    print("PASS: T18 historical replay event-time pipeline patch applied")
    print(f"backup={backups}")

if __name__ == "__main__":
    main()
