#ifndef TRADEMATCH_BENCH_JOURNALED_BOOK_HPP
#define TRADEMATCH_BENCH_JOURNALED_BOOK_HPP

#include "tradematch/journal.hpp"
#include "tradematch/order_book.hpp"

#include <cstddef>
#include <filesystem>

namespace tradematch::bench {

// The v2 order book behind a write-ahead journal: every request is appended to the journal before the book applies
// it, as JournaledEngine does. The journal describes a single symbol, so tradematch_replay can rebuild the final book.
class JournaledOrderBook {
public:
    JournaledOrderBook(const BookCapacity& capacity, const std::filesystem::path& path, Durability durability)
        : book_(capacity), journal_(path, JournalOptions{durability, true, 1}) {
        journal_.append_add_symbol(0, "BENCH", capacity);
    }

    SubmitResult submit(const OrderRequest& order) {
        journal_.append_submit(0, 0, order);
        return book_.submit(order);
    }

    CancelResult cancel(OrderId order_id) {
        journal_.append_cancel(0, 0, order_id);
        return book_.cancel(order_id);
    }

    ReplaceResult replace(const OrderRequest& replacement) {
        journal_.append_replace(0, 0, replacement);
        return book_.replace(replacement);
    }

    std::size_t order_count() const noexcept {
        return book_.order_count();
    }

private:
    OrderBook book_;
    JournalWriter journal_;
};

}  // namespace tradematch::bench

#endif
