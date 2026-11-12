# Price-time priority: better prices trade first, and at the same price, earlier orders trade first.
add ACME
buy 100 @ 100.00
buy 50 @ 100.00
buy 70 @ 100.25
book

# A sell for 200 takes the best bid at 100.25 first, then the two bids at 100.00 in the order they arrived.
# Every trade happens at the resting order's price.
sell 200 @ 99.50
book
trades
