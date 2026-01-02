# Polymarket C++ Client

[![build](https://github.com/SebastianBoehler/polymarket-cpp-client/actions/workflows/build.yml/badge.svg)](https://github.com/SebastianBoehler/polymarket-cpp-client/actions/workflows/build.yml)

Reusable C++20 client for Polymarket: REST, WebSocket streaming, and order signing (EIP-712) with examples and tests.

## Features

- **REST**: market discovery, orderbook/price queries, auth key management.
- **WebSocket**: orderbook streaming via IXWebSocket.
- **Signing**: EIP-712 order signing (secp256k1, keccak).
- **Examples**: REST (`rest_example`), signing (`sign_example`), WebSocket (`ws_example`).
- **Tests**: small utility test (`test_utils`) plus runnable examples.

## Requirements

- CMake 3.16+
- C++20 compiler
- libcurl, OpenSSL

## Build & install

```bash
cmake -S . -B build -DPOLYMARKET_CLIENT_BUILD_EXAMPLES=ON -DPOLYMARKET_CLIENT_BUILD_TESTS=ON
cmake --build build --parallel
# optional tests
ctest --test-dir build
# install (into system or a prefix you configure)
cmake --install build --prefix <install_prefix>
```

Consumers can use `polymarket::client` from the installed package config (`polymarket_clientTargets.cmake`).

## Examples

- `rest_example`: fetch markets from CLOB REST
- `sign_example`: sign a dummy order (requires `PRIVATE_KEY`)
- `ws_example`: connect to Polymarket WS and subscribe to orderbook agg

Build them with `POLYMARKET_CLIENT_BUILD_EXAMPLES=ON` and run from `build/`.

## Tests

`test_utils` exercises basic utility helpers. Run via `ctest --test-dir build`.

## Key components

- `include/` headers for client API
- `src/http_client.cpp`: libcurl HTTP client
- `src/websocket_client.cpp`: IXWebSocket wrapper
- `src/order_signer.cpp`: EIP-712 signing (secp256k1, keccak)
- `src/clob_client.cpp`: REST + trading endpoints
- `src/orderbook.cpp`: WS orderbook management

## GitHub Actions

The repo ships with `.github/workflows/build.yml` to configure and build on macOS.

## License

MIT
