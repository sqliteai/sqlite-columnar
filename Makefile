SQLITE3 ?= sqlite3
CC ?= cc
PKG_CONFIG ?= pkg-config
SQLITE_SRC ?= sqlite
SQLITE_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sqlite3 2>/dev/null)
SQLITE_AMALGAMATION ?= $(SQLITE_SRC)/sqlite3.c
BUILD_DIR ?= build
VARIANCE_REPEATS ?= 9
VARIANCE_DATASETS ?= small:10000:64 medium:50000:128 wide:50000:512

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SHLIB_EXT := dylib
  SHLIB_FLAGS := -dynamiclib -undefined dynamic_lookup
else
  SHLIB_EXT := so
  SHLIB_FLAGS := -shared
endif

CFLAGS ?= -O2 -Wall -Wextra
LDLIBS ?= -ldl -lpthread -lz
ifneq ($(wildcard $(SQLITE_SRC)/sqlite3ext.h),)
CPPFLAGS += -I$(SQLITE_SRC) -I$(SQLITE_SRC)/src
else
CPPFLAGS += $(SQLITE_CFLAGS)
endif

EXTENSION := columnar.$(SHLIB_EXT)
ROBUSTNESS_TEST := $(BUILD_DIR)/columnar-robustness-test
VARIANCE_BENCH := $(BUILD_DIR)/columnar-variance-bench
BENCHMARKS := \
	$(BUILD_DIR)/columnar-bench \
	$(BUILD_DIR)/columnar-agg-bench \
	$(BUILD_DIR)/columnar-analytics-bench \
	$(VARIANCE_BENCH)

.PHONY: all test robustness-test benchmarks bench variance-bench smoke-bench clean

all: $(EXTENSION)

$(EXTENSION): columnar.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC $(SHLIB_FLAGS) $< -o $@

test: $(EXTENSION) $(ROBUSTNESS_TEST)
	$(SQLITE3) :memory: ".read test/smoke.sql"
	$(ROBUSTNESS_TEST) ./columnar

robustness-test: $(EXTENSION) $(ROBUSTNESS_TEST)
	$(ROBUSTNESS_TEST) ./columnar

benchmarks: $(BENCHMARKS)

bench: benchmarks

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/columnar-bench: test/columnar-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-agg-bench: test/columnar-agg-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-analytics-bench: test/columnar-analytics-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-robustness-test: test/columnar-robustness-test.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-variance-bench: test/columnar-variance-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

variance-bench: all $(VARIANCE_BENCH)
	$(VARIANCE_BENCH) ./columnar $(VARIANCE_REPEATS) $(VARIANCE_DATASETS)

smoke-bench: all benchmarks
	$(BUILD_DIR)/columnar-bench ./columnar 1000 64
	$(BUILD_DIR)/columnar-agg-bench ./columnar 1000
	$(BUILD_DIR)/columnar-analytics-bench ./columnar 1000 64

clean:
	rm -f columnar.so columnar.dylib
	rm -rf $(BUILD_DIR)
