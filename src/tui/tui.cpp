// Notcurses TUI implementation — full-screen cockpit.
//
// Layout (responsive, designed for 120x40+, reorganised 2026-04-26):
//
//   +-----------------------------------------------------------------+ row 0   top bar
//   |  positions    |  active market               | watchlist        | rows 1..N
//   |  (~30%)       |   (~32%)                     |   (~38%)         |
//   |               |    + prediction engine        |                  |
//   |  recent W/L   |                              |------------------|
//   |               |                              | order book (UP)  |
//   |  signals      |                              |                  |
//   +-----------------------------------------------------------------+
//   | status hints                                                     |
//   +-----------------------------------------------------------------+
//
// Render strategy: full repaint at 10–30 fps. Notcurses already diffs the
// frame internally so we don't worry about per-cell deltas.

#include "tui.hpp"
#include "core/clock.hpp"

#include <notcurses/notcurses.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <ctime>
#include <thread>

#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>

namespace vp {

namespace {
// Color helpers
constexpr uint64_t make_channels(uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t br = 0, uint8_t bg = 0, uint8_t bb = 0) {
    uint64_t fg = (uint64_t(0x40) << 56) | (uint64_t(r) << 48) | (uint64_t(g) << 40) | (uint64_t(b) << 32);
    uint64_t bk = (uint64_t(0x40) << 24) | (uint64_t(br) << 16) | (uint64_t(bg) << 8)  |  uint64_t(bb);
    return fg | bk;
}

// Box drawing chars
constexpr const char* kBoxH  = "─";
constexpr const char* kBoxV  = "│";
constexpr const char* kBoxTL = "╭";
constexpr const char* kBoxTR = "╮";
constexpr const char* kBoxBL = "╰";
constexpr const char* kBoxBR = "╯";
constexpr const char* kBoxXL = "├";
constexpr const char* kBoxXR = "┤";
constexpr const char* kBoxXT = "┬";
constexpr const char* kBoxXB = "┴";
constexpr const char* kBoxX  = "┼";

void hline(struct ncplane* p, int y, int x, int w) {
    for (int i = 0; i < w; ++i) ncplane_putstr_yx(p, y, x + i, kBoxH);
}
void vline(struct ncplane* p, int y, int x, int h) {
    for (int i = 0; i < h; ++i) ncplane_putstr_yx(p, y + i, x, kBoxV);
}
void box(struct ncplane* p, int y, int x, int h, int w, const char* title = nullptr) {
    ncplane_putstr_yx(p, y, x, kBoxTL);
    ncplane_putstr_yx(p, y, x + w - 1, kBoxTR);
    ncplane_putstr_yx(p, y + h - 1, x, kBoxBL);
    ncplane_putstr_yx(p, y + h - 1, x + w - 1, kBoxBR);
    hline(p, y, x + 1, w - 2);
    hline(p, y + h - 1, x + 1, w - 2);
    vline(p, y + 1, x, h - 2);
    vline(p, y + 1, x + w - 1, h - 2);
    if (title) {
        std::string t = std::string(" ") + title + " ";
        if (int(t.size()) < w - 4)
            ncplane_putstr_yx(p, y, x + 2, t.c_str());
    }
}

[[maybe_unused]] std::string fmt_dollars(double d) {
    if (d >= 0) return fmt::format(" ${:>7.2f}", d);
    return fmt::format("-${:>7.2f}", -d);
}

std::string fmt_secs_remaining(int64_t close_ms) {
    int64_t now = Clock::wall_ns() / 1'000'000;
    int64_t left = (close_ms - now) / 1000;
    if (left < 0) left = 0;
    if (left >= 3600) return fmt::format("{}h{:02d}m", left/3600, (left%3600)/60);
    return fmt::format("{}:{:02d}", left/60, left%60);
}

// Tiny braille-style sparkline using block chars
std::string spark(const std::vector<double>& v, int width) {
    if (v.empty() || width <= 0) return std::string(width, ' ');
    static const char* kBars[] = {" ","▁","▂","▃","▄","▅","▆","▇","█"};
    double mn = *std::min_element(v.begin(), v.end());
    double mx = *std::max_element(v.begin(), v.end());
    double rng = mx - mn;
    if (rng < 1e-9) rng = 1.0;
    std::string s;
    s.reserve(width * 4);
    int n = std::min<int>(width, int(v.size()));
    int start = int(v.size()) - n;
    for (int i = 0; i < n; ++i) {
        double t = (v[start + i] - mn) / rng;
        int idx = std::clamp(int(t * 8.0), 0, 8);
        s += kBars[idx];
    }
    // pad left if narrower than width
    if (n < width) s = std::string(width - n, ' ') + s;
    return s;
}
} // namespace

struct Tui::Impl {
    enum class MenuMode { None, Trade, Options };
    notcurses* nc = nullptr;
    ncplane*   std_plane = nullptr;
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};
    int selected_row = 0;
    bool eof_seen = false;
    MenuMode menu_mode = MenuMode::None;
    int menu_index = 0;
    // Updated by renderer each frame so input thread can clamp
    std::atomic<int> watchlist_size{1};
    // Saved termios so we can restore stdin on shutdown
    struct termios saved_tio{};
    bool tio_saved = false;
    // Trading key callback (set by main.cpp)
    std::function<void(char)> key_cb;
    // Headless: no notcurses, no input thread, no render. run() just blocks.
    bool headless = false;
};

Tui::Tui() : p_(std::make_unique<Impl>()), conn_(std::make_unique<ConnectionStatus>()) {}
Tui::~Tui() { shutdown(); }

bool Tui::init(bool headless) {
    if (headless) {
        p_->headless = true;
        spdlog::info("tui: headless mode — engine + logging only, no terminal UI");
        return true;
    }
    notcurses_options opts{};
    // We don't use notcurses_get; we read stdin directly with raw termios.
    // Just disable the banner so the splash doesn't flash.
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    p_->nc = notcurses_init(&opts, stdout);
    if (!p_->nc) {
        spdlog::error("notcurses_init failed");
        return false;
    }
    p_->std_plane = notcurses_stdplane(p_->nc);

    // Put stdin into raw mode so we can read keystrokes one byte at a time
    // without the kernel echoing them or waiting for newline. Saves the
    // original termios for shutdown().
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &p_->saved_tio) == 0) {
        struct termios raw = p_->saved_tio;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
            p_->tio_saved = true;
        else
            spdlog::warn("tui: tcsetattr(raw) failed; q-quit may not work");
    } else {
        spdlog::warn("tui: stdin is not a tty; q-quit disabled");
    }
    return true;
}

void Tui::shutdown() {
    if (p_->tio_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &p_->saved_tio);
        p_->tio_saved = false;
    }
    if (p_->nc) {
        notcurses_stop(p_->nc);
        p_->nc = nullptr;
        p_->std_plane = nullptr;
    }
}

void Tui::draw_splash(const char* /*v*/) {}

void Tui::update_snapshot(const TuiSnapshot& s) {
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_ = s;
}

void Tui::set_key_callback(std::function<void(char)> cb) {
    p_->key_cb = std::move(cb);
}

bool Tui::wants_quit() const noexcept { return p_->quit.load(); }

void Tui::request_quit() noexcept { if (p_) p_->quit.store(true); }

int Tui::selected_row() const noexcept { return p_ ? p_->selected_row : 0; }

void Tui::run() {
    if (p_->headless) {
        // No UI: block until a quit signal (SIGINT/SIGTERM → request_quit()).
        // The engine/feeds run on their own threads and keep working.
        using namespace std::chrono;
        spdlog::info("tui: headless run loop active (Ctrl-C or SIGTERM to stop)");
        while (!p_->quit.load()) std::this_thread::sleep_for(milliseconds(200));
        spdlog::info("tui: headless run loop received quit");
        return;
    }
    if (!p_->nc) return;
    p_->running.store(true);
    using namespace std::chrono;
    auto next_frame = steady_clock::now();
    const auto frame_period = milliseconds(100);   // 10 fps — plenty for a TUI

    // Input runs on its OWN thread using a blocking notcurses_get with a
    // short timeout. Calling notcurses_get_nblock from the same thread that
    // calls notcurses_render jams the terminal in Konsole (proven by the
    // tui_smoke / tui_noinput / tui_input_slow / tui_input_thread isolation
    // tests).
    // Input thread: POSIX poll() on STDIN_FILENO + read() in raw mode.
    // We bypass notcurses_get because it returns -1 immediately in this
    // process (libpq/asio interaction). Raw read works regardless.
    std::thread input_th([this]{
        spdlog::info("tui input thread started (raw POSIX read on stdin)");
        int keys_seen = 0;
        int64_t last_warn_ms = 0;
        while (!p_->quit.load() && p_->running.load()) {
            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            int pr = poll(&pfd, 1, 200);   // 200 ms timeout
            if (pr < 0) {
                if (errno == EINTR) continue;
                spdlog::warn("tui input: poll errno={}, exiting", errno);
                break;
            }
            if (pr == 0) continue;
            if (!(pfd.revents & POLLIN)) continue;

            unsigned char buf[32];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                // ENOSPC (errno 28) means disk full — don't spam log.
                // n==0 (EOF) is normal when running in background (nohup).
                // Log first occurrence only, then keep quiet.
                if (errno == ENOSPC) { continue; }
                if (n == 0 && !p_->eof_seen) {
                    p_->eof_seen = true;
                    spdlog::info("tui input: stdin EOF (background mode)");
                }
                if (n < 0 && errno != EINTR) {
                    spdlog::warn("tui input: read returned {} errno={}", n, errno);
                }
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) {
                unsigned char c = buf[i];
                ++keys_seen;
                if (c == 'q' || c == 'Q' || c == 0x03 /*Ctrl-C*/ ||
                    c == 0x04 /*Ctrl-D*/) {
                    spdlog::info("tui input: quit key (0x{:02x}) → exiting", c);
                    p_->quit.store(true);
                    goto done;
                }
                if (c == '\r' || c == '\n') {
                    if (p_->menu_mode == Impl::MenuMode::None) {
                        p_->menu_mode = Impl::MenuMode::Trade;
                        p_->menu_index = 0;
                    } else if (p_->key_cb) {
                        static constexpr char kTradeKeys[] = {'B','N','S','M','K','A','L','X'};
                        static constexpr char kOptionKeys[] = {'A','L','R','X'};
                        if (p_->menu_mode == Impl::MenuMode::Trade) {
                            int idx = std::clamp(p_->menu_index, 0, (int)sizeof(kTradeKeys) - 1);
                            p_->key_cb(kTradeKeys[idx]);
                        } else {
                            int idx = std::clamp(p_->menu_index, 0, (int)sizeof(kOptionKeys) - 1);
                            p_->key_cb(kOptionKeys[idx]);
                        }
                    }
                    continue;
                }
                if (c == '\t') {
                    p_->menu_mode = (p_->menu_mode == Impl::MenuMode::Trade)
                        ? Impl::MenuMode::Options : Impl::MenuMode::Trade;
                    p_->menu_index = 0;
                    continue;
                }
                if (c == 'o' || c == 'O') {
                    p_->menu_mode = Impl::MenuMode::Options;
                    p_->menu_index = 0;
                    continue;
                }
                // ── Navigation: j/] = down one market/menu item, i/[ = up ──
                // Single-byte, no escape sequences — guaranteed reliable.
                // Arrows also handled below as fallback.
                if (c == 'j' || c == ']') {
                    if (p_->menu_mode != Impl::MenuMode::None) {
                        int max_idx = (p_->menu_mode == Impl::MenuMode::Trade) ? 7 : 3;
                        p_->menu_index = std::min(p_->menu_index + 1, max_idx);
                    } else {
                        int sz = p_->watchlist_size.load();
                        if (sz > 1) p_->selected_row = std::min(p_->selected_row + 1, sz - 1);
                    }
                    continue;
                }
                if (c == 'i' || c == '[') {
                    if (p_->menu_mode != Impl::MenuMode::None) {
                        if (p_->menu_index > 0) --p_->menu_index;
                    } else {
                        if (p_->selected_row > 0) --p_->selected_row;
                    }
                    continue;
                }
                // ── Arrow keys: ESC '[' A/B — can arrive split across reads ──
                if (c == 0x1b) {
                    unsigned char seq[16] = {0};
                    int have = 0;
                    while (have < (int)sizeof(seq) && (i + 1 + have) < n) {
                        seq[have] = buf[i + 1 + have];
                        ++have;
                        unsigned char ch = seq[have - 1];
                        if ((ch >= 'A' && ch <= 'D') || ch == '~') break;
                    }
                    while (have < (int)sizeof(seq)) {
                        struct pollfd epfd{STDIN_FILENO, POLLIN, 0};
                        if (poll(&epfd, 1, 50) <= 0 || !(epfd.revents & POLLIN)) break;
                        ssize_t en = read(STDIN_FILENO, seq + have, 1);
                        if (en <= 0) break;
                        have += (int)en;
                        unsigned char ch = seq[have - 1];
                        if ((ch >= 'A' && ch <= 'D') || ch == '~') break;
                    }
                    char final = 0;
                    if (have >= 2 && (seq[0] == '[' || seq[0] == 'O')) {
                        for (int si = have - 1; si >= 1; --si) {
                            if (seq[si] >= 'A' && seq[si] <= 'D') { final = (char)seq[si]; break; }
                        }
                        if (final) {
                            if (p_->menu_mode != Impl::MenuMode::None) {
                                int max_idx = (p_->menu_mode == Impl::MenuMode::Trade) ? 7 : 3;
                                if (final == 'A' && p_->menu_index > 0) --p_->menu_index;
                                else if (final == 'B') p_->menu_index = std::min(p_->menu_index + 1, max_idx);
                                else if (final == 'C' || final == 'D') {
                                    p_->menu_mode = (p_->menu_mode == Impl::MenuMode::Trade)
                                        ? Impl::MenuMode::Options : Impl::MenuMode::Trade;
                                    p_->menu_index = 0;
                                }
                            } else {
                                int sz = p_->watchlist_size.load();
                                if (final == 'A' && p_->selected_row > 0) --p_->selected_row;
                                else if (final == 'B' && sz > 1)
                                    p_->selected_row = std::min(p_->selected_row + 1, sz - 1);
                                else if (final == 'C') { p_->menu_mode = Impl::MenuMode::Trade; p_->menu_index = 0; }
                                else if (final == 'D') { p_->menu_mode = Impl::MenuMode::Options; p_->menu_index = 0; }
                            }
                        }
                    } else {
                        p_->menu_mode = Impl::MenuMode::None;
                        p_->menu_index = 0;
                    }
                    i += std::min(have, (int)(n - i - 1));
                    continue;
                }
                // Trading keys: dispatch to callback (non-blocking)
                if (p_->key_cb) {
                    switch (c) {
                        case 'b': case 'B':
                        case 'n': case 'N':
                        case 's': case 'S':
                        case 'm': case 'M':
                        case 'k': case 'K':
                        case 'a': case 'A':
                        case 'l': case 'L':
                        case 'r': case 'R':
                        case ' ':
                        case 'x': case 'X':
                            p_->key_cb((char)std::toupper((int)c));
                            break;
                        default: break;
                    }
                }
            }
        }
    done:
        spdlog::info("tui input thread exited (keys={})", keys_seen);
    });

    while (!p_->quit.load() && p_->running.load()) {
        // Take a snapshot copy
        TuiSnapshot snap;
        { std::lock_guard<std::mutex> lk(snap_mu_); snap = snap_; }
        // Tell input thread the current watchlist size so it can clamp selected_row
        p_->watchlist_size.store(std::max(1, (int)snap.watchlist.size()));

        // Term size
        unsigned R, C;
        ncplane_dim_yx(p_->std_plane, &R, &C);

        // Clear and use the terminal's default colors as the base. Setting a
        // custom base channel here previously painted everything with broken
        // alpha bits → black-on-black on some terminals.
        ncplane_erase(p_->std_plane);
        ncplane_set_base(p_->std_plane, " ", 0, 0);

        // ---- top bar (row 0) ----
        bool is_killed = snap.top.kill_switch;
        bool is_disk_full = snap.top.disk_full;
        // Background: dark blue normally, dark red when kill switch is active, dark orange when disk full
        ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
        uint32_t bg_color = is_killed ? 0x5a0000 : (is_disk_full ? 0x8a4500 : 0x202840);
        ncplane_set_bg_rgb(p_->std_plane, bg_color);
        std::string mode  = snap.top.mode_label.empty() ? "PAPER" : snap.top.mode_label;
        auto& cs = *conn_;
        auto pip = [](bool on)->const char*{ return on ? "●" : "○"; };
        std::string disk_warn = is_disk_full ? "  *** DISK FULL ***" : "";
        std::string kill_warn = is_killed ? " *** KILL SWITCH ***" : "";
        std::string topbar = fmt::format(
            " VANTAGE PULSE  [{}]   PM:{}  PMu:{}  BNC:{}  CB:{}  ORC:{}  LLM:{}  PG:{}   "
            "lat {:>4}us   AUTO:{}{}{}",
            mode,
            pip(cs.pm_md.load()), pip(cs.pm_user.load()),
            pip(cs.binance.load()), pip(cs.coinbase.load()), pip(cs.rtds.load()),
            pip(cs.llm.load()), pip(cs.pg.load()),
            cs.wire_to_proc_us.load(),
            snap.top.autopilot_global ? "ON " : "OFF",
            kill_warn,
            disk_warn);
        if (int(topbar.size()) > int(C)) topbar.resize(C);
        else topbar.append(C - topbar.size(), ' ');
        ncplane_putstr_yx(p_->std_plane, 0, 0, topbar.c_str());
        ncplane_set_bg_default(p_->std_plane);
        ncplane_set_fg_default(p_->std_plane);

        // ──────────────────────────────────────────────────────────────
        // WALLET STRIP (rows 1–5) — the BIG, OBVIOUS dashboard the user
        // has been asking for. 5 rows of accounting + cross-source price
        // truth. Row 0 stays the topbar; this strip occupies rows 1..5.
        //
        //   Row 1 : WALLET TOTAL ESTIMATE | WALLET CASH VALUE | TOTAL LIVE INVESTMENTS AT COST
        //   Row 2 : POLYMARKET LIVE COIN PRICE | BINANCE LIVE | COINBASE LIVE     (active asset)
        //   Row 3 : POLYMARKET WINDOW COIN OPEN PRICE (TO BEAT)
        //   Row 4 : UP SHARE COST (LIVE) | DOWN SHARE COST (LIVE)
        //   Row 5 : TRADES W/L | WIN RATE | DEPLOYED | P&L
        //
        // Critical: the POLYMARKET line must NEVER be backfilled from
        // Binance/Coinbase. If oracle hasn't ticked, render "—".
        // ──────────────────────────────────────────────────────────────
        {
            const auto& t = snap.top;
            int total_trades  = t.trades_won + t.trades_lost;
            double wr_pct     = total_trades > 0
                ? 100.0 * t.trades_won / total_trades : 0.0;
            bool winning      = t.day_pnl_usd >= 0;
            uint32_t pnl_fg   = winning ? 0x40ff80 : 0xff5050;
            uint32_t bg_strip = winning ? 0x0a2418 : 0x281010;

            // Paint background bar across all 5 rows.
            ncplane_set_bg_rgb(p_->std_plane, bg_strip);
            ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
            std::string blank(C, ' ');
            for (int rr = 1; rr <= 5; ++rr)
                ncplane_putstr_yx(p_->std_plane, rr, 0, blank.c_str());

            auto fmt_usd = [](double v) {
                return fmt::format("${:>8.2f}", v);
            };
            auto fmt_coin = [](double v) {
                if (v <= 0) return std::string("       —   ");
                return fmt::format("${:>10.2f}", v);
            };
            auto fmt_share = [](double v) {
                if (v <= 0) return std::string("  —  ");
                return fmt::format("$0.{:03d}", (int)std::round(v * 1000.0) % 1000);
            };

            // ── Row 1: WALLET TOTAL ESTIMATE | CASH VALUE | INVESTED ──
            ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 1, 2, "WALLET TOTAL ESTIMATE");
            ncplane_set_fg_rgb(p_->std_plane,
                t.wallet_total_if_wins >= t.wallet_start_usd ? 0x40ff80 : 0xffaa40);
            ncplane_putstr_yx(p_->std_plane, 1, 24, fmt_usd(t.wallet_total_if_wins).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 1, 38, "│  WALLET CASH VALUE");
            ncplane_set_fg_rgb(p_->std_plane,
                t.wallet_cash_value >= t.wallet_start_usd ? 0x40ff80 : 0xff5050);
            ncplane_putstr_yx(p_->std_plane, 1, 60, fmt_usd(t.wallet_cash_value).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 1, 73, "│  TOTAL LIVE INVESTMENTS AT COST");
            ncplane_set_fg_rgb(p_->std_plane, 0x80c0ff);
            ncplane_putstr_yx(p_->std_plane, 1, 108, fmt_usd(t.total_invested_cost).c_str());

            // ── Row 2: cross-source coin prices (active asset) ──
            std::string asset_label = t.active_asset.empty() ? "---" : t.active_asset;
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 2, 2,
                fmt::format("POLYMARKET ({}) LIVE", asset_label).c_str());
            // PM oracle = polymarket.com truth — render "—" if not ticked.
            ncplane_set_fg_rgb(p_->std_plane, 0xffd040);
            ncplane_putstr_yx(p_->std_plane, 2, 24, fmt_coin(t.pm_coin_price).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 2, 38, "│  BINANCE LIVE");
            ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
            ncplane_putstr_yx(p_->std_plane, 2, 56, fmt_coin(t.binance_coin_price).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 2, 71, "│  COINBASE LIVE");
            ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
            ncplane_putstr_yx(p_->std_plane, 2, 89, fmt_coin(t.coinbase_coin_price).c_str());

            // ── Row 3: window open price (the strike to beat) ──
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 3, 2,
                "POLYMARKET WINDOW COIN OPEN PRICE (TO BEAT)");
            ncplane_set_fg_rgb(p_->std_plane, 0xff80ff);
            ncplane_putstr_yx(p_->std_plane, 3, 47, fmt_coin(t.window_open_coin_price).c_str());

            // Show direction the active market needs to move
            if (t.pm_coin_price > 0 && t.window_open_coin_price > 0) {
                bool above = t.pm_coin_price >= t.window_open_coin_price;
                double delta_pct = 100.0 * (t.pm_coin_price - t.window_open_coin_price)
                                          / t.window_open_coin_price;
                ncplane_set_fg_rgb(p_->std_plane, above ? 0x40ff80 : 0xff5050);
                ncplane_putstr_yx(p_->std_plane, 3, 62,
                    fmt::format("{}  oracle vs strike: {:+.3f}%",
                        above ? "▲ UP wins" : "▼ DOWN wins",
                        delta_pct).c_str());
            }

            // ── Row 4: live UP/DOWN share asks ──
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 4, 2, "UP SHARE COST (LIVE)");
            ncplane_set_fg_rgb(p_->std_plane, 0x40c0ff);
            ncplane_putstr_yx(p_->std_plane, 4, 24, fmt_share(t.up_share_cost_live).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 4, 38, "│  DOWN SHARE COST (LIVE)");
            ncplane_set_fg_rgb(p_->std_plane, 0xff80c0);
            ncplane_putstr_yx(p_->std_plane, 4, 64, fmt_share(t.down_share_cost_live).c_str());

            // ── Row 5: scoreboard (W/L, win rate, deployed, P&L) ──
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 5, 2, "TRADES ");
            ncplane_set_fg_rgb(p_->std_plane, 0x40ff80);
            ncplane_putstr_yx(p_->std_plane, 5, 9,
                fmt::format("{} W", t.trades_won).c_str());
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 5, 15, " / ");
            ncplane_set_fg_rgb(p_->std_plane, 0xff5050);
            ncplane_putstr_yx(p_->std_plane, 5, 18,
                fmt::format("{} L", t.trades_lost).c_str());
            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 5, 25,
                fmt::format("WIN {:>5.1f}%", wr_pct).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 5, 40, "DEPLOYED ");
            uint32_t dep_col = (snap.deployed_usd > snap.max_deployed_usd * 0.8)
                ? 0xff4040
                : (snap.deployed_usd > snap.max_deployed_usd * 0.5)
                    ? 0xffaa00 : 0x80c0ff;
            ncplane_set_fg_rgb(p_->std_plane, dep_col);
            ncplane_putstr_yx(p_->std_plane, 5, 49,
                fmt::format("${:>5.2f}/${:>5.2f}",
                    snap.deployed_usd, snap.max_deployed_usd).c_str());

            ncplane_set_fg_rgb(p_->std_plane, 0xc0c0c0);
            ncplane_putstr_yx(p_->std_plane, 5, 70, "P&L TODAY ");
            ncplane_set_fg_rgb(p_->std_plane, pnl_fg);
            const char* arrow = winning ? "▲" : "▼";
            double pnl_pct = (t.wallet_start_usd > 0)
                ? 100.0 * t.day_pnl_usd / t.wallet_start_usd : 0.0;
            ncplane_putstr_yx(p_->std_plane, 5, 80,
                fmt::format("{} {:>+7.2f} ({:+5.2f}%)  start ${:.2f}",
                    arrow, t.day_pnl_usd, pnl_pct, t.wallet_start_usd).c_str());

            ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
            ncplane_set_bg_default(p_->std_plane);
            ncplane_set_fg_default(p_->std_plane);
        }

        // Layout below top bar (top bar = row 0; wallet strip = rows 1-5)
        const int top_h    = 6;
        const int status_h = 1;
        const int avail_h  = (int)R - top_h - status_h;
        if (avail_h < 8 || (int)C < 90) {
            ncplane_putstr_yx(p_->std_plane, 1, 1, "Terminal too small (need ≥90x14)");
            notcurses_render(p_->nc);
            std::this_thread::sleep_until(next_frame);
            next_frame += frame_period;
            continue;
        }

        // Column widths (NEW LAYOUT 2026-04-26):
        //   LEFT   — POSITIONS + RECENT RESULTS + SIGNALS  (~32%)
        //   CENTER — ACTIVE MARKET + PREDICTION ENGINE     (~32%)
        //   RIGHT  — WATCHLIST (top) + ORDER BOOK (bottom) (~36%)
        // The order book moved out of CENTER (it now uses full height for
        // the active-market + prediction-engine stack), and the watchlist
        // moved out of the left so the positions / results / signals
        // panels finally have room to show their full width.
        const int left_w   = std::max(34, std::min(44, (int)C * 30 / 100));
        const int right_w  = std::max(48, std::min(64, (int)C * 40 / 100));
        const int center_w = (int)C - left_w - right_w;
        const int center_x = left_w;
        const int right_x  = left_w + center_w;
        const int main_y   = top_h;
        // Right column split: watchlist on top (~55%), order book below.
        const int wl_h     = std::max(8, avail_h * 55 / 100);
        const int rbook_h  = avail_h - wl_h;

        // ══════════════════════════════════════════════════════════════
        // RIGHT-TOP: WATCHLIST  (j/]=down  i/[=up  or arrow keys)
        // ══════════════════════════════════════════════════════════════
        box(p_->std_plane, main_y, right_x, wl_h, right_w, "WATCHLIST  j/]=dn  i/[=up");
        // Header
        ncplane_set_fg_rgb(p_->std_plane, 0x808080);
        // Header: # AST HZ  COIN-OPEN   COIN-NOW    Δ%   VEL T-LFT S
        // OPEN/NOW = the Polymarket Chainlink oracle COIN price (BTC/ETH
        // in USD), NOT the share price. The strike at window-open is what
        // determines who wins; the live oracle vs that strike is the
        // edge. Share-price columns moved to the ACTIVE panel.
        std::string wl_hdr = fmt::format(
            " {:>2} {:<3} {:>3} {:>9} {:>9} {:>6} {:<2} {:>5} S",
            "#", "AST", "HZ", "COIN-OP", "COIN-NOW", "Δ%", "V", "T-LFT");
        if ((int)wl_hdr.size() > right_w - 2) wl_hdr.resize(right_w - 2);
        ncplane_putstr_yx(p_->std_plane, main_y + 1, right_x + 1, wl_hdr.c_str());
        ncplane_set_fg_default(p_->std_plane);

        for (size_t i = 0; i < snap.watchlist.size(); ++i) {
            int row = main_y + 2 + (int)i;
            if (row >= main_y + wl_h - 1) break;
            const auto& w = snap.watchlist[i];
            bool selected = ((int)i == p_->selected_row);

            std::string hz = (w.horizon_s == 300)   ? "5m"  :
                             (w.horizon_s == 900)   ? "15m" :
                             (w.horizon_s == 14400) ? "4h"  : "1h";
            // Δ% from oracle: how far has the Chainlink coin price moved
            // since window open? Positive = UP wins if it stays here.
            double delta_pct = w.oracle_delta_pct;
            // velocity arrow on coin price (use up_velocity as proxy for
            // share momentum, but the dominant column is now the oracle
            // delta — share momentum still useful as secondary)
            const char* vel_arrow = (w.up_velocity >  0.005) ? "▲▲" :
                                    (w.up_velocity >  0.001) ? "▲ " :
                                    (w.up_velocity < -0.005) ? "▼▼" :
                                    (w.up_velocity < -0.001) ? "▼ " : "─ ";
            // Format coin price compactly. ETH ~ $3.5k, BTC ~ $76k →
            // 8 chars at no decimals fits both. "—" if oracle untouched.
            auto fmt_coin = [](double v) {
                if (v <= 0) return std::string("       —");
                return fmt::format("{:>9.2f}", v);
            };
            std::string wl_line = fmt::format(
                " {:>2} {:<3} {:>3} {} {} {:>+5.2f}% {} {:>5}  {:c}",
                (int)i+1, w.asset, hz,
                fmt_coin(w.coin_open_price),
                fmt_coin(w.coin_now_price),
                delta_pct, vel_arrow,
                fmt_secs_remaining(w.window_close_ms),
                w.signal_glyph);
            if ((int)wl_line.size() > right_w - 2) wl_line.resize(right_w - 2);

            if (selected) {
                ncplane_set_bg_rgb(p_->std_plane, 0x1a2a4a);
                ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
            } else {
                // color by oracle delta (positive = UP advantage)
                uint32_t fg = (delta_pct >  0.05) ? 0x40ff80 :
                              (delta_pct < -0.05) ? 0xff6060 : 0xb0b0b0;
                if (w.signal_glyph == 'X') fg = 0xff4040;
                ncplane_set_fg_rgb(p_->std_plane, fg);
            }
            ncplane_putstr_yx(p_->std_plane, row, right_x + 1, wl_line.c_str());
            ncplane_set_bg_default(p_->std_plane);
            ncplane_set_fg_default(p_->std_plane);
        }

        // ══════════════════════════════════════════════════════════════
        // CENTER: ACTIVE MARKET PANEL
        // ══════════════════════════════════════════════════════════════
        const auto& a = snap.active;
        // Center column now uses the full height — order book moved to right.
        const int detail_h = avail_h;

        {
            std::string title_str;
            if (!a.condition_id.empty())
                title_str = fmt::format("ACTIVE: {} {}m  ─  {}",
                    a.asset, a.horizon_s/60, fmt_secs_remaining(a.window_close_ms));
            else
                title_str = "ACTIVE MARKET";
            box(p_->std_plane, main_y, center_x, detail_h, center_w, title_str.c_str());
        }

        if (a.condition_id.empty()) {
            ncplane_putstr_yx(p_->std_plane, main_y + 2, center_x + 2,
                              " (no markets discovered yet — pinging Gamma…)");
        } else {
            int cy = main_y + 1;  // current draw row

            // ══════════════════════════════════════════════════════════
            // POLYMARKET — the prices that ACTUALLY matter (window OPEN
            // and CURRENT shares from PM). Bold, bright, top of panel.
            // ══════════════════════════════════════════════════════════
            ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
            ncplane_set_fg_rgb(p_->std_plane, 0xfff060);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                " ◆ POLYMARKET WINDOW ◆");
            ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
            ncplane_set_fg_default(p_->std_plane);
            ++cy;

            // OPEN row (window strike share prices when window opened)
            ncplane_set_fg_rgb(p_->std_plane, 0x909090);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, " OPEN  ");
            ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
            ncplane_set_fg_rgb(p_->std_plane, 0xc0e0ff);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 9,
                fmt::format("UP {:>7.4f}    DN {:>7.4f}",
                    a.open_up_price, a.open_down_price).c_str());
            ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
            ncplane_set_fg_default(p_->std_plane);
            ++cy;

            // NOW row (live current PM share prices) — bold, color by direction
            double up_delta_pct = (a.open_up_price > 0)
                ? 100.0*(a.up_price - a.open_up_price)/a.open_up_price : 0.0;
            double dn_delta_pct = (a.open_down_price > 0)
                ? 100.0*(a.down_price - a.open_down_price)/a.open_down_price : 0.0;
            ncplane_set_fg_rgb(p_->std_plane, 0x909090);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, " NOW   ");
            ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
            // UP price (with delta)
            uint32_t up_col = (a.up_price > a.open_up_price && a.open_up_price > 0)
                              ? 0x40ff80
                              : (a.up_price < a.open_up_price && a.open_up_price > 0)
                                ? 0xff5050 : 0xffffff;
            ncplane_set_fg_rgb(p_->std_plane, up_col);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 9,
                fmt::format("UP {:>7.4f} ({:>+5.2f}%)",
                    a.up_price, up_delta_pct).c_str());
            // DN price (with delta)
            uint32_t dn_col = (a.down_price > a.open_down_price && a.open_down_price > 0)
                              ? 0x40ff80
                              : (a.down_price < a.open_down_price && a.open_down_price > 0)
                                ? 0xff5050 : 0xffffff;
            ncplane_set_fg_rgb(p_->std_plane, dn_col);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 9 + 22,
                fmt::format("   DN {:>7.4f} ({:>+5.2f}%)",
                    a.down_price, dn_delta_pct).c_str());
            ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
            ncplane_set_fg_default(p_->std_plane);
            ++cy;

            // ══════════════════════════════════════════════════════════
            // CROSS-SOURCE PRICE COMPARISON  (informational only —
            // the Chainlink RTDS oracle is the actual settlement source
            // used by Polymarket. Binance and Coinbase are shown for
            // informational cross-check only, and do NOT feed signals.)
            // ══════════════════════════════════════════════════════════
            double cb_gap_pct = (a.coinbase_price > 0 && a.asset_price > 0)
                ? 100.0 * (a.asset_price - a.coinbase_price) / a.coinbase_price : 0.0;
            ncplane_set_fg_rgb(p_->std_plane, 0x607080);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                " ─── reference oracles (settlement) ───────────");
            ++cy;
            // PM strike — most important reference (this is the price the
            // window will be settled against by Polymarket)
            ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
            ncplane_set_fg_rgb(p_->std_plane, 0xf0c060);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                fmt::format(" PM STRIKE {:>10.2f}", a.target_price).c_str());
            ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
            // dim Binance / Coinbase — they're cross-checks, not payouts
            ncplane_set_fg_rgb(p_->std_plane, 0x6080a0);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2 + 24,
                fmt::format("bnc {:>9.2f}", a.asset_price).c_str());
            ncplane_set_fg_rgb(p_->std_plane,
                std::abs(cb_gap_pct) > 0.05 ? 0xa07060 : 0x705080);
            ncplane_putstr_yx(p_->std_plane, cy, center_x + 2 + 40,
                fmt::format("cb {:>9.2f} ({:+.3f}%)",
                    a.coinbase_price, cb_gap_pct).c_str());
            ncplane_set_fg_default(p_->std_plane);
            ++cy;

            // PM PAYOUT PROJECTION — if the window closed right now at
            // the current Chainlink oracle price, which side pays $1.
            // Binance and Coinbase are informational only.
            {
                double settle_px = (a.oracle_price > 0) ? a.oracle_price : a.asset_price;
                bool up_wins = settle_px >= a.target_price;
                ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
                ncplane_set_fg_rgb(p_->std_plane, up_wins ? 0x40ff80 : 0xff5050);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                    fmt::format(" PM CLOSE NOW → {} WINS  ({:+.3f}% vs strike)",
                        up_wins ? "UP" : "DN",
                        a.target_price > 0
                          ? 100.0 * (settle_px - a.target_price) / a.target_price
                          : 0.0).c_str());
                ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
                ncplane_set_fg_default(p_->std_plane);
                ++cy;
            }

            // ── Row 4: separator ────────────────────────────────────
            hline(p_->std_plane, cy, center_x + 1, center_w - 2);
            ++cy;

            // ── Row 5-6: MOMENTUM BAR + SCALP ZONE INDICATOR ───────
            // Bar: fill = UP share price mapped 0..1 over [0,1] range
            // Colored zones: 0-30% red, 30-50% yellow, 50-70% green, 70%+ bright red
            // bw leaves room for ' MOM ' label (5) + ' XX +0.0000/s' velocity tag (~14)
            // and the right border. Total reserved on the right edge: 16 cells.
            {
                int bw = center_w - 26;  // usable bar width (was -18, overflowed)
                double up_pct = std::clamp(a.up_price, 0.0, 1.0);
                int filled_cells = (int)(up_pct * bw + 0.5);
                // Build colored bar segment by segment
                int zone_30 = (int)(0.30 * bw);
                int zone_50 = (int)(0.50 * bw);
                int zone_70 = (int)(0.70 * bw);

                ncplane_set_fg_rgb(p_->std_plane, 0xd0d0d0);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, " MOM ");
                for (int b = 0; b < bw; ++b) {
                    const char* cell = (b < filled_cells) ? "█" : "░";
                    if (b < zone_30)
                        ncplane_set_fg_rgb(p_->std_plane, b < filled_cells ? 0xff4040 : 0x442020);
                    else if (b < zone_50)
                        ncplane_set_fg_rgb(p_->std_plane, b < filled_cells ? 0xffaa00 : 0x443300);
                    else if (b < zone_70)
                        ncplane_set_fg_rgb(p_->std_plane, b < filled_cells ? 0x40ff80 : 0x204430);
                    else
                        ncplane_set_fg_rgb(p_->std_plane, b < filled_cells ? 0xff2020 : 0x441010);
                    ncplane_putstr_yx(p_->std_plane, cy, center_x + 7 + b, cell);
                }
                // velocity label + arrow
                const char* vel_glyph = (a.up_velocity >  0.005) ? "▲▲" :
                                        (a.up_velocity >  0.001) ? "▲ " :
                                        (a.up_velocity < -0.005) ? "▼▼" :
                                        (a.up_velocity < -0.001) ? "▼ " : "─ ";
                if (a.up_velocity > 0.001) ncplane_set_fg_rgb(p_->std_plane, 0x40ff80);
                else if (a.up_velocity < -0.001) ncplane_set_fg_rgb(p_->std_plane, 0xff5050);
                else ncplane_set_fg_rgb(p_->std_plane, 0x909090);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 7 + bw + 1,
                    fmt::format(" {} {:>+.4f}/s", vel_glyph, a.up_velocity).c_str());
                ncplane_set_fg_default(p_->std_plane);
                ++cy;

                // Scalp zone ruler:  0.00  [BUY<0.30]  0.30  [FAIR]  0.50  [SCALP]  1.00
                // mark where current UP price sits
                int marker_pos = center_x + 7 + std::clamp((int)(a.up_price * bw), 0, bw - 1);
                ncplane_set_fg_rgb(p_->std_plane, 0x606060);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, " ZONE ");
                ncplane_set_fg_rgb(p_->std_plane, 0xff6060);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 8,  "0.00─BUY");
                ncplane_set_fg_rgb(p_->std_plane, 0xffaa00);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 8 + zone_30, "─0.30─FAIR");
                ncplane_set_fg_rgb(p_->std_plane, 0x40ff80);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 8 + zone_50, "─0.50─SELL");
                ncplane_set_fg_rgb(p_->std_plane, 0xff2020);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 8 + zone_70, "─0.70─AVOID─1.00");
                // current price marker ▼
                ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
                ncplane_putstr_yx(p_->std_plane, cy - 1, marker_pos, "▼");
                ncplane_set_fg_default(p_->std_plane);
                ++cy;
            }

            // ── Row 7: separator ────────────────────────────────────
            hline(p_->std_plane, cy, center_x + 1, center_w - 2);
            ++cy;

            // ── Row 8-10: UP price sparkline (share price history) ──
            if (!a.up_price_hist.empty() && cy + 3 < main_y + detail_h - 1) {
                int spk_w = center_w - 12;
                // 3-row ASCII chart: hi/mid/lo tiers using block chars
                double mn = *std::min_element(a.up_price_hist.begin(), a.up_price_hist.end());
                double mx = *std::max_element(a.up_price_hist.begin(), a.up_price_hist.end());
                if (mx - mn < 0.002) { mn -= 0.01; mx += 0.01; }
                std::string spk = spark(a.up_price_hist, spk_w);
                ncplane_set_fg_rgb(p_->std_plane, 0x606060);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                    fmt::format(" UP  {:.3f}┤", mx).c_str());
                ncplane_set_fg_rgb(p_->std_plane,
                    a.up_velocity > 0.001 ? 0x40ff80 : a.up_velocity < -0.001 ? 0xff5050 : 0x80c0ff);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 11, spk.c_str());
                ++cy;
                ncplane_set_fg_rgb(p_->std_plane, 0x606060);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                    fmt::format("     {:.3f}┘", mn).c_str());
                ncplane_set_fg_default(p_->std_plane);
                ++cy;
            }

            // ── Decision / signal row ────────────────────────────────
            if (cy < main_y + detail_h - 1) {
                hline(p_->std_plane, cy, center_x + 1, center_w - 2);
                ++cy;
            }
            // ── PREDICTION ENGINE breakdown ──────────────────────────
            // Multi-signal ensemble: Black-Scholes fair value + technicals
            // (RSI, MACD, BB, slope) + microstructure imbalance.
            if (cy < main_y + detail_h - 2) {
                ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
                ncplane_set_fg_rgb(p_->std_plane, 0x80ffff);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2,
                    " ◆ PREDICTION ENGINE ◆");
                ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
                ++cy;
                // Probability bars: P(UP wins) ensemble + Black-Scholes anchor
                int pbw = std::max(10, center_w - 28);
                auto draw_prob = [&](int y, const char* label, double p, uint32_t col_full) {
                    int filled = (int)(std::clamp(p, 0.0, 1.0) * pbw + 0.5);
                    ncplane_set_fg_rgb(p_->std_plane, 0x808080);
                    ncplane_putstr_yx(p_->std_plane, y, center_x + 2, label);
                    for (int b = 0; b < pbw; ++b) {
                        ncplane_set_fg_rgb(p_->std_plane,
                            b < filled ? col_full : 0x303030);
                        ncplane_putstr_yx(p_->std_plane, y, center_x + 9 + b,
                            b < filled ? "█" : "░");
                    }
                    ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
                    ncplane_putstr_yx(p_->std_plane, y, center_x + 9 + pbw + 1,
                        fmt::format("{:>5.1f}%", p * 100).c_str());
                };
                if (cy < main_y + detail_h - 1) {
                    draw_prob(cy, " P UP ", a.pred_up, 0x40c0ff);
                    ++cy;
                }
                if (cy < main_y + detail_h - 1) {
                    draw_prob(cy, " FAIR ", a.pred_fair_up, 0x80ffa0);
                    ++cy;
                }
                // Component diagnostics line (compact)
                if (cy < main_y + detail_h - 1) {
                    ncplane_set_fg_rgb(p_->std_plane, 0xb0b0c0);
                    std::string ln = fmt::format(
                        " RSI {:>5.1f}  MACDh {:>+7.4f}  BBz {:>+5.2f}  tech {:>+5.2f}  conf {:>5.1f}%",
                        a.pred_rsi14, a.pred_macd_hist, a.pred_bb_z,
                        a.pred_tech_score, a.pred_conf * 100);
                    // Append new signals when non-zero
                    if (std::abs(a.pred_oracle_vel_1m) > 0.01 || std::abs(a.pred_trade_flow) > 0.01) {
                        ln += fmt::format(
                            " oV{:>+.2f} acl{:>+.2f} tF{:>+.2f}",
                            a.pred_oracle_vel_1m, a.pred_oracle_accel, a.pred_trade_flow);
                    }
                    if (a.pred_volume_surge_ratio > 1.2 || a.pred_candle_body > 0.5) {
                        ln += fmt::format(
                            "  surge{:.1f}x body{:.2f}",
                            a.pred_volume_surge_ratio, a.pred_candle_body);
                    }
                    if ((int)ln.size() > center_w - 4) ln.resize(center_w - 4);
                    ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, ln.c_str());
                    ncplane_set_fg_default(p_->std_plane);
                    ++cy;
                }
                // Edges line
                if (cy < main_y + detail_h - 1) {
                    uint32_t col_eu = a.pred_edge_up   > 0.02 ? 0x40ff80
                                    : a.pred_edge_up   < -0.02 ? 0xff5050 : 0x808080;
                    uint32_t col_ed = a.pred_edge_down > 0.02 ? 0x40ff80
                                    : a.pred_edge_down < -0.02 ? 0xff5050 : 0x808080;
                    ncplane_set_fg_rgb(p_->std_plane, 0x808080);
                    ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, " EDGE ");
                    ncplane_set_fg_rgb(p_->std_plane, col_eu);
                    ncplane_putstr_yx(p_->std_plane, cy, center_x + 8,
                        fmt::format("UP {:+.3f}", a.pred_edge_up).c_str());
                    ncplane_set_fg_rgb(p_->std_plane, col_ed);
                    ncplane_putstr_yx(p_->std_plane, cy, center_x + 8 + 14,
                        fmt::format("  DN {:+.3f}", a.pred_edge_down).c_str());
                    ncplane_set_fg_default(p_->std_plane);
                    ++cy;
                }
                if (cy < main_y + detail_h - 1) {
                    hline(p_->std_plane, cy, center_x + 1, center_w - 2);
                    ++cy;
                }
            }
            if (cy < main_y + detail_h - 1) {
                ncplane_set_fg_rgb(p_->std_plane, 0xffff80);
                std::string dec = fmt::format(" {} ", a.decision_text);
                if ((int)dec.size() > center_w - 4) dec.resize(center_w - 4);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, dec.c_str());
                ncplane_set_fg_default(p_->std_plane);
                ++cy;
            }
            if (!a.reason_top1.empty() && cy < main_y + detail_h - 1) {
                ncplane_set_fg_rgb(p_->std_plane, 0xa0a0a0);
                std::string r1 = fmt::format("  • {}", a.reason_top1);
                if ((int)r1.size() > center_w - 4) r1.resize(center_w - 4);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, r1.c_str());
                ncplane_set_fg_default(p_->std_plane);
                ++cy;
            }
            if (!a.reason_top2.empty() && cy < main_y + detail_h - 1) {
                ncplane_set_fg_rgb(p_->std_plane, 0x808080);
                std::string r2 = fmt::format("  • {}", a.reason_top2);
                if ((int)r2.size() > center_w - 4) r2.resize(center_w - 4);
                ncplane_putstr_yx(p_->std_plane, cy, center_x + 2, r2.c_str());
                ncplane_set_fg_default(p_->std_plane);
                ++cy;
            }
        }

        // ══════════════════════════════════════════════════════════════
        //   AI TRADER'S BRIEF  (LLM-authored situational update)
        //   Anchored at the bottom of the CENTER pane — renders ALWAYS,
        //   regardless of whether a market is active. Refreshed every
        //   few minutes by the brief loop in main.cpp.
        // ══════════════════════════════════════════════════════════════
        {
            const int brief_h = std::min(8, std::max(5, detail_h / 4));
            const int brief_y = main_y + detail_h - brief_h;
            box(p_->std_plane, brief_y, center_x, brief_h, center_w,
                "◆ AI TRADER'S BRIEF ◆");

            // Header row inside the box: age + mood tagline
            int by = brief_y + 1;
            std::string age = "no brief yet";
            if (snap.trader_brief_ts_ms > 0) {
                int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                int64_t secs_ago = std::max<int64_t>(0,
                    (now_ms - snap.trader_brief_ts_ms) / 1000);
                age = (secs_ago < 90)
                    ? fmt::format("updated {}s ago", secs_ago)
                    : fmt::format("updated {}m ago", secs_ago / 60);
            }
            int wins   = snap.top.trades_won;
            int losses = snap.top.trades_lost;
            double pnl = snap.top.day_pnl_usd;
            const char* mood; uint32_t mood_col;
            int total = wins + losses;
            double wr = total > 0 ? (double)wins / total : 0.5;
            if (pnl > 0.5 && wr > 0.55)      { mood = "▲ confident";    mood_col = 0x40ff80; }
            else if (pnl > 0)                { mood = "▲ in the green"; mood_col = 0x80ff80; }
            else if (pnl < -1.0 || wr < 0.40){ mood = "▼ cautious";     mood_col = 0xff6060; }
            else                             { mood = "─ steady";       mood_col = 0xb0b0b0; }
            ncplane_set_fg_rgb(p_->std_plane, 0x808080);
            ncplane_putstr_yx(p_->std_plane, by, center_x + 2, age.c_str());
            int mood_w = (int)std::string(mood).size();
            int mood_x = center_x + center_w - 2 - mood_w;
            if (mood_x > center_x + 2 + (int)age.size() + 2) {
                ncplane_set_fg_rgb(p_->std_plane, mood_col);
                ncplane_putstr_yx(p_->std_plane, by, mood_x, mood);
            }
            ncplane_set_fg_default(p_->std_plane);
            ++by;

            // Body: word-wrap trader_brief into available rows
            std::string body = snap.trader_brief;
            if (body.empty()) {
                body = snap.trader_brief_pending
                    ? "[ thinking... an LLM is reading the tape ]"
                    : "[ no brief yet -- first update arrives ~20s after launch, then every 3 min ]";
            } else if (snap.trader_brief_pending) {
                body += "  [refreshing...]";
            }
            int body_w = center_w - 4;
            int max_rows = (brief_y + brief_h - 1) - by;
            if (max_rows > 0 && body_w > 10) {
                std::vector<std::string> lines;
                std::string cur;
                auto flush_line = [&]() { if (!cur.empty()) { lines.push_back(cur); cur.clear(); } };
                std::string word;
                auto push_word = [&]() {
                    if (word.empty()) return;
                    if ((int)cur.size() + (cur.empty() ? 0 : 1) + (int)word.size() > body_w) {
                        flush_line();
                        cur = word;
                    } else {
                        if (!cur.empty()) cur += ' ';
                        cur += word;
                    }
                    word.clear();
                };
                for (char c : body) {
                    if (c == '\n') { push_word(); flush_line(); }
                    else if (c == ' ' || c == '\t') push_word();
                    else word += c;
                }
                push_word();
                flush_line();
                int nrows = std::min<int>((int)lines.size(), max_rows);
                for (int i = 0; i < nrows; ++i) {
                    uint32_t col = (i % 2 == 0) ? 0xa0e0ff : 0xd0d0e0;
                    ncplane_set_fg_rgb(p_->std_plane, col);
                    ncplane_putstr_yx(p_->std_plane, by + i,
                        center_x + 2, lines[i].c_str());
                }
                ncplane_set_fg_default(p_->std_plane);
            }
        }

        // ── RIGHT-BOTTOM: ORDER BOOK (UP) ───────────────────────────
        // Stacks under the watchlist on the right column. Shows top-N
        // asks (descending) and top-N bids (descending) for the
        // currently-selected market's UP token.
        {
            const int book_y = main_y + wl_h;
            box(p_->std_plane, book_y, right_x, rbook_h, right_w, "ORDER BOOK (UP)");
            if (!a.up_bids.empty() || !a.up_asks.empty()) {
                int rows = rbook_h - 2;
                int half = rows / 2;
                int n_asks = std::min<int>(half, (int)a.up_asks.size());
                for (int oi = 0; oi < n_asks; ++oi) {
                    const auto& lv = a.up_asks[n_asks - 1 - oi];
                    ncplane_set_fg_rgb(p_->std_plane, 0xff6060);
                    ncplane_putstr_yx(p_->std_plane,
                        book_y + 1 + oi, right_x + 2,
                        fmt::format(" ASK {:>6.4f}  x {:>10.2f}", lv.price, lv.size).c_str());
                }
                int n_bids = std::min<int>(rows - half, (int)a.up_bids.size());
                for (int oi = 0; oi < n_bids; ++oi) {
                    const auto& lv = a.up_bids[oi];
                    ncplane_set_fg_rgb(p_->std_plane, 0x40ff80);
                    ncplane_putstr_yx(p_->std_plane,
                        book_y + 1 + half + oi, right_x + 2,
                        fmt::format(" BID {:>6.4f}  x {:>10.2f}", lv.price, lv.size).c_str());
                }
                ncplane_set_fg_default(p_->std_plane);
            } else {
                ncplane_putstr_yx(p_->std_plane,
                    book_y + 1, right_x + 2, " (awaiting book…)");
            }
        }

        // ══════════════════════════════════════════════════════════════
        // LEFT: POSITIONS + RECENT RESULTS + SIGNAL HISTORY
        // ══════════════════════════════════════════════════════════════
        // Moved here from the right column so the position lines and
        // signal entries finally have the horizontal room they need.
        // Layout (top → bottom):
        //   POSITIONS       — open trades, dynamic 4..8 rows
        //   RECENT RESULTS  — settled W/L history (12 rows max)
        //   SIGNALS         — uses any leftover space
        const int lcol_x = 0;
        const int lcol_w = left_w;
        int n_pos = (int)snap.position_lines.size();
        int pos_h = std::clamp(n_pos + 3, 5, 9);
        if (pos_h > avail_h - 6) pos_h = std::max(4, avail_h - 6);
        box(p_->std_plane, main_y, lcol_x, pos_h, lcol_w, "POSITIONS");

        // Deployed bar
        double dep_pct = (snap.max_deployed_usd > 0)
            ? std::clamp(snap.deployed_usd / snap.max_deployed_usd, 0.0, 1.0) : 0.0;
        int bw2 = lcol_w - 14;
        int fill2 = (int)(dep_pct * bw2 + 0.5);
        std::string dep_bar(fill2, '#'); dep_bar.append(bw2 - fill2, '.');
        std::string dep_line = fmt::format(" ${:.2f}/${:.2f} [{}]",
            snap.deployed_usd, snap.max_deployed_usd, dep_bar);
        if ((int)dep_line.size() > lcol_w - 2) dep_line.resize(lcol_w - 2);
        uint32_t dep_col = dep_pct > 0.8 ? 0xff4040 : dep_pct > 0.5 ? 0xffaa00 : 0x80a0ff;
        ncplane_set_fg_rgb(p_->std_plane, dep_col);
        ncplane_putstr_yx(p_->std_plane, main_y + 1, lcol_x + 1, dep_line.c_str());
        ncplane_set_fg_default(p_->std_plane);

        if (snap.position_lines.empty()) {
            ncplane_set_fg_rgb(p_->std_plane, 0x707070);
            ncplane_putstr_yx(p_->std_plane, main_y + 2, lcol_x + 2,
                              " (no open positions — awaiting settlement)");
            ncplane_set_fg_default(p_->std_plane);
        } else {
            for (int pi = 0; pi < n_pos; ++pi) {
                int pr = main_y + 2 + pi;
                if (pr >= main_y + pos_h - 1) break;
                std::string s = snap.position_lines[pi];
                if ((int)s.size() > lcol_w - 4) s.resize(lcol_w - 4);
                ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
                ncplane_set_fg_rgb(p_->std_plane, 0xfff080);
                ncplane_putstr_yx(p_->std_plane, pr, lcol_x + 2, s.c_str());
                ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
                ncplane_set_fg_default(p_->std_plane);
            }
        }

        // ── RECENT RESULTS panel ────────────────────────────────────
        // The big W/L scoreboard. Rows are color-coded green (win) or
        // red (loss) so the user can SEE the wallet trajectory at a
        // glance instead of squinting at log files.
        int res_y = main_y + pos_h;
        int res_h = std::min<int>((int)snap.recent_results.size() + 3, 14);
        if (res_h < 5) res_h = 5;                       // always show header
        if (res_h > avail_h - pos_h - 3) res_h = avail_h - pos_h - 3;
        if (res_h >= 4) {
            box(p_->std_plane, res_y, lcol_x, res_h, lcol_w, "RECENT RESULTS  W/L");

            if (snap.recent_results.empty()) {
                ncplane_set_fg_rgb(p_->std_plane, 0x707070);
                ncplane_putstr_yx(p_->std_plane, res_y + 1, lcol_x + 2,
                                  " (no settled trades yet)");
                ncplane_set_fg_default(p_->std_plane);
            } else {
                // Highlight the most recent settlement as a banner.
                const auto& last = snap.recent_results.front();
                bool is_win = last.win;
                uint32_t banner_bg = is_win ? 0x0a3818 : 0x381010;
                uint32_t banner_fg = is_win ? 0x40ff80 : 0xff5050;
                ncplane_set_bg_rgb(p_->std_plane, banner_bg);
                ncplane_set_fg_rgb(p_->std_plane, banner_fg);
                ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
                std::string banner = fmt::format(" {} {:>+5.2f}  {} {}  ",
                    is_win ? "WON " : "LOST",
                    last.pnl_usd, last.asset, last.side_label);
                int pad = lcol_w - 2 - (int)banner.size();
                if (pad > 0) banner.append(pad, ' ');
                if ((int)banner.size() > lcol_w - 2) banner.resize(lcol_w - 2);
                ncplane_putstr_yx(p_->std_plane, res_y + 1, lcol_x + 1, banner.c_str());
                ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);
                ncplane_set_bg_default(p_->std_plane);
                ncplane_set_fg_default(p_->std_plane);

                // Then list older results below the banner.
                int rows_avail = res_h - 3;   // box(2) + banner(1)
                int n_show = std::min<int>(rows_avail,
                                           (int)snap.recent_results.size() - 1);
                for (int ri = 0; ri < n_show; ++ri) {
                    const auto& r = snap.recent_results[ri + 1];
                    int rr_y = res_y + 2 + ri;
                    // HH:MM
                    auto t = (std::time_t)(r.ts_ms / 1000);
                    std::tm tm_buf{};
                    localtime_r(&t, &tm_buf);
                    uint32_t fg = r.win ? 0x40ff80 : 0xff5050;
                    ncplane_set_fg_rgb(p_->std_plane, fg);
                    std::string ln = fmt::format(
                        " {:02d}:{:02d} {:<3} {:>2} {:>4}m {:>+5.2f}",
                        tm_buf.tm_hour, tm_buf.tm_min,
                        r.asset, r.side_label,
                        r.horizon_s / 60, r.pnl_usd);
                    if ((int)ln.size() > lcol_w - 2) ln.resize(lcol_w - 2);
                    ncplane_putstr_yx(p_->std_plane, rr_y, lcol_x + 1, ln.c_str());
                    ncplane_set_fg_default(p_->std_plane);
                }
            }
        }

        // Signal history fills any remaining space at the bottom.
        int sig_y = res_y + std::max(res_h, 0);
        int sig_h = (main_y + avail_h) - sig_y;
        if (sig_h > 2) {
            box(p_->std_plane, sig_y, lcol_x, sig_h, lcol_w, "SIGNALS");
            for (size_t si = 0; si < snap.signal_history.size(); ++si) {
                int sr = sig_y + 1 + (int)si;
                if (sr >= sig_y + sig_h - 1) break;
                std::string s = snap.signal_history[si];
                if ((int)s.size() > lcol_w - 4) s.resize(lcol_w - 4);
                ncplane_set_fg_rgb(p_->std_plane, 0x909090);
                ncplane_putstr_yx(p_->std_plane, sr, lcol_x + 2, s.c_str());
                ncplane_set_fg_default(p_->std_plane);
            }
        }

        // ---- modal command menu ----
        if (p_->menu_mode != Impl::MenuMode::None) {
            struct Item { const char* key; const char* label; const char* detail; };
            static constexpr Item kTradeItems[] = {
                {"B", "Buy UP",        "Spend configured $/order on selected UP token"},
                {"N", "Buy DOWN",      "Spend configured $/order on selected DOWN token"},
                {"S", "Sell UP",       "Flatten selected market's UP position"},
                {"M", "Sell DOWN",     "Flatten selected market's DOWN position"},
                {"K", "Flatten both",  "Sell any UP and DOWN inventory in selected market"},
                {"A", "Toggle auto",   "Enable/disable autonomous rule fires"},
                {"L", "Ask LLM",       "Request advisor context for selected market"},
                {"X", "Clear alerts",  "Clear transient alerts/signals"},
            };
            static constexpr Item kOptionItems[] = {
                {"A", "Autopilot",     "Toggle autonomous rule execution"},
                {"L", "Advisor",       "Refresh AI market brief for selected market"},
                {"R", "Reset PnL",     "Reset session P&L / W-L counters"},
                {"X", "Clear",         "Clear alerts and transient UI state"},
            };
            const bool trade = p_->menu_mode == Impl::MenuMode::Trade;
            const Item* items = trade ? kTradeItems : kOptionItems;
            const int count = trade ? (int)(sizeof(kTradeItems) / sizeof(kTradeItems[0]))
                                    : (int)(sizeof(kOptionItems) / sizeof(kOptionItems[0]));
            const int mw = std::min<int>(72, std::max<int>(52, C - 8));
            const int mh = count + 5;
            const int my = std::max(1, ((int)R - mh) / 2);
            const int mx = std::max(1, ((int)C - mw) / 2);

            ncplane_set_fg_rgb(p_->std_plane, 0x85e8ff);
            ncplane_set_bg_rgb(p_->std_plane, 0x06131a);
            box(p_->std_plane, my, mx, mh, mw, trade ? "TRADE MENU" : "OPTIONS MENU");
            ncplane_set_styles(p_->std_plane, NCSTYLE_BOLD);
            ncplane_putstr_yx(p_->std_plane, my + 1, mx + 2,
                trade ? "ENTER=fire selected   TAB/←/→=options   ESC=close"
                      : "ENTER=apply selected  TAB/←/→=trade     ESC=close");
            ncplane_set_styles(p_->std_plane, NCSTYLE_NONE);

            for (int mi = 0; mi < count; ++mi) {
                int y = my + 3 + mi;
                bool sel = mi == p_->menu_index;
                if (sel) {
                    ncplane_set_bg_rgb(p_->std_plane, 0x1f4560);
                    ncplane_set_fg_rgb(p_->std_plane, 0xffffff);
                } else {
                    ncplane_set_bg_rgb(p_->std_plane, 0x06131a);
                    ncplane_set_fg_rgb(p_->std_plane, 0xb8c8d8);
                }
                std::string line = fmt::format(" {}  [{:<1}] {:<13}  {}",
                    sel ? "▶" : " ", items[mi].key, items[mi].label, items[mi].detail);
                if ((int)line.size() > mw - 4) line.resize(mw - 4);
                line.append(std::max(0, mw - 4 - (int)line.size()), ' ');
                ncplane_putstr_yx(p_->std_plane, y, mx + 2, line.c_str());
            }
            ncplane_set_bg_default(p_->std_plane);
            ncplane_set_fg_default(p_->std_plane);
        }

        // ---- status line ----
        ncplane_set_bg_rgb(p_->std_plane, 0x202020);
        ncplane_set_fg_rgb(p_->std_plane, 0x909090);
        int total_trades = snap.top.trades_won + snap.top.trades_lost;
        double wr = total_trades > 0 ? 100.0 * snap.top.trades_won / total_trades : 0.0;
        std::string hint;
        if (snap.status_hint.empty()) {
            hint = fmt::format(
                "  ↑/↓ or j/i=select  ENTER=trade menu  O=options  ESC=close menu  q=quit"
                "  direct: B/N buy  S/M sell  A auto  L llm   W/L: {}/{} ({:.0f}%)  ",
                snap.top.trades_won, snap.top.trades_lost, wr);
        } else {
            hint = snap.status_hint;
        }
        if (int(hint.size()) > int(C)) hint.resize(C);
        else hint.append(C - hint.size(), ' ');
        ncplane_putstr_yx(p_->std_plane, R - 1, 0, hint.c_str());
        ncplane_set_bg_default(p_->std_plane);
        ncplane_set_fg_default(p_->std_plane);

        notcurses_render(p_->nc);
        std::this_thread::sleep_until(next_frame);
        next_frame += frame_period;
    }

    // Tell input thread to stop and wait for it
    p_->quit.store(true);
    p_->running.store(false);
    if (input_th.joinable()) input_th.join();
}

} // namespace vp

