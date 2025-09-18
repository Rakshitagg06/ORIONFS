CC = gcc
CFLAGS = -Wall -Wextra -pthread -I..
LDFLAGS = -pthread

TARGET = client
COMMON_DIR = ../common
SOURCES = client.c $(COMMON_DIR)/utils.c $(COMMON_DIR)/network.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

run: $(TARGET)
	# Example: connect to NM on another host
	./$(TARGET) 192.168.1.10 8080

.PHONY: all clean run
