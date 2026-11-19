// Stock Exchange Simulator: order book, matching engine, memory pool,
// lock-free queue, and random order generator. Header-only, C++20.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exchange {

// Prices are integer ticks (1 tick = $0.01) to avoid floating point error.
using Price = std::int64_t;
using Qty = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class Status : std::uint8_t {
    Accepted,        // fully rested, no fills
    PartiallyFilled, // some fills, remainder rested (limit) or discarded (market)
    Filled,          // fully filled
    Cancelled,
    RejectedDuplicateId,
    RejectedInvalid, // zero qty or non-positive price
    RejectedUnknownId,
};

struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    Price price; // the user's desired price
    Qty qty;
    Side taker_side;
};

struct Result {
    Status status;
    Qty filled;    // quantity executed by this call
    Qty remaining; // quantity left resting on the book (0 for market orders)
};

struct LevelInfo {
    Price price;
    Qty qty;
    std::uint32_t orders;
};

inline const char* to_string(Status s) {
    switch (s) {
        case Status::Accepted: return "ACCEPTED";
        case Status::PartiallyFilled: return "PARTIAL";
        case Status::Filled: return "FILLED";
        case Status::Cancelled: return "CANCELLED";
        case Status::RejectedDuplicateId: return "REJECT_DUPLICATE_ID";
        case Status::RejectedInvalid: return "REJECT_INVALID";
        case Status::RejectedUnknownId: return "REJECT_UNKNOWN_ID";
    }
    return "?";
}

}

namespace exchange {

/* Fixed-size object pool. Allocates memory in chunks and recycles freed slots
through an intrusive free list.*/
// Pointers stay valid until the object is released.
template <typename T, std::size_t ChunkSize = 4096>
class ObjectPool {
    union Slot {
        Slot* next_free;
        alignas(T) unsigned char storage[sizeof(T)];
    };

public:
    ObjectPool() = default;
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* acquire(Args&&... args) {
        if (!free_) grow();
        Slot* s = free_;
        free_ = s->next_free;
        ++live_;
        return ::new (static_cast<void*>(s->storage)) T{std::forward<Args>(args)...};
    }

    void release(T* obj) noexcept {
        obj->~T();
        Slot* s = reinterpret_cast<Slot*>(obj);
        s->next_free = free_;
        free_ = s;
        --live_;
    }

    // Pre-allocate capacity for at least n objects.
    void reserve(std::size_t n) {
        while (capacity() < n) grow();
    }

    std::size_t live() const noexcept { return live_; }
    std::size_t capacity() const noexcept { return chunks_.size() * ChunkSize; }

private:
    void grow() {
        auto chunk = std::make_unique<Slot[]>(ChunkSize);
        for (std::size_t i = 0; i < ChunkSize; ++i) {
            chunk[i].next_free = free_;
            free_ = &chunk[i];
        }
        chunks_.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<Slot[]>> chunks_;
    Slot* free_ = nullptr;
    std::size_t live_ = 0;
};

}

namespace exchange {

// 64 bytes on x86-64 and most ARM cores. Hardcoded because
// std::hardware_destructive_interference_size triggers ABI warnings in GCC.
inline constexpr std::size_t kCacheLine = 64;

// Lock-free single-producer / single-consumer buffer.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static constexpr std::size_t kMask = Capacity - 1;

public:
    // Producer thread only.
    bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == Capacity) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == Capacity) return false; // full
        }
        buffer_[tail & kMask] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false; // empty
        }
        out = buffer_[head & kMask];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // written by consumer
    alignas(kCacheLine) std::size_t tail_cache_ = 0;      // consumer's copy of tail
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // written by producer
    alignas(kCacheLine) std::size_t head_cache_ = 0;      // producer's copy of head
    alignas(kCacheLine) std::array<T, Capacity> buffer_{};
};

}

namespace exchange {

// A resting order. Orders at the same price form an intrusive doubly linked
// list (prev/next live inside the order), so cancel is O(1) once the order is
// found and no separate list nodes are ever allocated.
struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty qty; // remaining quantity
    Order* prev = nullptr;
    Order* next = nullptr;
};

// All orders at one price, oldest first (time priority).
struct PriceLevel {
    Order* head = nullptr;
    Order* tail = nullptr;
    Qty total_qty = 0;
    std::uint32_t count = 0;

    void push_back(Order* o) noexcept {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o; else head = o;
        tail = o;
        total_qty += o->qty;
        ++count;
    }

    void unlink(Order* o) noexcept {
        if (o->prev) o->prev->next = o->next; else head = o->next;
        if (o->next) o->next->prev = o->prev; else tail = o->prev;
        total_qty -= o->qty;
        --count;
    }

    bool empty() const noexcept { return head == nullptr; }
};

// Limit order book with a price-time priority matching engine.
//
// Bids: highest price first. Asks: lowest price first.
// An incoming order matches against the opposite side while prices cross,
// best price first, oldest order first within a price. Trades execute at the
// resting order's price. Any limit-order remainder rests on the book; any
// market-order remainder is discarded (immediate-or-cancel).
class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1 << 16) {
        pool_.reserve(expected_orders);
        index_.reserve(expected_orders);
        trades_.reserve(256);
    }

    Result add_limit(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0 || price <= 0) return {Status::RejectedInvalid, 0, 0};
        if (index_.count(id)) return {Status::RejectedDuplicateId, 0, 0};

        const Qty left = side == Side::Buy
            ? match(asks_, id, side, qty, [price](Price p) { return p <= price; })
            : match(bids_, id, side, qty, [price](Price p) { return p >= price; });

        const Qty filled = qty - left;
        if (left > 0) {
            Order* o = pool_.acquire(Order{id, side, price, left});
            if (side == Side::Buy) bids_[price].push_back(o);
            else asks_[price].push_back(o);
            index_.emplace(id, o);
        }
        const Status st = filled == 0 ? Status::Accepted
                        : left == 0   ? Status::Filled
                                      : Status::PartiallyFilled;
        return {st, filled, left};
    }

    Result add_market(OrderId id, Side side, Qty qty) {
        if (qty == 0) return {Status::RejectedInvalid, 0, 0};
        if (index_.count(id)) return {Status::RejectedDuplicateId, 0, 0};

        auto any = [](Price) { return true; };
        const Qty left = side == Side::Buy ? match(asks_, id, side, qty, any)
                                           : match(bids_, id, side, qty, any);
        const Qty filled = qty - left;
        const Status st = left == 0 ? Status::Filled
                        : filled == 0 ? Status::Cancelled // nothing to trade against
                                      : Status::PartiallyFilled;
        return {st, filled, 0};
    }

    Result cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return {Status::RejectedUnknownId, 0, 0};
        Order* o = it->second;
        const Qty left = o->qty;
        if (o->side == Side::Buy) remove_resting(bids_, o);
        else remove_resting(asks_, o);
        index_.erase(it);
        pool_.release(o);
        return {Status::Cancelled, 0, left};
    }

    // Trades produced since the last clear_trades(). The caller owns draining.
    const std::vector<Trade>& trades() const noexcept { return trades_; }
    void clear_trades() noexcept { trades_.clear(); }

    // Top-of-book helpers. Return 0 if that side is empty.
    Price best_bid() const noexcept { return bids_.empty() ? 0 : bids_.begin()->first; }
    Price best_ask() const noexcept { return asks_.empty() ? 0 : asks_.begin()->first; }

    std::vector<LevelInfo> depth(Side side, std::size_t levels) const {
        std::vector<LevelInfo> out;
        out.reserve(levels);
        auto collect = [&](const auto& book) {
            for (const auto& [px, lvl] : book) {
                if (out.size() == levels) break;
                out.push_back({px, lvl.total_qty, lvl.count});
            }
        };
        if (side == Side::Buy) collect(bids_); else collect(asks_);
        return out;
    }

    std::size_t order_count() const noexcept { return index_.size(); }
    std::size_t level_count(Side side) const noexcept {
        return side == Side::Buy ? bids_.size() : asks_.size();
    }
    bool contains(OrderId id) const { return index_.count(id) != 0; }

private:
    using Bids = std::map<Price, PriceLevel, std::greater<Price>>;
    using Asks = std::map<Price, PriceLevel, std::less<Price>>;

    // Walks the opposite side from its best price while `crosses(price)` holds.
    // Returns the quantity left unfilled.
    template <typename BookSide, typename Crosses>
    Qty match(BookSide& book, OrderId taker, Side taker_side, Qty qty, Crosses crosses) {
        while (qty > 0 && !book.empty()) {
            auto lvl_it = book.begin();
            if (!crosses(lvl_it->first)) break;
            PriceLevel& lvl = lvl_it->second;

            while (qty > 0 && !lvl.empty()) {
                Order* maker = lvl.head;
                const Qty fill = std::min(qty, maker->qty);
                trades_.push_back({taker, maker->id, maker->price, fill, taker_side});
                qty -= fill;
                maker->qty -= fill;
                lvl.total_qty -= fill;
                if (maker->qty == 0) {
                    lvl.unlink(maker); // qty already 0, so total_qty is unchanged
                    index_.erase(maker->id);
                    pool_.release(maker);
                }
            }
            if (lvl.empty()) book.erase(lvl_it);
        }
        return qty;
    }

    template <typename BookSide>
    void remove_resting(BookSide& book, Order* o) {
        auto lvl_it = book.find(o->price);
        lvl_it->second.unlink(o);
        if (lvl_it->second.empty()) book.erase(lvl_it);
    }

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Order*> index_;
    ObjectPool<Order> pool_;
    std::vector<Trade> trades_;
};

} // namespace exchange

namespace exchange {

struct Command {
    enum class Type : std::uint8_t { Limit, Market, Cancel };
    Type type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

inline Result apply(OrderBook& book, const Command& c) {
    switch (c.type) {
        case Command::Type::Limit: return book.add_limit(c.id, c.side, c.price, c.qty);
        case Command::Type::Market: return book.add_market(c.id, c.side, c.qty);
        case Command::Type::Cancel: return book.cancel(c.id);
    }
    return {Status::RejectedInvalid, 0, 0};
}

// Generates a realistic-looking random order stream: a drifting mid price,
// limit orders clustered around it (some crossing the spread), occasional
// market orders, and cancels of previously submitted orders.
class OrderFlow {
public:
    explicit OrderFlow(std::uint64_t seed = 42, Price start_mid = 10'000)
        : rng_(seed), mid_(start_mid) {}

    Command next() {
        const int roll = pct_(rng_);
        if (roll < 35 && !live_.empty()) return make_cancel();
        if (roll < 45) return make_market();
        return make_limit();
    }

    std::vector<Command> generate(std::size_t n) {
        std::vector<Command> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(next());
        return out;
    }

    Price mid() const noexcept { return mid_; }

private:
    Side random_side() { return coin_(rng_) ? Side::Buy : Side::Sell; }
    Qty random_qty() { return qty_(rng_); }

    Command make_limit() {
        if (drift_(rng_) == 0) mid_ = std::max<Price>(100, mid_ + (coin_(rng_) ? 1 : -1));
        const Side side = random_side();
        // Offset away from mid. Negative offsets cross the spread and trade.
        const Price offset = offset_(rng_);
        const Price price = std::max<Price>(1, side == Side::Buy ? mid_ - offset : mid_ + offset);
        const OrderId id = next_id_++;
        remember(id);
        return {Command::Type::Limit, side, id, price, random_qty()};
    }

    Command make_market() {
        return {Command::Type::Market, random_side(), next_id_++, 0, random_qty()};
    }

    Command make_cancel() {
        std::uniform_int_distribution<std::size_t> pick(0, live_.size() - 1);
        const std::size_t i = pick(rng_);
        const OrderId id = live_[i];
        live_[i] = live_.back();
        live_.pop_back();
        return {Command::Type::Cancel, Side::Buy, id, 0, 0};
    }

    void remember(OrderId id) {
        // Keep a bounded window of cancel candidates. Some will already be
        // filled, which exercises the unknown-id path like a real feed.
        if (live_.size() < 4096) live_.push_back(id);
        else live_[next_id_ % live_.size()] = id;
    }

    std::mt19937_64 rng_;
    std::uniform_int_distribution<int> pct_{0, 99};
    std::uniform_int_distribution<int> coin_{0, 1};
    std::uniform_int_distribution<int> drift_{0, 9};
    std::uniform_int_distribution<Price> offset_{-3, 25};
    std::uniform_int_distribution<Qty> qty_{1, 100};
    Price mid_;
    OrderId next_id_ = 1;
    std::vector<OrderId> live_;
};

} // namespace exchange
