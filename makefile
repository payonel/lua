VARIANT       = release
TEST_TEMP_DIR?=1

ifeq ($(findstring debug,$(MAKECMDGOALS)),debug)
VARIANT       = debug
endif

ifeq ($(findstring cov,$(MAKECMDGOALS)),cov)
VARIANT       = cov
endif

TOP_DIR       = .
OBJ_DIR_BASE  = ./obj
OBJ_DIR       = $(OBJ_DIR_BASE)/$(VARIANT)

OBJ_NAMES     = lapi.o lauxlib.o lbaselib.o lcode.o lcorolib.o lctype.o ldblib.o ldebug.o ldo.o ldump.o lfunc.o lgc.o
OBJ_NAMES    += linit.o liolib.o llex.o lmathlib.o lmem.o loadlib.o lobject.o lopcodes.o loslib.o lparser.o lstate.o
OBJ_NAMES    += lstring.o lstrlib.o ltable.o ltablib.o ltests.o ltm.o lundump.o lutf8lib.o lvm.o lzio.o
OBJ_NAMES    += util_cast.o os_api.o locale_util.o lnames.o lstyx.o
OBJ_FILES     = $(addprefix $(OBJ_DIR)/, $(OBJ_NAMES))

DEP_FILES     = $(OBJ_FILES:.o=.d)

CWARNSCPP= \
	-Wfatal-errors \
	-Wextra \
	-Wshadow \
	-Wundef \
	-Wwrite-strings \
	-Wredundant-decls \
	-Wdisabled-optimization \
	-Wdouble-promotion \
	-Wmissing-declarations \
	-Werror \
	-Wstrict-overflow=2 \
	-Wconversion \
	# -Wcast-qual \
	# -Wformat=2 \

# Warnings for gcc, not valid for clang
CWARNGCC= \
	-Wlogical-op \
	-Wno-aggressive-loop-optimizations \

# enable Linux goodies
CXXFLAGS.release =-O2 -DNDEBUG
CXXFLAGS.debug   =-Og -g3 -ggdb
CXXFLAGS.cov     =-O0 -g3 -ggdb --coverage

LDFLAGS.cov   =--coverage

CXXFLAGS      =-Wall $(CXXFLAGS.$(VARIANT)) $(TESTS) $(CWARNSCPP) $(CWARNGCC) -std=c++23 -fno-stack-protector -fno-common -march=native -MMD -fmax-errors=0
LDFLAGS       =-Wl,-E -lm -ldl -fuse-ld=lld $(LDFLAGS.$(VARIANT))
AR            =ar rc
RANLIB        =ranlib
RM            =rm -f
CXX           = g++

LUA_O         =$(OBJ_DIR)/lua.o
CORE_T        =$(OBJ_DIR)/liblua.a
LUA_T         =lua

TEST_API_O    =$(OBJ_DIR)/test_public_api.o
TEST_API_T    =test_public_api
ENGINE_TEST_O =$(OBJ_DIR)/engine_tests.o
ENGINE_TEST_T =test_engine
GATE_O        =$(OBJ_DIR)/gate.o
GATE_T        =test_gate
DEP_FILES    += $(LUA_O:.o=.d) $(TEST_API_O:.o=.d) $(ENGINE_TEST_O:.o=.d) $(GATE_O:.o=.d)
STYX_TEST_O  =$(OBJ_DIR)/styx_tests.o
STYX_TEST_T  =test_styx
STYX_RUN     =cd tests/styx && ../../$(STYX_TEST_T)
DEP_FILES    += $(LUA_O:.o=.d) $(TEST_API_O:.o=.d) $(ENGINE_TEST_O:.o=.d) $(STYX_TEST_O:.o=.d)

all: $(LUA_T) $(TEST_API_T) $(ENGINE_TEST_T) $(GATE_T)
all: $(LUA_T) $(TEST_API_T) $(ENGINE_TEST_T)
all: $(LUA_T) $(TEST_API_T) $(STYX_TEST_T)

include tests/libs/build.mk

debug: VARIANT=debug
debug: TESTS=-DLUA_DEBUG
debug: $(LUA_T) $(TEST_API_T) $(ENGINE_TEST_T) $(GATE_T)
debug: $(LUA_T) $(TEST_API_T) $(ENGINE_TEST_T)
debug: $(LUA_T) $(TEST_API_T) $(STYX_TEST_T)

cov: VARIANT=cov
cov: $(LUA_T) $(TEST_API_T) $(ENGINE_TEST_T) $(GATE_T)
cov: $(LUA_T) $(TEST_API_T) $(ENGINE_TEST_T)
cov: $(LUA_T) $(TEST_API_T) $(STYX_TEST_T)
	$(RM) $(OBJ_DIR)/*.gcda $(TEST_DIR)/*.gcda
	$(MAKE) test
	./cov-report.sh

-include $(DEP_FILES)

# The lua/test_public_api filenames are shared by all variants: stamp the
# active variant so switching variants forces a relink from the right objects.
VARIANT_STAMP = $(OBJ_DIR_BASE)/variant-$(VARIANT).stamp
$(VARIANT_STAMP):
	mkdir -p $(OBJ_DIR_BASE)
	$(RM) $(OBJ_DIR_BASE)/variant-*.stamp
	touch $@

# liblua.a is the published artifact: only the formal C API (lua.h,
# lualib.h, lauxlib.h — extern "C" symbols, plus the public luaL_Buffer
# class) is visible. All internals have C++ linkage; partial-link the
# objects and localize every strong mangled symbol so internals cannot be
# linked against. The lua interpreter uses internal APIs, so it links the
# raw objects instead of the archive. Debug builds skip the seal: LUA_DEBUG
# test instrumentation (ltests/lhack) reaches internals by design.
CORE_O        = $(OBJ_DIR)/liblua_combined.o

$(CORE_T): $(OBJ_FILES)
	ld -r -o $(CORE_O) $(OBJ_FILES)
	@if [ "$(VARIANT)" != "debug" ]; then \
	  nm $(CORE_O) | awk '$$2 ~ /^[TDBR]$$/ && $$3 ~ /^_Z/ && $$3 !~ /^_ZN11luaL_Buffer/ && $$3 !~ /^_Z[0-9]+(lua|luaL|luaopen)_/ {print $$3}' > $(OBJ_DIR)/localize.syms; \
	  objcopy --localize-symbols=$(OBJ_DIR)/localize.syms $(CORE_O); \
	  echo "sealed $@: internals localized"; \
	fi
	$(RM) $@
	$(AR) $@ $(CORE_O)
	$(RANLIB) $@

$(LUA_T): $(LUA_O) $(OBJ_FILES) $(TEST_OBJ_FILES) $(TEST_BUILD_FILES) $(VARIANT_STAMP)
	$(CXX) -o $@ $(LUA_O) $(OBJ_FILES) $(LDFLAGS)

$(TEST_API_T): $(TEST_API_O) $(CORE_T) $(VARIANT_STAMP)
	$(CXX) -o $@ $(TEST_API_O) $(CORE_T) $(LDFLAGS)

# engine internals tests link the raw objects (internals reachable)
$(ENGINE_TEST_O): tests/engine/engine_tests.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -I$(TOP_DIR) -MF $(OBJ_DIR)/engine_tests.d -o $@ -c $<

$(ENGINE_TEST_T): $(ENGINE_TEST_O) $(OBJ_FILES) $(VARIANT_STAMP)
	$(CXX) -o $@ $(ENGINE_TEST_O) $(OBJ_FILES) $(LDFLAGS)

# the freeze gate: a script run with a budget that freezes at every budget
# point, thawed until it ends (tests/gate/gate.cpp)
$(GATE_O): tests/gate/gate.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -I$(TOP_DIR) -MF $(OBJ_DIR)/gate.d -o $@ -c $<

$(GATE_T): $(GATE_O) $(CORE_T) $(VARIANT_STAMP)
	$(CXX) -o $@ $(GATE_O) $(CORE_T) $(LDFLAGS)

# the deep tests and the whole upstream suite, frozen and thawed at every
# budget point of several sizes (`make test` runs it; `make gate
# GATE_STEPS="1"` runs it alone, with other sizes)
GATE_STEPS ?= 97 17 3
gate: $(GATE_T) gate-run

gate-run:
	for n in $(GATE_STEPS); do \
	  ( cd tests && ../$(GATE_T) $$n deep/all.lua ) || exit 1; \
	  mkdir -p tests/libs/_$(TEST_TEMP_DIR)/P1; \
	  ( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd tests; \
	    cat /dev/null | ../$(GATE_T) $$n all.lua ) || exit 1; \
	done

# just the engine internals tests (`make test` runs them too)
engine-test: $(ENGINE_TEST_T)
	./$(ENGINE_TEST_T)
# The styx tests link the raw objects (internals reachable) and are not part
# of `test`/`full` while Styx is being built test-first.
$(STYX_TEST_O): tests/styx/styx_tests.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -I$(TOP_DIR) -MF $(OBJ_DIR)/styx_tests.d -o $@ -c $<
$(STYX_TEST_T): $(STYX_TEST_O) $(OBJ_FILES) $(VARIANT_STAMP)
	$(CXX) -o $@ $(STYX_TEST_O) $(OBJ_FILES) $(LDFLAGS)
test-styx: $(STYX_TEST_T)
	cd tests/styx && ../../$(STYX_TEST_T)

# just the Styx tests (`make test` runs them too, along with everything else)
styx-test: $(STYX_TEST_T)
	$(STYX_RUN)

clean:
	$(RM) -r $(OBJ_DIR_BASE)
	$(RM) $(LUA_T)
	$(RM) $(TEST_API_T)
	$(RM) $(ENGINE_TEST_T)
	$(RM) $(GATE_T)
	$(RM) $(STYX_TEST_T)
	$(RM) $(TEST_BUILD_FILES)
	$(RM) $(TEST_DIR)/*.gcno $(TEST_DIR)/*.gcda

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MF $(OBJ_DIR)/$*.d -o $@ -c $<

$(OBJ_DIR):
	mkdir -p $@

test:
	./$(TEST_API_T)
	./$(ENGINE_TEST_T)
	mkdir -p tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; \
		ulimit -S -s 8192; \
		cd tests; \
		cat /dev/null | /usr/bin/time -v ../$(LUA_T) -e "_TMP='_$(TEST_TEMP_DIR)'" -W all.lua )
	cd tests && ../$(LUA_T) deep/all.lua
	$(MAKE) gate-run
	cd tests/styx && ../../$(STYX_TEST_T)
	$(STYX_RUN)

full:
	make clean
	make -j
	make test > /tmp/lua-test-release.log
	make clean
	make debug -j
	make test > /tmp/lua-test-debug.log
	@echo ""; echo "total times:"; \
	grep 'total time' /tmp/lua-test-release.log | sed 's/^/  release: /'; \
	grep 'total time' /tmp/lua-test-debug.log | sed 's/^/  debug:   /'

.PHONY: all debug cov clean test engine-test gate gate-run full
.PHONY: all debug cov clean test engine-test full test-styx
.PHONY: all debug cov clean test engine-test full test-styx styx-test
