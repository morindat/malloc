CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -fPIC -g
LIB      = mymalloc.so
SRC      = src/allocator.cpp src/utils.cpp
INCLUDES = -Isrc

# ── Library ────────────────────────────────────────────────────────
$(LIB): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -shared -o $@ $^

# ── Tests ──────────────────────────────────────────────────────────
TEST_BINS = tests/test_basic \
            tests/test_coalesce \
            tests/test_realloc \
            tests/test_stress

tests/%: tests/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test: $(LIB) $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "── running $$t ──"; \
		LD_PRELOAD=./$(LIB) $$t; \
	done

# ── Benchmark ──────────────────────────────────────────────────────
bench: $(LIB) bench/bench.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -O2 -o bench/run bench/bench.cpp
	LD_PRELOAD=./$(LIB) ./bench/run

# ── Valgrind ───────────────────────────────────────────────────────
valgrind: $(LIB) $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "── valgrind $$t ──"; \
		LD_PRELOAD=./$(LIB) valgrind --leak-check=full --error-exitcode=1 $$t; \
	done

# ── Preload against a real binary ──────────────────────────────────
preload: $(LIB)
	bash scripts/preload_test.sh

# ── Clean ──────────────────────────────────────────────────────────
clean:
	rm -f $(LIB) $(TEST_BINS) bench/run