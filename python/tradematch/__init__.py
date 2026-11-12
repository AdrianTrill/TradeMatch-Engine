"""TradeMatch Engine from Python.

A price-time-priority order matching engine written in C++20, with its agent-based market simulator and NASDAQ
TotalView-ITCH 5.0 replay. Prices are integer ticks: cents in the simulator, and 1/10,000 of a dollar in ITCH.

    >>> import tradematch as tm
    >>> book = tm.OrderBook()
    >>> book.submit(1, tm.Side.SELL, 100, 10_050).rested
    True
    >>> [(trade.quantity, trade.price) for trade in book.submit(2, tm.Side.BUY, 60, 10_050).trades]
    [(60, 10050)]
"""

import os
import re

from ._tradematch import (
    AuctionIndicative,
    AuctionResult,
    BookSnapshot,
    CancelResult,
    MarketSimulator,
    OrderBook,
    OrderRequest,
    OrderType,
    PriceLevel,
    RejectReason,
    ReplaceResult,
    RestingOrder,
    SelfTradePrevention,
    Side,
    SubmitResult,
    TimeInForce,
    Trade,
    TradingPhase,
)
from ._tradematch import ItchReplica as _ItchReplica

__version__ = "1.0.0"

__all__ = [
    "AuctionIndicative",
    "AuctionResult",
    "BookSnapshot",
    "CancelResult",
    "ItchReplica",
    "MarketSimulator",
    "OrderBook",
    "OrderRequest",
    "OrderType",
    "PriceLevel",
    "RejectReason",
    "ReplaceResult",
    "RestingOrder",
    "SelfTradePrevention",
    "Side",
    "SubmitResult",
    "TimeInForce",
    "Trade",
    "TradingPhase",
    "time_of_day",
]


_TIME_OF_DAY = re.compile(r"([0-9]{1,2}):([0-9]{2})(?::([0-9]{2})(?:\.([0-9]{1,9}))?)?")


def time_of_day(text: str) -> int:
    """Nanoseconds since midnight for a time such as "10:00", "10:00:30", or "10:00:30.25".

    ITCH timestamps count from midnight. ItchReplica.advance() takes either form.
    """
    match = _TIME_OF_DAY.fullmatch(text)
    if not match or int(match[1]) > 23 or int(match[2]) > 59 or int(match[3] or 0) > 59:
        raise ValueError(f"expected a time of day such as 10:00, 10:00:30, or 10:00:30.25, not {text!r}")
    hours, minutes, seconds = int(match[1]), int(match[2]), int(match[3] or 0)
    nanoseconds = int((match[4] or "").ljust(9, "0"))
    return ((hours * 60 + minutes) * 60 + seconds) * 1_000_000_000 + nanoseconds


class ItchReplica(_ItchReplica):
    __doc__ = _ItchReplica.__doc__

    def __init__(self, path, watch=()):
        """Opens an ITCH 5.0 file, given as a str or os.PathLike, and records the trades of the stocks in `watch`."""
        super().__init__(os.fspath(path), list(watch))

    def advance(self, until=None):
        """Applies the file's messages up to the first one stamped after `until`, or to the end of the file.

        `until` is nanoseconds since midnight or a time of day such as "10:00". Returns how many messages it applied.
        """
        return super().advance(time_of_day(until) if isinstance(until, str) else until)
