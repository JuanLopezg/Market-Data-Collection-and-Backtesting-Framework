#pragma once

#include <string>

// In-process CURRENT PureRSI baseline over frozen PAPER market/price observations.
int runPaperBaseline(const std::string& manifestPath);
