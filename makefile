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
OBJ_NAMES    += util_cast.o os_api.o
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
CXXFLAGS.release =-O2
CXXFLAGS.debug   =-Og -g3 -ggdb
CXXFLAGS.cov     =-O0 -g3 -ggdb --coverage

LDFLAGS.cov   =--coverage

CXXFLAGS      =-Wall $(CXXFLAGS.$(VARIANT)) $(TESTS) $(CWARNSCPP) $(CWARNGCC) -std=c++23 -fno-stack-protector -fno-common -march=native -MMD
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
DEP_FILES    += $(TEST_API_O:.o=.d)

TEST_INT_O    =$(OBJ_DIR)/test_internal_api.o
TEST_INT_T    =test_internal_api
DEP_FILES    += $(TEST_INT_O:.o=.d)

all: $(LUA_T) $(TEST_API_T) $(TEST_INT_T)

include tests/libs/build.mk

debug: VARIANT=debug
debug: TESTS=-DLUA_DEBUG
debug: $(LUA_T) $(TEST_API_T) $(TEST_INT_T)

cov: VARIANT=cov
cov: $(LUA_T) $(TEST_API_T) $(TEST_INT_T)
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

$(CORE_T): $(OBJ_FILES)
	$(AR) $@ $?
	$(RANLIB) $@

$(LUA_T): $(LUA_O) $(CORE_T) $(TEST_OBJ_FILES) $(TEST_BUILD_FILES) $(VARIANT_STAMP)
	$(CXX) -o $@ $(LUA_O) $(CORE_T) $(LDFLAGS)

$(TEST_API_T): $(TEST_API_O) $(CORE_T) $(VARIANT_STAMP)
	$(CXX) -o $@ $(TEST_API_O) $(CORE_T) $(LDFLAGS)

$(TEST_INT_T): $(TEST_INT_O) $(CORE_T) $(VARIANT_STAMP)
	$(CXX) -o $@ $(TEST_INT_O) $(CORE_T) $(LDFLAGS)

clean:
	$(RM) -r $(OBJ_DIR_BASE)
	$(RM) $(LUA_T)
	$(RM) $(TEST_API_T)
	$(RM) $(TEST_INT_T)
	$(RM) $(TEST_BUILD_FILES)
	$(RM) $(TEST_DIR)/*.gcno $(TEST_DIR)/*.gcda

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MF $(OBJ_DIR)/$*.d -o $@ -c $<

$(OBJ_DIR):
	mkdir -p $@

test:
	./$(TEST_API_T)
	./$(TEST_INT_T)
	mkdir -p tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; \
		ulimit -S -s 8192; \
		cd tests; \
		cat /dev/null | perf stat -- ../$(LUA_T) -e "_TMP='_$(TEST_TEMP_DIR)'" -W all.lua )
	cd tests && ../$(LUA_T) deep/all.lua

full:
	make clean
	make -j
	make test > /tmp/lua-test-release.log 2>&1 && cat /tmp/lua-test-release.log
	make clean
	make debug -j
	make test > /tmp/lua-test-debug.log 2>&1 && cat /tmp/lua-test-debug.log
	@echo ""; echo "total times:"; \
	grep 'total time' /tmp/lua-test-release.log | sed 's/^/  release: /'; \
	grep 'total time' /tmp/lua-test-debug.log | sed 's/^/  debug:   /'

.PHONY: all debug cov clean test full
