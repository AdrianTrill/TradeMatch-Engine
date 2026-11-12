# Self-trade prevention. When an incoming order would trade with a resting order from the same owner, the incoming
# order's stp mode decides what happens.
add ACME
sell 50 @ 100.00 owner=7
sell 50 @ 100.10 owner=8

# newest: the incoming order stops matching, and the rest of it is cancelled.
buy 80 @ 100.10 owner=7 stp=newest

# oldest: the resting orders are cancelled, and matching continues past them.
sell 50 @ 100.00 owner=7
buy 80 @ 100.10 owner=7 stp=oldest
book

# both: the resting order and the rest of the incoming order are both cancelled.
sell 20 @ 100.20 owner=7
buy 30 @ 100.20 owner=7 stp=both

# none, the default: the owner trades with itself.
sell 10 @ 100.10 owner=7
book
