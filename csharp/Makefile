# the .NET SDK (10): ~/.dotnet10 (Microsoft's dotnet-install script)
DOTNET ?= $(HOME)/.dotnet10/dotnet
export DOTNET_CLI_TELEMETRY_OPTOUT = 1
export DOTNET_NOLOGO = 1
export DOTNET_SKIP_FIRST_TIME_EXPERIENCE = 1

TESTS_DIR = src/Tests
SRC := $(shell find src -name '*.cs' -not -path '$(TESTS_DIR)/*' -not -path '*/obj/*') src/LungoLua.csproj Directory.Build.props
TESTS_SRC := $(shell find $(TESTS_DIR) -name '*.cs' -not -path '*/obj/*') $(TESTS_DIR)/LungoLua.Tests.csproj

all: build/lungolua.dll build/lungolua-tests.dll

# the engine (and the standalone interpreter, its entry point); the tests
# assembly beside it, built against it. Both hold ReadyToRun code for this
# machine's platform (RID) besides their IL: the runtime starts with it
# (0.02 s instead of 0.07 for 'lua -e ""'), and on another platform runs the
# IL as any assembly; luaV_execute is always the JIT's optimized code
# (AggressiveOptimization: precompiled code is replaced only after enough
# calls, and luaV_execute is called seldom and loops long)
RID := $(shell $(DOTNET) msbuild -getProperty:NETCoreSdkRuntimeIdentifier src/LungoLua.csproj 2>/dev/null)
PUBLISH = $(DOTNET) publish -c Release -r $(RID) --self-contained false -p:PublishReadyToRun=true --nologo -v:q -clp:NoSummary

build/lungolua.dll: $(SRC)
	$(PUBLISH) src/LungoLua.csproj -o build
	echo "$(abspath $(DOTNET))" > build/dotnet

build/lungolua-tests.dll: build/lungolua.dll $(TESTS_SRC)
	$(PUBLISH) $(TESTS_DIR)/LungoLua.Tests.csproj -o build
	touch $@

# the tests run in RUN_DIR, a copy of ../tests with the test C modules
# (cpp/tests/libs/build.mk) as files for 'require', so that a run of
# another build (cpp/, java/, or the other of release and DEBUG=1) at the
# same time shares no files. The engine loads the C# library that a module
# file's name names, whatever the file holds (OsApi's 'os_dlopen')
RUN_DIR ?= $(if $(DEBUG),run-debug,run)
TESTLIBS = $(addprefix $(RUN_DIR)/libs/, lib1.so lib11.so lib2.so lib21.so lib2-v2.so)

$(RUN_DIR)/libs/%.so: CLASS = $(patsubst lib%,Lib%,$*)
$(RUN_DIR)/libs/lib2-v2.so: CLASS = Lib22

$(RUN_DIR)/libs/%.so:
	mkdir -p $(@D)
	echo "C# library Payonel.LungoLua.Tests.Libs.$(CLASS)" > $@

run-dir: $(TESTLIBS)
	cp -r ../tests/. $(RUN_DIR)/

# the tests, as the C++ build's `make test` runs them (DEBUG=1: the debug
# build, the same assemblies with the debug checks: lua.debug)
DEBUG ?=
LUA = $(if $(DEBUG),lua-debug,lua)
DFLAGS = $(if $(DEBUG),-Dlua.debug=true)
TEST_TEMP_DIR ?= 1
GATE_STEPS ?= 97 17 3

test: all run-dir
	mkdir -p $(RUN_DIR)/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd $(RUN_DIR); \
		cat /dev/null | $(DOTNET) $(CURDIR)/build/lungolua-tests.dll $(DFLAGS) -Dpublicapi.lua=$(CURDIR)/$(LUA) PublicApi )
	LUA_CS_DEBUG=$(DEBUG) ./test_engine
	$(DOTNET) build/lungolua-tests.dll $(DFLAGS) Inbox
	mkdir -p $(RUN_DIR)/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd $(RUN_DIR); \
		cat /dev/null | /usr/bin/time -v $(CURDIR)/$(LUA) -e "_TMP='_$(TEST_TEMP_DIR)'" -W all.lua )
	cd $(RUN_DIR) && $(CURDIR)/$(LUA) deep/all.lua
	$(MAKE) gate-run

gate-run: run-dir
	for n in $(GATE_STEPS); do \
	  ( cd $(RUN_DIR) && LUA_CS_DEBUG=$(DEBUG) $(CURDIR)/test_gate $$n deep/all.lua ) || exit 1; \
	  mkdir -p $(RUN_DIR)/libs/_$(TEST_TEMP_DIR)/P1; \
	  ( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd $(RUN_DIR); \
	    cat /dev/null | LUA_CS_DEBUG=$(DEBUG) $(CURDIR)/test_gate $$n all.lua ) || exit 1; \
	done

# both builds' tests, as the C++'s 'make full' (one after the other; the
# two can also run at once: they test in run and run-debug)
full: all
	$(MAKE) test > /tmp/lua-csharp-test-release.log
	$(MAKE) test DEBUG=1 > /tmp/lua-csharp-test-debug.log
	@echo ""; grep -h 'checks,\|final OK\|gate: ok' /tmp/lua-csharp-test-release.log | sed 's/^/  release: /'; \
	grep -h 'checks,\|final OK\|gate: ok' /tmp/lua-csharp-test-debug.log | sed 's/^/  debug:   /'

clean:
	rm -rf build

.PHONY: all clean full test gate-run run-dir
