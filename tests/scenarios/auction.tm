# A call auction collects orders without matching them, then trades everything that crosses at a single price: the
# one that matches the most volume, then leaves the smallest surplus, then follows the side with the surplus.
add ACME
auction begin
buy 100 @ 100.30
buy 50 @ 100.10
sell 80 @ 99.90
sell 60 @ 100.20

# The book may cross during the call. The indicative price shows what an uncross would do right now.
book
auction show

# Market orders take part with the highest priority.
buy 40 market
auction show
auction uncross
book
trades
