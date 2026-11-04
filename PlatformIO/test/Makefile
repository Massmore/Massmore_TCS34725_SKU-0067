# Host-side test suite for Massmore_TCS34725 — รันบน PC / Mac ไม่ต้องมีบอร์ด
#   make        build + run
#   make clean
# จำลองชิป TCS34725 (command byte, AEN/AVALID timing, AINT + persistence, WEN/WLONG)
CXX      ?= g++
CXXFLAGS ?= -std=gnu++17 -O0 -g -Wall -Wextra
SRCDIR   := ../lib/Massmore_TCS34725/src
SOURCES  := test_massmore_tcs34725.cpp $(SRCDIR)/Massmore_TCS34725.cpp $(SRCDIR)/Massmore_TCS34725_Color.cpp

all: run
testrun: $(SOURCES)
	$(CXX) $(CXXFLAGS) -Istub -I$(SRCDIR) -o $@ $(SOURCES)
run: testrun
	./testrun
clean:
	rm -f testrun
.PHONY: all run clean
