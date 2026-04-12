CC      = cc
CFLAGS  = -O2 -Wall -Wno-unused-result -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
LDFLAGS = -lcurl -lm

all: agent agent.v3 agent.v4 agent.v5 agent.v6 agent.v7 agent.v8 agent.v9 agent.v10

agent: agent.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent agent.c cJSON.c $(LDFLAGS)

agent.v3: agent.v3.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v3 agent.v3.c cJSON.c $(LDFLAGS)

agent.v4: agent.v4.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v4 agent.v4.c cJSON.c $(LDFLAGS)

agent.v5: agent.v5.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v5 agent.v5.c cJSON.c $(LDFLAGS)

agent.v6: agent.v6.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v6 agent.v6.c cJSON.c $(LDFLAGS)

agent.v7: agent.v7.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v7 agent.v7.c cJSON.c $(LDFLAGS)

agent.v8: agent.v8.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v8 agent.v8.c cJSON.c $(LDFLAGS)

agent.v9: agent.v9.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v9 agent.v9.c cJSON.c $(LDFLAGS)

agent.v10: agent.v10.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v10 agent.v10.c cJSON.c $(LDFLAGS)

test: agent.v3
	./eval.sh ./agent.v3

clean:
	rm -f agent agent.v2 agent.v3 agent.v4 agent.v5 agent.v6 agent.v7 agent.v8 agent.v9 agent.v10 *.o

.PHONY: all clean test
