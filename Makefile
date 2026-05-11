SQLITE3 ?= sqlite3
CC ?= cc
PKG_CONFIG ?= pkg-config
SQLITE_SRC ?= sqlite
SQLITE_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags sqlite3 2>/dev/null)
SQLITE_AMALGAMATION ?= $(SQLITE_SRC)/sqlite3.c
BUILD_DIR ?= build
DIST_DIR ?= dist
VARIANCE_REPEATS ?= 9
VARIANCE_DATASETS ?= small:10000:64 medium:50000:128 wide:50000:512

ifeq ($(OS),Windows_NT)
  SHLIB_EXT := dll
  SHLIB_FLAGS := -shared
  PIC_FLAGS :=
  EXE_EXT := .exe
  LDLIBS ?=
else
  UNAME_S := $(shell uname -s)
  PIC_FLAGS := -fPIC
  EXE_EXT :=
  LDLIBS ?= -ldl -lpthread
ifeq ($(UNAME_S),Darwin)
  SHLIB_EXT := dylib
  SHLIB_FLAGS := -dynamiclib -undefined dynamic_lookup
  ifneq ($(ARCH),)
    ARCH_FLAGS := $(foreach A,$(ARCH),-arch $(A))
  endif
else
  SHLIB_EXT := so
  SHLIB_FLAGS := -shared
endif
endif

CFLAGS ?= -O2 -Wall -Wextra
ifneq ($(wildcard $(SQLITE_SRC)/sqlite3ext.h),)
CPPFLAGS += -I$(SQLITE_SRC) -I$(SQLITE_SRC)/src
else
CPPFLAGS += $(SQLITE_CFLAGS)
endif

EXTENSION := columnar.$(SHLIB_EXT)
DIST_EXTENSION := $(DIST_DIR)/columnar.$(SHLIB_EXT)
ROBUSTNESS_TEST := $(BUILD_DIR)/columnar-robustness-test$(EXE_EXT)
VARIANCE_BENCH := $(BUILD_DIR)/columnar-variance-bench$(EXE_EXT)
BENCHMARKS := \
	$(BUILD_DIR)/columnar-bench$(EXE_EXT) \
	$(BUILD_DIR)/columnar-agg-bench$(EXE_EXT) \
	$(BUILD_DIR)/columnar-analytics-bench$(EXE_EXT) \
	$(VARIANCE_BENCH)

.PHONY: all extension test robustness-test benchmarks bench variance-bench smoke-bench clean

all: $(EXTENSION)

extension: $(DIST_EXTENSION)

$(EXTENSION): columnar.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ARCH_FLAGS) $(PIC_FLAGS) $(SHLIB_FLAGS) $< -o $@

$(DIST_EXTENSION): columnar.c | $(DIST_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(ARCH_FLAGS) $(PIC_FLAGS) $(SHLIB_FLAGS) $< -o $@

test: $(EXTENSION) $(ROBUSTNESS_TEST)
	$(SQLITE3) :memory: ".read test/smoke.sql"
	$(ROBUSTNESS_TEST) ./columnar

robustness-test: $(EXTENSION) $(ROBUSTNESS_TEST)
	$(ROBUSTNESS_TEST) ./columnar

benchmarks: $(BENCHMARKS)

bench: benchmarks

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(BUILD_DIR)/columnar-bench$(EXE_EXT): test/columnar-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-agg-bench$(EXE_EXT): test/columnar-agg-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-analytics-bench$(EXE_EXT): test/columnar-analytics-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-robustness-test$(EXE_EXT): test/columnar-robustness-test.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/columnar-variance-bench$(EXE_EXT): test/columnar-variance-bench.c $(SQLITE_AMALGAMATION) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

variance-bench: all $(VARIANCE_BENCH)
	$(VARIANCE_BENCH) ./columnar $(VARIANCE_REPEATS) $(VARIANCE_DATASETS)

smoke-bench: all benchmarks
	$(BUILD_DIR)/columnar-bench$(EXE_EXT) ./columnar 1000 64
	$(BUILD_DIR)/columnar-agg-bench$(EXE_EXT) ./columnar 1000
	$(BUILD_DIR)/columnar-analytics-bench$(EXE_EXT) ./columnar 1000 64

clean:
	rm -f columnar.so columnar.dylib columnar.dll
	rm -rf $(BUILD_DIR) $(DIST_DIR)
