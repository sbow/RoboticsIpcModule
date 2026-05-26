# Robotics IPC module — top-level Makefile
#
# Layout (Phase A):
#   ipc/src/     header-only library (transports, router, framing)
#   ipc/test/    demos + integration tests (NOT shipped as module API)
#   build/       all artefacts (gitignored)
#
# Public entry points (apps include exactly these):
#   #include "ipc.hpp"             # transports + buffer + endpoint
#   #include "router_protocol.hpp" # router + frame + factory + topology
#   #include "router_app.h"        # app-only: signal handlers, logger
#
# Apps MUST NOT include ipc/test/router_client_config.h
# (demo wiring, not part of the module contract — see ipc/MODULE.md).

CXX      ?= g++
CXXSTD   ?= -std=c++20
WARN     ?= -Wall -Wextra
CXXFLAGS ?= $(CXXSTD) $(WARN)
LDFLAGS  ?=

IPC_ROOT      := ipc
BUILD_ROOT    := build
IPC_TEST_DIR  := $(BUILD_ROOT)/ipc/test

IPC_INC       := -I$(IPC_ROOT)/src
IPC_TEST_FLAGS   := $(IPC_INC) -pthread
IPC_TEST_LDFLAGS := -lrt    # required by ipc/shm_spsc.hpp (shm_open / mmap)

IPC_IPC_HEADERS := \
	$(IPC_ROOT)/src/ipc.hpp \
	$(IPC_ROOT)/src/ipc.h \
	$(wildcard $(IPC_ROOT)/src/ipc/*.hpp)

IPC_ROUTER_HEADERS := \
	$(IPC_ROOT)/src/router_protocol.hpp \
	$(IPC_ROOT)/src/router_protocol.h \
	$(IPC_ROOT)/src/router_app.h \
	$(wildcard $(IPC_ROOT)/src/router/*.hpp)

IPC_ECHO_TEST             := $(IPC_TEST_DIR)/echo_tests
IPC_ECHO_SERVER           := $(IPC_TEST_DIR)/echo_server
IPC_ECHO_CLIENT           := $(IPC_TEST_DIR)/echo_client
IPC_ECHO_CLIENT_BENCHMARK := $(IPC_TEST_DIR)/echo_client_benchmark
IPC_ROUTER_SERVER         := $(IPC_TEST_DIR)/router_server
IPC_ROUTER_CLIENT         := $(IPC_TEST_DIR)/router_client
IPC_ROUTER_TEST           := $(IPC_TEST_DIR)/router_test

ALL_TARGETS := \
	$(IPC_ECHO_TEST) \
	$(IPC_ECHO_SERVER) \
	$(IPC_ECHO_CLIENT) \
	$(IPC_ECHO_CLIENT_BENCHMARK) \
	$(IPC_ROUTER_SERVER) \
	$(IPC_ROUTER_CLIENT) \
	$(IPC_ROUTER_TEST)

.PHONY: all clean debug help test-ipc test-ipc-shm test-router \
	ipc-echo-server ipc-echo-client ipc-echo-client-benchmark \
	ipc-router-server ipc-router-client

.DEFAULT_GOAL := all

all: $(ALL_TARGETS)

$(IPC_TEST_DIR):
	mkdir -p $@

$(IPC_ECHO_TEST): $(IPC_ROOT)/test/echo_tests.cpp $(IPC_IPC_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ECHO_SERVER): $(IPC_ROOT)/test/echo_server.cpp $(IPC_IPC_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ECHO_CLIENT): $(IPC_ROOT)/test/echo_client.cpp $(IPC_IPC_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ECHO_CLIENT_BENCHMARK): $(IPC_ROOT)/test/echo_client_benchmark.cpp $(IPC_IPC_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ROUTER_SERVER): $(IPC_ROOT)/test/router_server.cpp $(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) $(IPC_ROOT)/test/router_client_config.h | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ROUTER_CLIENT): $(IPC_ROOT)/test/router_client.cpp $(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) $(IPC_ROOT)/test/router_client_config.h | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ROUTER_TEST): $(IPC_ROOT)/test/router_test.cpp $(IPC_ROUTER_HEADERS) $(IPC_ROOT)/test/router_client_config.h | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

ipc-echo-server:            $(IPC_ECHO_SERVER)
ipc-echo-client:            $(IPC_ECHO_CLIENT)
ipc-echo-client-benchmark:  $(IPC_ECHO_CLIENT_BENCHMARK)
ipc-router-server:          $(IPC_ROUTER_SERVER)
ipc-router-client:          $(IPC_ROUTER_CLIENT)

# test-ipc runs UDP + UDS in-process echo benchmarks. The SHM portion is
# skipped via IPC_SKIP_SHM=1 because the demo client uses blocking
# shm_push_slot, which spins on a full ring (Phase C will replace it with
# try_send + bounded wait). Use `make test-ipc-shm` to force the full run
# during Phase C work.
test-ipc: $(IPC_ECHO_TEST)
	IPC_SKIP_SHM=1 ./$(IPC_ECHO_TEST)

test-ipc-shm: $(IPC_ECHO_TEST)
	./$(IPC_ECHO_TEST)

test-router: $(IPC_ROUTER_TEST) $(IPC_ROUTER_SERVER) $(IPC_ROUTER_CLIENT)
	./$(IPC_ROUTER_TEST)

debug: CXXFLAGS += -g -O0
debug: clean all

clean:
	rm -rf $(BUILD_ROOT)

help:
	@echo "Robotics IPC module"
	@echo ""
	@echo "  make [all]               build every demo/test binary under $(BUILD_ROOT)/"
	@echo "  make test-ipc            build + run UDP/UDS echo benchmark (SHM skipped, Phase C)"
	@echo "  make test-ipc-shm        force full UDP/UDS/SHM echo benchmark (Phase C diagnostic)"
	@echo "  make test-router         build + run router scenario test (uds/udp/shm)"
	@echo "  make ipc-router-server   build router server demo only"
	@echo "  make ipc-router-client   build router client demo only"
	@echo "  make debug               rebuild all with -g -O0"
	@echo "  make clean               rm -rf $(BUILD_ROOT)"
