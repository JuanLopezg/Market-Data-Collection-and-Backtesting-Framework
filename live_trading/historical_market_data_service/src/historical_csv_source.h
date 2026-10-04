/*
 * File purpose: Defines sequential historical CSV cursors, including the anti-lookahead open-only execution-price cursor.
 *
 * Keep this file focused on this responsibility. Trading decisions belong in
 * their domain component; process orchestration belongs in the service application.
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_types.h"

struct HistoricalMarketDay {
    Timestamp date = 0;
    OHLCVData bars;
    std::vector<std::pair<std::string, double>> volume_ranking;

    std::size_t rowCount() const noexcept;
};

// Sequential reader for the reference daily CSV:
//   date,symbol,open,high,low,close,volume
//
// The cursor never loads the full historical dataset into memory.  It exposes one
// calendar day at a time and requires the source file to be ordered by date.
class HistoricalCsvSource {
public:
    explicit HistoricalCsvSource(std::filesystem::path path);

    const std::filesystem::path& path() const noexcept { return path_; }

    // Returns the date at the current cursor without consuming it.
    std::optional<Timestamp> nextDate();

    // Consumes exactly one source day. Throws on malformed, duplicate, or
    // backwards-moving rows.
    HistoricalMarketDay readNextDay();

    // Restart helper: consume source days through checkpointDate so the next read
    // starts strictly after the durable canonical SQLite checkpoint.
    std::optional<HistoricalMarketDay> skipThrough(Timestamp checkpointDate);

private:
    std::filesystem::path path_;
    std::ifstream input_;
    std::size_t line_number_ = 1; // header is line 1
    Timestamp last_consumed_date_ = 0;

    struct ParsedRow {
        Timestamp date = 0;
        std::string symbol;
        OHLCV bar;
    };

    ParsedRow parseLine(const std::string& line, std::size_t lineNumber) const;
    std::optional<Timestamp> peekDateOnly();
};


/**************************************************************************************
 * Type    : HistoricalExecutionOpen
 * Purpose : Only the information that is economically visible at the UTC daily open.
 *
 * This deliberately does not contain high/low/close/volume.  The historical replay can
 * therefore release T+1 open prices without exposing the rest of the still-open candle.
 **************************************************************************************/
struct HistoricalExecutionOpen {
    Timestamp date = 0;
    std::unordered_map<Coin, double> prices;
};


/**************************************************************************************
 * Type    : HistoricalOpenCsvSource
 * Purpose : Independent sequential cursor that parses only date,symbol,open.
 *
 * It shares the same normalized CSV file as HistoricalCsvSource but never converts or
 * exposes high/low/close/volume for a day that is not yet completed.  This is the
 * anti-lookahead boundary used by distributed historical execution.
 **************************************************************************************/
class HistoricalOpenCsvSource {
public:
    explicit HistoricalOpenCsvSource(std::filesystem::path path);

    const std::filesystem::path& path() const noexcept { return path_; }
    std::optional<Timestamp> nextDate();
    HistoricalExecutionOpen readNextDay();

private:
    std::filesystem::path path_;
    std::ifstream input_;
    std::size_t line_number_ = 1;
    Timestamp last_consumed_date_ = 0;

    struct ParsedOpenRow {
        Timestamp date = 0;
        std::string symbol;
        double open = 0.0;
    };

    ParsedOpenRow parseOpenLine(const std::string& line, std::size_t lineNumber) const;
    std::optional<Timestamp> peekDateOnly();
};
