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

clean:
	rm -rf build

.PHONY: all clean
