#pragma once

#include <string>

/**************************************************************************************
 * Purpose : Declaration of the CURL write callback used when downloading data via
 *           libcurl. The callback appends incoming data chunks into a std::string
 *           provided by the caller.
 *
 * Args    : contents - Pointer to the memory block containing downloaded data.
 *           size     - Size of each element in the block (usually 1).
 *           nmemb    - Number of elements in the block.
 *           userp    - Pointer to a std::string where data should be appended.
 *
 * Return  : size_t   - Total bytes processed (size * nmemb). libcurl requires the
 *                      callback to return the number of bytes handled.
 **************************************************************************************/
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
