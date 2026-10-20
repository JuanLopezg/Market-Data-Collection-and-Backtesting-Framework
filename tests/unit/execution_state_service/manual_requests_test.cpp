// Manual Control request files, limits and time rules
// (tests/specs/execution_state_service.md, Part C).

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "manual_requests.h"
#include "test_support.h"

namespace fs = std::filesystem;

namespace {

const std::string VALID_REQUEST =
    "MANUAL_REQUEST_V1\n"
    "request_id=manual-0123456789abcdef\n"
    "actor=operator / OPERATOR\n"
    "request_hash=sha256:abc\n"
    "created_at=2026-10-14T12:00:00Z\n"
    "target=BTCUSDT,0.400000000000\n"
    "target=ETHUSDT,0.250000000000\n";

class ManualFolder : public ::testing::Test {
protected:
    test_support::TempDir dir{"manual-requests"};
    fs::path requests;
    fs::path processed;

    void SetUp() override
    {
        requests = dir.path() / "requests";
        processed = dir.path() / "processed";
        fs::create_directories(requests);
        fs::create_directories(processed);
    }

    fs::path writeRequest(const std::string& name, const std::string& content)
    {
        const fs::path file = requests / name;
        test_support::writeFile(file, content);
        return file;
    }

    // Replaces the line starting with `key=` (or appends `line` when key is empty).
    static std::string withLine(const std::string& key, const std::string& line)
    {
        std::string result;
        std::size_t start = 0;
        bool replaced = false;
        while (start < VALID_REQUEST.size()) {
            const std::size_t end = VALID_REQUEST.find('\n', start);
            const std::string current = VALID_REQUEST.substr(start, end - start);
            if (!key.empty() && current.rfind(key + "=", 0) == 0) {
                if (!line.empty())
                    result += line + "\n";
                replaced = true;
            }
            else
                result += current + "\n";
            start = end + 1;
        }
        if (!replaced && !line.empty())
            result += line + "\n";
        return result;
    }

    void expectInvalid(const std::string& content, const std::string& why)
    {
        const fs::path file = writeRequest("0001-manual-bad.request", content);
        const ManualRequestFolder folder(dir.path());
        EXPECT_THROW(folder.read(file), std::invalid_argument) << why;
    }
};

std::chrono::system_clock::time_point utc(int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    using namespace std::chrono;
    return sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}} + hours(hour) +
           minutes(minute) + seconds(second);
}

std::vector<std::string> linesOf(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

bool hasLine(const std::string& text, const std::string& line)
{
    for (const std::string& current : linesOf(text))
        if (current == line)
            return true;
    return false;
}

bool hasLineStarting(const std::string& text, const std::string& prefix)
{
    for (const std::string& current : linesOf(text))
        if (current.rfind(prefix, 0) == 0)
            return true;
    return false;
}

} // namespace


// ---------------------------------------------------------------- C1 / C2

TEST_F(ManualFolder, C1_FolderNeedsRequestsAndProcessed)
{
    test_support::TempDir empty("manual-empty");
    EXPECT_THROW(ManualRequestFolder{empty.path()}, std::invalid_argument);
    fs::create_directories(empty.path() / "requests");
    EXPECT_THROW(ManualRequestFolder{empty.path()}, std::invalid_argument);
    fs::create_directories(empty.path() / "processed");
    EXPECT_NO_THROW(ManualRequestFolder{empty.path()});
}

TEST_F(ManualFolder, C2_OldestRequestFirstAndOtherFilesIgnored)
{
    const ManualRequestFolder folder(dir.path());
    EXPECT_FALSE(folder.nextRequest().has_value());

    writeRequest("1000-manual-aaaaaaaa.request", VALID_REQUEST);
    writeRequest("0999-manual-bbbbbbbb.request", VALID_REQUEST);
    writeRequest("0001-manual-cccccccc.request.tmp", VALID_REQUEST);
    writeRequest("0002-manual-dddddddd", VALID_REQUEST);
    const auto next = folder.nextRequest();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->filename(), "0999-manual-bbbbbbbb.request");
}

// ---------------------------------------------------------------- C3

TEST_F(ManualFolder, C3_ValidRequestIsReadCompletely)
{
    const fs::path file = writeRequest("1000-manual-0123456789abcdef.request", VALID_REQUEST);
    const ManualRequestFolder folder(dir.path());
    const ManualRequest request = folder.read(file);
    EXPECT_EQ(request.file, file);
    EXPECT_EQ(request.request_id, "manual-0123456789abcdef");
    EXPECT_EQ(request.actor, "operator / OPERATOR");
    EXPECT_EQ(request.request_hash, "sha256:abc");
    EXPECT_EQ(request.created_at, "2026-10-14T12:00:00Z");
    ASSERT_EQ(request.weights.size(), 2u);
    EXPECT_DOUBLE_EQ(request.weights.at("BTCUSDT"), 0.40);
    EXPECT_DOUBLE_EQ(request.weights.at("ETHUSDT"), 0.25);
    EXPECT_EQ(request.raw_lines, linesOf(VALID_REQUEST.substr(0, VALID_REQUEST.size() - 1)));
}

TEST_F(ManualFolder, C3_CrlfAndBlankLinesAreTolerated)
{
    std::string crlf;
    for (const std::string& line : linesOf(VALID_REQUEST.substr(0, VALID_REQUEST.size() - 1)))
        crlf += line + "\r\n\r\n";
    const fs::path file = writeRequest("1000-manual-0123456789abcdef.request", crlf);
    const ManualRequestFolder folder(dir.path());
    const ManualRequest request = folder.read(file);
    EXPECT_EQ(request.request_id, "manual-0123456789abcdef");
    EXPECT_EQ(request.weights.size(), 2u);
    EXPECT_EQ(request.raw_lines.size(), 7u);
    for (const std::string& line : request.raw_lines)
        EXPECT_EQ(line.find('\r'), std::string::npos);
}

TEST_F(ManualFolder, C3_RequestWithoutTargetsMeansAllCash)
{
    const std::string allCash = withLine("target", "");
    const fs::path file = writeRequest("1000-manual-0123456789abcdef.request", allCash);
    const ManualRequestFolder folder(dir.path());
    const ManualRequest request = folder.read(file);
    EXPECT_TRUE(request.weights.empty());
}

// ---------------------------------------------------------------- C4

TEST_F(ManualFolder, C4_HeaderIsRequired)
{
    expectInvalid("MANUAL_REQUEST_V2\n" + VALID_REQUEST.substr(VALID_REQUEST.find('\n') + 1), "wrong header");
    expectInvalid(VALID_REQUEST.substr(VALID_REQUEST.find('\n') + 1), "missing header");
    expectInvalid("", "empty file");
    expectInvalid("\n\n", "only blank lines");
}

TEST_F(ManualFolder, C4_LineWithoutEqualsAndUnknownFieldsAreRefused)
{
    expectInvalid(VALID_REQUEST + "garbage\n", "line without '='");
    expectInvalid(VALID_REQUEST + "leverage=10\n", "unknown field");
}

TEST_F(ManualFolder, C4_RequestIdMustMatchThePattern)
{
    for (const char* id : {"manual-0123", "manual-0123456789ABCDEF", "request-0123456789abcdef", "",
                                  "manual-0123456789abcdeg"})
        expectInvalid(withLine("request_id", std::string("request_id=") + id), std::string("request id '") + id + "'");
}

TEST_F(ManualFolder, C4_ActorHashAndCreatedAtAreRequired)
{
    expectInvalid(withLine("actor", ""), "no actor");
    expectInvalid(withLine("request_hash", ""), "no hash");
    expectInvalid(withLine("created_at", ""), "no created_at");
    expectInvalid(withLine("actor", "actor="), "empty actor");
}

TEST_F(ManualFolder, C4_AssetNamesMustBeUpperCaseSymbols)
{
    for (const char* asset : {"btcusdt", "B", "BTC-USDT", "BTC USDT", "ABCDEFGHIJABCDEFGHIJABCDEFGHIJX"})
        expectInvalid(VALID_REQUEST + "target=" + asset + ",0.1\n", std::string("asset '") + asset + "'");
}

TEST_F(ManualFolder, C4_WeightMustBeAFiniteNonNegativeNumber)
{
    for (const char* weight : {"-0.1", "abc", "0.4x", "nan", "inf", ""})
        expectInvalid(VALID_REQUEST + "target=SOLUSDT," + weight + "\n", std::string("weight '") + weight + "'");
    expectInvalid(VALID_REQUEST + "target=SOLUSDT\n", "target without weight");
}

TEST_F(ManualFolder, C4_OutOfRangeWeightIsAnInvalidRequest)
{
    expectInvalid(VALID_REQUEST + "target=SOLUSDT,1e400\n", "weight 1e400");
}

TEST_F(ManualFolder, C4_SameAssetTwiceIsRefused)
{
    expectInvalid(VALID_REQUEST + "target=BTCUSDT,0.1\n", "BTCUSDT twice");
}

TEST_F(ManualFolder, C4_AtMostOneHundredTargets)
{
    std::string hundred = withLine("target", "");
    std::string hundredOne;
    for (int i = 0; i < 100; ++i)
        hundred += "target=A" + std::to_string(1000 + i) + ",0.001\n";
    {
        const fs::path file = writeRequest("0001-manual-ok.request", hundred);
        const ManualRequestFolder folder(dir.path());
        EXPECT_EQ(folder.read(file).weights.size(), 100u);
    }
    hundredOne = hundred + "target=B9999,0.001\n";
    expectInvalid(hundredOne, "101 targets");
}

TEST_F(ManualFolder, C4_FileOver64KbIsRefused)
{
    expectInvalid(VALID_REQUEST + "actor=" + std::string(70 * 1024, 'x') + "\n", "70 KB file");
}

// ---------------------------------------------------------------- C5

TEST_F(ManualFolder, C5_ExecutedRecordIsWrittenAndRequestRemoved)
{
    const std::string name = "1000-manual-0123456789abcdef.request";
    const fs::path file = writeRequest(name, VALID_REQUEST);
    const ManualRequestFolder folder(dir.path());
    const ManualRequest request = folder.read(file);
    EXPECT_FALSE(folder.alreadyProcessed(file));

    ManualOutcome outcome;
    outcome.executed = true;
    outcome.detail = "2 orders sent\nline two";
    outcome.execution_day = 20261014;
    outcome.first_order = 501;
    outcome.last_order = 502;
    folder.finish(file, request.raw_lines, outcome);

    EXPECT_FALSE(fs::exists(file));
    EXPECT_TRUE(folder.alreadyProcessed(file));
    const std::string record = test_support::readFile(processed / name);
    const std::vector<std::string> lines = linesOf(record);
    ASSERT_GE(lines.size(), request.raw_lines.size());
    for (std::size_t i = 0; i < request.raw_lines.size(); ++i)
        EXPECT_EQ(lines[i], request.raw_lines[i]);
    EXPECT_TRUE(hasLine(record, "result=EXECUTED"));
    EXPECT_TRUE(hasLine(record, "detail=2 orders sent line two"));
    EXPECT_TRUE(hasLineStarting(record, "processed_at="));
    EXPECT_TRUE(hasLine(record, "execution_day=20261014"));
    EXPECT_TRUE(hasLine(record, "orders=501-502"));
}

TEST_F(ManualFolder, C5_RejectedRecordHasNoOrders)
{
    const std::string name = "1000-manual-0123456789abcdef.request";
    const fs::path file = writeRequest(name, VALID_REQUEST);
    const ManualRequestFolder folder(dir.path());
    ManualOutcome outcome;
    outcome.detail = "BTCUSDT is above the risk limit per asset";
    folder.finish(file, folder.read(file).raw_lines, outcome);

    const std::string record = test_support::readFile(processed / name);
    EXPECT_TRUE(hasLine(record, "result=REJECTED"));
    EXPECT_TRUE(hasLine(record, "detail=BTCUSDT is above the risk limit per asset"));
    EXPECT_FALSE(hasLineStarting(record, "orders="));
    EXPECT_FALSE(hasLineStarting(record, "execution_day="));
    EXPECT_FALSE(fs::exists(file));
}

TEST_F(ManualFolder, C5_RedeliveredCopyIsRecognisedAsProcessed)
{
    const std::string name = "1000-manual-0123456789abcdef.request";
    const fs::path file = writeRequest(name, VALID_REQUEST);
    const ManualRequestFolder folder(dir.path());
    ManualOutcome outcome;
    outcome.executed = true;
    folder.finish(file, folder.read(file).raw_lines, outcome);

    writeRequest(name, VALID_REQUEST); // the same request appears again
    EXPECT_TRUE(folder.alreadyProcessed(requests / name));
}

TEST_F(ManualFolder, C5_RecordIsNotWorldReadable)
{
    const std::string name = "1000-manual-0123456789abcdef.request";
    const fs::path file = writeRequest(name, VALID_REQUEST);
    const ManualRequestFolder folder(dir.path());
    folder.finish(file, folder.read(file).raw_lines, ManualOutcome{});
    const fs::perms perms = fs::status(processed / name).permissions();
    EXPECT_EQ(perms & fs::perms::others_all, fs::perms::none);
    EXPECT_NE(perms & fs::perms::group_read, fs::perms::none);
    EXPECT_NE(perms & fs::perms::owner_read, fs::perms::none);
}

// ---------------------------------------------------------------- C6

TEST_F(ManualFolder, C6_NoMarkerNoResync)
{
    const ManualRequestFolder folder(dir.path());
    EXPECT_FALSE(folder.resyncForDay(20261015));
}

TEST_F(ManualFolder, C6_PendingMarkerBindsToTheFirstDayAsked)
{
    const ManualRequestFolder folder(dir.path());
    folder.markResyncPending();
    EXPECT_TRUE(folder.resyncForDay(20261015));
    EXPECT_TRUE(folder.resyncForDay(20261015)) << "redelivered snapshot of the same day";
    EXPECT_FALSE(folder.resyncForDay(20261016));
}

TEST_F(ManualFolder, C6_MarkerSurvivesARestart)
{
    {
        const ManualRequestFolder folder(dir.path());
        folder.markResyncPending();
    }
    const ManualRequestFolder restarted(dir.path());
    EXPECT_TRUE(restarted.resyncForDay(20261015));
    const ManualRequestFolder again(dir.path());
    EXPECT_TRUE(again.resyncForDay(20261015));
    EXPECT_FALSE(again.resyncForDay(20261016));
}

// ---------------------------------------------------------------- C7

namespace {

std::string strategyJson(int id, double allocation, double gross, double asset)
{
    return "{\"id\": " + std::to_string(id) + ", \"allocation_weight\": " + std::to_string(allocation) +
           ", \"risk\": {\"max_gross_leverage\": " + std::to_string(gross) +
           ", \"max_asset_weight\": " + std::to_string(asset) + "}}";
}

} // namespace

TEST(ManualRiskConfig, C7_StrictestLimitsAcrossStrategies)
{
    test_support::TempDir dir("manual-risk");
    const fs::path path = dir / "portfolio.json";
    test_support::writeFile(path, "{\"strategies\": [" + strategyJson(1, 0.6, 1.5, 0.5) + ", " +
                                      strategyJson(2, 0.4, 1.0, 0.8) + "]}");
    const ManualRiskConfig config = loadManualRiskConfig(path.string(), {1, 2});
    EXPECT_DOUBLE_EQ(config.max_gross_leverage, 1.0);
    EXPECT_DOUBLE_EQ(config.max_asset_weight, 0.5);
    ASSERT_EQ(config.allocation.size(), 2u);
    EXPECT_DOUBLE_EQ(config.allocation.at(1), 0.6);
    EXPECT_DOUBLE_EQ(config.allocation.at(2), 0.4);
}

TEST(ManualRiskConfig, C7_MissingStrategyIsRefused)
{
    test_support::TempDir dir("manual-risk");
    const fs::path path = dir / "portfolio.json";
    test_support::writeFile(path, "{\"strategies\": [" + strategyJson(1, 1.0, 1.5, 0.5) + "]}");
    EXPECT_THROW(loadManualRiskConfig(path.string(), {1, 2}), std::invalid_argument);
}

TEST(ManualRiskConfig, C7_InvalidLimitsAreRefused)
{
    test_support::TempDir dir("manual-risk");
    const fs::path path = dir / "portfolio.json";
    for (const std::string& strategy : {strategyJson(1, 1.0, 0.0, 0.5), strategyJson(1, 1.0, 1.5, 0.0),
                                        strategyJson(1, 1.0, -1.0, 0.5), strategyJson(1, -0.1, 1.5, 0.5)}) {
        test_support::writeFile(path, "{\"strategies\": [" + strategy + "]}");
        EXPECT_THROW(loadManualRiskConfig(path.string(), {1}), std::invalid_argument) << strategy;
    }
}

TEST(ManualRiskConfig, C7_UnreadableFileIsAnError)
{
    test_support::TempDir dir("manual-risk");
    EXPECT_ANY_THROW(loadManualRiskConfig((dir / "missing.json").string(), {1}));
}

TEST(ManualRiskConfig, C7_ProductionProfileLoads)
{
    const ManualRiskConfig config =
        loadManualRiskConfig(test_support::sourcePath("config/portfolio/pure_rsi_equal_weight.json").string(), {1});
    EXPECT_DOUBLE_EQ(config.allocation.at(1), 1.0);
    EXPECT_DOUBLE_EQ(config.max_gross_leverage, 1.5);
    EXPECT_DOUBLE_EQ(config.max_asset_weight, 1.5);
}

// ---------------------------------------------------------------- C8

TEST(ManualTime, C8_NoManualOrdersFrom2345To0015Utc)
{
    EXPECT_FALSE(inDailyCycleWindow(utc(2026, 10, 14, 23, 44, 59)));
    EXPECT_TRUE(inDailyCycleWindow(utc(2026, 10, 14, 23, 45, 0)));
    EXPECT_TRUE(inDailyCycleWindow(utc(2026, 10, 14, 23, 59, 59)));
    EXPECT_TRUE(inDailyCycleWindow(utc(2026, 10, 15, 0, 0, 0)));
    EXPECT_TRUE(inDailyCycleWindow(utc(2026, 10, 15, 0, 14, 59)));
    EXPECT_FALSE(inDailyCycleWindow(utc(2026, 10, 15, 0, 15, 0)));
    EXPECT_FALSE(inDailyCycleWindow(utc(2026, 10, 15, 12, 0, 0)));
}

// ---------------------------------------------------------------- C9

TEST(ManualTime, C9_ParsesRfc3339UtcSeconds)
{
    const auto parsed = parseUtcSeconds("2026-10-14T12:00:00Z");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, utc(2026, 10, 14, 12, 0, 0));
    const auto newYear = parseUtcSeconds("2027-01-01T00:00:01Z");
    ASSERT_TRUE(newYear.has_value());
    EXPECT_EQ(*newYear, utc(2027, 1, 1, 0, 0, 1));
}

TEST(ManualTime, C9_OtherShapesGiveNothing)
{
    for (const char* text : {"2026-10-14 12:00:00", "garbage", "", "14/10/2026 12:00"})
        EXPECT_FALSE(parseUtcSeconds(text).has_value()) << "'" << text << "'";
}

TEST(ManualTime, C9_OffsetOrTrailingTextGivesNothing)
{
    for (const char* text : {"2026-10-14T12:00:00+02:00", "2026-10-14T12:00:00Zjunk"})
        EXPECT_FALSE(parseUtcSeconds(text).has_value()) << "'" << text << "'";
}
