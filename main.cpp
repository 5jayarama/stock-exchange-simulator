// Stock Exchange Simulator
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "exchange.hpp"

using namespace exchange;

#ifdef __EMSCRIPTEN__

#define API extern "C" EMSCRIPTEN_KEEPALIVE


namespace {

// Bot orders use their own id space so they never collide with user orders.
constexpr OrderId kBotIdBase = 1'000'000'000;

struct Session {
    OrderBook book{1 << 16};
    OrderFlow bots{2024};
    OrderId next_user_id = 1;
    std::vector<Trade> recent; // trades not yet sent to the UI
    std::uint64_t total_trades = 0;
    std::uint64_t total_volume = 0;
    Price last_price = 0;
};

std::unique_ptr<Session> g;
std::string g_json;

void collect_trades() {
    for (const auto& t : g->book.trades()) {
        g->recent.push_back(t);
        ++g->total_trades;
        g->total_volume += t.qty;
        g->last_price = t.price;
    }
    g->book.clear_trades();
    if (g->recent.size() > 200) g->recent.erase(g->recent.begin(), g->recent.end() - 200);
}

void append_levels(std::string& s, Side side, int levels) {
    s += '[';
    bool first = true;
    for (const auto& l : g->book.depth(side, static_cast<std::size_t>(levels))) {
        if (!first) s += ',';
        first = false;
        s += '[' + std::to_string(l.price) + ',' + std::to_string(l.qty) + ',' +
             std::to_string(l.orders) + ']';
    }
    s += ']';
}

Session& session() {
    if (!g) g = std::make_unique<Session>();
    return *g;
}

int finish(Result r) {
    collect_trades();
    return static_cast<int>(r.status);
}

} // namespace

API void ob_reset() { g = std::make_unique<Session>(); }

// Returns the new order's id. Read the outcome with ob_last_status().
static int g_last_status = 0;
static double g_last_filled = 0;

API double ob_limit(int side, double price_ticks, double qty) {
    auto& s = session();
    const OrderId id = s.next_user_id++;
    const Result r = s.book.add_limit(id, side == 0 ? Side::Buy : Side::Sell,
                                      static_cast<Price>(price_ticks), static_cast<Qty>(qty));
    g_last_filled = static_cast<double>(r.filled);
    g_last_status = finish(r);
    return static_cast<double>(id);
}

API double ob_market(int side, double qty) {
    auto& s = session();
    const OrderId id = s.next_user_id++;
    const Result r = s.book.add_market(id, side == 0 ? Side::Buy : Side::Sell, static_cast<Qty>(qty));
    g_last_filled = static_cast<double>(r.filled);
    g_last_status = finish(r);
    return static_cast<double>(id);
}

API int ob_cancel(double id) {
    return finish(session().book.cancel(static_cast<OrderId>(id)));
}

API int ob_last_status() { return g_last_status; }
API double ob_last_filled() { return g_last_filled; }
API int ob_is_open(double id) { return session().book.contains(static_cast<OrderId>(id)) ? 1 : 0; }

// Sends n random orders from simulated traders.
API void ob_bots(int n) {
    auto& s = session();
    for (int i = 0; i < n; ++i) {
        Command c = s.bots.next();
        c.id += kBotIdBase;
        apply(s.book, c);
        collect_trades();
    }
}

// Book snapshot plus any trades since the previous call, as JSON.
API const char* ob_state_json(int levels) {
    auto& s = session();
    std::string& j = g_json;
    j.clear();
    j += "{\"bids\":";
    append_levels(j, Side::Buy, levels);
    j += ",\"asks\":";
    append_levels(j, Side::Sell, levels);
    j += ",\"orders\":" + std::to_string(s.book.order_count());
    j += ",\"totalTrades\":" + std::to_string(s.total_trades);
    j += ",\"volume\":" + std::to_string(s.total_volume);
    j += ",\"last\":" + std::to_string(s.last_price);
    j += ",\"trades\":[";
    for (std::size_t i = 0; i < s.recent.size(); ++i) {
        const auto& t = s.recent[i];
        if (i) j += ',';
        j += "{\"taker\":" + std::to_string(t.taker_id) + ",\"maker\":" + std::to_string(t.maker_id) +
             ",\"price\":" + std::to_string(t.price) + ",\"qty\":" + std::to_string(t.qty) +
             ",\"side\":\"" + (t.taker_side == Side::Buy ? "B" : "S") + "\"}";
    }
    j += "]}";
    s.recent.clear();
    return j.c_str();
}

// Runs n random orders on a separate book and reports speed, in the browser.
API const char* ob_bench(int n) {
    using Clock = std::chrono::steady_clock;
    OrderFlow flow(42);
    const auto cmds = flow.generate(static_cast<std::size_t>(n));
    OrderBook book(static_cast<std::size_t>(n));
    std::vector<std::int64_t> lat;
    lat.reserve(cmds.size());
    std::uint64_t trades = 0;
    const auto start = Clock::now();
    for (const auto& c : cmds) {
        const auto t0 = Clock::now();
        apply(book, c);
        trades += book.trades().size();
        book.clear_trades();
        lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double q) { return lat[std::min(lat.size() - 1, static_cast<std::size_t>(q * lat.size()))]; };
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"orders\":%d,\"trades\":%llu,\"seconds\":%.4f,\"mops\":%.3f,\"p50\":%lld,\"p99\":%lld}",
                  n, static_cast<unsigned long long>(trades), secs, n / secs / 1e6,
                  static_cast<long long>(pct(0.50)), static_cast<long long>(pct(0.99)));
    g_json = buf;
    return g_json.c_str();
}

#else
// Command-line exchange
namespace cli {

namespace {

// Parses "100.25" into 10025 ticks without floating point.
bool parse_price(const std::string& s, Price& out) {
    Price whole = 0, frac = 0;
    int frac_digits = 0;
    bool seen_dot = false, any = false;
    for (char c : s) {
        if (c == '.') {
            if (seen_dot) return false;
            seen_dot = true;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            any = true;
            if (seen_dot) {
                if (++frac_digits > 2) return false;
                frac = frac * 10 + (c - '0');
            } else {
                whole = whole * 10 + (c - '0');
            }
        } else {
            return false;
        }
    }
    if (frac_digits == 1) frac *= 10;
    out = whole * 100 + frac;
    return any;
}

std::string fmt_price(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld.%02lld", static_cast<long long>(p / 100),
                  static_cast<long long>(p % 100));
    return buf;
}

bool parse_side(std::string s, Side& out) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s == "BUY" || s == "B") { out = Side::Buy; return true; }
    if (s == "SELL" || s == "S") { out = Side::Sell; return true; }
    return false;
}

void print_book(const OrderBook& b) {
    auto asks = b.depth(Side::Sell, 5);
    auto bids = b.depth(Side::Buy, 5);
    std::printf("  %-10s %10s %7s\n", "", "PRICE", "QTY");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::printf("  %-10s %10s %7llu\n", "ASK", fmt_price(it->price).c_str(),
                    static_cast<unsigned long long>(it->qty));
    std::printf("  ----------------------------\n");
    for (const auto& l : bids)
        std::printf("  %-10s %10s %7llu\n", "BID", fmt_price(l.price).c_str(),
                    static_cast<unsigned long long>(l.qty));
}

void print_trades(OrderBook& b) {
    for (const auto& t : b.trades())
        std::printf("TRADE  %s taker=%llu maker=%llu price=%s qty=%llu\n",
                    t.taker_side == Side::Buy ? "BUY " : "SELL",
                    static_cast<unsigned long long>(t.taker_id),
                    static_cast<unsigned long long>(t.maker_id), fmt_price(t.price).c_str(),
                    static_cast<unsigned long long>(t.qty));
    b.clear_trades();
}

} // namespace

int run(int argc, char** argv) {
    std::ifstream file;
    std::istream* in = &std::cin;
    if (argc > 1) {
        file.open(argv[1]);
        if (!file) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 1;
        }
        in = &file;
    }

    OrderBook book;
    std::string line;
    int lineno = 0;
    while (std::getline(*in, line)) {
        ++lineno;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        for (auto& c : cmd) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        Result r{};
        std::string side_s, price_s;
        OrderId id = 0;
        Qty qty = 0;
        Side side{};
        Price price = 0;

        if (cmd == "LIMIT" && (ss >> side_s >> id >> price_s >> qty) && parse_side(side_s, side) &&
            parse_price(price_s, price)) {
            r = book.add_limit(id, side, price, qty);
        } else if (cmd == "MARKET" && (ss >> side_s >> id >> qty) && parse_side(side_s, side)) {
            r = book.add_market(id, side, qty);
        } else if (cmd == "CANCEL" && (ss >> id)) {
            r = book.cancel(id);
        } else if (cmd == "BOOK") {
            print_book(book);
            continue;
        } else {
            std::fprintf(stderr, "line %d: cannot parse: %s\n", lineno, line.c_str());
            continue;
        }

        print_trades(book);
        std::printf("%-17s id=%llu filled=%llu resting=%llu\n", to_string(r.status),
                    static_cast<unsigned long long>(id), static_cast<unsigned long long>(r.filled),
                    static_cast<unsigned long long>(r.remaining));
    }
    return 0;
}

} // namespace cli

// Benchmarks
namespace bench {

using Clock = std::chrono::steady_clock;

namespace {

// Pin the calling thread to one core so the OS does not migrate it mid-run.
void pin_to_core(int core) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core % static_cast<int>(std::thread::hardware_concurrency()), &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#else
    (void)core;
#endif
}

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

struct Stats {
    double p50, p90, p99, p999, max;
};

Stats percentiles(std::vector<std::int64_t>& v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        return static_cast<double>(v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))]);
    };
    return {at(0.50), at(0.90), at(0.99), at(0.999), static_cast<double>(v.back())};
}

void print_stats(const char* label, const Stats& s) {
    std::printf("  %-28s p50 %6.0f ns | p90 %6.0f | p99 %6.0f | p99.9 %7.0f | max %8.0f\n", label,
                s.p50, s.p90, s.p99, s.p999, s.max);
}

void bench_single(const std::vector<Command>& cmds) {
    std::printf("\n[1] Single-thread matching (%zu orders)\n", cmds.size());

    // Throughput: no per-order timing overhead.
    {
        OrderBook book(cmds.size());
        const auto t0 = Clock::now();
        std::uint64_t trades = 0;
        for (const auto& c : cmds) {
            apply(book, c);
            trades += book.trades().size();
            book.clear_trades();
        }
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("  throughput                   %.2f M orders/sec  (%llu trades)\n",
                    cmds.size() / secs / 1e6, static_cast<unsigned long long>(trades));
    }

    // Latency: time every call (includes ~20 ns clock overhead).
    {
        OrderBook book(cmds.size());
        std::vector<std::int64_t> lat;
        lat.reserve(cmds.size());
        for (const auto& c : cmds) {
            const auto t0 = now_ns();
            apply(book, c);
            book.clear_trades();
            lat.push_back(now_ns() - t0);
        }
        auto s = percentiles(lat);
        print_stats("latency per order", s);
    }
}

struct Timed {
    Command cmd;
    std::int64_t sent_ns;
};

// Mutex + deque baseline with the same try_push/try_pop interface.
class MutexQueue {
public:
    bool try_push(const Timed& v) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= 65536) return false;
        q_.push_back(v);
        return true;
    }
    bool try_pop(Timed& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex m_;
    std::deque<Timed> q_;
};

// Runs producer (gateway) and consumer (matcher) threads.
// pace_ns = 0 floods the queue to measure max throughput; pace_ns > 0 sends
// at a fixed rate so latency reflects the pipeline, not a backed-up queue.
template <typename Queue>
void run_pipeline(Queue& q, const std::vector<Command>& cmds, std::int64_t pace_ns,
                  double& out_mops, std::vector<std::int64_t>* lat) {
    OrderBook book(cmds.size());
    std::atomic<bool> start{false};
    const std::size_t n = cmds.size();

    std::thread producer([&] {
        pin_to_core(0);
        while (!start.load(std::memory_order_acquire)) {}
        std::int64_t next = now_ns();
        for (std::size_t i = 0; i < n; ++i) {
            if (pace_ns) {
                while (now_ns() < next) {}
                next += pace_ns;
            }
            Timed t{cmds[i], now_ns()};
            while (!q.try_push(t)) {}
        }
    });

    pin_to_core(1);
    const auto t0 = Clock::now();
    start.store(true, std::memory_order_release);
    Timed t;
    for (std::size_t done = 0; done < n;) {
        if (!q.try_pop(t)) continue;
        apply(book, t.cmd);
        book.clear_trades();
        if (lat) lat->push_back(now_ns() - t.sent_ns);
        ++done;
    }
    const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    producer.join();
    out_mops = n / secs / 1e6;
}

void bench_pipeline(const std::vector<Command>& cmds) {
    std::printf("\n[2] Two-thread pipeline: gateway -> queue -> matcher (%zu orders)\n",
                cmds.size());
    if (std::thread::hardware_concurrency() < 2)
        std::printf("  WARNING: fewer than 2 cores, results will be meaningless\n");

    double spsc_mops = 0, mutex_mops = 0, ignored = 0;
    {
        auto q = std::make_unique<SpscQueue<Timed, 65536>>();
        run_pipeline(*q, cmds, 0, spsc_mops, nullptr);
    }
    {
        MutexQueue q;
        run_pipeline(q, cmds, 0, mutex_mops, nullptr);
    }
    std::printf("  max throughput, lock-free    %.2f M orders/sec\n", spsc_mops);
    std::printf("  max throughput, mutex        %.2f M orders/sec\n", mutex_mops);
    std::printf("  lock-free speedup            %.0f%%\n", (spsc_mops / mutex_mops - 1) * 100);

    // Latency at a sustainable rate: 1 order every 1 µs (1M orders/sec).
    const std::size_t m = std::min<std::size_t>(cmds.size(), 500'000);
    std::vector<Command> sub(cmds.begin(), cmds.begin() + static_cast<long>(m));
    std::vector<std::int64_t> lat_spsc, lat_mutex;
    lat_spsc.reserve(m);
    lat_mutex.reserve(m);
    {
        auto q = std::make_unique<SpscQueue<Timed, 65536>>();
        run_pipeline(*q, sub, 1000, ignored, &lat_spsc);
    }
    {
        MutexQueue q;
        run_pipeline(q, sub, 1000, ignored, &lat_mutex);
    }
    std::printf("  end-to-end latency at 1M orders/sec (%zu orders):\n", m);
    print_stats("lock-free queue", percentiles(lat_spsc));
    print_stats("mutex queue", percentiles(lat_mutex));
}

} // namespace

int run(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    OrderFlow flow(42);
    std::printf("Generating %zu random orders (55%% limit, 10%% market, 35%% cancel)...\n", n);
    const auto cmds = flow.generate(n);

    // Warm caches and the allocator.
    {
        OrderBook warm(n);
        for (std::size_t i = 0; i < std::min<std::size_t>(n, 200'000); ++i) {
            apply(warm, cmds[i]);
            warm.clear_trades();
        }
    }

    bench_single(cmds);
    bench_pipeline(cmds);
    return 0;
}

}
namespace tests {




static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failed;                                                      \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                    \
    } while (0)

#define TEST(name) static void name()
#define RUN(name)                          \
    do {                                   \
        std::printf("%s\n", #name);        \
        name();                            \
    } while (0)

TEST(limit_order_rests_when_not_crossing) {
    OrderBook b;
    auto r = b.add_limit(1, Side::Buy, 10'000, 100);
    CHECK(r.status == Status::Accepted);
    CHECK(r.remaining == 100);
    CHECK(b.best_bid() == 10'000);
    CHECK(b.best_ask() == 0);
    b.add_limit(2, Side::Sell, 10'010, 50);
    CHECK(b.best_ask() == 10'010);
    CHECK(b.trades().empty());
}

TEST(crossing_limit_trades_at_maker_price) {
    OrderBook b;
    b.add_limit(1, Side::Sell, 10'000, 100);
    auto r = b.add_limit(2, Side::Buy, 10'050, 40); // willing to pay more
    CHECK(r.status == Status::Filled);
    CHECK(r.filled == 40);
    CHECK(b.trades().size() == 1);
    CHECK(b.trades()[0].price == 10'000); // maker's price, not taker's
    CHECK(b.trades()[0].maker_id == 1);
    CHECK(b.trades()[0].taker_id == 2);
    CHECK(b.depth(Side::Sell, 1)[0].qty == 60);
}

TEST(price_priority_best_price_fills_first) {
    OrderBook b;
    b.add_limit(1, Side::Sell, 10'020, 10);
    b.add_limit(2, Side::Sell, 10'000, 10); // better price, arrived later
    b.add_limit(3, Side::Sell, 10'010, 10);
    b.add_market(4, Side::Buy, 25);
    const auto& t = b.trades();
    CHECK(t.size() == 3);
    CHECK(t[0].maker_id == 2 && t[0].price == 10'000);
    CHECK(t[1].maker_id == 3 && t[1].price == 10'010);
    CHECK(t[2].maker_id == 1 && t[2].qty == 5);
}

TEST(time_priority_oldest_first_at_same_price) {
    OrderBook b;
    b.add_limit(1, Side::Buy, 10'000, 10);
    b.add_limit(2, Side::Buy, 10'000, 10);
    b.add_limit(3, Side::Buy, 10'000, 10);
    b.add_limit(4, Side::Sell, 10'000, 15);
    const auto& t = b.trades();
    CHECK(t.size() == 2);
    CHECK(t[0].maker_id == 1 && t[0].qty == 10);
    CHECK(t[1].maker_id == 2 && t[1].qty == 5);
    auto d = b.depth(Side::Buy, 5);
    CHECK(d.size() == 1 && d[0].qty == 15 && d[0].orders == 2);
}

TEST(partial_fill_rests_remainder) {
    OrderBook b;
    b.add_limit(1, Side::Sell, 10'000, 30);
    auto r = b.add_limit(2, Side::Buy, 10'000, 100);
    CHECK(r.status == Status::PartiallyFilled);
    CHECK(r.filled == 30 && r.remaining == 70);
    CHECK(b.best_ask() == 0);
    CHECK(b.best_bid() == 10'000);
    CHECK(b.contains(2) && !b.contains(1));
}

TEST(limit_stops_at_its_price) {
    OrderBook b;
    b.add_limit(1, Side::Sell, 10'000, 10);
    b.add_limit(2, Side::Sell, 10'005, 10);
    auto r = b.add_limit(3, Side::Buy, 10'000, 20);
    CHECK(r.filled == 10 && r.remaining == 10);
    CHECK(b.best_bid() == 10'000);
    CHECK(b.best_ask() == 10'005);
}

TEST(market_order_sweeps_and_discards_remainder) {
    OrderBook b;
    b.add_limit(1, Side::Buy, 10'000, 10);
    b.add_limit(2, Side::Buy, 9'990, 10);
    auto r = b.add_market(3, Side::Sell, 50);
    CHECK(r.status == Status::PartiallyFilled);
    CHECK(r.filled == 20 && r.remaining == 0);
    CHECK(b.order_count() == 0);
    CHECK(!b.contains(3)); // market orders never rest
}

TEST(market_order_on_empty_book) {
    OrderBook b;
    auto r = b.add_market(1, Side::Buy, 10);
    CHECK(r.status == Status::Cancelled && r.filled == 0);
}

TEST(cancel_removes_order_and_empty_level) {
    OrderBook b;
    b.add_limit(1, Side::Buy, 10'000, 10);
    b.add_limit(2, Side::Buy, 10'000, 20);
    b.add_limit(3, Side::Buy, 9'990, 5);
    auto r = b.cancel(2);
    CHECK(r.status == Status::Cancelled && r.remaining == 20);
    CHECK(b.depth(Side::Buy, 1)[0].qty == 10);
    b.cancel(1);
    CHECK(b.best_bid() == 9'990); // empty level removed
    CHECK(b.level_count(Side::Buy) == 1);
    CHECK(b.cancel(1).status == Status::RejectedUnknownId);
}

TEST(cancel_middle_of_queue_keeps_order) {
    OrderBook b;
    b.add_limit(1, Side::Sell, 10'000, 10);
    b.add_limit(2, Side::Sell, 10'000, 10);
    b.add_limit(3, Side::Sell, 10'000, 10);
    b.cancel(2);
    b.add_market(4, Side::Buy, 20);
    const auto& t = b.trades();
    CHECK(t.size() == 2);
    CHECK(t[0].maker_id == 1 && t[1].maker_id == 3);
}

TEST(rejects_invalid_and_duplicate) {
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 10'000, 0).status == Status::RejectedInvalid);
    CHECK(b.add_limit(1, Side::Buy, 0, 10).status == Status::RejectedInvalid);
    CHECK(b.add_market(1, Side::Buy, 0).status == Status::RejectedInvalid);
    b.add_limit(1, Side::Buy, 10'000, 10);
    CHECK(b.add_limit(1, Side::Sell, 10'100, 10).status == Status::RejectedDuplicateId);
    CHECK(b.add_market(1, Side::Sell, 10).status == Status::RejectedDuplicateId);
}

TEST(filled_id_can_be_reused) {
    OrderBook b;
    b.add_limit(1, Side::Sell, 10'000, 10);
    b.add_market(2, Side::Buy, 10);
    CHECK(b.add_limit(1, Side::Buy, 9'000, 5).status == Status::Accepted);
}

// Randomized check: after every command the book must be internally
// consistent (levels sum correctly, book never crossed, counts match).
TEST(random_flow_invariants) {
    OrderBook b;
    OrderFlow flow(7);
    Qty traded_by_takers = 0, traded_total = 0;
    for (int i = 0; i < 200'000; ++i) {
        const auto r = apply(b, flow.next());
        traded_by_takers += r.filled;
        for (const auto& t : b.trades()) traded_total += t.qty;
        b.clear_trades();
        if (b.best_bid() && b.best_ask()) {
            if (!(b.best_bid() < b.best_ask())) { CHECK(!"book crossed"); return; }
        }
    }
    CHECK(traded_by_takers == traded_total);
    std::size_t orders = 0;
    for (Side s : {Side::Buy, Side::Sell})
        for (const auto& l : b.depth(s, 1'000'000)) {
            CHECK(l.qty > 0 && l.orders > 0);
            orders += l.orders;
        }
    CHECK(orders == b.order_count());
}

TEST(spsc_queue_transfers_in_order) {
    SpscQueue<std::uint64_t, 1024> q;
    constexpr std::uint64_t N = 2'000'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i)
            while (!q.try_push(i)) {}
    });
    std::uint64_t expected = 0, v = 0;
    bool in_order = true;
    while (expected < N) {
        if (q.try_pop(v)) {
            if (v != expected) in_order = false;
            ++expected;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK(!q.try_pop(v));
}

TEST(spsc_queue_full_and_empty) {
    SpscQueue<int, 4> q;
    int v;
    CHECK(!q.try_pop(v));
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    CHECK(q.try_pop(v) && v == 0);
    CHECK(q.try_push(4));
}

int run() {
    RUN(limit_order_rests_when_not_crossing);
    RUN(crossing_limit_trades_at_maker_price);
    RUN(price_priority_best_price_fills_first);
    RUN(time_priority_oldest_first_at_same_price);
    RUN(partial_fill_rests_remainder);
    RUN(limit_stops_at_its_price);
    RUN(market_order_sweeps_and_discards_remainder);
    RUN(market_order_on_empty_book);
    RUN(cancel_removes_order_and_empty_level);
    RUN(cancel_middle_of_queue_keeps_order);
    RUN(rejects_invalid_and_duplicate);
    RUN(filled_id_can_be_reused);
    RUN(random_flow_invariants);
    RUN(spsc_queue_transfers_in_order);
    RUN(spsc_queue_full_and_empty);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace tests

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "test") == 0) return tests::run();
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0) return bench::run(argc - 1, argv + 1);
    return cli::run(argc, argv);
}
#endif
