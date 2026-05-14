# Linux Serial UART Interface

Production-ready UART communication for Linux using the `termios` API.
Targets the RISC-V ACT Framework and M-Mode firmware validation workflows,
but works on any Linux platform with a serial port.

## Features

- Configurable baud rate, data bits, parity, and stop bits (`8N1`, `7E2`, etc.)
- Non-blocking I/O with `select()` and a configurable receive timeout
- Hardware flow control (RTS/CTS)
- Statistics tracking — bytes sent/received, errors, throughput
- Hex dump and CRC-16 validation in verbose mode
- File logging with `--log`
- Exclusive device locking via `flock()` — prevents two processes sharing a port
- Signal handling (`SIGINT`/`SIGTERM`) for graceful shutdown and terminal restore
- Retry logic for slow USB-serial enumeration
- Cross-compilation for RISC-V

## Quick Start

```bash
# Build
make

# Basic run
./uart_interface /dev/ttyUSB0

# Full options
./uart_interface -v --log test.log /dev/ttyUSB0 115200 8N1 5 "ACK"

# Loopback test (no hardware needed)
make test-loop

# Verbose run with stats printed on exit
make run-stats DEVICE=/dev/ttyUSB0
```

## Usage

```
uart_interface [-v] [--log FILE] <device> [baud] [format] [timeout] [expected]

  -v / --verbose   Enable hex dumps, CRC-16 output, and modem line status
  --log FILE       Append all output to FILE in addition to stdout
  device           Serial device path (e.g. /dev/ttyUSB0, /dev/ttyS0)
  baud             Baud rate — default 115200
  format           Data/parity/stop bits as e.g. 8N1, 7E2, 8O2 — default 8N1
  timeout          Receive wait in seconds — default 3
  expected         Optional substring to validate in the received response
```

## Build

```bash
# Native
make clean && make

# Cross-compile for RISC-V
make riscv
file uart_interface   # ELF 64-bit LSB executable, UCB RISC-V
```

## Requirements

| Tool | Purpose |
|---|---|
| GCC (or Clang) | Native build |
| `riscv64-linux-gnu-gcc` | RISC-V cross-compilation |
| `socat` | Loopback PTY test (`make test-loop`) |
| `valgrind` | Memory-leak check (`make valgrind`) |

Check which tools are available:

```bash
make check-deps
```

## Makefile Targets

| Target | Description |
|---|---|
| `make` | Build the binary |
| `make run` | Run against `DEVICE`, `BAUD`, `FORMAT` |
| `make run-stats` | Same with `-v` (verbose hex + CRC + stats) |
| `make debug` | Debug build (`-O0`) |
| `make test-loop` | Loopback smoke-test via socat PTY pair |
| `make perf-test` | 100-iteration stress test |
| `make valgrind` | Memory-leak check |
| `make riscv` | Cross-compile for RISC-V |
| `make check-deps` | Warn about missing optional tools |
| `make clean` | Remove build artefacts |

Override defaults on the command line:

```bash
make run DEVICE=/dev/ttyS0 BAUD=9600 FORMAT=7E2
```

## License

MIT
