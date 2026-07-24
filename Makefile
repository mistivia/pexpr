CC ?= gcc
CSTD := -std=c11
WARN := -Wall -Wextra
INCLUDES := -Iinclude -Isrc

LIB_SRCS := src/pbuf.c src/pnode.c src/serialize.c src/parser.c src/stackless.c
TEST_SRCS := tests/test_main.c tests/test_pnode.c tests/test_serialize.c tests/test_parser.c tests/test_stream.c

BUILD_DIR ?= build/debug
CFLAGS ?= $(CSTD) $(WARN) $(INCLUDES) -g -O0 -pthread
LDFLAGS ?= -pthread

LIB_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(TEST_SRCS))
TEST_BIN := $(BUILD_DIR)/pexpr_tests

.PHONY: all test asan tsan msan coverage check clean

all: $(BUILD_DIR)/libpexpr.a

$(BUILD_DIR)/libpexpr.a: $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(LIB_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

# AddressSanitizer + UndefinedBehaviorSanitizer.
asan:
	$(MAKE) BUILD_DIR=build/asan \
		CFLAGS="$(CSTD) $(WARN) $(INCLUDES) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -pthread" \
		LDFLAGS="-fsanitize=address,undefined -pthread" \
		test

# ThreadSanitizer.
tsan:
	$(MAKE) BUILD_DIR=build/tsan \
		CFLAGS="$(CSTD) $(WARN) $(INCLUDES) -g -O1 -fsanitize=thread -fno-sanitize-recover=all -pthread" \
		LDFLAGS="-fsanitize=thread -pthread" \
		test

# MemorySanitizer (uninitialized reads). Requires clang.
msan:
	$(MAKE) CC=clang BUILD_DIR=build/msan \
		CFLAGS="$(CSTD) $(WARN) $(INCLUDES) -g -O1 -fsanitize=memory -fsanitize-memory-track-origins -fno-sanitize-recover=all -pthread" \
		LDFLAGS="-fsanitize=memory -pthread" \
		test

# gcov/lcov line coverage report under build/coverage/.
coverage:
	$(MAKE) BUILD_DIR=build/coverage \
		CFLAGS="$(CSTD) $(WARN) $(INCLUDES) -g -O0 --coverage -pthread" \
		LDFLAGS="--coverage -pthread" \
		test
	lcov --capture --directory build/coverage --base-directory . \
		--output-file build/coverage/coverage.raw.info --rc branch_coverage=0
	lcov --remove build/coverage/coverage.raw.info \
		'*/tests/*' \
		--output-file build/coverage/coverage.info --rc branch_coverage=0
	genhtml build/coverage/coverage.info --output-directory build/coverage/html --rc branch_coverage=0
	lcov --summary build/coverage/coverage.info --rc branch_coverage=0

# Run everything CI would run.
check: test asan tsan msan coverage

clean:
	rm -rf build
