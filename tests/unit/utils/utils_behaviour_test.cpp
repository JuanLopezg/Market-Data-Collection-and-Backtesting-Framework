#include <gtest/gtest.h>

#include "csv_utils.h"
#include "json_utils.h"
#include "test_support.h"
#include "time_handler_factory.h"
#include "time_utils.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

namespace {
using namespace std::chrono;

// Each environment test starts from an isolated configuration and restores the caller.
class TimeEnvironment : public ::testing::Test {
protected:
    const std::array<const char*, 3> names{TimeHandlerFactory::SPEED_ENV,
        TimeHandlerFactory::REAL_REFERENCE_ENV, TimeHandlerFactory::SIMULATED_REFERENCE_ENV};
    std::array<std::optional<std::string>, 3> original;
    void SetUp() override
    {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (const char* value = std::getenv(names[i])) original[i] = value;
            ASSERT_EQ(unsetenv(names[i]), 0);
        }
    }
    void TearDown() override
    {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (original[i]) setenv(names[i], original[i]->c_str(), 1);
            else unsetenv(names[i]);
        }
    }
    void set(std::size_t index, const char* value) { ASSERT_EQ(setenv(names[index], value, 1), 0); }
};

TEST(TimeHandler, R1_IdentityKeepsBusinessTimeInsideRealReadBounds)
{
    const TimeHandler handler(1.0, {}, {});
    const auto before = system_clock::now();
    const auto business = handler.getTime();
    const auto after = system_clock::now();
    EXPECT_GE(business, before);
    EXPECT_LE(business, after);
}

TEST(TimeHandler, R1_ShiftedReferenceMapsExactlyOneDayBack)
{
    const TimeHandler handler(1.0, -days{1}, sys_days{2024y/January/1});
    const auto before = system_clock::now();
    const auto business = handler.getTime();
    const auto after = system_clock::now();
    EXPECT_GE(business, before - days{1});
    EXPECT_LE(business, after - days{1});
}

TEST(TimeHandler, R1_AccelerationUsesTheSharedReference)
{
    const auto reference = system_clock::now() - seconds{10};
    const TimeHandlerConfig config{2.0, reference, reference - days{1}};
    const auto handler = TimeHandlerFactory::create(config);
    const auto before = system_clock::now();
    const auto business = handler.getTime();
    const auto after = system_clock::now();
    EXPECT_GE(business, config.simulated_reference_utc + 2 * (before - reference));
    EXPECT_LE(business, config.simulated_reference_utc + 2 * (after - reference));
}

TEST(TimeHandler, R2_RejectsNonpositiveAndNonfiniteSpeed)
{
    for (double speed : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
        EXPECT_THROW((TimeHandler{speed, {}, {}}), std::invalid_argument);
        EXPECT_THROW(TimeHandlerFactory::create({speed, {}, {}}), std::invalid_argument);
    }
}

TEST(TimeHandler, R2_OverflowThrowsRatherThanWrappingBusinessTime)
{
    const TimeHandler handler(std::numeric_limits<double>::max(), {},
                              system_clock::now() - seconds{1});
    EXPECT_THROW(handler.getTime(), std::overflow_error);
}

TEST_F(TimeEnvironment, R3_DefaultAndEmptyConfigurationAreIdentity)
{
    EXPECT_TRUE(TimeHandlerFactory::loadConfigFromEnvironment().identity());
    for (const char* name : names) ASSERT_EQ(setenv(name, "", 1), 0);
    EXPECT_TRUE(TimeHandlerFactory::loadConfigFromEnvironment().identity());
}

TEST_F(TimeEnvironment, R3_AccelerationWithoutSharedReferencesIsRejected)
{
    set(0, "2");
    EXPECT_THROW(TimeHandlerFactory::createFromEnvironment(), std::invalid_argument);
}

TEST_F(TimeEnvironment, R3_EitherMissingReferenceIsRejected)
{
    set(1, "2024-03-01T00:00:00Z");
    EXPECT_THROW(TimeHandlerFactory::loadConfigFromEnvironment(), std::invalid_argument);
    ASSERT_EQ(unsetenv(names[1]), 0);
    set(2, "2024-02-29T00:00:00Z");
    EXPECT_THROW(TimeHandlerFactory::loadConfigFromEnvironment(), std::invalid_argument);
}

TEST_F(TimeEnvironment, R4_LeapDayReferencesDeriveBiasAndRemainStable)
{
    set(0, "2");
    set(1, "2024-03-01T00:00:00Z");
    set(2, "2024-02-29T00:00:00Z");
    const auto config = TimeHandlerFactory::loadConfigFromEnvironment();
    EXPECT_EQ(config.speed, 2.0);
    EXPECT_EQ(config.bias(), -days{1});
    EXPECT_EQ(config.real_reference_utc, sys_days{2024y/March/1});
    EXPECT_EQ(config.simulated_reference_utc,
              TimeHandlerFactory::loadConfigFromEnvironment().simulated_reference_utc);
    EXPECT_NO_THROW(TimeHandlerFactory::createFromEnvironment());
}

TEST_F(TimeEnvironment, R4_InvalidUtcReferencesAreRejected)
{
    set(2, "2024-02-29T00:00:00Z");
    for (const char* invalid : {"2023-02-29T00:00:00Z", "2024-02-29T24:00:00Z",
            "2024-02-29T00:00:60Z", "2024-02-29T00:00:00+00:00",
            "2024-02-29T00:00:00Zx"}) {
        SCOPED_TRACE(invalid);
        set(1, invalid);
        EXPECT_THROW(TimeHandlerFactory::loadConfigFromEnvironment(), std::invalid_argument);
    }
}

TEST_F(TimeEnvironment, R4_InvalidSpeedStringsAreRejected)
{
    for (const char* invalid : {"0", "-1", "nan", "inf", "2garbage"}) {
        SCOPED_TRACE(invalid);
        set(0, invalid);
        EXPECT_THROW(TimeHandlerFactory::loadConfigFromEnvironment(), std::invalid_argument);
    }
}

TEST(UtcDates, R5_LeapDayAndYearTransitions)
{
    EXPECT_EQ(previousDay(20240301), 20240229u);
    EXPECT_EQ(nextDay(20240229), 20240301u);
    EXPECT_EQ(nextDay(20231231), 20240101u);
    EXPECT_EQ(previousDay(20240101), 20231231u);
    EXPECT_EQ(getPreviousDayDate(2024y/March/1), 2024y/February/29);
}

TEST(UtcDates, R5_UnixMillisecondsUseUtcDays)
{
    EXPECT_EQ(toUnixMillis(19700101), 0);
    EXPECT_EQ(toUnixMillis(19700102), 86400000);
    EXPECT_EQ(toUnixMillis(20240301) - toUnixMillis(20240229), 86400000);
    EXPECT_EQ(toYYYYMMDD(2024y/February/29), 20240229);
    EXPECT_EQ(formatYMD(2024y/February/29), "2024-02-29");
}

TEST(UtcDates, R5_InvalidCompactDatesAreRejected)
{
    for (unsigned date : {20230229u, 20241301u, 20240001u, 20240100u}) {
        EXPECT_THROW(toUnixMillis(static_cast<int>(date)), std::invalid_argument);
        EXPECT_THROW(previousDay(date), std::invalid_argument);
        EXPECT_THROW(nextDay(date), std::invalid_argument);
    }
}

TEST(UtcDates, R6_CloseBeforeMidnightStaysOnTheCompletedUtcDate)
{
    const auto now = sys_days{2024y/February/29} + hours{23} + minutes{59}
                     + seconds{59} + milliseconds{250};
    EXPECT_EQ(getCurrentUtcDate(now), 2024y/February/29);
    EXPECT_EQ(computeNextMidnightUTC(now), sys_days{2024y/March/1});
    EXPECT_EQ(timeUntilUtcMidnight(now), "00h 00m 00s until UTC midnight");
}

TEST(UtcDates, R6_MidnightTargetsTheFollowingDay)
{
    const auto midnight = sys_days{2024y/March/1};
    EXPECT_EQ(computeNextMidnightUTC(midnight), midnight + days{1});
    EXPECT_EQ(timeUntilUtcMidnight(midnight), "24h 00m 00s until UTC midnight");
    EXPECT_EQ(currentUtcTimestamp(midnight + hours{12} + minutes{34} + seconds{56}
                                  + milliseconds{123}), "12:34:56.123 UTC");
}

TEST(JsonUtilities, R7_LoadPreservesFinancialValuesAcrossRepeatedReads)
{
    test_support::TempDir dir;
    const auto file = dir / "config.json";
    test_support::writeFile(file, R"({"capital":125.5,"enabled":true,"assets":["BTC","ETH"]})");
    const auto data = LoadJsonFile(file.string());
    EXPECT_EQ(data.at("capital"), 125.5);
    EXPECT_EQ(data.at("enabled"), true);
    EXPECT_EQ(data.at("assets"), nlohmann::json::array({"BTC", "ETH"}));
    EXPECT_EQ(LoadJsonFile(file.string()), data);
}

TEST(JsonUtilities, R7_MissingAndMalformedFilesThrow)
{
    test_support::TempDir dir;
    EXPECT_THROW(LoadJsonFile((dir / "absent.json").string()), std::runtime_error);
    test_support::writeFile(dir / "broken.json", "{broken");
    EXPECT_THROW(LoadJsonFile((dir / "broken.json").string()), std::runtime_error);
}

TEST(JsonUtilities, R7_SchemaEnforcesRequiredNonnegativeCapital)
{
    const auto schema = nlohmann::json::parse(R"({"type":"object","required":["capital"],
        "properties":{"capital":{"type":"number","minimum":0}}})");
    EXPECT_NO_THROW(ValidateJson({{"capital", 125.5}}, schema));
    EXPECT_THROW(ValidateJson({{"capital", -1}}, schema), std::runtime_error);
    EXPECT_THROW(ValidateJson(nlohmann::json::object(), schema), std::runtime_error);
}

TEST(CsvUtilities, R8_OrderedCurvePreservesBalanceAndEquity)
{
    test_support::TempDir dir;
    const auto file = dir / "curve.csv";
    saveCurveToCSV({{100.5, 101.25}, {99, 98.5}}, file.string());
    EXPECT_EQ(test_support::readFile(file), "Index;Balance;Equity\n0;100,5;101,25\n1;99;98,5\n");
}

TEST(CsvUtilities, R8_ReexportReplacesRowsAndEmptyCurveKeepsHeader)
{
    test_support::TempDir dir;
    const auto file = dir / "curve.csv";
    saveCurveToCSV({{100.5, 101.25}, {99, 98.5}}, file.string());
    saveCurveToCSV({}, file.string());
    EXPECT_EQ(test_support::readFile(file), "Index;Balance;Equity\n");
}

TEST(CsvUtilities, R8_UnopenableDestinationDoesNotCreateCurve)
{
    test_support::TempDir dir;
    const auto file = dir / "missing/curve.csv";
    EXPECT_NO_THROW(saveCurveToCSV({{100, 101}}, file.string()));
    EXPECT_FALSE(std::filesystem::exists(file));
}
} // namespace
