CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -Iinc
SRC_DIR := src
OBJ_DIR := build
BIN_DIR := bin
TARGET  := $(BIN_DIR)/my_shell

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

run: $(TARGET)
	$(TARGET)

.PHONY: all clean run
-include $(DEPS)
