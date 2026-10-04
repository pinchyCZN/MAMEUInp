# MAMEUI netplay relay server - Linux build.
#
#     make                 build ./netplay_server
#     make run             build, then listen on 7000 for 2 players
#     make clean
#     sudo make install    to $(PREFIX)/bin
#
# Two translation units, no dependencies beyond libstdc++, no configure
# step. Peak memory is a few MB, so a 512 MB box builds this in seconds.
#
# NETPLAY_STANDALONE is what lets netplay_packet.cpp compile without MAME:
# it makes netplay_types.h define u8..u64 itself and suppresses the emu.h
# include. The server and the emulator therefore share ONE copy of the wire
# format rather than each carrying its own - see the comment in
# netplay_types.h. Building without it fails on a missing emu.h, which is
# the intended outcome rather than a confusing one.

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++14 -Wall
CPPFLAGS += -I. -DNETPLAY_STANDALONE
PREFIX   ?= /usr/local

PORT     ?= 7000
PLAYERS  ?= 2

BIN     := netplay_server
OBJ     := netplay_server.o netplay/netplay_packet.o
HEADERS := netplay/netplay_packet.h netplay/netplay_types.h

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN) -port $(PORT) -players $(PLAYERS)

install: $(BIN)
	install -D -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all run install clean
