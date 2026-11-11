#ifndef TRADEMATCH_BOOK_LISTENER_HPP
#define TRADEMATCH_BOOK_LISTENER_HPP

#include "tradematch/types.hpp"

#include <cstdint>

namespace tradematch {

// The displayed state of one price level after a request changed it. A quantity of 0 means the level is gone.
struct LevelUpdate {
    Side side{Side::Buy};
    Price price{};
    Volume quantity{};  // displayed quantity, matching the order book's depth (iceberg reserves stay hidden)
    std::uint32_t order_count{};

    bool operator==(const LevelUpdate&) const = default;
};

// Receives market data from one order book. After each submit, cancel, replace, or end-of-day expiry, the book emits
// every trade the request caused, in execution order, followed by one update per price level the request changed,
// carrying that level's final state. Consumers can therefore maintain an exact replica of the book's depth from the
// updates alone. With no listener attached, the book skips all of this bookkeeping.
class BookListener {
public:
    BookListener() = default;
    BookListener(const BookListener&) = default;
    BookListener& operator=(const BookListener&) = default;
    BookListener(BookListener&&) = default;
    BookListener& operator=(BookListener&&) = default;
    virtual ~BookListener() = default;

    virtual void on_trade(const Trade& trade) = 0;
    virtual void on_level_update(const LevelUpdate& update) = 0;
};

}  // namespace tradematch

#endif
