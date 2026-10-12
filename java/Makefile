JDK ?= $(HOME)/.gradle/jdks/eclipse_adoptium-25-amd64-linux.2
JAVAC = $(JDK)/bin/javac
JAR = $(JDK)/bin/jar
JAVA = $(JDK)/bin/java

SRC := $(shell find src -name '*.java')
CLASSES = build/classes

# the test C modules (tests/libs/build.mk) as files for 'require': the JVM
# loads the Java library that a file's name names, whatever the file holds
# (OsApi's 'os_dlopen'); dated 1970, so that the C++ build makes its own
TESTLIBS_DIR = ../tests/libs
TESTLIBS = $(addprefix $(TESTLIBS_DIR)/, lib1.so lib11.so lib2.so lib21.so lib2-v2.so)

all: build/lua.jar $(TESTLIBS)

build/lua.jar: $(SRC)
	rm -rf $(CLASSES)
	mkdir -p $(CLASSES)
	$(JAVAC) --release 21 -encoding UTF-8 -Xlint:all -Xlint:-serial -Xlint:-fallthrough -Xlint:-cast -Xmaxerrs 10000 -d $(CLASSES) $(SRC)
	$(JAR) --create --file $@ --main-class lua.host.Lua -C $(CLASSES) .
	echo "$(abspath $(JDK))" > build/jdk

$(TESTLIBS_DIR)/%.so: CLASS = $(patsubst lib%,Lib%,$*)
$(TESTLIBS_DIR)/lib2-v2.so: CLASS = Lib22

$(TESTLIBS_DIR)/%.so:
	echo "Java library lua.tests.libs.$(CLASS)" > $@
	touch -d @0 $@

# the tests, as the C++ build's `make test` runs them (DEBUG=1: the debug
# build, the same jar with assertions and lua.debug on)
DEBUG ?=
LUA = $(if $(DEBUG),lua-debug,lua)
JFLAGS = -XX:-DontCompileHugeMethods $(if $(DEBUG),-ea -Dlua.debug=true)
TEST_TEMP_DIR = java$(if $(DEBUG),d)
GATE_STEPS ?= 97 17 3
STYX_GATE_DEEP ?= 7
STYX_GATE_ALL ?= 5000

test: all
	cd .. && $(JAVA) $(JFLAGS) -cp java/build/lua.jar lua.tests.PublicApi
	LUA_JAVA_DEBUG=$(DEBUG) ./test_engine
	mkdir -p ../tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd ../tests; \
		cat /dev/null | /usr/bin/time -v ../java/$(LUA) -e "_TMP='_$(TEST_TEMP_DIR)'" -W all.lua )
	cd ../tests && ../java/$(LUA) deep/all.lua
	$(MAKE) gate-run
	$(MAKE) styx-gate-run
	cd ../tests/styx && LUA_JAVA_DEBUG=$(DEBUG) ../../java/test_styx

gate-run:
	for n in $(GATE_STEPS); do \
	  ( cd ../tests && LUA_JAVA_DEBUG=$(DEBUG) ../java/test_gate $$n deep/all.lua ) || exit 1; \
	  mkdir -p ../tests/libs/_$(TEST_TEMP_DIR)/P1; \
	  ( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd ../tests; \
	    cat /dev/null | LUA_JAVA_DEBUG=$(DEBUG) ../java/test_gate $$n all.lua ) || exit 1; \
	done

styx-gate-run:
	( cd ../tests && LUA_JAVA_DEBUG=$(DEBUG) ../java/test_gate 97 --styx $(STYX_GATE_DEEP) deep/all.lua ) || exit 1
	mkdir -p ../tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd ../tests; \
	  cat /dev/null | LUA_JAVA_DEBUG=$(DEBUG) ../java/test_gate 97 --styx $(STYX_GATE_ALL) all.lua ) || exit 1

clean:
	rm -rf build

.PHONY: all clean test gate-run styx-gate-run
