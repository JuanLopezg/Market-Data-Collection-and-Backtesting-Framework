#include "binance_http_server.h"
#include "historical_market_data.h"

#include <boost/asio/ip/address.hpp>
#include <boost/program_options.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace po = boost::program_options;

namespace {

std::chrono::system_clock::time_point parseClockDate(const std::string& value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        throw std::runtime_error("Invalid --clock-date. Expected YYYY-MM-DD");

    int year = std::stoi(value.substr(0, 4));
    unsigned month = static_cast<unsigned>(std::stoi(value.substr(5, 2)));
    unsigned day = static_cast<unsigned>(std::stoi(value.substr(8, 2)));

    std::chrono::year_month_day ymd{
        std::chrono::year{year},
        std::chrono::month{month},
        std::chrono::day{day}
    };

    if (!ymd.ok())
        throw std::runtime_error("Invalid --clock-date. Expected YYYY-MM-DD");

    return std::chrono::sys_days{ymd};
}

}

int main(int argc, char** argv)
{
    std::string csvPath;
    std::string clockDate;
    std::string host = "127.0.0.1";
    int port = 8080;

    try {
        po::options_description desc("Options");
        desc.add_options()
            ("help,h", "Show help")
            ("csv", po::value<std::string>(&csvPath)->required(), "Historical CSV: date,symbol,open,high,low,close,volume")
            ("clock-date", po::value<std::string>(&clockDate)->required(), "Simulated UTC date YYYY-MM-DD at 00:00")
            ("host", po::value<std::string>(&host)->default_value("127.0.0.1"), "Listen address")
            ("port,p", po::value<int>(&port)->default_value(8080), "Listen port");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << '\n';
            return 0;
        }

        po::notify(vm);

        if (port < 1 || port > 65535)
            throw std::runtime_error("Invalid --port");

        auto clock = parseClockDate(clockDate);
        auto clockMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock.time_since_epoch()).count();

        HistoricalMarketData marketData(csvPath, clockMs);
        BinanceHttpServer server(
            marketData,
            boost::asio::ip::make_address(host),
            static_cast<std::uint16_t>(port));

        std::cout << "Historical CSV: " << csvPath << '\n';
        std::cout << "Simulated clock: " << clockDate << " 00:00 UTC\n";
        std::cout << "Active instruments: " << marketData.activeInstruments().size() << '\n';

        server.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Binance simulator error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
