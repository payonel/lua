
TEST_SRC_DIR    = tests/libs
TEST_DIR        = $(OBJ_DIR_BASE)/libs
TEST_CXXFLAGS   = -fPIC -shared -I$(TOP_DIR) -I$(TEST_SRC_DIR)
TEST_OBJS       = lib1.so lib11.so lib2.so lib21.so lib2-v2.so
TEST_BUILD_FILES= $(addprefix $(TEST_DIR)/, $(TEST_OBJS))
TEST_DEP_FILES  = $(addsuffix .d, $(TEST_BUILD_FILES))

test_all: $(TEST_BUILD_FILES)

-include $(TEST_DEP_FILES)

$(TEST_DIR)/%.so: $(TEST_SRC_DIR)/%.cpp
	mkdir -p $(TEST_DIR)
	$(CXX)  $(TEST_CXXFLAGS) $(CXXFLAGS) -MF $@.d -o $@ $<

$(TEST_DIR)/lib2-v2.so: $(TEST_SRC_DIR)/lib22.cpp $(TEST_SRC_DIR)/lib21.cpp
	mkdir -p $(TEST_DIR)
	$(CXX)  $(TEST_CXXFLAGS) $(CXXFLAGS) -MF $@.d -o $@ $(TEST_SRC_DIR)/lib22.cpp
