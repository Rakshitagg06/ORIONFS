CC = gcc
CFLAGS = -Wall -Wextra -pthread -I..
LDFLAGS = -pthread

TARGET = naming_server
COMMON_DIR = ../common
SOURCES = naming_server.c nm_storage.c $(COMMON_DIR)/utils.c $(COMMON_DIR)/network.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

run: $(TARGET)
	./$(TARGET) 8080

.PHONY: all clean run
