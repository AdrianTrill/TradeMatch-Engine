# Stop orders wait until the last trade reaches their stop price, then become market orders, or limit orders for
# stop-limits.
add ACME
sell 100 @ 101.00
sell 100 @ 102.00
buy 100 @ 99.00
stop buy 50 @ 101.00
stop sell 30 @ 99.50 limit 99.00
book

# A trade at 101.00 triggers the buy stop. It buys 50 at market: 40 more at 101.00, then 10 at 102.00.
buy 60 @ 101.00
book

# A trade at 99.00 triggers the sell stop-limit, which sells 30 at 99.00 or better.
sell 10 @ 99.00
book
