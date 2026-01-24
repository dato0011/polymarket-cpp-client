#include "clob_client.hpp"
#include "order_signer.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <memory>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace polymarket
{

    // Exchange addresses for Polygon mainnet
    static const std::string EXCHANGE_ADDRESS = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
    static const std::string NEG_RISK_EXCHANGE_ADDRESS = "0xC5d563A36AE78145C45a50134d48A1215220f80a";

    // Data API URL for positions
    static const std::string DATA_API_URL = "https://data-api.polymarket.com";

    namespace
    {
        struct RoundConfig
        {
            int price;
            int size;
            int amount;
        };

        const std::map<std::string, RoundConfig> kRoundingConfig = {
            {"0.1", {1, 2, 3}},
            {"0.01", {2, 2, 4}},
            {"0.001", {3, 2, 5}},
            {"0.0001", {4, 2, 6}},
        };

        std::string normalize_tick_size(const std::string &tick_size)
        {
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss << std::setprecision(6) << std::stod(tick_size);
            std::string normalized = oss.str();

            auto dot = normalized.find('.');
            if (dot == std::string::npos)
            {
                return normalized;
            }

            size_t end = normalized.size();
            while (end > dot + 1 && normalized[end - 1] == '0')
            {
                --end;
            }
            if (end == dot + 1)
            {
                end = dot;
            }
            normalized.resize(end);
            return normalized;
        }

        bool is_tick_size_smaller(const std::string &a, const std::string &b)
        {
            return std::stod(a) < std::stod(b);
        }

        bool price_valid(double price, const std::string &tick_size)
        {
            double tick = std::stod(tick_size);
            return price >= tick && price <= 1.0 - tick;
        }

        int decimal_places(double value)
        {
            if (std::floor(value) == value)
            {
                return 0;
            }

            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss << std::setprecision(12) << value;
            std::string str = oss.str();

            auto dot = str.find('.');
            if (dot == std::string::npos)
            {
                return 0;
            }

            size_t end = str.size();
            while (end > dot + 1 && str[end - 1] == '0')
            {
                --end;
            }
            if (end == dot + 1)
            {
                return 0;
            }
            return static_cast<int>(end - dot - 1);
        }

        double round_normal(double value, int decimals)
        {
            if (decimal_places(value) <= decimals)
            {
                return value;
            }
            double scale = std::pow(10.0, decimals);
            return std::round((value + std::numeric_limits<double>::epsilon()) * scale) / scale;
        }

        double round_down(double value, int decimals)
        {
            if (decimal_places(value) <= decimals)
            {
                return value;
            }
            double scale = std::pow(10.0, decimals);
            return std::floor(value * scale) / scale;
        }

        double round_up(double value, int decimals)
        {
            if (decimal_places(value) <= decimals)
            {
                return value;
            }
            double scale = std::pow(10.0, decimals);
            return std::ceil(value * scale) / scale;
        }

        RoundConfig get_round_config(const std::string &tick_size)
        {
            auto normalized = normalize_tick_size(tick_size);
            auto it = kRoundingConfig.find(normalized);
            if (it == kRoundingConfig.end())
            {
                throw std::runtime_error("unsupported tick size: " + tick_size);
            }
            return it->second;
        }

        double calculate_buy_market_price(const std::vector<PriceLevel> &positions,
                                           double amount_to_match,
                                           OrderType order_type)
        {
            if (positions.empty())
            {
                throw std::runtime_error("no match");
            }

            double sum = 0.0;
            for (auto it = positions.rbegin(); it != positions.rend(); ++it)
            {
                sum += it->size * it->price;
                if (sum >= amount_to_match)
                {
                    return it->price;
                }
            }

            if (order_type == OrderType::FOK)
            {
                throw std::runtime_error("no match");
            }

            return positions.front().price;
        }

        double calculate_sell_market_price(const std::vector<PriceLevel> &positions,
                                            double amount_to_match,
                                            OrderType order_type)
        {
            if (positions.empty())
            {
                throw std::runtime_error("no match");
            }

            double sum = 0.0;
            for (auto it = positions.rbegin(); it != positions.rend(); ++it)
            {
                sum += it->size;
                if (sum >= amount_to_match)
                {
                    return it->price;
                }
            }

            if (order_type == OrderType::FOK)
            {
                throw std::runtime_error("no match");
            }

            return positions.front().price;
        }
    } // namespace

    ClobClient::ClobClient(const std::string &base_url, int chain_id)
        : chain_id_(chain_id), base_url_(base_url), sig_type_(SignatureType::EOA)
    {
        http_.set_base_url(base_url);
        http_.set_timeout_ms(10000);
    }

    ClobClient::ClobClient(const std::string &base_url, int chain_id,
                           const std::string &private_key,
                           const ApiCredentials &creds,
                           SignatureType sig_type,
                           const std::string &funder_address)
        : chain_id_(chain_id), base_url_(base_url), funder_address_(funder_address), sig_type_(sig_type)
    {
        http_.set_base_url(base_url);
        http_.set_timeout_ms(10000);

        order_signer_ = std::make_unique<OrderSigner>(private_key, chain_id);
        api_creds_ = std::make_unique<ApiCredentials>(creds);
    }

    ClobClient::~ClobClient() = default;

    std::string ClobClient::get_exchange_address() const
    {
        return EXCHANGE_ADDRESS;
    }

    std::string ClobClient::get_neg_risk_exchange_address() const
    {
        return NEG_RISK_EXCHANGE_ADDRESS;
    }

    bool ClobClient::warm_connection()
    {
        // Step 1: Hit a cheap GET endpoint to establish TCP/TLS
        auto time_response = get_server_time();
        if (!time_response.has_value())
        {
            return false;
        }

        // Step 2: Hit markets endpoint to warm Cloudflare cache
        auto markets = get_markets("");

        // Connection is now warm
        return true;
    }

    std::string ClobClient::get_address() const
    {
        if (!order_signer_)
            return "";
        return order_signer_->address();
    }

    std::map<std::string, std::string> ClobClient::get_l2_headers(const std::string &method,
                                                                  const std::string &path,
                                                                  const std::string &body) const {
        if (!order_signer_ || !api_creds_)
        {
            throw std::runtime_error("Client not authenticated");
        }

        auto headers = order_signer_->generate_l2_headers(*api_creds_, method, path, body);

        std::map<std::string, std::string> result;
        result["POLY_ADDRESS"] = headers.poly_address;
        result["POLY_SIGNATURE"] = headers.poly_signature;
        result["POLY_TIMESTAMP"] = headers.poly_timestamp;
        result["POLY_API_KEY"] = headers.poly_api_key;
        result["POLY_PASSPHRASE"] = headers.poly_passphrase;

        return result;
    }

    std::string ClobClient::order_type_to_string(OrderType type)
    {
        switch (type)
        {
        case OrderType::GTC:
            return "GTC";
        case OrderType::GTD:
            return "GTD";
        case OrderType::FOK:
            return "FOK";
        case OrderType::FAK:
            return "IOC";
        default:
            return "GTC";
        }
    }

    std::string ClobClient::order_side_to_string(OrderSide side)
    {
        return side == OrderSide::BUY ? "BUY" : "SELL";
    }

    // ============================================================
    // PUBLIC ENDPOINTS
    // ============================================================

    std::optional<uint64_t> ClobClient::get_server_time()
    {
        auto response = http_.get("/time");
        if (!response.ok())
            return std::nullopt;

        try
        {
            // Server returns plain timestamp, not JSON
            return std::stoull(response.body);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<ClobMarket> ClobClient::get_markets(const std::string &next_cursor)
    {
        std::string path = "/markets";
        if (!next_cursor.empty())
        {
            path += "?next_cursor=" + next_cursor;
        }

        auto response = http_.get(path);
        if (!response.ok())
            return {};

        return parse_markets(response.body);
    }

    std::optional<ClobMarket> ClobClient::get_market(const std::string &condition_id)
    {
        auto response = http_.get("/markets/" + condition_id);
        if (!response.ok())
            return std::nullopt;

        auto markets = parse_markets("[" + response.body + "]");
        if (markets.empty())
            return std::nullopt;

        return markets[0];
    }

    std::vector<ClobMarket> ClobClient::get_sampling_markets(const std::string &next_cursor)
    {
        std::string path = "/sampling-markets";
        if (!next_cursor.empty())
        {
            path += "?next_cursor=" + next_cursor;
        }

        auto response = http_.get(path);
        if (!response.ok())
            return {};

        return parse_markets(response.body);
    }

    std::vector<ClobMarket> ClobClient::get_simplified_markets(const std::string &next_cursor)
    {
        std::string path = "/simplified-markets";
        if (!next_cursor.empty())
        {
            path += "?next_cursor=" + next_cursor;
        }

        auto response = http_.get(path);
        if (!response.ok())
            return {};

        return parse_markets(response.body);
    }

    std::vector<ClobMarket> ClobClient::get_sampling_simplified_markets(const std::string &next_cursor)
    {
        std::string path = "/sampling-simplified-markets";
        if (!next_cursor.empty())
        {
            path += "?next_cursor=" + next_cursor;
        }

        auto response = http_.get(path);
        if (!response.ok())
            return {};

        return parse_markets(response.body);
    }

    std::optional<Orderbook> ClobClient::get_order_book(const std::string &token_id)
    {
        auto response = http_.get("/book?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        return parse_orderbook(response.body);
    }

    std::map<std::string, Orderbook> ClobClient::get_order_books(const std::vector<std::string> &token_ids)
    {
        std::map<std::string, Orderbook> result;

        // Build comma-separated token IDs
        std::string ids;
        for (size_t i = 0; i < token_ids.size(); i++)
        {
            if (i > 0)
                ids += ",";
            ids += token_ids[i];
        }

        auto response = http_.get("/books?token_ids=" + ids);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    if (item.contains("asset_id"))
                    {
                        auto book = parse_orderbook(item.dump());
                        if (book)
                        {
                            result[item["asset_id"].get<std::string>()] = *book;
                        }
                    }
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    double ClobClient::calculate_market_price(const std::string &token_id, OrderSide side, double amount,
                                              OrderType order_type)
    {
        auto book = get_order_book(token_id);
        if (!book)
        {
            throw std::runtime_error("no orderbook");
        }

        if (side == OrderSide::BUY)
        {
            if (book->asks.empty())
            {
                throw std::runtime_error("no match");
            }
            return calculate_buy_market_price(book->asks, amount, order_type);
        }

        if (book->bids.empty())
        {
            throw std::runtime_error("no match");
        }
        return calculate_sell_market_price(book->bids, amount, order_type);
    }

    std::optional<PriceInfo> ClobClient::get_price(const std::string &token_id, const std::string &side)
    {
        auto response = http_.get("/price?token_id=" + token_id + "&side=" + side);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            PriceInfo info;
            info.token_id = token_id;
            info.price = std::stod(j.value("price", "0"));
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<PriceInfo> ClobClient::get_prices(const std::vector<std::string> &token_ids, const std::string &side)
    {
        std::vector<PriceInfo> result;

        std::string ids;
        for (size_t i = 0; i < token_ids.size(); i++)
        {
            if (i > 0)
                ids += ",";
            ids += token_ids[i];
        }

        auto response = http_.get("/prices?token_ids=" + ids + "&side=" + side);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (size_t i = 0; i < j.size() && i < token_ids.size(); i++)
                {
                    PriceInfo info;
                    info.token_id = token_ids[i];
                    info.price = std::stod(j[i].value("price", "0"));
                    result.push_back(info);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::optional<PriceInfo> ClobClient::get_last_trade_price(const std::string &token_id)
    {
        auto response = http_.get("/last-trade-price?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            PriceInfo info;
            info.token_id = token_id;
            info.price = std::stod(j.value("price", "0"));
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<PriceInfo> ClobClient::get_last_trades_prices(const std::vector<std::string> &token_ids)
    {
        std::vector<PriceInfo> result;

        std::string ids;
        for (size_t i = 0; i < token_ids.size(); i++)
        {
            if (i > 0)
                ids += ",";
            ids += token_ids[i];
        }

        auto response = http_.get("/last-trades-prices?token_ids=" + ids);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (size_t i = 0; i < j.size() && i < token_ids.size(); i++)
                {
                    PriceInfo info;
                    info.token_id = token_ids[i];
                    info.price = std::stod(j[i].value("price", "0"));
                    result.push_back(info);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::optional<MidpointInfo> ClobClient::get_midpoint(const std::string &token_id)
    {
        auto response = http_.get("/midpoint?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            MidpointInfo info;
            info.token_id = token_id;
            info.mid = std::stod(j.value("mid", "0"));
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<MidpointInfo> ClobClient::get_midpoints(const std::vector<std::string> &token_ids)
    {
        std::vector<MidpointInfo> result;

        std::string ids;
        for (size_t i = 0; i < token_ids.size(); i++)
        {
            if (i > 0)
                ids += ",";
            ids += token_ids[i];
        }

        auto response = http_.get("/midpoints?token_ids=" + ids);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (size_t i = 0; i < j.size() && i < token_ids.size(); i++)
                {
                    MidpointInfo info;
                    info.token_id = token_ids[i];
                    info.mid = std::stod(j[i].value("mid", "0"));
                    result.push_back(info);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::optional<SpreadInfo> ClobClient::get_spread(const std::string &token_id)
    {
        auto response = http_.get("/spread?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            SpreadInfo info;
            info.token_id = token_id;
            info.spread = std::stod(j.value("spread", "0"));
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<SpreadInfo> ClobClient::get_spreads(const std::vector<std::string> &token_ids)
    {
        std::vector<SpreadInfo> result;

        std::string ids;
        for (size_t i = 0; i < token_ids.size(); i++)
        {
            if (i > 0)
                ids += ",";
            ids += token_ids[i];
        }

        auto response = http_.get("/spreads?token_ids=" + ids);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (size_t i = 0; i < j.size() && i < token_ids.size(); i++)
                {
                    SpreadInfo info;
                    info.token_id = token_ids[i];
                    info.spread = std::stod(j[i].value("spread", "0"));
                    result.push_back(info);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::optional<TickSizeInfo> ClobClient::get_tick_size(const std::string &token_id)
    {
        auto response = http_.get("/tick-size?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            TickSizeInfo info;
            info.minimum_tick_size = j.value("minimum_tick_size", "0.01");
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<NegRiskInfo> ClobClient::get_neg_risk(const std::string &token_id)
    {
        auto response = http_.get("/neg-risk?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            NegRiskInfo info;
            info.neg_risk = j.value("neg_risk", false);
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<int> ClobClient::get_fee_rate_bps(const std::string &token_id)
    {
        auto response = http_.get("/fee-rate?token_id=" + token_id);
        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            if (j.contains("base_fee"))
            {
                return j.value("base_fee", 0);
            }
            return 0;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<ClobClient::PriceHistoryPoint> ClobClient::get_prices_history(
        const std::string &token_id,
        uint64_t start_ts,
        uint64_t end_ts,
        const std::string &interval,
        const std::string &fidelity)
    {
        std::vector<PriceHistoryPoint> result;

        std::string path = "/prices-history?token_id=" + token_id;
        if (start_ts > 0)
            path += "&startTs=" + std::to_string(start_ts);
        if (end_ts > 0)
            path += "&endTs=" + std::to_string(end_ts);
        path += "&interval=" + interval;
        path += "&fidelity=" + fidelity;

        auto response = http_.get(path);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.contains("history") && j["history"].is_array())
            {
                for (const auto &item : j["history"])
                {
                    PriceHistoryPoint point;
                    point.timestamp = item.value("t", 0ULL);
                    point.price = std::stod(item.value("p", "0"));
                    result.push_back(point);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::vector<Trade> ClobClient::get_market_trades_events(const std::string &condition_id,
                                                            const std::string &next_cursor)
    {
        std::string path = "/trades?market=" + condition_id;
        if (!next_cursor.empty())
        {
            path += "&next_cursor=" + next_cursor;
        }

        auto response = http_.get(path);
        if (!response.ok())
            return {};

        return parse_trades(response.body);
    }

    // ============================================================
    // AUTHENTICATED ENDPOINTS (L1)
    // ============================================================

    ApiCredentials ClobClient::create_api_key(uint64_t nonce)
    {
        if (!order_signer_)
        {
            throw std::runtime_error("Client not authenticated");
        }
        return order_signer_->create_api_credentials(http_, nonce, funder_address_);
    }

    ApiCredentials ClobClient::derive_api_key()
    {
        if (!order_signer_)
        {
            throw std::runtime_error("Client not authenticated");
        }
        return order_signer_->derive_api_credentials(http_, funder_address_);
    }

    ApiCredentials ClobClient::create_or_derive_api_key()
    {
        if (!order_signer_)
        {
            throw std::runtime_error("Client not authenticated");
        }
        return order_signer_->create_or_derive_api_credentials(http_, funder_address_);
    }

    std::vector<std::string> ClobClient::get_api_keys()
    {
        std::vector<std::string> result;

        auto headers = get_l2_headers("GET", "/auth/api-keys", "");
        auto response = http_.get("/auth/api-keys", headers);

        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    result.push_back(item.get<std::string>());
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    bool ClobClient::delete_api_key() const {
        auto headers = get_l2_headers("DELETE", "/auth/api-key", "");

        // Need to add DELETE method to HttpClient - for now use POST with method override
        // This is a limitation - would need to extend HttpClient
        return false; // TODO: Implement DELETE method
    }

    // ============================================================
    // AUTHENTICATED ENDPOINTS (L2 - Trading)
    // ============================================================

    SignedOrder ClobClient::create_order(const CreateOrderParams &params)
    {
        if (!order_signer_)
        {
            throw std::runtime_error("Client not authenticated");
        }

        // Use cached neg_risk if provided, otherwise fetch from API
        bool is_neg_risk = false;
        if (params.neg_risk.has_value())
        {
            is_neg_risk = params.neg_risk.value();
        }
        else
        {
            auto neg_risk_info = get_neg_risk(params.token_id);
            is_neg_risk = neg_risk_info && neg_risk_info->neg_risk;
        }

        std::string exchange_addr = is_neg_risk ? NEG_RISK_EXCHANGE_ADDRESS : EXCHANGE_ADDRESS;

        // Calculate amounts
        double maker_amount, taker_amount;
        if (params.side == OrderSide::BUY)
        {
            // BUY: maker pays USDC, receives shares
            maker_amount = params.size * params.price;
            taker_amount = params.size;
        }
        else
        {
            // SELL: maker pays shares, receives USDC
            maker_amount = params.size;
            taker_amount = params.size * params.price;
        }

        OrderData order_data;
        order_data.maker = funder_address_.empty() ? order_signer_->address() : funder_address_;
        order_data.taker = "0x0000000000000000000000000000000000000000";
        order_data.token_id = params.token_id;
        order_data.maker_amount = to_wei(maker_amount, 6);
        order_data.taker_amount = to_wei(taker_amount, 6);
        order_data.side = params.side;
        order_data.fee_rate_bps = params.fee_rate_bps;
        order_data.nonce = params.nonce;
        order_data.signer = order_signer_->address();
        order_data.expiration = params.expiration;
        order_data.signature_type = sig_type_;

        return order_signer_->sign_order(order_data, exchange_addr);
    }

    SignedOrder ClobClient::create_market_order(const CreateMarketOrderParams &params)
    {
        if (!order_signer_)
        {
            throw std::runtime_error("Client not authenticated");
        }

        // Get current price if not specified
        double price = 0.5;
        if (params.price)
        {
            price = *params.price;
        }
        else
        {
            auto price_info = get_price(params.token_id, params.side == OrderSide::BUY ? "buy" : "sell");
            if (price_info)
            {
                price = price_info->price;
            }
        }

        // Calculate size from amount
        double size;
        if (params.side == OrderSide::BUY)
        {
            size = params.amount / price;
        }
        else
        {
            size = params.amount;
        }

        CreateOrderParams order_params;
        order_params.token_id = params.token_id;
        order_params.price = price;
        order_params.size = size;
        order_params.side = params.side;

        return create_order(order_params);
    }

    SignedOrder ClobClient::create_market_order_v2(const CreateMarketOrderParams &params)
    {
        if (!order_signer_)
        {
            throw std::runtime_error("Client not authenticated");
        }

        std::string tick_size;
        double price = 0.0;
        bool is_neg_risk = false;
        int market_fee_rate_bps = 0;

        if (params.strict_no_fetch)
        {
            if (!params.tick_size.has_value() || params.tick_size->empty())
            {
                throw std::runtime_error("strict_no_fetch requires tick_size");
            }
            if (!params.price.has_value() || *params.price <= 0.0)
            {
                throw std::runtime_error("strict_no_fetch requires price");
            }
            if (!params.neg_risk.has_value())
            {
                throw std::runtime_error("strict_no_fetch requires neg_risk");
            }
            if (!params.fee_rate_bps_provided || params.fee_rate_bps.empty())
            {
                throw std::runtime_error("strict_no_fetch requires fee_rate_bps");
            }

            tick_size = *params.tick_size;
            price = *params.price;
            is_neg_risk = params.neg_risk.value();
        }
        else
        {
            std::string min_tick_size = "0.01";
            // if (auto tick_info = get_tick_size(params.token_id))
            // {
            //     min_tick_size = tick_info->minimum_tick_size;
            // }

            tick_size = min_tick_size;
            if (params.tick_size.has_value() && !params.tick_size->empty())
            {
                if (is_tick_size_smaller(*params.tick_size, min_tick_size))
                {
                    throw std::runtime_error("invalid tick size (" + *params.tick_size +
                                             "), minimum for the market is " + min_tick_size);
                }
                tick_size = *params.tick_size;
            }

            if (params.price.has_value() && *params.price > 0.0)
            {
                price = *params.price;
            }
            else
            {
                price = calculate_market_price(params.token_id, params.side, params.amount, params.order_type);
            }

            if (params.neg_risk.has_value())
            {
                is_neg_risk = params.neg_risk.value();
            }
            else
            {
                auto neg_risk_info = get_neg_risk(params.token_id);
                is_neg_risk = neg_risk_info && neg_risk_info->neg_risk;
            }

            if (!params.fee_rate_bps_provided)
            {
                market_fee_rate_bps = 1000; // TODO
            }
        }

        if (!price_valid(price, tick_size))
        {
            throw std::runtime_error("invalid price (" + std::to_string(price) + "), min: " + tick_size +
                                     " - max: " + std::to_string(1.0 - std::stod(tick_size)));
        }

        std::string exchange_addr = is_neg_risk ? NEG_RISK_EXCHANGE_ADDRESS : EXCHANGE_ADDRESS;
        RoundConfig round_config = get_round_config(tick_size);
        double raw_price = round_normal(price, round_config.price);

        double raw_maker_amt = round_down(params.amount, round_config.size);
        double raw_taker_amt = 0.0;
        if (params.side == OrderSide::BUY)
        {
            raw_taker_amt = raw_maker_amt / raw_price;
            if (decimal_places(raw_taker_amt) > round_config.amount)
            {
                raw_taker_amt = round_up(raw_taker_amt, round_config.amount + 4);
                if (decimal_places(raw_taker_amt) > round_config.amount)
                {
                    raw_taker_amt = round_down(raw_taker_amt, round_config.amount);
                }
            }
        }
        else if (params.side == OrderSide::SELL)
        {
            raw_taker_amt = raw_maker_amt * raw_price;
            if (decimal_places(raw_taker_amt) > round_config.amount)
            {
                raw_taker_amt = round_up(raw_taker_amt, round_config.amount + 4);
                if (decimal_places(raw_taker_amt) > round_config.amount)
                {
                    raw_taker_amt = round_down(raw_taker_amt, round_config.amount);
                }
            }
        }
        else
        {
            throw std::runtime_error("invalid order side");
        }

        OrderData order_data;
        order_data.maker = funder_address_.empty() ? order_signer_->address() : funder_address_;
        order_data.taker = params.taker.empty() ? "0x0000000000000000000000000000000000000000" : params.taker;
        order_data.token_id = params.token_id;
        order_data.maker_amount = to_wei(raw_maker_amt, 6);
        order_data.taker_amount = to_wei(raw_taker_amt, 6);
        order_data.side = params.side;
        std::string fee_rate_bps = params.fee_rate_bps.empty() ? "0" : params.fee_rate_bps;
        if (params.fee_rate_bps_provided)
        {
            // Caller-provided fee rate; skip lookup/validation.
        }
        else if (market_fee_rate_bps > 0)
        {
            if (fee_rate_bps != "0")
            {
                int provided_fee = 0;
                try
                {
                    provided_fee = std::stoi(fee_rate_bps);
                }
                catch (...)
                {
                    throw std::runtime_error("invalid fee rate (" + fee_rate_bps + ")");
                }
                if (provided_fee != market_fee_rate_bps)
                {
                    throw std::runtime_error("invalid fee rate (" + fee_rate_bps +
                                             "), current market's taker fee: " +
                                             std::to_string(market_fee_rate_bps));
                }
            }
            fee_rate_bps = std::to_string(market_fee_rate_bps);
        }
        order_data.fee_rate_bps = fee_rate_bps;
        order_data.nonce = params.nonce.empty() ? "0" : params.nonce;
        order_data.signer = order_signer_->address();
        order_data.expiration = params.expiration.empty() ? "0" : params.expiration;
        order_data.signature_type = sig_type_;

        return order_signer_->sign_order(order_data, exchange_addr);
    }

    OrderResponse ClobClient::post_order(const SignedOrder &order, OrderType order_type, bool post_only)
    {
        if (post_only && order_type != OrderType::GTC && order_type != OrderType::GTD)
        {
            throw std::runtime_error("post_only is only supported for GTC and GTD orders");
        }
        if (!api_creds_)
        {
            throw std::runtime_error("Client not authenticated");
        }

        nlohmann::ordered_json body;
        nlohmann::ordered_json order_json;
        order_json["salt"] = std::stoll(order.salt);
        order_json["maker"] = order.maker;
        order_json["signer"] = order.signer;
        order_json["taker"] = order.taker;
        order_json["tokenId"] = order.token_id;
        order_json["makerAmount"] = order.maker_amount;
        order_json["takerAmount"] = order.taker_amount;
        order_json["side"] = order.side == 0 ? "BUY" : "SELL";
        order_json["expiration"] = order.expiration;
        order_json["nonce"] = order.nonce;
        order_json["feeRateBps"] = order.fee_rate_bps;
        order_json["signatureType"] = order.signature_type;
        order_json["signature"] = order.signature;

        body["order"] = order_json;
        body["owner"] = api_creds_->api_key;
        body["orderType"] = order_type_to_string(order_type);
        body["deferExec"] = false;
        if (post_only)
        {
            body["postOnly"] = true;
        }

        std::string body_str = body.dump();
        auto headers = get_l2_headers("POST", "/order", body_str);
        auto response = http_.post("/order", body_str, headers);

        auto result = parse_order_response(response.body);
        result.elapsed_ms = response.elapsed_ms;
        return result;
    }

    std::vector<OrderResponse> ClobClient::post_orders(const std::vector<BatchOrderEntry> &orders, bool post_only)
    {
        std::vector<OrderResponse> results;

        if (orders.empty())
            return results;

        if (post_only)
        {
            for (const auto &entry : orders)
            {
                if (entry.order_type != OrderType::GTC && entry.order_type != OrderType::GTD)
                {
                    throw std::runtime_error("post_only is only supported for GTC and GTD orders");
                }
            }
        }
        if (!api_creds_)
        {
            throw std::runtime_error("Client not authenticated");
        }

        nlohmann::ordered_json body = nlohmann::ordered_json::array();
        for (const auto &entry : orders)
        {
            nlohmann::ordered_json order_json;
            nlohmann::ordered_json signed_json;
            signed_json["salt"] = std::stoll(entry.order.salt);
            signed_json["maker"] = entry.order.maker;
            signed_json["signer"] = entry.order.signer;
            signed_json["taker"] = entry.order.taker;
            signed_json["tokenId"] = entry.order.token_id;
            signed_json["makerAmount"] = entry.order.maker_amount;
            signed_json["takerAmount"] = entry.order.taker_amount;
            signed_json["side"] = entry.order.side == 0 ? "BUY" : "SELL";
            signed_json["expiration"] = entry.order.expiration;
            signed_json["nonce"] = entry.order.nonce;
            signed_json["feeRateBps"] = entry.order.fee_rate_bps;
            signed_json["signatureType"] = entry.order.signature_type;
            signed_json["signature"] = entry.order.signature;
            order_json["order"] = signed_json;
            order_json["owner"] = api_creds_->api_key;
            order_json["orderType"] = order_type_to_string(entry.order_type);
            order_json["deferExec"] = false;
            if (post_only)
            {
                order_json["postOnly"] = true;
            }
            body.push_back(order_json);
        }

        std::string body_str = body.dump();
        auto headers = get_l2_headers("POST", "/orders", body_str);
        auto response = http_.post("/orders", body_str, headers);

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    auto parsed = parse_order_response(item.dump());
                    parsed.elapsed_ms = response.elapsed_ms;
                    results.push_back(parsed);
                }
            }
        }
        catch (...)
        {
            // Single error response
            auto parsed = parse_order_response(response.body);
            parsed.elapsed_ms = response.elapsed_ms;
            results.push_back(parsed);
        }

        return results;
    }

    OrderResponse ClobClient::create_and_post_order(const CreateOrderParams &params, OrderType order_type)
    {
        auto signed_order = create_order(params);
        return post_order(signed_order, order_type);
    }

    OrderResponse ClobClient::create_and_post_market_order(const CreateMarketOrderParams &params, OrderType order_type)
    {
        auto signed_order = create_market_order(params);
        return post_order(signed_order, order_type);
    }

    OrderResponse ClobClient::create_and_post_market_order_v2(const CreateMarketOrderParams &params)
    {
        auto signed_order = create_market_order_v2(params);
        return post_order(signed_order, params.order_type);
    }

    void ClobClient::create_and_post_market_order_v2_async(
        const CreateMarketOrderParams &params,
        std::function<void(const OrderResponse &)> callback)
    {
        if (!api_creds_ || !order_signer_)
        {
            OrderResponse response;
            response.success = false;
            response.error_msg = "Client not authenticated";
            if (callback)
            {
                callback(response);
            }
            return;
        }

        struct AsyncMarketOrderState
        {
            CreateMarketOrderParams params;
            std::function<void(const OrderResponse &)> callback;
            bool done = false;
            std::string tick_size;
            double price = 0.0;
            bool neg_risk = false;
            int fee_rate_bps = 0;
            bool fee_rate_provided = false;
        };

        auto state = std::make_shared<AsyncMarketOrderState>();
        state->params = params;
        state->callback = std::move(callback);
        state->fee_rate_provided = params.fee_rate_bps_provided;

        auto finish_with_error = [state](const std::string &message)
        {
            if (state->done)
            {
                return;
            }
            state->done = true;
            OrderResponse response;
            response.success = false;
            response.error_msg = message;
            response.client_order_id = state->params.client_order_id;
            if (state->callback)
            {
                state->callback(response);
            }
        };

        auto resolve_price = std::make_shared<std::function<void()>>();
        auto resolve_neg_risk = std::make_shared<std::function<void()>>();
        auto resolve_fee_rate = std::make_shared<std::function<void()>>();
        auto submit_order = std::make_shared<std::function<void()>>();

        *resolve_price = [this, state, finish_with_error, resolve_neg_risk]()
        {
            if (state->params.price.has_value() && state->params.price.value() > 0.0)
            {
                state->price = state->params.price.value();
                if (!price_valid(state->price, state->tick_size))
                {
                    finish_with_error("invalid price (" + std::to_string(state->price) +
                                      "), min: " + state->tick_size +
                                      " - max: " + std::to_string(1.0 - std::stod(state->tick_size)));
                    return;
                }
                (*resolve_neg_risk)();
                return;
            }

            std::string path = "/book?token_id=" + state->params.token_id;
            http_.get_async(path,
                            [this, state, finish_with_error, resolve_neg_risk](const HttpResponse &http_response) mutable
                            {
                                if (!http_response.ok())
                                {
                                    finish_with_error("no orderbook");
                                    return;
                                }

                                auto book = parse_orderbook(http_response.body);
                                if (!book)
                                {
                                    finish_with_error("no orderbook");
                                    return;
                                }

                                try
                                {
                                    if (state->params.side == OrderSide::BUY)
                                    {
                                        state->price = calculate_buy_market_price(
                                            book->asks, state->params.amount, state->params.order_type);
                                    }
                                    else
                                    {
                                        state->price = calculate_sell_market_price(
                                            book->bids, state->params.amount, state->params.order_type);
                                    }
                                }
                                catch (const std::exception &e)
                                {
                                    finish_with_error(e.what());
                                    return;
                                }

                                if (!price_valid(state->price, state->tick_size))
                                {
                                    finish_with_error("invalid price (" + std::to_string(state->price) +
                                                      "), min: " + state->tick_size +
                                                      " - max: " + std::to_string(1.0 - std::stod(state->tick_size)));
                                    return;
                                }

                                (*resolve_neg_risk)();
                            });
        };

        *resolve_neg_risk = [this, state, finish_with_error, resolve_fee_rate]()
        {
            if (state->params.neg_risk.has_value())
            {
                state->neg_risk = state->params.neg_risk.value();
                (*resolve_fee_rate)();
                return;
            }

            std::string path = "/neg-risk?token_id=" + state->params.token_id;
            http_.get_async(path,
                            [state, finish_with_error, resolve_fee_rate](const HttpResponse &http_response) mutable
                            {
                                if (!http_response.ok())
                                {
                                    finish_with_error("failed to fetch neg risk");
                                    return;
                                }

                                try
                                {
                                    auto j = json::parse(http_response.body);
                                    state->neg_risk = j.value("neg_risk", false);
                                }
                                catch (...)
                                {
                                    finish_with_error("failed to parse neg risk");
                                    return;
                                }

                                (*resolve_fee_rate)();
                            });
        };

        *resolve_fee_rate = [this, state, finish_with_error, submit_order]()
        {
            if (state->fee_rate_provided)
            {
                (*submit_order)();
                return;
            }
            std::string path = "/fee-rate?token_id=" + state->params.token_id;
            http_.get_async(path,
                            [state, finish_with_error, submit_order](const HttpResponse &http_response) mutable
                            {
                                if (!http_response.ok())
                                {
                                    finish_with_error("failed to fetch fee rate");
                                    return;
                                }

                                try
                                {
                                    auto j = json::parse(http_response.body);
                                    state->fee_rate_bps = j.value("base_fee", 0);
                                }
                                catch (...)
                                {
                                    finish_with_error("failed to parse fee rate");
                                    return;
                                }

                                (*submit_order)();
                            });
        };

        *submit_order = [this, state, finish_with_error]()
        {
            std::string fee_rate_bps = state->params.fee_rate_bps.empty() ? "0" : state->params.fee_rate_bps;
            if (!state->fee_rate_provided && state->fee_rate_bps > 0)
            {
                if (fee_rate_bps != "0")
                {
                    int provided_fee = 0;
                    try
                    {
                        provided_fee = std::stoi(fee_rate_bps);
                    }
                    catch (...)
                    {
                        finish_with_error("invalid fee rate (" + fee_rate_bps + ")");
                        return;
                    }
                    if (provided_fee != state->fee_rate_bps)
                    {
                        finish_with_error("invalid fee rate (" + fee_rate_bps +
                                          "), current market's taker fee: " +
                                          std::to_string(state->fee_rate_bps));
                        return;
                    }
                }
                fee_rate_bps = std::to_string(state->fee_rate_bps);
            }

            std::string exchange_addr = state->neg_risk ? NEG_RISK_EXCHANGE_ADDRESS : EXCHANGE_ADDRESS;
            RoundConfig round_config = get_round_config(state->tick_size);
            double raw_price = round_normal(state->price, round_config.price);

            double raw_maker_amt = round_down(state->params.amount, round_config.size);
            double raw_taker_amt = 0.0;
            if (state->params.side == OrderSide::BUY)
            {
                raw_taker_amt = raw_maker_amt / raw_price;
                if (decimal_places(raw_taker_amt) > round_config.amount)
                {
                    raw_taker_amt = round_up(raw_taker_amt, round_config.amount + 4);
                    if (decimal_places(raw_taker_amt) > round_config.amount)
                    {
                        raw_taker_amt = round_down(raw_taker_amt, round_config.amount);
                    }
                }
            }
            else
            {
                raw_taker_amt = raw_maker_amt * raw_price;
                if (decimal_places(raw_taker_amt) > round_config.amount)
                {
                    raw_taker_amt = round_up(raw_taker_amt, round_config.amount + 4);
                    if (decimal_places(raw_taker_amt) > round_config.amount)
                    {
                        raw_taker_amt = round_down(raw_taker_amt, round_config.amount);
                    }
                }
            }

            OrderData order_data;
            order_data.maker = funder_address_.empty() ? order_signer_->address() : funder_address_;
            order_data.taker = state->params.taker.empty()
                                   ? "0x0000000000000000000000000000000000000000"
                                   : state->params.taker;
            order_data.token_id = state->params.token_id;
            order_data.maker_amount = to_wei(raw_maker_amt, 6);
            order_data.taker_amount = to_wei(raw_taker_amt, 6);
            order_data.side = state->params.side;
            order_data.fee_rate_bps = fee_rate_bps;
            order_data.nonce = state->params.nonce.empty() ? "0" : state->params.nonce;
            order_data.signer = order_signer_->address();
            order_data.expiration = state->params.expiration.empty() ? "0" : state->params.expiration;
            order_data.signature_type = sig_type_;

            SignedOrder signed_order = order_signer_->sign_order(order_data, exchange_addr);

            nlohmann::ordered_json body;
            nlohmann::ordered_json order_json;
            order_json["salt"] = std::stoll(signed_order.salt);
            order_json["maker"] = signed_order.maker;
            order_json["signer"] = signed_order.signer;
            order_json["taker"] = signed_order.taker;
            order_json["tokenId"] = signed_order.token_id;
            order_json["makerAmount"] = signed_order.maker_amount;
            order_json["takerAmount"] = signed_order.taker_amount;
            order_json["side"] = signed_order.side == 0 ? "BUY" : "SELL";
            order_json["expiration"] = signed_order.expiration;
            order_json["nonce"] = signed_order.nonce;
            order_json["feeRateBps"] = signed_order.fee_rate_bps;
            order_json["signatureType"] = signed_order.signature_type;
            order_json["signature"] = signed_order.signature;

            body["order"] = order_json;
            body["owner"] = api_creds_->api_key;
            body["orderType"] = order_type_to_string(state->params.order_type);
            body["deferExec"] = false;

            std::string body_str = body.dump();
            std::map<std::string, std::string> headers;
            try
            {
                headers = get_l2_headers("POST", "/order", body_str);
            }
            catch (const std::exception &e)
            {
                finish_with_error(e.what());
                return;
            }

            http_.post_async("/order", body_str, headers,
                             [state](const HttpResponse &http_response) mutable
                             {
                                 OrderResponse order_response = parse_order_response(http_response.body);
                                 if (!http_response.ok())
                                 {
                                     order_response.success = false;
                                     if (order_response.error_msg.empty())
                                     {
                                         if (!http_response.error.empty())
                                         {
                                             order_response.error_msg = http_response.error;
                                         }
                                         else
                                         {
                                             order_response.error_msg =
                                                 "http error: " + std::to_string(http_response.status_code);
                                         }
                                     }
                                     if (order_response.status.empty())
                                     {
                                         order_response.status = std::to_string(http_response.status_code);
                                     }
                                 }
                                 order_response.elapsed_ms = http_response.elapsed_ms;
                                 if (state->callback && !state->done)
                                 {
                                     state->done = true;
                                     order_response.client_order_id = state->params.client_order_id;
                                     state->callback(order_response);
                                 }
                             });
        };

        if (params.strict_no_fetch)
        {
            if (!params.tick_size.has_value() || params.tick_size->empty())
            {
                finish_with_error("strict_no_fetch requires tick_size");
                return;
            }
            if (!params.price.has_value() || *params.price <= 0.0)
            {
                finish_with_error("strict_no_fetch requires price");
                return;
            }
            if (!params.neg_risk.has_value())
            {
                finish_with_error("strict_no_fetch requires neg_risk");
                return;
            }
            if (!params.fee_rate_bps_provided || params.fee_rate_bps.empty())
            {
                finish_with_error("strict_no_fetch requires fee_rate_bps");
                return;
            }

            state->tick_size = *params.tick_size;
            state->price = *params.price;
            state->neg_risk = params.neg_risk.value();

            if (!price_valid(state->price, state->tick_size))
            {
                finish_with_error("invalid price (" + std::to_string(state->price) +
                                  "), min: " + state->tick_size +
                                  " - max: " + std::to_string(1.0 - std::stod(state->tick_size)));
                return;
            }

            (*submit_order)();
            return;
        }

        if (state->params.tick_size.has_value() && !state->params.tick_size->empty())
        {
            state->tick_size = *state->params.tick_size;
            (*resolve_price)();
            return;
        }

        std::string tick_path = "/tick-size?token_id=" + state->params.token_id;
        http_.get_async(tick_path,
                        [state, finish_with_error, resolve_price](const HttpResponse &http_response) mutable
                        {
                            if (!http_response.ok())
                            {
                                finish_with_error("failed to fetch tick size");
                                return;
                            }

                            std::string min_tick_size = "0.01";
                            try
                            {
                                auto j = json::parse(http_response.body);
                                if (j.contains("minimum_tick_size"))
                                {
                                    if (j["minimum_tick_size"].is_string())
                                    {
                                        min_tick_size = j["minimum_tick_size"].get<std::string>();
                                    }
                                    else if (j["minimum_tick_size"].is_number())
                                    {
                                        std::ostringstream oss;
                                        oss.setf(std::ios::fixed);
                                        oss << std::setprecision(6) << j["minimum_tick_size"].get<double>();
                                        min_tick_size = normalize_tick_size(oss.str());
                                    }
                                }
                            }
                            catch (...)
                            {
                                finish_with_error("failed to parse tick size");
                                return;
                            }

                            state->tick_size = min_tick_size;
                            (*resolve_price)();
                        });
    }

    bool ClobClient::cancel_order(const std::string &order_id)
    {
        json body;
        body["orderID"] = order_id;

        std::string body_str = body.dump();
        auto headers = get_l2_headers("DELETE", "/order", body_str);

        // Use POST with body for cancel (API accepts this)
        auto response = http_.post("/order", body_str, headers);
        return response.ok();
    }

    bool ClobClient::cancel_orders(const std::vector<std::string> &order_ids)
    {
        json body = order_ids;

        std::string body_str = body.dump();
        auto headers = get_l2_headers("DELETE", "/orders", body_str);

        auto response = http_.post("/orders", body_str, headers);
        return response.ok();
    }

    bool ClobClient::cancel_all()
    {
        auto headers = get_l2_headers("DELETE", "/cancel-all", "");
        auto response = http_.post("/cancel-all", "{}", headers);
        return response.ok();
    }

    bool ClobClient::cancel_market_orders(const std::string &condition_id)
    {
        json body;
        body["market"] = condition_id;

        std::string body_str = body.dump();
        auto headers = get_l2_headers("DELETE", "/cancel-market-orders", body_str);

        auto response = http_.post("/cancel-market-orders", body_str, headers);
        return response.ok();
    }

    std::optional<OpenOrder> ClobClient::get_order(const std::string &order_id)
    {
        auto headers = get_l2_headers("GET", "/order/" + order_id, "");
        auto response = http_.get("/order/" + order_id, headers);

        if (!response.ok())
            return std::nullopt;

        auto orders = parse_open_orders("[" + response.body + "]");
        if (orders.empty())
            return std::nullopt;

        return orders[0];
    }

    std::vector<OpenOrder> ClobClient::get_open_orders(const std::string &market)
    {
        std::string path = "/orders";
        if (!market.empty())
        {
            path += "?market=" + market;
        }

        auto headers = get_l2_headers("GET", path, "");
        auto response = http_.get(path, headers);

        if (!response.ok())
            return {};

        return parse_open_orders(response.body);
    }

    std::vector<Trade> ClobClient::get_trades(const std::string &next_cursor)
    {
        std::string path = "/trades";
        if (!next_cursor.empty())
        {
            path += "?next_cursor=" + next_cursor;
        }

        auto headers = get_l2_headers("GET", path, "");
        auto response = http_.get(path, headers);

        if (!response.ok())
            return {};

        return parse_trades(response.body);
    }

    BalanceAllowance ClobClient::get_balance_allowance(const std::string &asset_type)
    {
        BalanceAllowance ba;
        std::string base_path = "/balance-allowance";
        std::string sig_type = std::to_string(static_cast<int>(sig_type_));
        std::string path_with_params = base_path + "?asset_type=COLLATERAL&signature_type=" + sig_type;
        auto headers = get_l2_headers("GET", base_path, "");
        auto response = http_.get(path_with_params, headers);

        ba.elapsed_ms = response.elapsed_ms;
        ba.status_code = response.status_code;
        if (!response.ok())
        {
            ba.error_message = response.error.empty()
                                   ? "http error: " + std::to_string(response.status_code)
                                   : response.error;
            return ba;
        }

        try
        {
            auto j = json::parse(response.body);
            ba.balance = j.value("balance", "0");
            ba.allowance = j.value("allowance", "0");
            return ba;
        }
        catch (...)
        {
            ba.error_message = "failed to parse balance allowance";
            return ba;
        }
    }

    void ClobClient::get_balance_allowance_async(
        const std::string &asset_type,
        std::function<void(const BalanceAllowance &)> callback)
    {
        std::string base_path = "/balance-allowance";
        std::string sig_type = std::to_string(static_cast<int>(sig_type_));
        std::string path_with_params = base_path + "?asset_type=COLLATERAL&signature_type=" + sig_type;
        std::map<std::string, std::string> headers;
        try
        {
            headers = get_l2_headers("GET", base_path, "");
        }
        catch (...)
        {
            if (callback)
            {
                BalanceAllowance ba;
                ba.error_message = "Client not authenticated";
                callback(ba);
            }
            return;
        }

        http_.get_async(path_with_params, headers,
                        [callback = std::move(callback)](const HttpResponse &http_response) mutable
                        {
                            BalanceAllowance ba;
                            ba.elapsed_ms = http_response.elapsed_ms;
                            ba.status_code = http_response.status_code;
                            if (http_response.ok())
                            {
                                try
                                {
                                    auto j = json::parse(http_response.body);
                                    ba.balance = j.value("balance", "0");
                                    ba.allowance = j.value("allowance", "0");
                                }
                                catch (...)
                                {
                                    ba.error_message = "failed to parse balance allowance";
                                }
                            }
                            else
                            {
                                ba.error_message = http_response.error.empty()
                                                       ? "http error: " + std::to_string(http_response.status_code)
                                                       : http_response.error;
                            }

                            if (callback)
                            {
                                callback(ba);
                            }
                        });
    }

    void ClobClient::poll_async(long timeout_ms)
    {
        http_.poll_async(timeout_ms);
    }

    size_t ClobClient::pending_async() const
    {
        return http_.pending_async();
    }

    bool ClobClient::update_balance_allowance(const std::string &asset_type)
    {
        json body;
        body["asset_type"] = asset_type;

        std::string body_str = body.dump();
        auto headers = get_l2_headers("POST", "/balance-allowance", body_str);

        auto response = http_.post("/balance-allowance", body_str, headers);
        return response.ok();
    }

    std::optional<OrderScoringResult> ClobClient::is_order_scoring(const SignedOrder &order)
    {
        json body;
        body["orderId"] = order.salt; // Use salt as order identifier for scoring check

        std::string body_str = body.dump();
        auto response = http_.post("/order-scoring", body_str);

        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            OrderScoringResult result;
            result.scoring = j.value("scoring", false);
            return result;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::vector<OrderScoringResult> ClobClient::are_orders_scoring(const std::vector<SignedOrder> &orders)
    {
        std::vector<OrderScoringResult> results;

        json body = json::array();
        for (const auto &order : orders)
        {
            body.push_back({{"orderId", order.salt}});
        }

        std::string body_str = body.dump();
        auto response = http_.post("/orders-scoring", body_str);

        if (!response.ok())
            return results;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    OrderScoringResult result;
                    result.scoring = item.value("scoring", false);
                    results.push_back(result);
                }
            }
        }
        catch (...)
        {
        }

        return results;
    }

    std::vector<ClobClient::Notification> ClobClient::get_notifications()
    {
        std::vector<Notification> result;

        auto headers = get_l2_headers("GET", "/notifications", "");
        auto response = http_.get("/notifications", headers);

        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    Notification n;
                    n.id = item.value("id", "");
                    n.type = item.value("type", "");
                    n.message = item.value("message", "");
                    n.created_at = item.value("createdAt", "");
                    result.push_back(n);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    bool ClobClient::drop_notifications(const std::vector<std::string> &notification_ids)
    {
        json body = notification_ids;

        std::string body_str = body.dump();
        auto headers = get_l2_headers("DELETE", "/notifications", body_str);

        auto response = http_.post("/notifications", body_str, headers);
        return response.ok();
    }

    std::vector<ClobClient::RewardsInfo> ClobClient::get_rewards_markets_current()
    {
        std::vector<RewardsInfo> result;

        auto response = http_.get("/rewards/markets/current");
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    RewardsInfo info;
                    info.market = item.value("market", "");
                    info.min_size = item.value("minSize", "");
                    info.max_spread = item.value("maxSpread", "");
                    info.reward_epoch = item.value("rewardEpoch", "");
                    result.push_back(info);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::vector<ClobClient::RewardsInfo> ClobClient::get_rewards_markets(const std::string &epoch)
    {
        std::vector<RewardsInfo> result;

        std::string path = "/rewards/markets";
        if (!epoch.empty())
        {
            path += "?epoch=" + epoch;
        }

        auto response = http_.get(path);
        if (!response.ok())
            return result;

        try
        {
            auto j = json::parse(response.body);
            if (j.is_array())
            {
                for (const auto &item : j)
                {
                    RewardsInfo info;
                    info.market = item.value("market", "");
                    info.min_size = item.value("minSize", "");
                    info.max_spread = item.value("maxSpread", "");
                    info.reward_epoch = item.value("rewardEpoch", "");
                    result.push_back(info);
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::optional<ClobClient::EarningsInfo> ClobClient::get_earnings_for_user_for_day(const std::string &date)
    {
        std::string path = "/rewards/earnings";
        if (!date.empty())
        {
            path += "?date=" + date;
        }

        auto headers = get_l2_headers("GET", path, "");
        auto response = http_.get(path, headers);

        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            EarningsInfo info;
            info.market = j.value("market", "");
            info.earnings = j.value("earnings", "0");
            info.epoch = j.value("epoch", "");
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<ClobClient::EarningsInfo> ClobClient::get_total_earnings_for_user_for_day(const std::string &date)
    {
        std::string path = "/rewards/total-earnings";
        if (!date.empty())
        {
            path += "?date=" + date;
        }

        auto headers = get_l2_headers("GET", path, "");
        auto response = http_.get(path, headers);

        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            EarningsInfo info;
            info.earnings = j.value("earnings", "0");
            info.epoch = j.value("epoch", "");
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<ClobClient::FeeRateInfo> ClobClient::get_fee_rate()
    {
        auto headers = get_l2_headers("GET", "/fee-rate", "");
        auto response = http_.get("/fee-rate", headers);

        if (!response.ok())
            return std::nullopt;

        try
        {
            auto j = json::parse(response.body);
            FeeRateInfo info;
            info.maker = j.value("maker", "0");
            info.taker = j.value("taker", "0");
            return info;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    // ============================================================
    // POSITION MANAGEMENT (Data API)
    // ============================================================

    std::vector<ClobClient::Position> ClobClient::get_positions(const std::string &user_address) const {
        std::vector<Position> result;

        std::string address = user_address.empty() ? get_funder_address() : user_address;
        if (address.empty())
        {
            address = get_address();
        }
        if (address.empty())
        {
            return result;
        }

        // Use a separate HTTP client for Data API
        HttpClient data_http;
        data_http.set_base_url(DATA_API_URL);
        data_http.set_timeout_ms(10000);

        auto response = data_http.get("/positions?user=" + address);
        if (!response.ok())
        {
            return result;
        }

        try
        {
            auto j = json::parse(response.body);
            if (!j.is_array())
            {
                return result;
            }

            for (const auto &item : j)
            {
                Position pos;
                pos.proxy_wallet = item.value("proxyWallet", "");
                pos.asset = item.value("asset", "");
                pos.condition_id = item.value("conditionId", "");
                pos.size = item.value("size", 0.0);
                pos.avg_price = item.value("avgPrice", 0.0);
                pos.initial_value = item.value("initialValue", 0.0);
                pos.current_value = item.value("currentValue", 0.0);
                pos.cash_pnl = item.value("cashPnl", 0.0);
                pos.percent_pnl = item.value("percentPnl", 0.0);
                pos.cur_price = item.value("curPrice", 0.0);
                pos.redeemable = item.value("redeemable", false);
                pos.mergeable = item.value("mergeable", false);
                pos.title = item.value("title", "");
                pos.slug = item.value("slug", "");
                pos.outcome = item.value("outcome", "");
                pos.outcome_index = item.value("outcomeIndex", 0);
                pos.opposite_asset = item.value("oppositeAsset", "");
                pos.end_date = item.value("endDate", "");
                pos.negative_risk = item.value("negativeRisk", false);
                result.push_back(pos);
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::vector<ClobClient::Position> ClobClient::get_redeemable_positions(const std::string &user_address)
    {
        auto all_positions = get_positions(user_address);
        std::vector<Position> result;

        for (const auto &pos : all_positions)
        {
            if (pos.redeemable)
            {
                result.push_back(pos);
            }
        }

        return result;
    }

    std::vector<ClobClient::Position> ClobClient::get_mergeable_positions(const std::string &user_address)
    {
        auto all_positions = get_positions(user_address);
        std::vector<Position> result;

        for (const auto &pos : all_positions)
        {
            if (pos.mergeable)
            {
                result.push_back(pos);
            }
        }

        return result;
    }

    // ============================================================
    // JSON PARSING HELPERS
    // ============================================================

    std::vector<ClobMarket> ClobClient::parse_markets(const std::string &json_str)
    {
        std::vector<ClobMarket> markets;

        try
        {
            auto j = json::parse(json_str);

            json market_array;
            if (j.is_array())
            {
                market_array = j;
            }
            else if (j.contains("data") && j["data"].is_array())
            {
                market_array = j["data"];
            }
            else
            {
                return markets;
            }

            for (const auto &item : market_array)
            {
                ClobMarket market;

                if (item.contains("condition_id"))
                {
                    market.condition_id = item["condition_id"].get<std::string>();
                }
                if (item.contains("question") && !item["question"].is_null())
                {
                    market.question = item["question"].get<std::string>();
                }
                if (item.contains("market_slug") && !item["market_slug"].is_null())
                {
                    market.market_slug = item["market_slug"].get<std::string>();
                }
                if (item.contains("neg_risk"))
                {
                    market.neg_risk = item["neg_risk"].get<bool>();
                }
                if (item.contains("active"))
                {
                    market.active = item["active"].get<bool>();
                }
                if (item.contains("closed"))
                {
                    market.closed = item["closed"].get<bool>();
                }

                if (item.contains("tokens") && item["tokens"].is_array())
                {
                    for (const auto &t : item["tokens"])
                    {
                        Token token;
                        if (t.contains("token_id"))
                        {
                            token.token_id = t["token_id"].get<std::string>();
                        }
                        if (t.contains("outcome"))
                        {
                            token.outcome = t["outcome"].get<std::string>();
                        }
                        market.tokens.push_back(token);
                    }
                }

                markets.push_back(std::move(market));
            }
        }
        catch (...)
        {
        }

        return markets;
    }

    std::optional<Orderbook> ClobClient::parse_orderbook(const std::string &json_str)
    {
        try
        {
            auto j = json::parse(json_str);

            Orderbook book;
            book.timestamp_ns = now_ns();
            if (j.contains("timestamp"))
            {
                if (j["timestamp"].is_number_unsigned())
                {
                    book.server_timestamp = j["timestamp"].get<uint64_t>();
                }
                else if (j["timestamp"].is_number())
                {
                    book.server_timestamp = static_cast<uint64_t>(j["timestamp"].get<double>());
                }
                else if (j["timestamp"].is_string())
                {
                    try
                    {
                        book.server_timestamp = std::stoull(j["timestamp"].get<std::string>());
                    }
                    catch (...)
                    {
                        book.server_timestamp = 0;
                    }
                }
            }

            if (j.contains("asset_id"))
            {
                book.asset_id = j["asset_id"].get<std::string>();
            }

            if (j.contains("bids") && j["bids"].is_array())
            {
                for (const auto &bid : j["bids"])
                {
                    PriceLevel level;
                    level.price = std::stod(bid["price"].get<std::string>());
                    level.size = std::stod(bid["size"].get<std::string>());
                    book.bids.push_back(level);
                }
            }

            if (j.contains("asks") && j["asks"].is_array())
            {
                for (const auto &ask : j["asks"])
                {
                    PriceLevel level;
                    level.price = std::stod(ask["price"].get<std::string>());
                    level.size = std::stod(ask["size"].get<std::string>());
                    book.asks.push_back(level);
                }
            }

            return book;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    OrderResponse ClobClient::parse_order_response(const std::string &json_str)
    {
        OrderResponse result;
        result.success = false;

        try
        {
            auto j = json::parse(json_str);

            result.success = j.value("success", false);
            result.error_msg = j.value("errorMsg", "");
            if (result.error_msg.empty())
            {
                result.error_msg = j.value("error", "");
            }
            if (result.error_msg.empty())
            {
                result.error_msg = j.value("message", "");
            }
            result.order_id = j.value("orderID", "");
            result.status = j.value("status", "");
            result.taking_amount = j.value("takingAmount", "0");
            result.making_amount = j.value("makingAmount", "0");

            if (j.contains("transactionsHashes") && j["transactionsHashes"].is_array())
            {
                for (const auto &hash : j["transactionsHashes"])
                {
                    result.transaction_hashes.push_back(hash.get<std::string>());
                }
            }
        }
        catch (...)
        {
        }

        return result;
    }

    std::vector<OpenOrder> ClobClient::parse_open_orders(const std::string &json_str)
    {
        std::vector<OpenOrder> orders;

        try
        {
            auto j = json::parse(json_str);

            json order_array;
            if (j.is_array())
            {
                order_array = j;
            }
            else if (j.contains("data") && j["data"].is_array())
            {
                order_array = j["data"];
            }
            else
            {
                return orders;
            }

            for (const auto &item : order_array)
            {
                OpenOrder order;
                order.id = item.value("id", "");
                order.market = item.value("market", "");
                order.asset_id = item.value("asset_id", "");
                order.side = item.value("side", "");
                order.original_size = item.value("original_size", "0");
                order.size_matched = item.value("size_matched", "0");
                order.price = item.value("price", "0");
                order.status = item.value("status", "");
                order.created_at = item.value("created_at", "");
                order.expiration = item.value("expiration", "0");
                order.order_type = item.value("order_type", "GTC");
                orders.push_back(order);
            }
        }
        catch (...)
        {
        }

        return orders;
    }

    std::vector<Trade> ClobClient::parse_trades(const std::string &json_str)
    {
        std::vector<Trade> trades;

        try
        {
            auto j = json::parse(json_str);

            json trade_array;
            if (j.is_array())
            {
                trade_array = j;
            }
            else if (j.contains("data") && j["data"].is_array())
            {
                trade_array = j["data"];
            }
            else
            {
                return trades;
            }

            for (const auto &item : trade_array)
            {
                Trade trade;
                trade.id = item.value("id", "");
                trade.market = item.value("market", "");
                trade.asset_id = item.value("asset_id", "");
                trade.side = item.value("side", "");
                trade.size = item.value("size", "0");
                trade.price = item.value("price", "0");
                trade.fee_rate_bps = item.value("fee_rate_bps", "0");
                trade.status = item.value("status", "");
                trade.created_at = item.value("created_at", "");
                trade.match_time = item.value("match_time", "");
                trade.transaction_hash = item.value("transaction_hash", "");
                trades.push_back(trade);
            }
        }
        catch (...)
        {
        }

        return trades;
    }

} // namespace polymarket
