CC      = cc
CFLAGS  = -O2 -Wall -Wno-unused-result -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
LDFLAGS = -lcurl -lm

all: agent agent.v3

agent: agent.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent agent.c cJSON.c $(LDFLAGS)

agent.v3: agent.v3.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v3 agent.v3.c cJSON.c $(LDFLAGS)

test: agent.v3
	./eval.sh ./agent.v3

clean:
	rm -f agent agent.v2 agent.v3 agent.v4 agent.v5 *.o

.PHONY: all clean test
