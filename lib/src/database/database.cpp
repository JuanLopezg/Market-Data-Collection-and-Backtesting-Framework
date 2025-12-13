#include <string>
#include "database.h"

/**************************************************************************************
 * Purpose : CURL write callback used to accumulate incoming HTTP response data into
 *           a std::string. libcurl calls this function repeatedly while downloading
 *           content. The user-provided buffer (userp) is appended to as data arrives.
 * Args    : contents - Pointer to the downloaded data chunk.
 *           size     - Size of each element (usually 1).
 *           nmemb    - Number of elements in this chunk.
 *           userp    - Pointer to the std::string accumulator.
 * Return  : size_t   - Total bytes processed (size * nmemb), required by libcurl.
 **************************************************************************************/
size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}
