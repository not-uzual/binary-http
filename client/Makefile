# bcurl -- binary-http client
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
BIN := bcurl

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

src/main.o:  src/codec.hpp src/url.hpp
src/codec.o: src/codec.hpp
src/url.o:   src/url.hpp
