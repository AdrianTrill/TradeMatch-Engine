# Market orders, time in force, and post-only orders.
add ACME
sell 50 @ 101.00
sell 50 @ 101.50

# A market order sweeps the book. Whatever it can't fill is cancelled rather than left resting.
buy 120 market

# IOC trades what it can at its limit and cancels the rest.
sell 50 @ 102.00
buy 80 @ 102.00 ioc

# FOK is all or nothing: 40 is more than the 30 offered, so nothing trades. 30 fills in full.
sell 30 @ 102.50
buy 40 @ 102.50 fok
buy 30 @ 102.50 fok

# Post-only orders must add liquidity: one that would trade is rejected instead.
sell 10 @ 103.00
buy 10 @ 103.00 post-only
buy 10 @ 102.75 post-only

# DAY orders expire at the end of the day; GTC orders stay.
sell 20 @ 104.00 day
expire
book
