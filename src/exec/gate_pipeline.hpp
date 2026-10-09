#pragma once
// Gate pipeline — named, tiered entry gates with stable machine-readable
// reason codes (IDEAS.md Phase 1.3 / Homerun gate_pipeline.py port).
//
// One gate per reason. Every failure emits exactly ONE stable code so the
// skip-reason telemetry cannot conflate meanings (the legacy diag_skip_price
// counter mixed three different causes under one name).
//
// Tiering: L0 pure-memory gates (<5us) run first, then L1 cached-state gates
// (<50us), then L2 I/O gates (<500ms). Short-circuit on the first failure —
// cheapest-first ordering keeps the per-market sweep cost bounded.
//
// Per-gate metrics: call count, total µs, max µs — enough to spot a gate that
// silently became the bottleneck (auto-demotion: move a persistently slow
// gate to a later tier in the add() order).

#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vp::gates {

// Stable reason codes — add new ones here; NEVER reuse a code for a new meaning.
namespace code {
    inline constexpr const char* kSignalNone       = "signal_none";
    inline constexpr const char* kConfidenceFloor  = "confidence_below_floor";
    inline constexpr const char* kEntryLimit       = "entry_limit_reached";
    inline constexpr const char* kSweepDedup       = "already_traded_this_sweep";
    inline constexpr const char* kSecondEntryWeak  = "second_entry_delta_weak";
    inline constexpr const char* kPriceNoBook      = "price_no_book";
    inline constexpr const char* kPriceAboveCap    = "price_above_signal_cap";
    inline constexpr const char* kSizeInvalid      = "size_invalid";
    inline constexpr const char* kBookStale        = "book_stale";
    inline constexpr const char* kSpreadWide       = "spread_too_wide";
    inline constexpr const char* kRiskCapExceeded  = "risk_cap_exceeded";
    inline constexpr const char* kAuthCircuitOpen  = "auth_circuit_open";
}

struct GateResult {
    bool        passed = true;
    const char* code   = nullptr;   // stable code when !passed
    std::string detail;              // human-readable context
    // Filled by GatePipeline::run() on failure.
    const char* gate   = nullptr;   // gate name that stopped the pipeline
    int         tier   = 0;          // 0=L0 memory 1=L1 cached 2=L2 I/O
};

struct Gate {
    int         tier;
    const char* name;
    std::function<GateResult()> fn;
};

class GatePipeline {
public:
    // Add gates in cheapest-first order (tier ascending). No re-sorting:
    // the caller's add order IS the execution order, so a tier can be
    // demoted by simply adding it later.
    void add(int tier, const char* name, std::function<GateResult()> fn) {
        gates_.push_back(Gate{tier, name, std::move(fn)});
    }

    // Run all gates in add-order; short-circuit on the first failure.
    // Records per-gate timing for the metrics() accessor.
    GateResult run() {
        for (auto& g : gates_) {
            const auto t0 = std::chrono::steady_clock::now();
            GateResult r = g.fn();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            auto& m = metrics_[g.name];
            ++m.calls;
            m.total_us += us;
            if (us > m.max_us) m.max_us = us;
            if (!r.passed) {
                r.gate = g.name;
                r.tier = g.tier;
                ++m.failures;
                return r;
            }
        }
        return {};
    }

    struct GateMetrics {
        int64_t calls    = 0;
        int64_t failures = 0;
        int64_t total_us = 0;
        int64_t max_us   = 0;
    };

    // Per-gate call/failure counts + timing (for p-max and hot-gate alerts).
    const std::unordered_map<std::string, GateMetrics>& metrics() const {
        return metrics_;
    }

private:
    std::vector<Gate>                             gates_;
    std::unordered_map<std::string, GateMetrics>  metrics_;
};

} // namespace vp::gates
