CC ?= gcc
CFLAGS ?= -std=c99 -O3 -Wall -Wextra -pedantic
LDFLAGS ?= -pthread -lm

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests
BENCH_DIR = benchmarks

TARGET = $(BUILD_DIR)/swim-vector-cluster

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all build test clean benchmark directories

all: directories build

directories:
	@mkdir -p $(BUILD_DIR)

build: directories $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: build
	@if [ -d "$(TEST_DIR)" ] && [ -n "$$($(MAKE) --version 2>/dev/null)" ]; then \
		echo "Running tests..."; \
		$(MAKE) -C $(TEST_DIR) run || { echo "Tests failed"; exit 1; }; \
	else \
		echo "No test directory found or tests not configured. Skipping."; \
	fi

benchmark: build
	@if [ -d "$(BENCH_DIR)" ]; then \
		echo "Running benchmarks..."; \
		$(MAKE) -C $(BENCH_DIR) run || { echo "Benchmarks failed"; exit 1; }; \
	else \
		echo "No benchmark directory found. Skipping."; \
	fi

clean:
	@rm -rf $(BUILD_DIR)
	@if [ -d "$(TEST_DIR)" ]; then $(MAKE) -C $(TEST_DIR) clean 2>/dev/null || true; fi
	@if [ -d "$(BENCH_DIR)" ]; then $(MAKE) -C $(BENCH_DIR) clean 2>/dev/null || true; fi
	@echo "Cleaned build artifacts."