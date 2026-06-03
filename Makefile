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

IPC_ROOT       := ipc
BUILD_ROOT     := build
IPC_TEST_DIR   := $(BUILD_ROOT)/ipc/test
THIRD_PARTY    := third_party

IPC_INC        := -I$(IPC_ROOT)/src -I$(THIRD_PARTY)/tomlplusplus
IPC_TEST_FLAGS := $(IPC_INC) -pthread
# -lrt: required by ipc/shm_spsc.hpp (shm_open / mmap).
# Order: link flags after sources/object files so they resolve symbols.
IPC_TEST_LDFLAGS := -lrt

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

# Phase B unit tests (plain-C++ asserts, no test framework dep).
IPC_TOPOLOGY_LOADER_TEST  := $(IPC_TEST_DIR)/topology_loader_test
IPC_LAST_VALUE_CACHE_TEST := $(IPC_TEST_DIR)/last_value_cache_test

# Phase F C5 Scope B — declarative topic registry.
IPC_TOPIC_REGISTRY_TEST   := $(IPC_TEST_DIR)/topic_registry_test

# Phase C unit tests.
IPC_SHM_BACKPRESSURE_TEST := $(IPC_TEST_DIR)/shm_backpressure_test

# RouterFrame v2 layout test (ADR 0008).
IPC_FRAME_TEST            := $(IPC_TEST_DIR)/frame_test

# Phase D1 unit tests.
IPC_DATAGRAM_SEQ_TEST     := $(IPC_TEST_DIR)/datagram_seq_test
IPC_ROUTING_TEST          := $(IPC_TEST_DIR)/routing_test
IPC_RESOLVER_TEST         := $(IPC_TEST_DIR)/resolver_test
IPC_CLI_ARGS_TEST         := $(IPC_TEST_DIR)/cli_args_test

# Phase D2 integration tests.
IPC_SLOW_RECORDER_TEST    := $(IPC_TEST_DIR)/slow_recorder_test
IPC_BURST_SENSOR_TEST     := $(IPC_TEST_DIR)/burst_sensor_test
IPC_PROFILE_SWITCH_TEST   := $(IPC_TEST_DIR)/profile_switch_test
IPC_ROUTER_RESTART_TEST   := $(IPC_TEST_DIR)/router_restart_test

# Phase G — per-topic dispatch integration test (ADR 0013).
IPC_TOPIC_DISPATCH_TEST   := $(IPC_TEST_DIR)/topic_dispatch_test

# Phase H — mixed-transport router integration test (ADR 0014).
IPC_MIXED_TRANSPORT_TEST  := $(IPC_TEST_DIR)/mixed_transport_test

# Phase D4 fault injection.
IPC_FAULT_INJECTION_TEST  := $(IPC_TEST_DIR)/fault_injection_test

ALL_TARGETS := \
	$(IPC_ECHO_TEST) \
	$(IPC_ECHO_SERVER) \
	$(IPC_ECHO_CLIENT) \
	$(IPC_ECHO_CLIENT_BENCHMARK) \
	$(IPC_ROUTER_SERVER) \
	$(IPC_ROUTER_CLIENT) \
	$(IPC_ROUTER_TEST) \
	$(IPC_TOPOLOGY_LOADER_TEST) \
	$(IPC_LAST_VALUE_CACHE_TEST) \
	$(IPC_SHM_BACKPRESSURE_TEST) \
	$(IPC_FRAME_TEST) \
	$(IPC_DATAGRAM_SEQ_TEST) \
	$(IPC_ROUTING_TEST) \
	$(IPC_RESOLVER_TEST) \
	$(IPC_CLI_ARGS_TEST) \
	$(IPC_SLOW_RECORDER_TEST) \
	$(IPC_BURST_SENSOR_TEST) \
	$(IPC_PROFILE_SWITCH_TEST) \
	$(IPC_ROUTER_RESTART_TEST) \
	$(IPC_FAULT_INJECTION_TEST) \
	$(IPC_TOPIC_REGISTRY_TEST) \
	$(IPC_TOPIC_DISPATCH_TEST) \
	$(IPC_MIXED_TRANSPORT_TEST)

.PHONY: all clean debug help test-ipc test-ipc-shm test-router \
	test-ipc-unit test-topology-loader test-last-value-cache \
	test-shm-backpressure test-frame \
	test-datagram-seq test-routing test-resolver test-cli-args \
	test-topic-registry \
	test-ipc-integration test-slow-recorder test-burst-sensor \
	test-profile-switch test-router-restart test-fault-injection \
	test-topic-dispatch test-mixed-transport \
	build-ipc-unit build-ipc-integration \
	test-soak test-leak-check test-idle-cpu test-latency-histogram \
	ci \
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

$(IPC_ROUTER_CLIENT): $(IPC_ROOT)/test/router_client.cpp $(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) $(IPC_ROOT)/test/router_client_config.h $(IPC_ROOT)/test/router_cli_args.hpp | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ROUTER_TEST): $(IPC_ROOT)/test/router_test.cpp $(IPC_ROUTER_HEADERS) $(IPC_ROOT)/test/router_client_config.h | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase B unit-test binaries. topology_loader_test transitively pulls in
# toml.hpp from third_party/tomlplusplus/ via -I above.
$(IPC_TOPOLOGY_LOADER_TEST): $(IPC_ROOT)/test/topology_loader_test.cpp \
		$(IPC_ROUTER_HEADERS) $(THIRD_PARTY)/tomlplusplus/toml.hpp | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_LAST_VALUE_CACHE_TEST): $(IPC_ROOT)/test/last_value_cache_test.cpp \
		$(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase F C5 Scope B — topic registry. Pulls in toml++ via topology_loader.hpp.
$(IPC_TOPIC_REGISTRY_TEST): $(IPC_ROOT)/test/topic_registry_test.cpp \
		$(IPC_ROUTER_HEADERS) $(THIRD_PARTY)/tomlplusplus/toml.hpp | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase C unit test: SHM router backpressure + metrics (ADR 0006).
$(IPC_SHM_BACKPRESSURE_TEST): $(IPC_ROOT)/test/shm_backpressure_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# RouterFrame v2 layout / accessor test (ADR 0008). No SHM, no fork.
$(IPC_FRAME_TEST): $(IPC_ROOT)/test/frame_test.cpp \
		$(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase D1 unit tests — all pure in-process, no fork, no SHM, no kernel.
$(IPC_DATAGRAM_SEQ_TEST): $(IPC_ROOT)/test/datagram_seq_test.cpp \
		$(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ROUTING_TEST): $(IPC_ROOT)/test/routing_test.cpp \
		$(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_RESOLVER_TEST): $(IPC_ROOT)/test/resolver_test.cpp \
		$(IPC_ROUTER_HEADERS) $(IPC_IPC_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_CLI_ARGS_TEST): $(IPC_ROOT)/test/cli_args_test.cpp \
		$(IPC_ROOT)/test/router_cli_args.hpp | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase D2 integration tests.
#
# slow_recorder_test / burst_sensor_test use ShmRouterLink + std::thread
# directly (no fork). profile_switch_test loads real TOML profiles, so it
# pulls in toml.hpp. router_restart_test execs $(IPC_ROUTER_SERVER), so it
# depends on that binary being built first.
$(IPC_SLOW_RECORDER_TEST): $(IPC_ROOT)/test/slow_recorder_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_BURST_SENSOR_TEST): $(IPC_ROOT)/test/burst_sensor_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase G — topic_dispatch_test uses ShmRouterLink + std::thread directly
# (no fork, no toml), same shape as burst_sensor_test.
$(IPC_TOPIC_DISPATCH_TEST): $(IPC_ROOT)/test/topic_dispatch_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase H — mixed_transport_test drives MixedRouterServer with SHM + UDS peers
# directly via std::thread (no fork, no toml), same shape as topic_dispatch_test.
$(IPC_MIXED_TRANSPORT_TEST): $(IPC_ROOT)/test/mixed_transport_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_PROFILE_SWITCH_TEST): $(IPC_ROOT)/test/profile_switch_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) \
		$(THIRD_PARTY)/tomlplusplus/toml.hpp | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

$(IPC_ROUTER_RESTART_TEST): $(IPC_ROOT)/test/router_restart_test.cpp \
		| $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -pthread $(IPC_TEST_LDFLAGS) -o $@ $<

# Phase D4 — fault injection. Pulls in toml.hpp (Scenario 5 calls
# load_topology_from_toml_string directly) and the router_server
# binary (Scenario 6 spawns it via execv for the SIGKILL test).
$(IPC_FAULT_INJECTION_TEST): $(IPC_ROOT)/test/fault_injection_test.cpp \
		$(IPC_IPC_HEADERS) $(IPC_ROUTER_HEADERS) \
		$(THIRD_PARTY)/tomlplusplus/toml.hpp | $(IPC_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IPC_TEST_FLAGS) $(IPC_TEST_LDFLAGS) -o $@ $<

ipc-echo-server:            $(IPC_ECHO_SERVER)
ipc-echo-client:            $(IPC_ECHO_CLIENT)
ipc-echo-client-benchmark:  $(IPC_ECHO_CLIENT_BENCHMARK)
ipc-router-server:          $(IPC_ROUTER_SERVER)
ipc-router-client:          $(IPC_ROUTER_CLIENT)

# test-ipc runs the in-process UDP + UDS + SHM echo benchmark. Phase C made
# the SHM client interruptible (try_send + try_recv + stop check), so the
# IPC_SKIP_SHM escape hatch is no longer needed by default. Set IPC_SKIP_SHM=1
# explicitly only if your CI host disallows /dev/shm.
test-ipc: $(IPC_ECHO_TEST)
	./$(IPC_ECHO_TEST)

# Backwards-compatible alias for prior tooling that called `make test-ipc-shm`
# during Phase A/B to opt back in to SHM. Identical to test-ipc now.
test-ipc-shm: $(IPC_ECHO_TEST)
	./$(IPC_ECHO_TEST)

test-router: $(IPC_ROUTER_TEST) $(IPC_ROUTER_SERVER) $(IPC_ROUTER_CLIENT)
	./$(IPC_ROUTER_TEST)

# Phase B unit tests — fast, no SHM, no forks. Run on every change.
test-topology-loader: $(IPC_TOPOLOGY_LOADER_TEST)
	./$(IPC_TOPOLOGY_LOADER_TEST)

test-last-value-cache: $(IPC_LAST_VALUE_CACHE_TEST)
	./$(IPC_LAST_VALUE_CACHE_TEST)

test-shm-backpressure: $(IPC_SHM_BACKPRESSURE_TEST)
	./$(IPC_SHM_BACKPRESSURE_TEST)

test-frame: $(IPC_FRAME_TEST)
	./$(IPC_FRAME_TEST)

test-datagram-seq: $(IPC_DATAGRAM_SEQ_TEST)
	./$(IPC_DATAGRAM_SEQ_TEST)

test-routing: $(IPC_ROUTING_TEST)
	./$(IPC_ROUTING_TEST)

test-resolver: $(IPC_RESOLVER_TEST)
	./$(IPC_RESOLVER_TEST)

test-cli-args: $(IPC_CLI_ARGS_TEST)
	./$(IPC_CLI_ARGS_TEST)

test-topic-registry: $(IPC_TOPIC_REGISTRY_TEST)
	./$(IPC_TOPIC_REGISTRY_TEST)

# Build-only aggregates so `make -jN` can fan out the test compiles
# without also fanning out the test invocations themselves (some Phase D
# scenarios bind well-known /dev/shm and /tmp/*.sock paths and would
# clobber each other if run concurrently — see ipc/MODULE.md).
.PHONY: build-ipc-unit build-ipc-integration
build-ipc-unit: $(IPC_FRAME_TEST) $(IPC_TOPOLOGY_LOADER_TEST) \
	$(IPC_LAST_VALUE_CACHE_TEST) $(IPC_SHM_BACKPRESSURE_TEST) \
	$(IPC_DATAGRAM_SEQ_TEST) $(IPC_ROUTING_TEST) $(IPC_RESOLVER_TEST) \
	$(IPC_CLI_ARGS_TEST) $(IPC_TOPIC_REGISTRY_TEST)

build-ipc-integration: $(IPC_SLOW_RECORDER_TEST) $(IPC_BURST_SENSOR_TEST) \
	$(IPC_PROFILE_SWITCH_TEST) $(IPC_ROUTER_RESTART_TEST) \
	$(IPC_FAULT_INJECTION_TEST) $(IPC_TOPIC_DISPATCH_TEST) \
	$(IPC_MIXED_TRANSPORT_TEST) $(IPC_ROUTER_SERVER)

# Sequence sub-makes so the children run serially even when the parent
# was invoked with -j. test-ipc-unit's children are independent today
# (test-shm-backpressure uses test-private SHM names), but keeping a
# single ordering rule for both unit and integration aggregates makes
# the failure mode predictable.
test-ipc-unit: build-ipc-unit
	@$(MAKE) test-frame
	@$(MAKE) test-topology-loader
	@$(MAKE) test-last-value-cache
	@$(MAKE) test-shm-backpressure
	@$(MAKE) test-datagram-seq
	@$(MAKE) test-routing
	@$(MAKE) test-resolver
	@$(MAKE) test-cli-args
	@$(MAKE) test-topic-registry

# Phase D2 — integration scenarios (no fork: slow_recorder, burst_sensor,
# profile_switch; subprocess fork: router_restart, fault_injection).
# These must run one at a time: profile_switch_test, router_restart_test
# and fault_injection_test all bind /rim_router_* SHM regions via
# jetson_prod.toml and would race for the same names under -jN.
test-slow-recorder: $(IPC_SLOW_RECORDER_TEST)
	./$(IPC_SLOW_RECORDER_TEST)

test-burst-sensor: $(IPC_BURST_SENSOR_TEST)
	./$(IPC_BURST_SENSOR_TEST)

test-profile-switch: $(IPC_PROFILE_SWITCH_TEST)
	./$(IPC_PROFILE_SWITCH_TEST)

test-router-restart: $(IPC_ROUTER_RESTART_TEST) $(IPC_ROUTER_SERVER)
	./$(IPC_ROUTER_RESTART_TEST)

test-fault-injection: $(IPC_FAULT_INJECTION_TEST) $(IPC_ROUTER_SERVER)
	./$(IPC_FAULT_INJECTION_TEST)

test-topic-dispatch: $(IPC_TOPIC_DISPATCH_TEST)
	./$(IPC_TOPIC_DISPATCH_TEST)

test-mixed-transport: $(IPC_MIXED_TRANSPORT_TEST)
	./$(IPC_MIXED_TRANSPORT_TEST)

test-ipc-integration: build-ipc-integration
	@$(MAKE) test-slow-recorder
	@$(MAKE) test-burst-sensor
	@$(MAKE) test-profile-switch
	@$(MAKE) test-router-restart
	@$(MAKE) test-fault-injection
	@$(MAKE) test-topic-dispatch
	@$(MAKE) test-mixed-transport

# Phase D3 — stress / soak scripts. These wrap the existing test
# binaries and add timing, leak detection, and CPU regression gates.
# Each script is self-cleaning (rm -f /dev/shm/rim_* on exit).
#
#   test-soak [N=10]      loop test-router N times; abort on first fail
#   test-leak-check       count rim_* resources around full test pass
#   test-idle-cpu         60s pidstat on jetson_prod router; <= 5% gate
#   test-latency-histogram throughput variance probe (optional)
SOAK_ITERATIONS ?= 10

test-soak: $(IPC_ROUTER_TEST) $(IPC_ROUTER_SERVER) $(IPC_ROUTER_CLIENT)
	bash robotics-ipc-module/scripts/soak_router.sh $(SOAK_ITERATIONS)

test-leak-check: all
	bash robotics-ipc-module/scripts/shm_leak_check.sh

test-idle-cpu: $(IPC_ROUTER_SERVER)
	bash robotics-ipc-module/scripts/idle_cpu_check.sh

test-latency-histogram: $(IPC_ECHO_TEST)
	bash robotics-ipc-module/scripts/latency_histogram.sh

# `make ci` mirrors what .github/workflows/ci.yml runs on every PR.
# Run this before pushing if you want to be sure the gate is green
# without waiting on a runner. Total wall time on a developer box is
# typically well under 1 min.
#
# Order matters: build first (so test stages fail on the cheapest signal
# first), then unit (fastest), integration, router scenarios, and finally
# the leak check (the strictest gate — re-runs the suites and asserts
# zero leftover /dev/shm or /tmp socket files).
#
# Each stage is dispatched via a recursive $(MAKE) so the sub-makes run
# in series even when the parent is invoked with `-j`. Running the test
# suites in parallel would have them stomp on each other's /dev/shm and
# /tmp/*.sock paths (the router scenarios in particular bind well-known
# names) and produce flaky 0-frame failures.
ci:
	@$(MAKE) all
	@$(MAKE) test-ipc-unit
	@$(MAKE) test-ipc-integration
	@$(MAKE) test-router
	@$(MAKE) test-leak-check

debug: CXXFLAGS += -g -O0
debug: clean all

clean:
	rm -rf $(BUILD_ROOT)

help:
	@echo "Robotics IPC module"
	@echo ""
	@echo "  make [all]               build every demo/test binary under $(BUILD_ROOT)/"
	@echo "  make test-ipc            build + run full UDP/UDS/SHM echo benchmark (Phase C: SHM is interruptible)"
	@echo "  make test-ipc-shm        alias for test-ipc (kept for older docs)"
	@echo "  make test-router         build + run router scenario test (uds/udp/shm)"
	@echo "  make test-ipc-unit       all unit tests (frame v2, topology loader, LV cache, shm backpressure, D1 unit suites)"
	@echo "  make test-frame          build + run RouterFrame v2 layout unit test only"
	@echo "  make test-topology-loader build + run TOML topology loader unit test only"
	@echo "  make test-last-value-cache build + run last-value-cache unit test only"
	@echo "  make test-shm-backpressure build + run Phase C SHM backpressure unit test only"
	@echo "  make test-datagram-seq   build + run subscriber-side seq / gap tracker unit test only"
	@echo "  make test-routing        build + run route_targets_for edge-case unit test only"
	@echo "  make test-resolver       build + run UDS/UDP peer_id_from_recv unit test only"
	@echo "  make test-cli-args       build + run log_path_for_role arity regression unit test only"
	@echo "  make test-ipc-integration Phase D2 integration scenarios (slow recorder, burst sensor, profile switch, router restart)"
	@echo "  make test-slow-recorder  Phase D2 — per-peer drop attribution under slow subscriber"
	@echo "  make test-burst-sensor   Phase D2 — SourceSeqTracker accounting under burst publish"
	@echo "  make test-profile-switch Phase D2 — load jetson_prod + hil profiles back-to-back"
	@echo "  make test-router-restart Phase D2 — SIGKILL router; next bind succeeds (SHM cleanup)"
	@echo "  make test-fault-injection Phase D4 — truncated UDP / unknown source / wrong UDS path / SIGKILL mid-traffic"
	@echo "  make test-soak           Phase D3 — loop test-router (SOAK_ITERATIONS=10 default)"
	@echo "  make test-leak-check     Phase D3 — assert no leaked /dev/shm/ or /tmp/*.sock files"
	@echo "  make test-idle-cpu       Phase D3 — pidstat router_server, assert <= 5% CPU"
	@echo "  make test-latency-histogram Phase D3 — throughput variance probe (optional)"
	@echo "  make ci                  Phase D — mirror the GitHub Actions PR gate locally"
	@echo "  make ipc-router-server   build router server demo only"
	@echo "  make ipc-router-client   build router client demo only"
	@echo "  make debug               rebuild all with -g -O0"
	@echo "  make clean               rm -rf $(BUILD_ROOT)"
