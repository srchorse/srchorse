CC ?= cc
CFLAGS ?= -O2
MYSQL_CFLAGS := $(shell mysql_config --cflags 2>/dev/null)
MYSQL_LIBS := $(shell mysql_config --libs 2>/dev/null)
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)
MAKEFLAGS += -j$(JOBS)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -I. -pthread
LDFLAGS ?=
LDFLAGS += -pthread

REPO_ROOT := ../..
BIN_DIR := $(REPO_ROOT)/bin
IMPORT_BIN := $(BIN_DIR)/db-import
IMPORT_SRC := main.c

.PHONY: all clean

all: $(IMPORT_BIN)

$(IMPORT_BIN): $(IMPORT_SRC)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(MYSQL_CFLAGS) $(IMPORT_SRC) -o $@ $(LDFLAGS) $(MYSQL_LIBS)

clean:
	rm -f $(IMPORT_BIN) $(BIN_DIR)/import
