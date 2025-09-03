# 使用 bash 并让管道失败时正确返回非零
SHELL := /bin/bash
.SHELLFLAGS := -o pipefail -c

# 使用同一前缀的一整套 clang/llvm 14
CLANG_PREFIX  := /opt/clang-14
CXX           := $(CLANG_PREFIX)/bin/clang++
LLVMCONFIG    := $(CLANG_PREFIX)/bin/llvm-config

# 来自 llvm-config 的编译/链接选项（包含 -I/-L/-Wl 之类）
LLVM_CXXFLAGS := $(shell $(LLVMCONFIG) --cxxflags)
LLVM_LDFLAGS  := $(shell $(LLVMCONFIG) --ldflags)

# 仅用一个库就行（包含所有 clang/llvm C++ API）
CLANG_LIBS    := -lclang-cpp

# 运行时能找到 so
RPATH         := -Wl,-rpath,$(CLANG_PREFIX)/lib

CXXFLAGS := -std=c++17 -pthread $(LLVM_CXXFLAGS) -I$(CLANG_PREFIX)/include
LDFLAGS  := $(LLVM_LDFLAGS) -L$(CLANG_PREFIX)/lib $(RPATH)
LDLIBS   := $(CLANG_LIBS)

LOG_FILE := analyze-compile.log
OBJ_FILES := helper.o

all: analyze usage

analyze: analyze.cpp $(OBJ_FILES)
	$(CXX) $^ -o $@ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) 2>&1 | tee $(LOG_FILE)

usage: usage.cpp $(OBJ_FILES)
	$(CXX) $^ -o $@ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) 2>&1 | tee -a $(LOG_FILE)

# 编译 .o 时不要带链接库，只用编译器与头文件/宏选项
helper.o: helper.cpp helper.hpp
	$(CXX) -c $< -o $@ $(CXXFLAGS) 2>&1 | tee -a $(LOG_FILE)

clean:
	rm -f analyze usage $(OBJ_FILES) $(LOG_FILE)

.PHONY: all clean

