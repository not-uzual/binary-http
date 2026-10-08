# bserve -- binary-http static file server
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=
LDLIBS   ?= -lpthread

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
BIN := bserve

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN) ./www 9000

clean:
	rm -f $(OBJ) $(BIN)

src/main.o:       src/connection.hpp
src/connection.o: src/connection.hpp src/wire.hpp src/files.hpp
src/wire.o:       src/wire.hpp
src/files.o:      src/files.hpp
