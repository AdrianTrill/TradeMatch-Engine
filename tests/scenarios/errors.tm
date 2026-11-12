# The engine validates every request, and the command line explains what it can't parse. Nothing changes either way.
buy 10 @ 100.00
add ACME

# Rejected by the engine.
buy 0 @ 100.00
buy 10 @ 0
sell 10 @ 100.00 display=20
buy 10 market post-only
cancel 42
buy 10 @ 100.00 id=5
buy 10 @ 100.00 id=5

# Rejected by the command line.
buy 10 @ 100.001
stop buy 10
frobnicate
buy 10 @ 100.00 color=red
book GLOBEX
book
