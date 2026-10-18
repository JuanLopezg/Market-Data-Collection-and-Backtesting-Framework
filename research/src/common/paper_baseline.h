#pragma once

#include <string>

// In-process PureRSI baseline over recorded PAPER market/price observations.
int runPaperBaseline(const std::string& manifestPath);
