import pytest

import tradematch as tm

BUY, SELL = tm.Side.BUY, tm.Side.SELL


def test_orders_fill_by_price_then_time():
    book = tm.OrderBook()
    book.submit(1, SELL, 100, 10_100)
    book.submit(2, SELL, 100, 10_100)
    book.submit(3, SELL, 100, 10_050)
    result = book.submit(4, BUY, 250, 10_100)
    assert [(trade.sell_order_id, trade.quantity, trade.price) for trade in result.trades] == [
        (3, 100, 10_050),
        (1, 100, 10_100),
        (2, 50, 10_100),
    ]
    assert all(trade.aggressor_side == BUY and not trade.auction for trade in result.trades)
    assert result.filled_quantity == 250 and result.remaining_quantity == 0 and not result.rested
    assert book.get_order(2).remaining_quantity == 50
    assert book.get_order(1) is None
    assert book.last_trade_price == 10_100


def test_a_limit_order_rests_until_it_can_trade():
    book = tm.OrderBook()
    assert book.best_bid is None and book.best_ask is None
    assert book.submit(1, BUY, 100, 9_990).rested
    assert book.submit(2, SELL, 40, 10_010).rested
    assert (book.best_bid, book.best_ask) == (9_990, 10_010)
    assert book.depth() == ([(9_990, 100, 1)], [(10_010, 40, 1)])
    assert book.order_count == 2
    snapshot = book.snapshot()
    assert [(level.price, level.total_quantity, level.order_ids) for level in snapshot.bids] == [(9_990, 100, [1])]


def test_time_in_force():
    book = tm.OrderBook()
    book.submit(1, SELL, 100, 10_000)
    ioc = book.submit(2, BUY, 150, 10_000, time_in_force=tm.TimeInForce.IOC)
    assert ioc.filled_quantity == 100 and ioc.expired and not ioc.rested and book.order_count == 0

    book.submit(3, SELL, 100, 10_000)
    fok = book.submit(4, BUY, 150, 10_000, time_in_force=tm.TimeInForce.FOK)
    assert fok.filled_quantity == 0 and fok.expired and book.get_order(3).remaining_quantity == 100

    day = book.submit(5, BUY, 10, 9_000, time_in_force=tm.TimeInForce.DAY)
    assert day.rested
    book.expire_day_orders()
    assert book.get_order(5) is None and book.get_order(3) is not None


def test_requests_that_break_the_rules_are_rejected():
    book = tm.OrderBook()
    book.submit(1, SELL, 100, 10_000)
    assert book.submit(2, BUY, 0, 10_000).reject_reason == tm.RejectReason.INVALID_QUANTITY
    assert book.submit(1, BUY, 10, 9_000).reject_reason == tm.RejectReason.DUPLICATE_ORDER_ID
    post_only = book.submit(3, BUY, 10, 10_000, post_only=True)
    assert not post_only.accepted and post_only.reject_reason == tm.RejectReason.POST_ONLY_WOULD_CROSS
    assert post_only.message
    cancel = book.cancel(99)
    assert not cancel.cancelled and cancel.reject_reason == tm.RejectReason.UNKNOWN_ORDER
    assert book.cancel(1).cancelled_quantity == 100 and book.order_count == 0


def test_a_smaller_replace_keeps_its_place_and_a_larger_one_loses_it():
    book = tm.OrderBook()
    book.submit(1, SELL, 100, 10_000)
    book.submit(2, SELL, 100, 10_000)
    smaller = book.replace(1, SELL, 60, 10_000)
    assert smaller.replaced and smaller.priority_kept and smaller.previous_remaining_quantity == 100
    assert [order.order_id for order in book.orders_at(SELL, 10_000)] == [1, 2]
    larger = book.replace(1, SELL, 200, 10_000)
    assert larger.replaced and not larger.priority_kept
    assert [order.order_id for order in book.orders_at(SELL, 10_000)] == [2, 1]
    assert book.replace(1, BUY, 10, 10_000).reject_reason == tm.RejectReason.SIDE_CHANGE


def test_an_iceberg_shows_one_tranche_at_a_time():
    book = tm.OrderBook()
    book.submit(1, SELL, 300, 10_000, display_quantity=100)
    book.submit(2, SELL, 100, 10_000)
    assert book.depth() == ([], [(10_000, 200, 2)])
    iceberg = book.get_order(1)
    assert (iceberg.remaining_quantity, iceberg.visible_quantity, iceberg.display_quantity) == (300, 100, 100)
    # The first tranche trades, and the next one joins the back of the queue, behind order 2.
    result = book.submit(3, BUY, 150, 10_000)
    assert [(trade.sell_order_id, trade.quantity) for trade in result.trades] == [(1, 100), (2, 50)]
    assert [order.order_id for order in book.orders_at(SELL, 10_000)] == [2, 1]


def test_a_stop_order_triggers_when_the_market_trades_through_it():
    book = tm.OrderBook()
    book.submit(1, SELL, 100, 10_100)
    stop = book.submit(2, BUY, 50, type=tm.OrderType.STOP, stop_price=10_050)
    assert stop.pending_trigger and book.stop_order_count == 1
    book.submit(3, SELL, 10, 10_060)
    trigger = book.submit(4, BUY, 10, 10_060)
    assert trigger.triggered_stops == [2] and book.stop_order_count == 0
    assert book.get_order(1).remaining_quantity == 50


def test_self_trade_prevention():
    book = tm.OrderBook()
    book.submit(1, SELL, 100, 10_000, owner=7)
    newest = book.submit(2, BUY, 100, 10_000, owner=7, self_trade_prevention=tm.SelfTradePrevention.CANCEL_NEWEST)
    assert newest.self_trade_prevented and not newest.trades and not newest.rested
    assert book.get_order(1).remaining_quantity == 100

    oldest = book.submit(3, BUY, 100, 10_000, owner=7, self_trade_prevention=tm.SelfTradePrevention.CANCEL_OLDEST)
    assert oldest.self_trade_cancels == [1] and not oldest.self_trade_prevented and oldest.rested
    assert book.get_order(1) is None and book.get_order(3).remaining_quantity == 100

    other_owner = book.submit(4, SELL, 60, 10_000, owner=8)
    assert [(trade.buy_order_id, trade.quantity) for trade in other_owner.trades] == [(3, 60)]


def test_a_call_auction_uncrosses_at_one_price():
    book = tm.OrderBook()
    book.begin_auction()
    assert book.phase == tm.TradingPhase.AUCTION
    assert book.submit(1, BUY, 100, 10_500).rested
    assert book.submit(9, SELL, 20, type=tm.OrderType.MARKET).queued_for_auction
    book.submit(2, SELL, 40, 10_000)
    book.submit(3, SELL, 70, 10_300)
    indicative = book.indicative_auction()
    assert (indicative.price, indicative.matched_quantity, indicative.buy_surplus, indicative.sell_surplus) == (
        10_300,
        100,
        0,
        30,
    )
    result = book.uncross()
    assert result.executed and result.price == 10_300 and result.matched_quantity == 100
    # Market orders go first, then limit orders by price.
    assert [(trade.sell_order_id, trade.quantity, trade.auction) for trade in result.trades] == [
        (9, 20, True),
        (2, 40, True),
        (3, 40, True),
    ]
    assert result.expired_market_orders == []
    assert book.phase == tm.TradingPhase.CONTINUOUS and book.depth() == ([], [(10_300, 30, 1)])
    assert not book.uncross().executed and book.indicative_auction() is None


def test_an_order_request_can_be_built_and_reused():
    request = tm.OrderRequest(1, BUY, 100, 10_000, time_in_force=tm.TimeInForce.DAY)
    request.quantity = 50
    book = tm.OrderBook()
    assert book.submit(request).rested and book.get_order(1).remaining_quantity == 50
    request.price = 10_010
    assert book.replace(request).replaced and book.best_bid == 10_010
    assert "BUY" in repr(request)


def test_the_same_requests_build_the_same_book():
    def build():
        book = tm.OrderBook()
        for order_id in range(1, 200):
            side = BUY if order_id % 3 else SELL
            book.submit(order_id, side, 10 * (order_id % 7 + 1), 10_000 + (order_id * 37) % 21 - 10)
        return book

    first, second = build(), build()
    assert first.state_hash() == second.state_hash()
    second.cancel(second.snapshot().bids[0].order_ids[0])
    assert first.state_hash() != second.state_hash()


def test_arguments_are_checked():
    book = tm.OrderBook()
    with pytest.raises(TypeError):
        book.submit(1, "BUY", 100, 10_000)
    with pytest.raises(TypeError):
        book.submit(-1, BUY, 100, 10_000)
