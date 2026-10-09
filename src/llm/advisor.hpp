#pragma once
// LLM advisor — asks the LLM for trade commentary given market context.
//
// INVARIANT: the advisor NEVER places orders. Its output populates
//            TuiSnapshot::decision_text for display only.
//
// The advisor runs async (spawns a thread per query) so it never blocks
// the TUI render or engine thread. Only one outstanding query at a time.
#include "config/config.hpp"
#include <functional>
#include <string>
#include <atomic>

namespace vp {

class LlmAdvisor {
public:
    // on_reply called (from background thread) with advice string when done.
    explicit LlmAdvisor(LlmConfig cfg, std::function<void(std::string)> on_reply);
    ~LlmAdvisor();

    // Submit an async advisory query. Drops if one is already in-flight.
    // context: JSON or text summary of current market state (REDACTED: no keys/addresses)
    void query_async(const std::string& context);

    bool is_busy() const;

private:
    LlmConfig cfg_;
    std::function<void(std::string)> on_reply_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> shutdown_{false};
};

} // namespace vp
