CC = gcc
CFLAGS = -Wall -O2 -I./src
LDFLAGS = -lcurl -lsqlite3

SRCS = main.c src/http/server.c src/db/database.c src/utils/logger.c
OBJS = $(SRCS:.c=.o)

TARGET = app

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)
