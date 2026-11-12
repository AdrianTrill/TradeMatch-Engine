import struct

import pytest

import tradematch as tm

# A hand-encoded ITCH 5.0 file, written from the specification's byte layouts independently of the C++ encoder.
# Prices are in 1/10,000 of a dollar and times in nanoseconds since midnight.


def frame(body):
    return struct.pack(">H", len(body)) + body


def header(kind, locate, time):
    return kind.encode() + struct.pack(">HH", locate, 0) + time.to_bytes(6, "big")


def stock(name):
    return name.encode().ljust(8)


def system_event(time, code):
    return frame(header("S", 0, time) + code.encode())


def directory(locate, time, name):
    body = header("R", locate, time) + stock(name) + b"QN" + struct.pack(">I", 100) + b"NCZ PNN1N"
    return frame(body + struct.pack(">I", 0) + b"N")


def add(locate, time, reference, side, shares, name, price):
    return frame(header("A", locate, time) + struct.pack(">QcI8sI", reference, side.encode(), shares, stock(name), price))


def executed(locate, time, reference, shares, match):
    return frame(header("E", locate, time) + struct.pack(">QIQ", reference, shares, match))


def executed_at(locate, time, reference, shares, match, printable, price):
    return frame(header("C", locate, time) + struct.pack(">QIQcI", reference, shares, match, printable.encode(), price))


def cancel(locate, time, reference, shares):
    return frame(header("X", locate, time) + struct.pack(">QI", reference, shares))


def delete(locate, time, reference):
    return frame(header("D", locate, time) + struct.pack(">Q", reference))


def replace(locate, time, original, new, shares, price):
    return frame(header("U", locate, time) + struct.pack(">QQII", original, new, shares, price))


def hidden_trade(locate, time, shares, name, price, match):
    return frame(header("P", locate, time) + struct.pack(">QcI8sIQ", 0, b"B", shares, stock(name), price, match))


def cross(locate, time, shares, name, price, match):
    return frame(header("Q", locate, time) + struct.pack(">Q8sIQc", shares, stock(name), price, match, b"O"))


def at(text):
    return tm.time_of_day(text)


@pytest.fixture
def feed(tmp_path):
    messages = [
        system_event(at("4:00"), "O"),
        directory(1, at("4:00"), "ACME"),
        directory(2, at("4:00"), "WIDGET"),
        add(1, at("9:30"), 1, "B", 100, "ACME", 1_000_000),
        add(1, at("9:30"), 2, "S", 200, "ACME", 1_000_100),
        executed(1, at("9:30:01"), 2, 50, 1),  # a buyer takes 50 of the offer
        executed_at(1, at("9:30:02"), 1, 30, 2, "Y", 999_900),  # a seller takes 30 of the bid
        executed_at(1, at("9:30:02.5"), 1, 5, 3, "N", 999_900),  # printed by a cross instead
        hidden_trade(1, at("9:30:03"), 25, "ACME", 1_000_050, 4),
        cancel(1, at("9:30:04"), 1, 20),
        replace(1, at("9:30:05"), 2, 3, 100, 1_000_200),
        cross(1, at("10:00"), 1_000, "ACME", 1_000_000, 5),
        add(2, at("10:00:01"), 4, "B", 10, "WIDGET", 500_000),
        executed(2, at("10:00:02"), 4, 10, 6),
        delete(1, at("16:00"), 1),
        system_event(at("20:00"), "C"),
    ]
    path = tmp_path / "day.itch"
    path.write_bytes(b"".join(messages))
    return path, len(messages)


def test_time_of_day():
    assert tm.time_of_day("9:30") == (9 * 3600 + 30 * 60) * 10**9
    assert tm.time_of_day("10:00:30.25") == 36_030_250_000_000
    assert tm.time_of_day("23:59:59.999999999") == 86_400 * 10**9 - 1
    assert tm.time_of_day("09:30:00.5") == tm.time_of_day("9:30") + 500_000_000
    for text in ["10", "24:00", "10:60", "10:5", "10:00:60", "10:00:00.1234567890", "10:00:30.", "1_0:00", " 9:30", ""]:
        with pytest.raises(ValueError):
            tm.time_of_day(text)


def test_a_replay_advances_through_the_day(feed):
    path, count = feed
    replica = tm.ItchReplica(path, watch=["ACME"])
    assert not replica.finished and replica.symbols == []

    assert replica.advance("9:30:02") == 7
    assert replica.top_of_book("ACME") == (1_000_000, 70, 1_000_100, 150)
    assert replica.take_trades() == [
        (at("9:30:01"), "ACME", 1_000_100, 50, "B"),
        (at("9:30:02"), "ACME", 999_900, 30, "S"),
    ]
    assert replica.advance("9:30:02") == 0, "the next message is later, so it waits"

    assert replica.advance(at("10:00")) == 5
    assert replica.top_of_book("ACME") == (1_000_000, 45, 1_000_200, 100)
    assert replica.depth("ACME") == ([(1_000_000, 45, 1)], [(1_000_200, 100, 1)])
    assert replica.take_trades() == [
        (at("9:30:03"), "ACME", 1_000_050, 25, "?"),
        (at("10:00"), "ACME", 1_000_000, 1_000, "X"),
    ]

    assert replica.advance() == count - 12 and replica.finished
    assert replica.advance() == 0
    assert replica.top_of_book("ACME") == (None, 0, 1_000_200, 100)
    assert replica.take_trades() == [], "WIDGET isn't watched"
    assert replica.symbols == ["ACME", "WIDGET"]
    # The non-printable execution's 5 shares are left to the cross.
    assert replica.stock_stats("ACME") == {
        "volume": 50 + 30 + 25 + 1_000,
        "value": pytest.approx(50 * 100.01 + 30 * 99.99 + 25 * 100.005 + 1_000 * 100.0),
        "executions": 4,
        "last_price": 1_000_000,
    }
    assert replica.stock_stats("WIDGET")["volume"] == 10

    stats = replica.stats
    assert stats["messages"] == count and stats["invalid_messages"] == 0 and stats["anomalies"] == 0
    assert stats["by_type"] == {"S": 2, "R": 2, "A": 3, "E": 2, "C": 2, "P": 1, "X": 1, "U": 1, "Q": 1, "D": 1}
    assert stats["live_orders"] == 1 and stats["peak_orders"] == 3 and stats["last_timestamp"] == at("20:00")


def test_unknown_stocks_and_files(feed, tmp_path):
    replica = tm.ItchReplica(str(feed[0]))
    with pytest.raises(KeyError):
        replica.top_of_book("ACME")
    with pytest.raises(FileNotFoundError):
        tm.ItchReplica(tmp_path / "missing.itch")


def test_a_truncated_file_is_an_error(feed, tmp_path):
    truncated = tmp_path / "truncated.itch"
    truncated.write_bytes(feed[0].read_bytes()[:-3])
    replica = tm.ItchReplica(truncated)
    with pytest.raises(RuntimeError):
        replica.advance()
