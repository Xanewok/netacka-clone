CXX = g++
CXXFLAGS = -Wall -std=c++14

ifeq ($(DEBUG), 1)
CXXFLAGS += -g3 -ggdb
else
CXXFLAGS += -O2
endif

BINS = siktacka-server siktacka-client
OBJS = rand.o util.o protocol.o crc32.o map.o

all: $(BINS)

siktacka-server: server.o $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $< -o $@ -lpthread

siktacka-client: client.o $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $< -o $@ -lpthread

%.o: %.cc
	$(CXX) -c $(CXXFLAGS) $< -o $@

clean:
	rm -f $(BINS) *.o
