JDK ?= $(HOME)/.gradle/jdks/eclipse_adoptium-25-amd64-linux.2
JAVAC = $(JDK)/bin/javac
JAR = $(JDK)/bin/jar
JAVA = $(JDK)/bin/java

SRC := $(shell find src -name '*.java')
CLASSES = build/classes

all: build/lua.jar

build/lua.jar: $(SRC)
	rm -rf $(CLASSES)
	mkdir -p $(CLASSES)
	$(JAVAC) --release 21 -encoding UTF-8 -Xlint:all -Xlint:-serial -Xlint:-fallthrough -Xlint:-cast -Xmaxerrs 10000 -d $(CLASSES) $(SRC)
	$(JAR) --create --file $@ --main-class lua.host.Lua -C $(CLASSES) .
	echo "$(abspath $(JDK))" > build/jdk

clean:
	rm -rf build

.PHONY: all clean
