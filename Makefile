# Makefile for the C<< (C-Shift) compiler
#
# Requires: llvm-dev, lld
#   sudo apt install llvm-dev lld
#
# Build:
#   make
#
# Run on a .cll file:
#   ./cshift hello.cll -o hello
#   ./hello

CXX      ?= c++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra \
            $(shell llvm-config --cflags)
LDFLAGS  := $(shell llvm-config --ldflags) \
            $(shell llvm-config --libs core analysis target \
                x86 aarch64 riscv arm all-targets) \
            $(shell llvm-config --system-libs)

TARGET   := cshift
SRCS     := main.cpp

.PHONY: all clean example

all: $(TARGET)

$(TARGET): $(SRCS) compiler/lexer.hh compiler/parser.hh compiler/checker.hh compiler/codegen.hh compiler/ezllvm.h
	$(CXX) $(CXXFLAGS) $(SRCS) $(LDFLAGS) -o $@
	@echo "Built $(TARGET)"

# Quick smoke test
example: $(TARGET)
	@echo '--- Compiling hello.cll ---'
	./$(TARGET) examples/hello.cll -o hello
	@echo '--- Running ---'
	./hello

clean:
	rm -f $(TARGET) hello *.o *.ll *.s *.ez_tmp.o
