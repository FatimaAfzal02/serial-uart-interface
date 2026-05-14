SHELL  := /bin/bash
.DEFAULT_GOAL := all

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -O2 -g
LDFLAGS := -lrt
TARGET  := uart_interface
SRC     := uart_interface.c

DEVICE  ?= /dev/ttyUSB0
BAUD    ?= 115200
FORMAT  ?= 8N1

.PHONY: all clean run run-stats debug check-deps test-loop perf-test valgrind riscv help

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Build OK -> ./$(TARGET)"

clean:
	rm -f $(TARGET)

check-deps:
	@command -v socat    >/dev/null 2>&1 || echo "[WARN] socat not installed    (needed for test-loop)"
	@command -v valgrind >/dev/null 2>&1 || echo "[WARN] valgrind not installed (needed for valgrind target)"
	@command -v riscv64-linux-gnu-gcc >/dev/null 2>&1 || echo "[WARN] riscv64-linux-gnu-gcc not installed (needed for riscv target)"

run: $(TARGET)
	./$(TARGET) $(DEVICE) $(BAUD) $(FORMAT)

run-stats: $(TARGET)
	./$(TARGET) -v $(DEVICE) $(BAUD) $(FORMAT)

debug: CFLAGS += -DDEBUG -O0
debug: clean $(TARGET)

test-loop: $(TARGET)
	@command -v socat >/dev/null 2>&1 || \
		{ echo "[ERROR] socat not found. Install: sudo apt install socat"; exit 1; }
	@echo "[TEST] Creating PTY pair with socat..."
	@socat -d -d pty,raw,echo=0 pty,raw,echo=0 2>/tmp/socat_pts_$$.log & \
	 SOCAT_PID=$$!; \
	 sleep 1; \
	 PTY0=$$(grep -o '/dev/pts/[0-9]*' /tmp/socat_pts_$$.log | sed -n '1p'); \
	 PTY1=$$(grep -o '/dev/pts/[0-9]*' /tmp/socat_pts_$$.log | sed -n '2p'); \
	 if [ -z "$$PTY0" ] || [ -z "$$PTY1" ]; then \
	   echo "[ERROR] Could not read PTY paths from socat output"; \
	   cat /tmp/socat_pts_$$.log; \
	   kill $$SOCAT_PID 2>/dev/null; \
	   rm -f /tmp/socat_pts_$$.log; \
	   exit 1; \
	 fi; \
	 echo "[TEST] PTY pair: $$PTY0 <-> $$PTY1"; \
	 cat $$PTY1 & CAT_PID=$$!; \
	 sleep 0.3; \
	 ./$(TARGET) $$PTY0 $(BAUD) $(FORMAT); \
	 STATUS=$$?; \
	 kill $$CAT_PID 2>/dev/null; \
	 kill $$SOCAT_PID 2>/dev/null; \
	 rm -f /tmp/socat_pts_$$.log; \
	 exit $$STATUS

perf-test: $(TARGET)
	@echo "Running performance test (100 iterations)..."
	@for i in $$(seq 1 100); do \
	    printf "Iteration %d/100\r" $$i; \
	    ./$(TARGET) $(DEVICE) $(BAUD) $(FORMAT) >/dev/null 2>&1 || \
	        { echo "\n[FAIL] Failed at iteration $$i"; exit 1; }; \
	done
	@echo "\nAll 100 iterations passed!"

valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all \
	         --track-origins=yes \
	         ./$(TARGET) $(DEVICE) $(BAUD) $(FORMAT)

riscv: CC = riscv64-linux-gnu-gcc
riscv: CFLAGS += -static
riscv: $(TARGET)

help:
	@echo "Targets:"
	@echo "  make               Build ./$(TARGET)"
	@echo "  make check-deps    Check for optional tools (socat, valgrind, riscv gcc)"
	@echo "  make run           Run: DEVICE=$(DEVICE) BAUD=$(BAUD) FORMAT=$(FORMAT)"
	@echo "  make run-stats     Same with -v (verbose hex + CRC)"
	@echo "  make debug         Debug build (-O0)"
	@echo "  make test-loop     Loopback smoke-test via socat PTY pair"
	@echo "  make perf-test     100-iteration stress test"
	@echo "  make valgrind      Memory-leak check"
	@echo "  make riscv         Cross-compile for RISC-V"
	@echo "  make clean         Remove build artefacts"
	@echo ""
	@echo "  Override: make run DEVICE=/dev/ttyS0 BAUD=9600 FORMAT=7E2"
