CC = gcc
CFLAGS = -Wall -Wextra -pthread -I..
LDFLAGS = -pthread

TARGET = storage_server
COMMON_DIR = ../common
SOURCES = storage_server.c $(COMMON_DIR)/utils.c $(COMMON_DIR)/network.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

run: $(TARGET)
	# Example run; provide NM IP explicitly for multi-host use
	./$(TARGET) SS1 8080 192.168.1.10 9001 10001

.PHONY: all clean run
