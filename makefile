VARIANT       = release
TEST_TEMP_DIR?=1

ifeq ($(findstring debug,$(MAKECMDGOALS)),debug)
VARIANT       = debug
endif

TOP_DIR       = .
OBJ_DIR_BASE  = ./obj
OBJ_DIR       = $(OBJ_DIR_BASE)/$(VARIANT)

OBJ_NAMES     = lapi.o lauxlib.o lbaselib.o lcode.o lcorolib.o lctype.o ldblib.o ldebug.o ldo.o ldump.o lfunc.o lgc.o
OBJ_NAMES    += linit.o liolib.o llex.o lmathlib.o lmem.o loadlib.o lobject.o lopcodes.o loslib.o lparser.o lstate.o
OBJ_NAMES    += lstring.o lstrlib.o ltable.o ltablib.o ltests.o ltm.o lundump.o lutf8lib.o lvm.o lzio.o
OBJ_NAMES    += util_cast.o
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

CXXFLAGS      =-Wall $(CXXFLAGS.$(VARIANT)) $(TESTS) $(CWARNSCPP) $(CWARNGCC) -std=c++23 -fno-stack-protector -fno-common -march=native -MMD
LDFLAGS       =-Wl,-E -lm -ldl -fuse-ld=lld
AR            =ar rc
RANLIB        =ranlib
RM            =rm -f
CXX           = g++

LUA_O         =$(OBJ_DIR)/lua.o
CORE_T        =$(OBJ_DIR)/liblua.a
LUA_T         =lua

all: $(LUA_T)

include tests/libs/build.mk

debug: VARIANT=debug
debug: TESTS=-DLUA_DEBUG
debug: $(LUA_T)

-include $(DEP_FILES)

$(CORE_T): $(OBJ_FILES)
	$(AR) $@ $?
	$(RANLIB) $@

$(LUA_T): $(LUA_O) $(CORE_T) $(TEST_OBJ_FILES) $(TEST_BUILD_FILES)
	$(CXX) -o $@ $(LUA_O) $(CORE_T) $(LDFLAGS)

clean:
	$(RM) -r $(OBJ_DIR_BASE)
	$(RM) $(LUA_T)
	$(RM) $(TEST_BUILD_FILES)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MF $(OBJ_DIR)/$*.d -o $@ -c $<

$(OBJ_DIR):
	mkdir -p $@

test:
	mkdir -p tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; \
		ulimit -S -s 1100; \
		cd tests; \
		cat /dev/null | perf stat -- ../$(LUA_T) -e "_TMP='_$(TEST_TEMP_DIR)'" -W all.lua )

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

.PHONY: all debug clean test full
