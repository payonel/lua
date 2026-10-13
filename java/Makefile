JDK ?= $(HOME)/.gradle/jdks/eclipse_adoptium-25-amd64-linux.2
JAVAC = $(JDK)/bin/javac
JAR = $(JDK)/bin/jar
JAVA = $(JDK)/bin/java

TESTS_DIR = src/com/payonel/lungolua/tests
SRC := $(shell find src -name '*.java' -not -path '$(TESTS_DIR)/*')
TESTS_SRC := $(shell find $(TESTS_DIR) -name '*.java')
CLASSES = build/classes
TEST_CLASSES = build/test-classes
CLASSPATH = build/lungolua.jar:build/lungolua-tests.jar

# JLine, the line editor of the JVM's readline (lib/README): the sources
# compile against it, and lungolua.jar's manifest names its jars (Class-Path,
# relative to build/) and lets them call C (Enable-Native-Access: the FFM
# terminal), so that 'java -jar build/lungolua.jar' edits lines as the launchers do
JLINE_VERSION = 3.30.17
JLINE = $(foreach m, reader terminal terminal-ffm, lib/jline-$(m)-$(JLINE_VERSION).jar)
empty :=
space := $(empty) $(empty)
MANIFEST = build/manifest.txt

# the test C modules (tests/libs/build.mk) as files for 'require': the JVM
# loads the Java library that a file's name names, whatever the file holds
# (OsApi's 'os_dlopen'); dated 1970, so that the C++ build makes its own
TESTLIBS_DIR = ../tests/libs
TESTLIBS = $(addprefix $(TESTLIBS_DIR)/, lib1.so lib11.so lib2.so lib21.so lib2-v2.so)

all: build/lungolua.jar build/lungolua-tests.jar build/lungolua.aot $(TESTLIBS)

build/lungolua.jar: $(SRC) $(JLINE)
	rm -rf $(CLASSES)
	mkdir -p $(CLASSES)
	$(JAVAC) --release 21 -encoding UTF-8 -Xlint:all -Xlint:-serial -Xlint:-fallthrough -Xlint:-cast -Xmaxerrs 10000 -cp $(subst $(space),:,$(JLINE)) -d $(CLASSES) $(SRC)
	printf 'Class-Path: %s\nEnable-Native-Access: ALL-UNNAMED\n' "$(addprefix ../,$(JLINE))" > $(MANIFEST)
	$(JAR) --create --file $@ --main-class com.payonel.lungolua.host.Lua --manifest $(MANIFEST) -C $(CLASSES) .
	echo "$(abspath $(JDK))" > build/jdk

build/lungolua-sources.jar: $(SRC)
	mkdir -p build
	$(JAR) --create --file $@ $(foreach f,$(patsubst src/%,%,$(SRC)),-C src $(f))

# the test library and the tests, with the test scripts (tests/, as git
# keeps them) for the projects that run them against the engine
TESTS_LUA := $(shell cd .. && git ls-files tests 2>/dev/null)

build/lungolua-tests.jar: build/lungolua.jar $(TESTS_SRC) $(addprefix ../,$(TESTS_LUA))
	rm -rf $(TEST_CLASSES)
	mkdir -p $(TEST_CLASSES)
	$(JAVAC) --release 21 -encoding UTF-8 -Xlint:all -Xlint:-serial -Xlint:-fallthrough -Xlint:-cast -Xmaxerrs 10000 -cp build/lungolua.jar:$(subst $(space),:,$(JLINE)) -d $(TEST_CLASSES) $(TESTS_SRC)
	cd .. && git ls-files -z tests | tar --null -T - -cf - | tar -xf - -C java/$(TEST_CLASSES)
	$(JAR) --create --file $@ -C $(TEST_CLASSES) .

# the JVM's options (the launchers' too)
JOPTS = --enable-native-access=ALL-UNNAMED

# the AOT cache of the jar (JDK 25): the classes loaded and linked as a
# training run with the launchers' options left them, without the run's
# profiles (-XX:-AOTRecordTraining: with them, C2 compiles luaV_execute for
# the training's mix of instructions, and loops run up to twice as slow); the
# launchers use it when it is newer than the jar (the JVM then checks that it
# matches its JDK, jars and options, and starts without it if not); neither
# step fails when the cache cannot be written, hence the test
build/lungolua.aot: build/lungolua.jar build/lungolua-tests.jar aot-training.lua $(JLINE) $(JDK)/release
	rm -f $@
	$(JAVA) $(JOPTS) -XX:+UnlockDiagnosticVMOptions -XX:-AOTRecordTraining -XX:AOTMode=record -XX:AOTConfiguration=build/lungolua.aotconf -cp $(CLASSPATH) com.payonel.lungolua.host.Lua aot-training.lua > /dev/null
	$(JAVA) $(JOPTS) -XX:AOTMode=create -XX:AOTConfiguration=build/lungolua.aotconf -XX:AOTCache=$@ -cp $(CLASSPATH) com.payonel.lungolua.host.Lua
	rm build/lungolua.aotconf
	test -s $@

$(TESTLIBS_DIR)/%.so: CLASS = $(patsubst lib%,Lib%,$*)
$(TESTLIBS_DIR)/lib2-v2.so: CLASS = Lib22

$(TESTLIBS_DIR)/%.so:
	echo "Java library com.payonel.lungolua.tests.libs.$(CLASS)" > $@
	touch -d @0 $@

# the tests, as the C++ build's `make test` runs them (DEBUG=1: the debug
# build, the same jar with assertions and lua.debug on)
DEBUG ?=
LUA = $(if $(DEBUG),lua-debug,lua)
JFLAGS = $(JOPTS) $(if $(DEBUG),-ea -Dlua.debug=true)
TEST_TEMP_DIR ?= 1
GATE_STEPS ?= 97 17 3

test: all
	mkdir -p ../tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd ../tests; \
		cat /dev/null | $(JAVA) $(JFLAGS) -Dpublicapi.lua=../java/$(LUA) -cp ../java/build/lungolua.jar:../java/build/lungolua-tests.jar com.payonel.lungolua.tests.PublicApi )
	LUA_JAVA_DEBUG=$(DEBUG) ./test_engine
	mkdir -p ../tests/libs/_$(TEST_TEMP_DIR)/P1
	( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd ../tests; \
		cat /dev/null | /usr/bin/time -v ../java/$(LUA) -e "_TMP='_$(TEST_TEMP_DIR)'" -W all.lua )
	cd ../tests && ../java/$(LUA) deep/all.lua
	$(MAKE) gate-run

gate-run:
	for n in $(GATE_STEPS); do \
	  ( cd ../tests && LUA_JAVA_DEBUG=$(DEBUG) ../java/test_gate $$n deep/all.lua ) || exit 1; \
	  mkdir -p ../tests/libs/_$(TEST_TEMP_DIR)/P1; \
	  ( trap "rm -rf libs/_$(TEST_TEMP_DIR)" EXIT; ulimit -S -s 8192; cd ../tests; \
	    cat /dev/null | LUA_JAVA_DEBUG=$(DEBUG) ../java/test_gate $$n all.lua ) || exit 1; \
	done

clean:
	rm -rf build

.PHONY: all clean test gate-run
