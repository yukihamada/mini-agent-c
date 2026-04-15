CC      = cc
CFLAGS  = -O2 -Wall -Wno-unused-result -Wno-comment -std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
LDFLAGS = -lcurl -lm

all: agent agent.v3 agent.v4 agent.v5 agent.v6 agent.v7 agent.v8 agent.v9 agent.v10 agent.v11 server

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

agent.v11: agent.v11.c cJSON.c cJSON.h
	$(CC) $(CFLAGS) -o agent.v11 agent.v11.c cJSON.c $(LDFLAGS)

test: agent.v11
	@echo "=== compile check: agent.v11 ==="
	$(CC) $(CFLAGS) -o /tmp/agent-test agent.v11.c cJSON.c $(LDFLAGS) && echo "OK"
	@echo "=== eval ==="
	./eval.sh ./agent.v11

server: web/server.swift
	swiftc -O web/server.swift -o web/server-swift
	@echo "Swift server built: web/server-swift"

run: server agent.v11
	CPU_REFUSE_PCT=85 CPU_KILL_PCT=92 web/run.sh

clean:
	rm -f agent agent.v2 agent.v3 agent.v4 agent.v5 agent.v6 agent.v7 agent.v8 agent.v9 agent.v10 agent.v11 web/server-swift *.o

.PHONY: all clean test server run
