#pragma once
// Polymarket user-scoped WebSocket — receives fill/order updates for the
// authenticated user. Only used in live mode.
//
// Subscribes to wss://ws-subscriptions-clob.polymarket.com/ws/user with
// EIP-712 authentication. Delivers fill callbacks to Positions + pg.
#include "core/types.hpp"
#include <functional>
#include <memory>
#include <string>

namespace vp {

class WsClient;

class PolymarketUserWs {
public:
    struct FillUpdate {
        std::string order_id;
        std::string token_id;
        double      price = 0;
        double      size  = 0;   // shares
        std::int64_t exchange_ms = 0;
    };

    using OnFill = std::function<void(const FillUpdate&)>;

    // clob_user_ws_url: wss://ws-subscriptions-clob.polymarket.com/ws/user
    // api_key, api_secret, api_passphrase: CLOB HMAC credentials
    // address: signer / EOA address
    PolymarketUserWs(std::string url,
                     std::string api_key,
                     std::string api_secret,
                     std::string api_passphrase,
                     std::string address,
                     OnFill on_fill);
    ~PolymarketUserWs();

    void start();
    void stop();

private:
    std::string url_;
    std::string api_key_, api_secret_, api_passphrase_, address_;
    OnFill on_fill_;
    std::unique_ptr<WsClient> ws_;
    std::shared_ptr<void> state_;   // UserWsState (opaque to header)
};

} // namespace vp
