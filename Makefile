CC=gcc
CFLAGS=-O2 -Wall -Wextra -std=c11 -pthread
LDFLAGS=

BIN_DIR=bin
BUILD_DIR=build

COMMON_SRC=$(wildcard common/*.c)
COMMON_OBJ=$(patsubst common/%.c,$(BUILD_DIR)/common/%.o,$(COMMON_SRC))

NM_SRC=$(wildcard nm/*.c)
NM_OBJ=$(patsubst nm/%.c,$(BUILD_DIR)/nm/%.o,$(NM_SRC)) $(COMMON_OBJ)

SS_SRC=$(wildcard ss/*.c)
SS_OBJ=$(patsubst ss/%.c,$(BUILD_DIR)/ss/%.o,$(SS_SRC)) $(COMMON_OBJ)

CLI_SRC=$(wildcard client/*.c)
CLI_OBJ=$(patsubst client/%.c,$(BUILD_DIR)/client/%.o,$(CLI_SRC)) $(COMMON_OBJ)

.PHONY: all clean dirs

all: dirs $(BIN_DIR)/nm $(BIN_DIR)/ss $(BIN_DIR)/client

$(BIN_DIR)/nm: $(NM_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/ss: $(SS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/client: $(CLI_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/common/%.o: common/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nm/%.o: nm/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ss/%.o: ss/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/client/%.o: client/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(BUILD_DIR)/common/%.o $(BUILD_DIR)/nm/%.o $(BUILD_DIR)/ss/%.o $(BUILD_DIR)/client/%.o

dirs:
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)/common $(BUILD_DIR)/nm $(BUILD_DIR)/ss $(BUILD_DIR)/client

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
