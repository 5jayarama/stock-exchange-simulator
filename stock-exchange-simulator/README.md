# Stock Exchange Simulation

A simulated stock exchange written in C++. Users can send orders to buy or sell a stock, and the program pairs up buyers and sellers whose prices match, just like a real exchange. For example, if person A wants to sell 100 shares at $100 and person B wants to buy 50 shares at $100, the program makes the trade: 50 shares change hands at $100, and the other 50 wait for the next buyer. You can try it in your browser. It is a simulation, so fake traders will populate the exhange, but you can contribute as well with your own orders and purchases.

**Live demo:** `enter here`

## What it does

- **Limit orders** rest on the book until they are matched or cancelled.
- **Market orders** fill immediately at the best available prices. Any unfilled remainder is discarded.
- **Cancels** remove a resting order in O(1) once it is found.
- **Price-time priority.** The best price fills first, and at the same price the oldest order fills first. Trades execute at the resting order's price.
- **Validation** rejects zero quantities, non-positive prices, duplicate order IDs, and orders by unknown IDs.

## Performance

This is a single threaded pipeline. It is efficient for a single threaded system, 
due to decent memory management.

## Files

exchange.hpp   Engine
main.cpp       CLI, benchmarks, tests
index.html     Browser demo
.github/workflows/ci.yml  For github pages


## Build and run

GCC 11+ or Clang 14+ needed

```bash
g++ -std=c++20 -O3 -pthread main.cpp -o exchange
./exchange test          # unit tests
./exchange bench         # benchmarks, about 10 seconds
./exchange orders.txt    # replay an order file (or pipe orders in)
```

Example:

```bash
printf 'LIMIT SELL 1 100.10 50\nLIMIT BUY 2 100.10 20\nBOOK\n' | ./exchange
```

Tests

```bash
g++ -std=c++20 -g -fsanitize=address,undefined -pthread main.cpp -o exchange-asan && ./exchange-asan test
```

### Order file format

```
LIMIT  BUY|SELL <id> <price> <qty>    LIMIT BUY 1 100.25 50
MARKET BUY|SELL <id> <qty>            MARKET SELL 2 30
CANCEL <id>                           CANCEL 1
BOOK                                  print top 5 levels
```