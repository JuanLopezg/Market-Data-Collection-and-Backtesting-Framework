#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "entry_exit_only_rebalance_policy.h"
#include "equal_weight_sizer.h"
#include "full_system_replay_runtime_v1.h"
#include "indicator_ranker.h"
#include "liquidity_universe.h"
#include "manual_control_mock_pipeline_v1.h"
#include "mock_accounting_fixed_point_v1.h"
#include "pureRSI.h"
#include "risk_constraints.h"
#include "time_handler.h"

using namespace FullSystemReplayV1;
using namespace MockVenueV1;
using namespace ManualControlV1;
using namespace VenueContracts::V1;

namespace {

struct Args {
    std::filesystem::path csv;
    std::filesystem::path mapping;
    std::filesystem::path durable;
    std::filesystem::path state_dir;
    std::string start_date = "2020-01-01";
    std::string end_date = "2020-04-16";
    double speed = 100000000.0;
    int ui_delay_ms = 250;
    int manual_idle_timeout_ms = 0;
    bool manual_once = false;
    bool exit_after_replay = false;
};

struct DayData {
    std::string date;
    unsigned int yyyymmdd = 0U;
    Timestamp open_timestamp = 0U;
    Timestamp close_timestamp = 0U;
    TimeHandler::TimePoint open_time{};
    TimeHandler::TimePoint close_time{};
    MarketSliceSnapshot slice;
    ExecutionReferencePrices open_prices;
};

struct EquityPoint {
    std::string label;
    double equity = 0.0;
};

struct ManualRequest {
    std::string request_id;
    std::string correlation_id;
    std::string actor;
    std::string request_hash;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    std::uint64_t reference_generation = 0U;
    std::map<std::string,double> targets;
    double cash_weight = 0.0;
};

struct ManualDisplay {
    bool ready = false;
    std::string blocker;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    std::string last_correlation_id;
    std::string last_status;
    std::string last_detail;
};

std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> fields;
    std::string field;
    std::stringstream stream(line);
    while (std::getline(stream, field, ','))
        fields.push_back(field);
    return fields;
}

Args parseArgs(int argc, char** argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing value for " + key);
            return argv[++i];
        };
        if (key == "--csv") args.csv = value();
        else if (key == "--mapping") args.mapping = value();
        else if (key == "--durable") args.durable = value();
        else if (key == "--state-dir") args.state_dir = value();
        else if (key == "--start") args.start_date = value();
        else if (key == "--end") args.end_date = value();
        else if (key == "--speed") args.speed = std::stod(value());
        else if (key == "--ui-delay-ms") args.ui_delay_ms = std::stoi(value());
        else if (key == "--manual-idle-timeout-ms") args.manual_idle_timeout_ms = std::stoi(value());
        else if (key == "--manual-once") args.manual_once = true;
        else if (key == "--exit-after-replay") args.exit_after_replay = true;
        else throw std::invalid_argument("unknown argument: " + key);
    }
    if (args.csv.empty() || args.mapping.empty() || args.durable.empty() || args.state_dir.empty())
        throw std::invalid_argument("--csv --mapping --durable --state-dir are required");
    if (!std::isfinite(args.speed) || args.speed <= 0.0)
        throw std::invalid_argument("speed must be finite and positive");
    if (args.ui_delay_ms < 0 || args.manual_idle_timeout_ms < 0)
        throw std::invalid_argument("delay/timeout must be non-negative");
    if (args.start_date > args.end_date)
        throw std::invalid_argument("start date must not be after end date");
    return args;
}

unsigned int dateInt(const std::string& date)
{
    if (date.size() != 10U || date[4] != '-' || date[7] != '-')
        throw std::invalid_argument("invalid YYYY-MM-DD date: " + date);
    return static_cast<unsigned int>(std::stoul(date.substr(0,4) + date.substr(5,2) + date.substr(8,2)));
}

TimeHandler::TimePoint dateTime(const std::string& date, int hour, int minute, int second)
{
    const int year = std::stoi(date.substr(0,4));
    const unsigned month = static_cast<unsigned>(std::stoi(date.substr(5,2)));
    const unsigned day = static_cast<unsigned>(std::stoi(date.substr(8,2)));
    const std::chrono::year_month_day ymd{std::chrono::year{year},std::chrono::month{month},std::chrono::day{day}};
    if (!ymd.ok()) throw std::invalid_argument("invalid calendar date: " + date);
    return std::chrono::sys_days{ymd} + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
}

std::unordered_map<std::string,std::string> loadMapping(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("unable to open source mapping: " + path.string());
    std::string line;
    if (!std::getline(in,line) || line != "source_symbol,canonical_asset")
        throw std::runtime_error("unexpected source mapping header");
    std::unordered_map<std::string,std::string> mapping;
    while (std::getline(in,line)) {
        if (line.empty()) continue;
        const auto fields = splitCsv(line);
        if (fields.size()!=2U || fields[0].empty() || fields[1].empty())
            throw std::runtime_error("invalid source mapping row");
        if (!mapping.emplace(fields[0],fields[1]).second)
            throw std::runtime_error("duplicate source symbol mapping: " + fields[0]);
        const auto* entry = findByCanonicalAssetExact(fields[1]);
        if (entry==nullptr || !entry->enabled)
            throw std::runtime_error("mapping target absent/disabled in MOCK catalog: " + fields[1]);
    }
    return mapping;
}

std::map<std::string,DayData> loadDays(const Args& args, const std::unordered_map<std::string,std::string>& mapping)
{
    std::ifstream in(args.csv);
    if (!in) throw std::runtime_error("unable to open historical CSV: " + args.csv.string());
    std::string line;
    if (!std::getline(in,line) || line != "date,symbol,open,high,low,close,volume")
        throw std::runtime_error("unexpected historical CSV header");
    std::map<std::string,DayData> days;
    while (std::getline(in,line)) {
        if (line.empty()) continue;
        const auto fields=splitCsv(line);
        if (fields.size()!=7U) throw std::runtime_error("invalid historical row");
        const std::string& date=fields[0];
        if (date<args.start_date) continue;
        if (date>args.end_date) break;
        const auto it=mapping.find(fields[1]);
        if (it==mapping.end()) throw std::runtime_error("UNMAPPED historical source symbol: "+fields[1]);
        DayData& day=days[date];
        if (day.date.empty()) {
            day.date=date; day.yyyymmdd=dateInt(date);
            day.open_timestamp=day.yyyymmdd*10U+1U; day.close_timestamp=day.yyyymmdd*10U+9U;
            day.open_time=dateTime(date,0,0,0); day.close_time=dateTime(date,23,59,59); day.slice.timestamp=day.close_timestamp;
        }
        MarketBarSnapshot v; v.coin=it->second;
        v.bar.open=std::stod(fields[2]); v.bar.high=std::stod(fields[3]); v.bar.low=std::stod(fields[4]); v.bar.close=std::stod(fields[5]); v.bar.volume=std::stod(fields[6]);
        day.slice.bars.push_back(v); day.open_prices.set(v.coin,v.bar.open);
    }
    if (days.empty()) throw std::runtime_error("simulation window contains no historical days");
    return days;
}

StrategySignalEngine makeStrategyEngine()
{
    StrategySignalPortfolio strategies;
    strategies.emplace_back(1U,std::make_unique<StrategyPureRSI>(10U,
        std::make_unique<TopNLiquidityUniverse>(IndicatorSpec{IndicatorKind::SMA,PriceField::Volume,25U,0U},20U,true,true),
        std::make_unique<IndicatorRanker>(IndicatorSpec{IndicatorKind::RSI,PriceField::Close,7U,0U},true,true),
        1000000U,7U,80.0,70.0));
    return StrategySignalEngine(std::move(strategies));
}

PortfolioRiskEngine makeRiskEngine()
{
    std::vector<PortfolioRiskStrategyConfig> configs;
    configs.emplace_back(1U,"Pure_RSI",1.0,std::make_unique<EqualWeightSizer>(0.10),RiskConstraints(1.50,1.50),std::make_unique<EntryExitOnlyRebalancePolicy>());
    return PortfolioRiskEngine(std::move(configs));
}

std::string jsonEscape(const std::string& input)
{
    std::string out; out.reserve(input.size()+8U);
    for (const char ch:input) {
        switch(ch){case '\\':out+="\\\\";break;case '"':out+="\\\"";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:out+=ch;break;}
    }
    return out;
}

std::string reconState(ReconciliationStateV1 state)
{
    switch(state){case ReconciliationStateV1::Clean:return "CLEAN";case ReconciliationStateV1::Blocked:return "BLOCKED";case ReconciliationStateV1::Pending:return "PENDING";}
    return "PENDING";
}

std::string issueKind(ReconciliationIssueKindV1 kind)
{
    return std::to_string(static_cast<int>(kind));
}

std::string accountingType(AccountingEventType type)
{
    return VenueContracts::V1::toString(type);
}

std::string ledgerKind(LedgerEntryKindV1 kind)
{
    switch(kind){case LedgerEntryKindV1::Fill:return "FILL";case LedgerEntryKindV1::TradingFee:return "TRADING_FEE";case LedgerEntryKindV1::Rebate:return "REBATE";case LedgerEntryKindV1::FundingPayment:return "FUNDING_PAYMENT";}
    return "UNKNOWN";
}

std::string sideText(Side side){return side==Side::Buy?"BUY":"SELL";}

void atomicWrite(const std::filesystem::path& path, const std::string& data)
{
    std::filesystem::create_directories(path.parent_path());
    const auto tmp=path.string()+".tmp";
    {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("open temp state failed");out<<data;out.flush();if(!out)throw std::runtime_error("write temp state failed");}
    std::filesystem::rename(tmp,path);
}

std::vector<MarketBarSnapshot> openPhaseBars(const DayData& day)
{
    auto bars=day.slice.bars;
    for(auto& b:bars){b.bar.high=b.bar.open;b.bar.low=b.bar.open;b.bar.close=b.bar.open;b.bar.volume=0.0;}
    return bars;
}

double cashTotal(const VenueContracts::V1::AccountSnapshot& account)
{
    for(const auto& b:account.balances) if(b.asset=="USD") return b.total;
    return account.equity;
}

double cashAvailable(const VenueContracts::V1::AccountSnapshot& account)
{
    for(const auto& b:account.balances) if(b.asset=="USD") return b.available;
    return account.equity-account.margin_used;
}

double quantityUnitsToDouble(const std::string& asset,std::int64_t units)
{
    const auto* entry=findByCanonicalAssetExact(asset);
    const auto* rules=entry==nullptr?nullptr:rulesForExact(*entry);
    if(rules==nullptr) return 0.0;
    long double factor=1.0L;for(std::size_t i=0;i<rules->size_scale;++i)factor*=10.0L;
    return static_cast<double>(static_cast<long double>(units)/factor);
}

double moneyUnitsToDouble(std::int64_t units){return signedScaledToDouble(units,kMockMoneyScaleV1);}
double moneyUnitsToDoubleU(std::uint64_t units){return static_cast<double>(units)/static_cast<double>(kMockMoneyFactorV1);}

std::string buildStateJson(
    MockExchangeAdapterV1& adapter,
    const std::string& phase,
    const std::string& date,
    Timestamp ts,
    int day_index,
    int day_count,
    int rows_processed,
    double speed,
    const std::vector<MarketBarSnapshot>& market,
    const std::vector<EquityPoint>& equity_history,
    const ManualDisplay& manual,
    std::uint64_t generation,
    const std::string& step57_fingerprint)
{
    const auto report=adapter.reconcile(ts);
    const auto account=adapter.chaos().runtime().account().accountSnapshot(ts);
    MockReconciliationLedgerParityV1 ledger_builder;
    const auto local=ledger_builder.buildLocalExpected(adapter.chaos().runtime().stream());
    const auto& orders=adapter.chaos().runtime().lifecycle().orders();
    const auto& stream=adapter.chaos().runtime().stream();

    std::vector<VenueContracts::V1::Fill> fills;
    std::vector<VenueContracts::V1::AccountingEvent> accounting;
    for(const auto& env:stream){
        if(std::holds_alternative<VenueContracts::V1::Fill>(env.event)) fills.push_back(std::get<VenueContracts::V1::Fill>(env.event));
        else if(std::holds_alternative<VenueContracts::V1::AccountingEvent>(env.event)) accounting.push_back(std::get<VenueContracts::V1::AccountingEvent>(env.event));
    }

    std::ostringstream out; out<<std::setprecision(15);
    out<<"{\n";
    out<<"\"schemaVersion\":1,\n\"generation\":"<<generation<<",\n\"phase\":\""<<jsonEscape(phase)<<"\",\n";
    out<<"\"historicalDate\":\""<<jsonEscape(date)<<"\",\n\"businessTimestamp\":"<<ts<<",\n";
    out<<"\"dayIndex\":"<<day_index<<",\n\"dayCount\":"<<day_count<<",\n\"sourceRowsProcessed\":"<<rows_processed<<",\n";
    out<<"\"venueId\":\"MOCK\",\n\"environment\":\"MOCK\",\n\"replaySpeed\":"<<speed<<",\n\"routeSafe\":"<<(adapter.canRouteNewSubmit()?"true":"false")<<",\n";
    out<<"\"reconciliation\":{\"state\":\""<<reconState(report.state)<<"\",\"localSequence\":"<<report.local_sequence<<",\"venueSequence\":"<<report.venue_sequence<<",\"ledgerHeadHash\":\""<<jsonEscape(report.ledger_head_hash)<<"\",\"issues\":[";
    for(std::size_t i=0;i<report.issues.size();++i){if(i)out<<',';const auto& x=report.issues[i];out<<"{\"kind\":\""<<issueKind(x.kind)<<"\",\"asset\":\""<<jsonEscape(x.asset)<<"\",\"orderId\":\""<<x.local_order_id<<"\",\"localValue\":\""<<jsonEscape(x.local_value)<<"\",\"venueValue\":\""<<jsonEscape(x.venue_value)<<"\",\"message\":\""<<jsonEscape(x.message)<<"\"}";}
    out<<"]},\n";
    out<<"\"account\":{\"equity\":"<<account.equity<<",\"marginUsed\":"<<account.margin_used<<",\"cashTotal\":"<<cashTotal(account)<<",\"cashAvailable\":"<<cashAvailable(account)<<",\"positions\":[";
    for(std::size_t i=0;i<account.positions.size();++i){if(i)out<<',';const auto& p=account.positions[i];out<<"{\"asset\":\""<<jsonEscape(p.instrument.market.canonical_asset)<<"\",\"quantity\":"<<p.signed_quantity<<",\"entryPrice\":"<<p.entry_price<<",\"markPrice\":"<<p.mark_price<<",\"unrealizedPnl\":"<<p.unrealized_pnl<<",\"leverage\":"<<p.leverage<<"}";}
    out<<"]},\n";
    out<<"\"orders\":[";
    bool first=true;for(const auto& pair:orders){if(!first)out<<',';first=false;const auto& o=pair.second;out<<"{\"orderId\":\""<<pair.first<<"\",\"strategyId\":"<<o.intent.strategy_id<<",\"asset\":\""<<jsonEscape(o.intent.instrument.market.canonical_asset)<<"\",\"side\":\""<<sideText(o.intent.side)<<"\",\"quantity\":"<<o.intent.quantity<<",\"filledQuantity\":"<<o.cumulative_filled_quantity<<",\"remainingQuantity\":"<<o.remaining_quantity<<",\"limitPrice\":"<<o.intent.limit_price<<",\"status\":\""<<VenueContracts::V1::toString(o.status)<<"\",\"nativeOrderId\":\""<<jsonEscape(o.native_references.native_order_id)<<"\",\"createdAt\":"<<o.intent.created_at<<",\"activeFrom\":"<<o.intent.active_from<<"}";}
    out<<"],\n\"fills\":[";
    for(std::size_t i=0;i<fills.size();++i){if(i)out<<',';const auto& f=fills[i];out<<"{\"fillId\":\""<<jsonEscape(f.native_references.native_fill_id)<<"\",\"orderId\":\""<<f.local_order_id<<"\",\"strategyId\":"<<f.strategy_id<<",\"timestamp\":"<<f.timestamp<<",\"asset\":\""<<jsonEscape(f.instrument.market.canonical_asset)<<"\",\"side\":\""<<sideText(f.side)<<"\",\"quantity\":"<<f.quantity<<",\"price\":"<<f.price<<"}";}
    out<<"],\n\"accounting\":[";
    for(std::size_t i=0;i<accounting.size();++i){if(i)out<<',';const auto& e=accounting[i];double amount=0.0;try{amount=std::stod(e.amount);}catch(...){amount=0.0;}out<<"{\"correlationId\":\""<<jsonEscape(e.correlation_id)<<"\",\"timestamp\":"<<e.timestamp<<",\"type\":\""<<accountingType(e.type)<<"\",\"amount\":"<<amount<<",\"asset\":\""<<jsonEscape(e.market?e.market->canonical_asset:std::string{})<<"\",\"nativeFillId\":\""<<jsonEscape(e.native_references.native_fill_id)<<"\"}";}
    out<<"],\n\"market\":[";
    std::vector<MarketBarSnapshot> sorted_market=market;std::sort(sorted_market.begin(),sorted_market.end(),[](const auto&a,const auto&b){return a.coin<b.coin;});
    for(std::size_t i=0;i<sorted_market.size();++i){if(i)out<<',';const auto& b=sorted_market[i];out<<"{\"asset\":\""<<jsonEscape(b.coin)<<"\",\"open\":"<<b.bar.open<<",\"high\":"<<b.bar.high<<",\"low\":"<<b.bar.low<<",\"close\":"<<b.bar.close<<",\"volume\":"<<b.bar.volume<<"}";}
    out<<"],\n\"equityHistory\":[";
    for(std::size_t i=0;i<equity_history.size();++i){if(i)out<<',';out<<"{\"label\":\""<<jsonEscape(equity_history[i].label)<<"\",\"equity\":"<<equity_history[i].equity<<"}";}
    out<<"],\n\"ledger\":{\"valid\":"<<(local.ledger.valid?"true":"false")<<",\"error\":\""<<jsonEscape(local.ledger.error)<<"\",\"headHash\":\""<<jsonEscape(local.ledger.head_hash)<<"\",\"entries\":[";
    for(std::size_t i=0;i<local.ledger.entries.size();++i){if(i)out<<',';const auto& e=local.ledger.entries[i];std::string side="";if(e.kind==LedgerEntryKindV1::Fill)side=e.position_delta_units>=0?"BUY":"SELL";out<<"{\"sequence\":"<<e.stream_sequence<<",\"kind\":\""<<ledgerKind(e.kind)<<"\",\"entryId\":\""<<jsonEscape(e.entry_id)<<"\",\"eventTime\":"<<e.event_time<<",\"asset\":\""<<jsonEscape(e.asset)<<"\",\"orderId\":\""<<e.local_order_id<<"\",\"strategyId\":"<<e.strategy_id<<",\"nativeFillId\":\""<<jsonEscape(e.native_fill_id)<<"\",\"side\":\""<<side<<"\",\"positionDelta\":"<<quantityUnitsToDouble(e.asset,e.position_delta_units)<<",\"cashDelta\":"<<moneyUnitsToDouble(e.cash_delta_units)<<",\"realizedPnl\":"<<moneyUnitsToDouble(e.realized_pnl_units)<<",\"grossNotional\":"<<moneyUnitsToDoubleU(e.gross_notional_units)<<",\"entryHash\":\""<<jsonEscape(e.entry_hash)<<"\"}";}
    out<<"]},\n";
    out<<"\"routableAssets\":[";for(std::size_t i=0;i<sorted_market.size();++i){if(i)out<<',';out<<"\""<<jsonEscape(sorted_market[i].coin)<<"\"";}out<<"],\n";
    out<<"\"manual\":{\"ready\":"<<(manual.ready?"true":"false")<<",\"blocker\":\""<<jsonEscape(manual.blocker)<<"\",\"decisionTimestamp\":"<<manual.decision_timestamp<<",\"executionTimestamp\":"<<manual.execution_timestamp<<",\"lastCorrelationId\":\""<<jsonEscape(manual.last_correlation_id)<<"\",\"lastStatus\":\""<<jsonEscape(manual.last_status)<<"\",\"lastDetail\":\""<<jsonEscape(manual.last_detail)<<"\"},\n";
    out<<"\"economicFingerprint\":\""<<jsonEscape(adapter.chaos().runtime().account().economicFingerprint())<<"\",\n\"streamFingerprint\":\""<<jsonEscape(adapter.chaos().runtime().streamFingerprint())<<"\",\n";
    out<<"\"step56RuntimeFingerprint\":\""<<kStep56RuntimeFingerprint<<"\",\n\"step57ManualFingerprint\":\""<<jsonEscape(step57_fingerprint)<<"\"\n}";
    return out.str();
}

void emitState(MockExchangeAdapterV1& adapter,const Args& args,const std::string& phase,const std::string& date,Timestamp ts,int day_index,int day_count,int rows_processed,const std::vector<MarketBarSnapshot>& market,std::vector<EquityPoint>& history,const ManualDisplay& manual,std::uint64_t& generation,const std::string& step57_fp,bool append_equity)
{
    ++generation;
    if(append_equity){const auto account=adapter.chaos().runtime().account().accountSnapshot(ts);history.push_back({date,account.equity});if(history.size()>80U)history.erase(history.begin());}
    atomicWrite(args.state_dir/"state.json",buildStateJson(adapter,phase,date,ts,day_index,day_count,rows_processed,args.speed,market,history,manual,generation,step57_fp));
    std::cout<<"STEP58_STATE generation="<<generation<<" phase="<<phase<<" date="<<date<<" ts="<<ts<<"\n"<<std::flush;
    if(args.ui_delay_ms>0)std::this_thread::sleep_for(std::chrono::milliseconds(args.ui_delay_ms));
}

std::string manualStatus(ManualRouteStatusV1 status)
{
    switch(status){case ManualRouteStatusV1::Noop:return "NOOP";case ManualRouteStatusV1::Submitted:return "SUBMITTED";case ManualRouteStatusV1::RiskRejected:return "RISK_REJECTED";case ManualRouteStatusV1::ReconciliationBlocked:return "RECONCILIATION_BLOCKED";case ManualRouteStatusV1::VenueRejected:return "VENUE_REJECTED";case ManualRouteStatusV1::AmbiguousOrPending:return "AMBIGUOUS_OR_PENDING";}
    return "UNKNOWN";
}

ManualRequest parseManualRequest(const std::filesystem::path& path)
{
    std::ifstream in(path);if(!in)throw std::runtime_error("open manual request failed");
    std::string line;if(!std::getline(in,line)||line!="STEP58_MANUAL_V1")throw std::runtime_error("invalid manual request magic");
    ManualRequest r;
    while(std::getline(in,line)){
        if(line.empty())continue;
        const auto pos=line.find('=');if(pos==std::string::npos)throw std::runtime_error("invalid manual request line");
        const std::string key=line.substr(0,pos), value=line.substr(pos+1);
        if(key=="request_id")r.request_id=value;else if(key=="correlation_id")r.correlation_id=value;else if(key=="actor")r.actor=value;else if(key=="request_hash")r.request_hash=value;
        else if(key=="decision_timestamp")r.decision_timestamp=static_cast<Timestamp>(std::stoull(value));else if(key=="execution_timestamp")r.execution_timestamp=static_cast<Timestamp>(std::stoull(value));else if(key=="reference_generation")r.reference_generation=std::stoull(value);
        else if(key=="cash")r.cash_weight=std::stod(value);else if(key=="target"){const auto comma=value.find(',');if(comma==std::string::npos)throw std::runtime_error("invalid target row");const std::string asset=value.substr(0,comma);const double weight=std::stod(value.substr(comma+1));if(!r.targets.emplace(asset,weight).second)throw std::runtime_error("duplicate manual target");}
        else throw std::runtime_error("unknown manual request field: "+key);
    }
    return r;
}

std::optional<std::filesystem::path> nextRequest(const std::filesystem::path& dir)
{
    if(!std::filesystem::exists(dir))return std::nullopt;
    std::vector<std::filesystem::path> files;
    for(const auto& e:std::filesystem::directory_iterator(dir)) if(e.is_regular_file()&&e.path().extension()==".request")files.push_back(e.path());
    if(files.empty()) return std::nullopt;
    std::sort(files.begin(),files.end());
    return files.front();
}

ExecutionReferencePrices lastClosePrices(const DayData& day)
{
    ExecutionReferencePrices p;for(const auto& b:day.slice.bars)p.set(b.coin,b.bar.close);return p;
}

std::vector<MarketBarSnapshot> flatManualBars(const DayData& day,const std::map<std::string,double>& targets)
{
    std::map<std::string,MarketBarSnapshot> by;
    for(const auto& b:day.slice.bars)by.emplace(b.coin,b);
    std::vector<MarketBarSnapshot> out;
    for(const auto& [asset,weight]:targets){(void)weight;const auto it=by.find(asset);if(it==by.end())throw std::runtime_error("manual target lacks final historical bar: "+asset);MarketBarSnapshot b=it->second;b.bar.open=b.bar.close;b.bar.high=b.bar.close;b.bar.low=b.bar.close;b.bar.volume=1000000000.0;out.push_back(b);}
    return out;
}

} // namespace

int main(int argc,char** argv)
{
    try {
        const Args args=parseArgs(argc,argv);
        const auto mapping=loadMapping(args.mapping);
        const auto days=loadDays(args,mapping);
        std::filesystem::create_directories(args.state_dir/"requests");
        std::filesystem::create_directories(args.state_dir/"processed");
        std::error_code ignored;std::filesystem::remove_all(args.durable,ignored);std::filesystem::create_directories(args.durable);
        for(const auto& e:std::filesystem::directory_iterator(args.state_dir/"requests"))if(e.is_regular_file())std::filesystem::remove(e.path(),ignored);

        StrategySignalEngine strategy=makeStrategyEngine();PortfolioRiskEngine risk=makeRiskEngine();NotionalOrderPlannerEngine planner;
        MockChaosConfigV1 chaos;chaos.auto_submit_faults=false;chaos.submit_limit=100000U;chaos.cancel_limit=100000U;chaos.modify_limit=100000U;chaos.market_source_limit=1000000U;chaos.reconcile_limit=100000U;chaos.stream_read_limit=1000000U;
        MockExchangeAdapterV1 adapter(args.durable,chaos);
        FullSystemReplayRuntimeV1 replay(strategy,risk,planner,{1U},adapter);

        const auto first=days.begin()->second.open_time;const auto real_reference=TimeHandler::Clock::now();const auto bias=first-real_reference;TimeHandler release_clock(args.speed,bias,real_reference);
        std::vector<EquityPoint> history;std::uint64_t generation=0U;int day_index=0;int rows_processed=0;ManualDisplay manual;
        constexpr const char* step57_fp="65c0a7b418d7f3873f38f6a1daa87d5d915e4a254452865b79a69df24c3dc5f0";
        for(const auto& pair:days){
            const DayData& day=pair.second;++day_index;
            release_clock.sleepUntil(day.open_time);replay.onExecutionOpen(day.open_timestamp,day.open_prices);
            emitState(adapter,args,"REPLAY_OPEN",day.date,day.open_timestamp,day_index,static_cast<int>(days.size()),rows_processed,openPhaseBars(day),history,manual,generation,step57_fp,false);
            release_clock.sleepUntil(day.close_time);replay.onClosedSlice(day.slice);rows_processed+=static_cast<int>(day.slice.bars.size());
            emitState(adapter,args,"REPLAY_CLOSE",day.date,day.close_timestamp,day_index,static_cast<int>(days.size()),rows_processed,day.slice.bars,history,manual,generation,step57_fp,true);
        }

        const DayData& last=days.rbegin()->second;const Timestamp final_ts=last.close_timestamp;
        const auto evidence=replay.finalizeEvidence(final_ts);(void)evidence;
        const auto final_account=adapter.chaos().runtime().account().accountSnapshot(final_ts);
        manual.ready=final_account.positions.empty()&&adapter.reconcile(final_ts).clean()&&adapter.canRouteNewSubmit();
        manual.blocker=manual.ready?"":"MANUAL_SESSION_REQUIRES_FLAT_CLEAN_HANDOFF";
        manual.decision_timestamp=final_ts+10U;manual.execution_timestamp=final_ts+11U;
        emitState(adapter,args,manual.ready?"MANUAL_READY":"REPLAY_COMPLETE",last.date,final_ts,day_index,static_cast<int>(days.size()),rows_processed,last.slice.bars,history,manual,generation,step57_fp,false);
        std::cout<<"STEP58_REPLAY_COMPLETE streamEvents="<<adapter.chaos().runtime().stream().size()<<" manualReady="<<(manual.ready?"true":"false")<<"\n"<<std::flush;
        if(args.exit_after_replay)return manual.ready?0:3;
        if(!manual.ready)return 3;

        // Historical Strategy runtime is complete. The event handler may now be handed
        // to the already-validated Step57 manual pipeline without two owners competing.
        ManualControlMockPipelineV1 manual_pipeline(adapter);
        const auto closes=lastClosePrices(last);const auto opens=closes;
        const auto request_dir=args.state_dir/"requests";const auto processed_dir=args.state_dir/"processed";
        const auto idle_start=std::chrono::steady_clock::now();
        while(true){
            const auto next=nextRequest(request_dir);
            if(!next){
                if(args.manual_idle_timeout_ms>0&&std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-idle_start).count()>=args.manual_idle_timeout_ms)return 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;
            }
            ManualRequest req=parseManualRequest(*next);
            manual.last_correlation_id=req.correlation_id;
            if(req.reference_generation!=generation||req.decision_timestamp!=manual.decision_timestamp||req.execution_timestamp!=manual.execution_timestamp){
                manual.last_status="STALE_REJECTED";manual.last_detail="request generation/timestamps no longer match current MANUAL_READY snapshot";
            } else {
                ManualTargetIntentV1 intent;intent.request_id=req.request_id;intent.correlation_id=req.correlation_id;intent.actor=req.actor;intent.request_hash=req.request_hash;intent.decision_timestamp=req.decision_timestamp;intent.execution_timestamp=req.execution_timestamp;intent.asset_weights=req.targets;intent.cash_weight=req.cash_weight;
                const auto result=manual_pipeline.route(intent,closes,opens);manual.last_status=manualStatus(result.status);manual.last_detail=result.reason;
                if(result.status==ManualRouteStatusV1::Submitted){
                    const auto bars=flatManualBars(last,req.targets);const Timestamp fill_ts=req.execution_timestamp+1U;
                    for(const auto& b:bars){MarketBarObservationV1 obs;obs.canonical_asset=b.coin;obs.event_time=fill_ts;obs.bar=b.bar;const auto source=manual_pipeline.processMarketBar(obs);if(source.status!=ChaosStatusV1::Delivered&&source.status!=ChaosStatusV1::DuplicateIgnored)throw std::runtime_error("manual fill bar rejected");}
                    adapter.reconcile(fill_ts);manual.last_detail += "; deterministic MOCK bar processed and reconciliation refreshed";
                }
            }
            manual.decision_timestamp+=100U;manual.execution_timestamp+=100U;manual.ready=adapter.reconcile(manual.execution_timestamp-1U).clean()&&adapter.canRouteNewSubmit();if(!manual.ready)manual.blocker="RECONCILIATION_NOT_CLEAN_AFTER_MANUAL_COMMAND";else manual.blocker="";
            const Timestamp display_ts=manual.execution_timestamp-1U;
            emitState(adapter,args,manual.ready?"MANUAL_READY":"MANUAL_BLOCKED",last.date,display_ts,day_index,static_cast<int>(days.size()),rows_processed,last.slice.bars,history,manual,generation,step57_fp,false);
            std::filesystem::rename(*next,processed_dir/next->filename());
            if(args.manual_once)return manual.ready?0:4;
        }
    } catch(const std::exception& e){std::cerr<<"STEP58_FATAL="<<e.what()<<"\n";return 2;}
}
