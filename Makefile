# Makefile for the TSAM mail server and client.
# Running "make" builds both tsamserver and tsamclient.

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

all: tsamserver tsamclient

tsamserver: server.o protocol.o
	$(CXX) $(CXXFLAGS) -o $@ $^

tsamclient: client.o protocol.o
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp protocol.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f *.o tsamserver tsamclient

.PHONY: all clean
