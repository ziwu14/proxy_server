HEADER_PATH=/code/header
LIB_PATH=/code/lib
# LIB_PATH2=/usr/lib/x86_64-linux-gnu
SHARED_LIB=-lboost_system -lboost_thread -lpthread -lboost_regex
PROG=proxy_server.cpp lru_cache.cpp

.PHONYE: clean all

all: clean proxy

proxy:
	clang++ -std=c++17 -I$(HEADER_PATH)  -L$(LIB_PATH) -Wl,-rpath-link=$(LIB_PATH) -o $@ $(PROG) $(SHARED_LIB)
clean:
	rm -rf proxy *~ *#
