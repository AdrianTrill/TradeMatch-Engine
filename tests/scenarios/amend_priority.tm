# Replacing an order. A smaller size at the same price is amended in place and keeps the order's place in the
# queue. A larger size or a new price re-enters the order at the back.
add ACME
buy 100 @ 100.00
buy 100 @ 100.00

# Order 1 shrinks to 60 and stays first in line, so the next sell trades with it.
replace 1 60 @ 100.00
sell 60 @ 100.00

# Order 2 grows to 150, which sends it behind order 4.
buy 100 @ 100.00
replace 2 150 @ 100.00
order 2
sell 100 @ 100.00
book

# A new price also resets priority. Replacing an order that isn't there is rejected.
replace 2 150 @ 100.25
book
replace 9 10 @ 100.00
