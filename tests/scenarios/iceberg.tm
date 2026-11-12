# Icebergs show one tranche at a time; the rest stays hidden. Each new tranche joins the back of the queue, so the
# hidden part never jumps ahead of orders that were displayed first.
add ACME
sell 300 @ 100.00 display=100
sell 50 @ 100.00
book
order 1

# The buy fills the visible tranche of order 1. Its next tranche goes behind order 2, which fills next.
buy 120 @ 100.00
book
order 1

# Order 2's last 30, then order 1's last two tranches. The unfilled 20 rests.
buy 250 @ 100.00
book
