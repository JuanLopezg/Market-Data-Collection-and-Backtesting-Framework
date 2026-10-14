#include "kraken_futures_protocol.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace Venue {

namespace {

using Json = nlohmann::json;

const std::string API_PREFIX = "/derivatives";
const std::string API_PATH = "/api/v3/";
const std::string PERMISSIONS_PATH = "/api/auth/v1/api-keys/v3/check";
const std::string ACCOUNT_LOG_PATH = "/api/history/v2/account-log";
const std::string CLIENT_ID_PREFIX = "algo-";

// ------------------------------------------------------------------ encoding

std::string base64Encode(const unsigned char* data, std::size_t length)
{
    std::string output(4 * ((length + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), data, static_cast<int>(length));
    output.resize(static_cast<std::size_t>(written));
    return output;
}

std::string base64Decode(const std::string& text)
{
    static const std::regex BASE64("^[A-Za-z0-9+/]*={0,2}$");
    if (text.empty() || text.size() % 4 != 0 || !std::regex_match(text, BASE64))
        throw std::invalid_argument("Invalid Base64 secret");
    std::string output(3 * text.size() / 4, '\0');
    const int written = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                        reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    if (written < 0)
        throw std::invalid_argument("Invalid Base64 secret");
    const std::size_t padding = static_cast<std::size_t>(std::count(text.end() - 2, text.end(), '='));
    output.resize(static_cast<std::size_t>(written) - padding);
    return output;
}

std::string urlEncode(const std::string& value)
{
    static const char* HEX = "0123456789ABCDEF";
    std::string output;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output += static_cast<char>(c);
        }
        else {
            output += '%';
            output += HEX[c >> 4];
            output += HEX[c & 15];
        }
    }
    return output;
}

// ------------------------------------------------------------------ parsing

Json parseBody(const HttpResponse& response, const std::string& endpoint)
{
    if (!response.transport_ok)
        throw ProtocolError(endpoint + ": " + response.transport_error);
    if (response.status != 200)
        throw ProtocolError(endpoint + ": HTTP " + std::to_string(response.status));
    Json body = Json::parse(response.body, nullptr, false);
    if (body.is_discarded() || !body.is_object())
        throw ProtocolError(endpoint + ": response is not a JSON object");
    return body;
}

// Successful Kraken envelope ("result": "success"); errors keep only the short error code.
Json successBody(const HttpResponse& response, const std::string& endpoint)
{
    Json body = parseBody(response, endpoint);
    if (body.value("result", "") != "success") {
        std::string code = body.contains("error") && body["error"].is_string() ? body["error"].get<std::string>() : "unknown";
        throw ProtocolError(endpoint + ": venue error " + code.substr(0, 64));
    }
    return body;
}

const Json& field(const Json& object, const char* name, const std::string& endpoint)
{
    if (!object.is_object() || !object.contains(name))
        throw ProtocolError(endpoint + ": missing field " + name);
    return object.at(name);
}

std::string text(const Json& object, const char* name, const std::string& endpoint)
{
    const Json& value = field(object, name, endpoint);
    if (!value.is_string() || value.get<std::string>().empty())
        throw ProtocolError(endpoint + ": field " + name + " is not a non-empty string");
    return value.get<std::string>();
}

std::string optionalText(const Json& object, const char* name)
{
    if (!object.is_object() || !object.contains(name) || !object.at(name).is_string())
        return {};
    return object.at(name).get<std::string>();
}

double number(const Json& object, const char* name, const std::string& endpoint)
{
    const Json& value = field(object, name, endpoint);
    double result = 0.0;
    if (value.is_number())
        result = value.get<double>();
    else if (value.is_string()) {
        try {
            std::size_t used = 0;
            result = std::stod(value.get<std::string>(), &used);
            if (used != value.get<std::string>().size())
                throw std::invalid_argument("trailing characters");
        }
        catch (const std::exception&) {
            throw ProtocolError(endpoint + ": field " + name + " is not a number");
        }
    }
    else
        throw ProtocolError(endpoint + ": field " + name + " is not a number");
    if (!std::isfinite(result))
        throw ProtocolError(endpoint + ": field " + name + " is not finite");
    return result;
}

OrderSide side(const Json& object, const char* name, const std::string& endpoint)
{
    const std::string value = text(object, name, endpoint);
    if (value == "buy")
        return OrderSide::Buy;
    if (value == "sell")
        return OrderSide::Sell;
    throw ProtocolError(endpoint + ": unknown side");
}

const Json& rows(const Json& body, const char* name, const std::string& endpoint)
{
    const Json& value = field(body, name, endpoint);
    if (!value.is_array())
        throw ProtocolError(endpoint + ": field " + name + " is not a list");
    return value;
}

// "2026-10-11T08:00:00.123Z" -> Unix milliseconds and the UTC day key.
std::pair<long long, Timestamp> utcTime(const std::string& value, const std::string& endpoint)
{
    static const std::regex ISO(R"(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(\.(\d{1,9}))?(Z|\+00:00)$)");
    std::smatch match;
    if (!std::regex_match(value, match, ISO))
        throw ProtocolError(endpoint + ": time is not UTC ISO-8601");
    std::tm parts{};
    parts.tm_year = std::stoi(match[1]) - 1900;
    parts.tm_mon = std::stoi(match[2]) - 1;
    parts.tm_mday = std::stoi(match[3]);
    parts.tm_hour = std::stoi(match[4]);
    parts.tm_min = std::stoi(match[5]);
    parts.tm_sec = std::stoi(match[6]);
    long long millis = 0;
    if (match[8].matched) {
        std::string fraction = match[8].str() + "00";
        millis = std::stoll(fraction.substr(0, 3));
    }
    const long long seconds = static_cast<long long>(timegm(&parts));
    const Timestamp day = static_cast<Timestamp>(std::stoi(match[1]) * 10000 + std::stoi(match[2]) * 100 + std::stoi(match[3]));
    return {seconds * 1000 + millis, day};
}

std::string formatQuantity(double quantity, double step)
{
    const int decimals = std::max(0, static_cast<int>(std::lround(-std::log10(step))));
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, quantity);
    return buffer;
}

std::string upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

} // namespace


// ------------------------------------------------------------------ credentials

KrakenCredentials loadKrakenCredentials(const std::string& path)
{
    const std::filesystem::path file(path);
    std::error_code error;
    if (std::filesystem::is_symlink(file, error) || !std::filesystem::is_regular_file(file, error))
        throw std::runtime_error("Kraken credentials must be a regular file (not a link)");
    if (std::filesystem::file_size(file, error) > 16384 || error)
        throw std::runtime_error("Kraken credential file must contain only the two Kraken variables");

    std::ifstream input(file);
    if (!input)
        throw std::runtime_error("Kraken credential file cannot be read");

    KrakenCredentials credentials;
    std::set<std::string> seen;
    std::string line;
    while (std::getline(input, line)) {
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#')
            continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            throw std::runtime_error("Use KRAKEN_API_KEY=value and KRAKEN_API_SECRET=value, once each");
        std::string name = line.substr(first, separator - first);
        std::string value = line.substr(separator + 1);
        name.erase(name.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if ((name != "KRAKEN_API_KEY" && name != "KRAKEN_API_SECRET") || !seen.insert(name).second || value.empty() ||
            value.find_first_of(" \t") != std::string::npos)
            throw std::runtime_error("Use KRAKEN_API_KEY=value and KRAKEN_API_SECRET=value, once each");
        (name == "KRAKEN_API_KEY" ? credentials.api_key : credentials.api_secret) = value;
    }
    if (seen.size() != 2)
        throw std::runtime_error("Add both KRAKEN_API_KEY and KRAKEN_API_SECRET to the credential file");
    return credentials;
}


// ------------------------------------------------------------------ protocol

KrakenFuturesProtocol::KrakenFuturesProtocol(std::string host, KrakenCredentials credentials)
    : host_(std::move(host)), credentials_(std::move(credentials))
{
    if (host_.rfind("https://", 0) != 0)
        throw std::invalid_argument("Kraken host must use HTTPS");
    static const std::regex KEY("^[A-Za-z0-9+/=_-]{8,256}$");
    if (!std::regex_match(credentials_.api_key, KEY))
        throw std::invalid_argument("Invalid Kraken API key format");
    try {
        if (base64Decode(credentials_.api_secret).size() < 16)
            throw std::invalid_argument("short");
    }
    catch (const std::invalid_argument&) {
        throw std::invalid_argument("Invalid Kraken API secret format");
    }
}


int KrakenFuturesProtocol::cost(Endpoint endpoint) const
{
    switch (endpoint) {
        case Endpoint::Permissions:    return 1; // Not listed in the budget table; counted conservatively.
        case Endpoint::Instruments:    return 0; // Public.
        case Endpoint::Tickers:        return 0; // Public.
        case Endpoint::Accounts:       return 2;
        case Endpoint::Positions:      return 2;
        case Endpoint::OpenOrders:     return 2;
        case Endpoint::Fills:          return 2; // Without lastFillTime.
        case Endpoint::SendOrder:      return 10;
        case Endpoint::CancelOrder:    return 10;
        case Endpoint::DeadMansSwitch: return 25;
        // History API: its own limits, not listed in the derivatives budget table;
        // counted conservatively and polled every few minutes only. UNCONFIRMED.
        case Endpoint::AccountLog:     return 10;
    }
    return 25;
}


std::string KrakenFuturesProtocol::clientOrderId(const std::string& accountNamespace, OrderID localOrderId) const
{
    static const std::regex NAMESPACE("^[a-z0-9]{1,24}$");
    if (!std::regex_match(accountNamespace, NAMESPACE))
        throw std::invalid_argument("Account namespace must be 1-24 lowercase letters or digits");
    return CLIENT_ID_PREFIX + accountNamespace + "-" + std::to_string(localOrderId);
}


bool KrakenFuturesProtocol::isOurClientOrderId(const std::string& accountNamespace, const std::string& clientOrderId) const
{
    const std::string prefix = CLIENT_ID_PREFIX + accountNamespace + "-";
    return clientOrderId.rfind(prefix, 0) == 0;
}


std::string KrakenFuturesProtocol::authent(const std::string& secretBase64, const std::string& postData,
                                           const std::string& nonce, const std::string& signingPath)
{
    const std::string message = postData + nonce + signingPath;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;
    if (EVP_Digest(message.data(), message.size(), digest, &digestLength, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");

    const std::string secret = base64Decode(secretBase64);
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signatureLength = 0;
    if (HMAC(EVP_sha512(), secret.data(), static_cast<int>(secret.size()), digest, digestLength,
             signature, &signatureLength) == nullptr)
        throw std::runtime_error("HMAC-SHA512 failed");
    return base64Encode(signature, signatureLength);
}


std::string KrakenFuturesProtocol::nextNonce()
{
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    last_nonce_ = std::max(now, last_nonce_ + 1);
    return std::to_string(last_nonce_);
}


HttpRequest KrakenFuturesProtocol::privateGet(const std::string& endpoint, const std::string& name)
{
    // No query string and no nonce: postData and nonce are empty, as in the read-only tool.
    const std::string signingPath = API_PATH + endpoint;
    HttpRequest request;
    request.method = "GET";
    request.url = host_ + API_PREFIX + signingPath;
    request.endpoint = name;
    request.headers = {{"APIKey", credentials_.api_key},
                       {"Authent", authent(credentials_.api_secret, "", "", signingPath)},
                       {"Accept", "application/json"}};
    return request;
}


HttpRequest KrakenFuturesProtocol::privatePost(const std::string& endpoint, const std::string& name, const std::string& body)
{
    const std::string signingPath = API_PATH + endpoint;
    const std::string nonce = nextNonce();
    HttpRequest request;
    request.method = "POST";
    request.url = host_ + API_PREFIX + signingPath;
    request.endpoint = name;
    request.body = body;
    request.headers = {{"APIKey", credentials_.api_key},
                       {"Nonce", nonce},
                       {"Authent", authent(credentials_.api_secret, body, nonce, signingPath)},
                       {"Content-Type", "application/x-www-form-urlencoded"},
                       {"Accept", "application/json"}};
    return request;
}


HttpRequest KrakenFuturesProtocol::permissionsRequest()
{
    HttpRequest request;
    request.method = "GET";
    request.url = host_ + PERMISSIONS_PATH;
    request.endpoint = "permissions";
    request.headers = {{"APIKey", credentials_.api_key},
                       {"Authent", authent(credentials_.api_secret, "", "", PERMISSIONS_PATH)},
                       {"Accept", "application/json"}};
    return request;
}


Permissions KrakenFuturesProtocol::parsePermissions(const HttpResponse& response) const
{
    const Json body = parseBody(response, "permissions");
    const Json& permissions = field(body, "permissions", "permissions");
    if (!permissions.is_object())
        throw ProtocolError("permissions: field permissions is not an object");

    Permissions result;
    const std::string general = text(permissions, "general", "permissions");
    result.read = general == "READ_ONLY" || general == "FULL_ACCESS";
    result.trade = general == "FULL_ACCESS"; // UNCONFIRMED: value for a trading key.
    for (const auto& [key, value] : permissions.items()) {
        // Anything beyond the general (read/trade) permission, such as transfers or
        // withdrawals, is treated as fund movement and refused by the adapter.
        if (key != "general" && (!value.is_string() || value.get<std::string>() != "NO_ACCESS"))
            result.withdraw = true;
    }
    return result;
}


HttpRequest KrakenFuturesProtocol::instrumentsRequest()
{
    HttpRequest request;
    request.method = "GET";
    request.url = host_ + API_PREFIX + API_PATH + "instruments";
    request.endpoint = "instruments";
    request.headers = {{"Accept", "application/json"}};
    return request;
}


std::vector<Market> KrakenFuturesProtocol::parseMarkets(const HttpResponse& response) const
{
    const std::string endpoint = "instruments";
    const Json body = successBody(response, endpoint);
    std::vector<Market> markets;
    std::set<std::string> assets;
    for (const Json& instrument : rows(body, "instruments", endpoint)) {
        const std::string symbol = optionalText(instrument, "symbol");
        // Multi-M linear perpetuals only; tokenized traditional assets are excluded.
        if (symbol.rfind("PF_", 0) != 0 || instrument.value("tradfi", false))
            continue;
        if (optionalText(instrument, "type") != "flexible_futures" || optionalText(instrument, "quote") != "USD")
            continue;

        Market market;
        market.symbol = symbol;
        market.asset = upper(text(instrument, "base", endpoint));
        if (market.asset == "XBT")
            market.asset = "BTC";
        if (!assets.insert(market.asset).second)
            throw ProtocolError(endpoint + ": two perpetuals for " + market.asset);

        const bool tradeable = instrument.value("tradeable", false);
        const bool expired = instrument.value("isExpired", false);
        const double contractSize = number(instrument, "contractSize", endpoint);
        if (!tradeable || expired) {
            market.unavailable_reason = expired ? "expired" : "not tradeable";
        }
        else if (std::abs(contractSize - 1.0) > 1e-12) {
            market.unavailable_reason = "unsupported contract size";
        }
        else {
            const Json& precision = field(instrument, "contractValueTradePrecision", endpoint);
            if (!precision.is_number_integer() || precision.get<int>() < -12 || precision.get<int>() > 12)
                throw ProtocolError(endpoint + ": invalid quantity precision for " + symbol);
            market.quantity_step = std::pow(10.0, -precision.get<int>());
            market.tick_size = number(instrument, "tickSize", endpoint);
            market.trading = market.tick_size > 0.0;
            if (!market.trading)
                market.unavailable_reason = "invalid tick size";
        }
        markets.push_back(market);
    }
    if (markets.empty())
        throw ProtocolError(endpoint + ": no Multi-M perpetuals listed");
    return markets;
}


HttpRequest KrakenFuturesProtocol::tickersRequest()
{
    HttpRequest request;
    request.method = "GET";
    request.url = host_ + API_PREFIX + API_PATH + "tickers";
    request.endpoint = "tickers";
    request.headers = {{"Accept", "application/json"}};
    return request;
}


std::map<std::string, double> KrakenFuturesProtocol::parseMarkPrices(const HttpResponse& response) const
{
    const std::string endpoint = "tickers";
    const Json body = successBody(response, endpoint);
    std::map<std::string, double> prices;
    for (const Json& ticker : rows(body, "tickers", endpoint)) {
        const std::string symbol = optionalText(ticker, "symbol");
        // Suspended contracts and rows without a mark price are left out: no price, no order.
        if (symbol.rfind("PF_", 0) != 0 || ticker.value("suspended", false) || !ticker.contains("markPrice"))
            continue;
        const double mark = number(ticker, "markPrice", endpoint);
        if (mark > 0.0)
            prices[symbol] = mark;
    }
    return prices;
}


HttpRequest KrakenFuturesProtocol::submitRequest(const OrderRequest& order)
{
    if (order.client_order_id.size() > 100)
        throw std::invalid_argument("Kraken client order id exceeds 100 characters");
    if (!(order.quantity > 0.0))
        throw std::invalid_argument("Kraken order size must be positive");
    // The step is not in OrderRequest; the size is already rounded, so print it exactly
    // with up to 12 decimals and trim trailing zeros.
    std::string size = formatQuantity(order.quantity, 1e-12);
    size.erase(size.find_last_not_of('0') + 1);
    if (!size.empty() && size.back() == '.')
        size.pop_back();

    const std::string body = "orderType=mkt&symbol=" + urlEncode(order.symbol) +
                             "&side=" + std::string(order.side == OrderSide::Buy ? "buy" : "sell") +
                             "&size=" + size + "&cliOrdId=" + urlEncode(order.client_order_id);
    return privatePost("sendorder", "sendorder", body);
}


SubmitResult KrakenFuturesProtocol::parseSubmit(const HttpResponse& response) const
{
    if (!response.transport_ok)
        return {SubmitOutcome::Unknown, {}, response.transport_error};
    if (response.status == 429)
        return {SubmitOutcome::NotProcessed, {}, "HTTP 429"};
    if (response.status >= 500)
        return {SubmitOutcome::Unknown, {}, "HTTP " + std::to_string(response.status)};
    if (response.status != 200)
        return {SubmitOutcome::Rejected, {}, "HTTP " + std::to_string(response.status)};

    const Json body = parseBody(response, "sendorder");
    if (body.value("result", "") != "success") {
        const std::string code = optionalText(body, "error");
        static const std::set<std::string> NOT_PROCESSED{"apiLimitExceeded", "nonceBelowThreshold", "nonceDuplicate"};
        static const std::set<std::string> UNCERTAIN{"Unavailable", "Server Error", "unknownError", ""};
        if (NOT_PROCESSED.count(code))
            return {SubmitOutcome::NotProcessed, {}, code};
        if (UNCERTAIN.count(code))
            return {SubmitOutcome::Unknown, {}, code.empty() ? "error without code" : code};
        return {SubmitOutcome::Rejected, {}, code.substr(0, 64)};
    }

    const Json& status = field(body, "sendStatus", "sendorder");
    const std::string state = text(status, "status", "sendorder");
    if (state == "placed" || state == "partiallyFilled" || state == "filled")
        return {SubmitOutcome::Accepted, optionalText(status, "order_id"), state};
    if (state == "clientOrderIdAlreadyExist")
        // A previous send with this id reached Kraken: evidence decides, never a rejection.
        return {SubmitOutcome::Unknown, {}, state};
    return {SubmitOutcome::Rejected, {}, state.substr(0, 64)};
}


HttpRequest KrakenFuturesProtocol::cancelRequest(const std::string& clientOrderId)
{
    return privatePost("cancelorder", "cancelorder", "cliOrdId=" + urlEncode(clientOrderId));
}


CancelResult KrakenFuturesProtocol::parseCancel(const HttpResponse& response) const
{
    if (!response.transport_ok)
        return {CancelOutcome::Unknown, response.transport_error};
    if (response.status != 200)
        return {CancelOutcome::Unknown, "HTTP " + std::to_string(response.status)};
    const Json body = parseBody(response, "cancelorder");
    if (body.value("result", "") != "success")
        return {CancelOutcome::Unknown, optionalText(body, "error").substr(0, 64)};
    const std::string state = text(field(body, "cancelStatus", "cancelorder"), "status", "cancelorder");
    if (state == "cancelled")
        return {CancelOutcome::Cancelled, state};
    if (state == "filled")
        return {CancelOutcome::AlreadyFilled, state};
    if (state == "notFound")
        return {CancelOutcome::NotFound, state};
    return {CancelOutcome::Unknown, state.substr(0, 64)};
}


HttpRequest KrakenFuturesProtocol::openOrdersRequest()
{
    return privateGet("openorders", "openorders");
}


std::vector<OpenOrder> KrakenFuturesProtocol::parseOpenOrders(const HttpResponse& response) const
{
    const std::string endpoint = "openorders";
    const Json body = successBody(response, endpoint);
    std::vector<OpenOrder> result;
    for (const Json& row : rows(body, "openOrders", endpoint)) {
        OpenOrder order;
        order.venue_order_id = text(row, "order_id", endpoint);
        order.client_order_id = optionalText(row, "cliOrdId");
        order.symbol = text(row, "symbol", endpoint);
        order.side = side(row, "side", endpoint);
        order.filled = number(row, "filledSize", endpoint);
        order.unfilled = number(row, "unfilledSize", endpoint);
        result.push_back(order);
    }
    return result;
}


HttpRequest KrakenFuturesProtocol::fillsRequest()
{
    return privateGet("fills", "fills"); // Last 100 fills, newest first.
}


std::vector<VenueFill> KrakenFuturesProtocol::parseFills(const HttpResponse& response) const
{
    const std::string endpoint = "fills";
    const Json body = successBody(response, endpoint);
    std::vector<VenueFill> result;
    for (const Json& row : rows(body, "fills", endpoint)) {
        VenueFill fill;
        fill.venue_fill_id = text(row, "fill_id", endpoint);
        fill.venue_order_id = text(row, "order_id", endpoint);
        fill.client_order_id = optionalText(row, "cliOrdId");
        fill.symbol = text(row, "symbol", endpoint);
        fill.side = side(row, "side", endpoint);
        fill.quantity = number(row, "size", endpoint);
        fill.price = number(row, "price", endpoint);
        fill.fill_time = text(row, "fillTime", endpoint);
        fill.fill_type = optionalText(row, "fillType");
        if (fill.quantity <= 0.0 || fill.price <= 0.0)
            throw ProtocolError(endpoint + ": fill size and price must be positive");
        std::tie(fill.fill_time_ms, fill.day) = utcTime(fill.fill_time, endpoint);
        result.push_back(fill);
    }
    return result;
}


HttpRequest KrakenFuturesProtocol::accountsRequest()
{
    return privateGet("accounts", "accounts");
}


HttpRequest KrakenFuturesProtocol::positionsRequest()
{
    return privateGet("openpositions", "openpositions");
}


AccountState KrakenFuturesProtocol::parseAccount(const HttpResponse& accounts, const HttpResponse& positions) const
{
    AccountState state;

    const Json wallets = successBody(accounts, "accounts");
    const Json& flex = field(field(wallets, "accounts", "accounts"), "flex", "accounts");
    if (optionalText(flex, "type") != "multiCollateralMarginAccount")
        throw ProtocolError("accounts: Multi-M wallet unavailable");
    const Json& currencies = field(flex, "currencies", "accounts");
    if (!currencies.is_object())
        throw ProtocolError("accounts: currencies is not an object");
    for (const auto& [code, row] : currencies.items()) {
        if (upper(code) == "USD")
            state.usd_balance = number(row, "quantity", "accounts");
        else
            // "collateral": the currency's USD value after Kraken's haircut (1% for BTC), the
            // amount Kraken itself counts as margin.
            state.collateral_usd += number(row, "collateral", "accounts");
    }
    if (state.collateral_usd < 0.0)
        throw ProtocolError("accounts: negative non-USD collateral");

    const Json open = successBody(positions, "openpositions");
    for (const Json& row : rows(open, "openPositions", "openpositions")) {
        const std::string direction = text(row, "side", "openpositions");
        if (direction != "long" && direction != "short")
            throw ProtocolError("openpositions: unknown side");
        const double size = number(row, "size", "openpositions");
        // "price": the position's average entry price (UNCONFIRMED until the first real position).
        state.positions.push_back({text(row, "symbol", "openpositions"), direction == "long" ? size : -size,
                                   number(row, "price", "openpositions")});
    }
    return state;
}


HttpRequest KrakenFuturesProtocol::accountLogRequest()
{
    // History API (outside /derivatives): GET with a query string, signed with the query as
    // postData and no nonce, like the other GETs. The newest 500 entries; the adapter keeps
    // the ids it has seen. UNCONFIRMED until the first read with a real key.
    const std::string query = "count=500";
    HttpRequest request;
    request.method = "GET";
    request.url = host_ + ACCOUNT_LOG_PATH + "?" + query;
    request.endpoint = "account-log";
    request.headers = {{"APIKey", credentials_.api_key},
                       {"Authent", authent(credentials_.api_secret, query, "", ACCOUNT_LOG_PATH)},
                       {"Accept", "application/json"}};
    return request;
}


AccountLog KrakenFuturesProtocol::parseAccountLog(const HttpResponse& response) const
{
    const std::string endpoint = "account-log";
    const Json body = parseBody(response, endpoint);
    AccountLog log;
    for (const Json& row : rows(body, "logs", endpoint)) {
        const Json& idField = field(row, "id", endpoint);
        if (!idField.is_number_integer())
            throw ProtocolError(endpoint + ": field id is not an integer");
        const std::string id = std::to_string(idField.get<long long>());
        const std::string info = text(row, "info", endpoint);
        const auto [timeMs, day] = utcTime(text(row, "date", endpoint), endpoint);

        const auto optionalNumber = [&](const char* name) -> double {
            return row.contains(name) && !row.at(name).is_null() ? number(row, name, endpoint) : 0.0;
        };
        const double fee = optionalNumber("fee");                    // Paid, positive.
        const double funding = optionalNumber("realized_funding");   // Balance change. UNCONFIRMED sign.
        const std::string currency = upper(optionalText(row, "asset"));
        const bool usd = currency.empty() || currency == "USD";
        const std::string wallet = optionalText(row, "margin_account");

        if (fee != 0.0 || funding != 0.0) {
            if (!usd) {
                log.unaccounted.push_back(info + " in " + currency);
                continue;
            }
            if (fee != 0.0)
                log.entries.push_back({id + ":fee", VenueCostKind::Fee, day, timeMs, optionalText(row, "contract"),
                                       optionalText(row, "execution"), -fee});
            if (funding != 0.0)
                log.entries.push_back({id + ":funding", VenueCostKind::Funding, day, timeMs,
                                       optionalText(row, "contract"), {}, funding});
            continue;
        }
        // A trade's PnL follows from the fills; nothing else to book.
        if (info == "futures trade" || info == "funding rate change")
            continue;
        if (!usd || (!wallet.empty() && wallet != "flex")) {
            log.not_cash.push_back(info + (usd ? " in wallet " + wallet : " in " + currency));
            continue;
        }

        // Other USD changes of the Multi-M wallet: the change of its USD balance. Kinds are
        // recognised by words in "info" (UNCONFIRMED: Kraken does not list them).
        const double change = optionalNumber("new_balance") - optionalNumber("old_balance");
        if (change == 0.0)
            continue;
        const auto mentions = [&](const char* word) { return info.find(word) != std::string::npos; };
        if (mentions("interest"))
            log.entries.push_back({id + ":interest", VenueCostKind::Interest, day, timeMs, {}, {}, change});
        else if (mentions("transfer") || mentions("conversion"))
            log.entries.push_back({id + ":flow", VenueCostKind::CapitalFlow, day, timeMs, {}, {}, change});
        else
            log.unaccounted.push_back(info);
    }
    return log;
}


std::optional<HttpRequest> KrakenFuturesProtocol::deadMansSwitchRequest(int timeoutSeconds)
{
    // UNCONFIRMED: path and parameter taken from Kraken's rate-limit guide and SDK usage.
    if (timeoutSeconds <= 0)
        return std::nullopt;
    return privatePost("cancelallordersafter", "cancelallordersafter", "timeout=" + std::to_string(timeoutSeconds));
}

} // namespace Venue
