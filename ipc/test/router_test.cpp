#include "router_client_config.h"
#include "router_protocol.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>

namespace {

constexpr const char* kRouterServerBin = "build/ipc/test/router_server";
constexpr const char* kRouterClientBin = "build/ipc/test/router_client";
constexpr int kScenarioMs = 3000;
constexpr int kStopGraceMs = 2000;

void cleanup_paths() {
    ::unlink(kRouterUdsPath);
    ::unlink(kSensorUdsPath);
    ::unlink(kControllerUdsPath);
    ::unlink(kRecorderUdsPath);
    ::unlink(kRecorderLogPath);
    ::unlink(kControllerLogPath);
}

void cleanup_shm_paths() {
    ::shm_unlink(kRouterShmName);
    ::shm_unlink(kSensorShmName);
    ::shm_unlink(kControllerShmName);
    ::shm_unlink(kRecorderShmName);
}

void redirect_stdio_to_devnull() {
    const int fd = ::open("/dev/null", O_WRONLY);
    if (fd < 0) {
        return;
    }
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
}

const char* basename_path(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

pid_t spawn_child(const char* bin, std::initializer_list<const char*> args) {
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }

    redirect_stdio_to_devnull();
    std::vector<const char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(basename_path(bin));
    for (const char* arg : args) {
        argv.push_back(arg);
    }
    argv.push_back(nullptr);
    execv(bin, const_cast<char* const*>(argv.data()));
    _exit(1);
}

void stop_pid(pid_t pid) {
    if (pid <= 0) {
        return;
    }

    ::kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kStopGraceMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            return;
        }
        if (reaped == -1 && errno == ECHILD) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

void stop_all(pid_t recorder, pid_t controller, pid_t router) {
    stop_pid(recorder);
    stop_pid(controller);
    stop_pid(router);
}

int count_source(const std::string& path, int source_id) {
    std::ifstream in(path);
    if (!in) {
        return 0;
    }

    int count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto comma = line.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        if (std::stoi(line.substr(0, comma)) == source_id) {
            ++count;
        }
    }
    return count;
}

int run_scenario(const char* transport) {
    ::unlink(kRecorderLogPath);
    ::unlink(kControllerLogPath);
    if (std::strcmp(transport, "shm") == 0) {
        cleanup_shm_paths();
    } else {
        cleanup_paths();
    }
    std::cout << "starting " << transport << " router scenario..." << std::endl;

    char router_port[16];
    char sensor_port[16];
    char controller_port[16];
    char recorder_port[16];
    std::snprintf(router_port, sizeof(router_port), "%u", kRouterUdpPort);
    std::snprintf(sensor_port, sizeof(sensor_port), "%u", kSensorUdpPort);
    std::snprintf(controller_port, sizeof(controller_port), "%u", kControllerUdpPort);
    std::snprintf(recorder_port, sizeof(recorder_port), "%u", kRecorderUdpPort);

    const bool uds = std::strcmp(transport, "uds") == 0;
    const bool shm = std::strcmp(transport, "shm") == 0;

    const pid_t router = shm
        ? spawn_child(kRouterServerBin, {"shm"})
        : uds
            ? spawn_child(kRouterServerBin, {"uds", kRouterUdsPath})
            : spawn_child(kRouterServerBin, {"udp", router_port});
    if (router < 0) {
        std::cerr << transport << " router test: fork router failed\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const pid_t recorder = shm
        ? spawn_child(kRouterClientBin, {"recorder", "shm", kRecorderLogPath})
        : uds
            ? spawn_child(kRouterClientBin,
                {"recorder", "uds", kRecorderUdsPath, kRecorderLogPath})
            : spawn_child(kRouterClientBin,
                {"recorder", "udp", recorder_port, kRecorderLogPath});
    if (recorder < 0) {
        stop_pid(router);
        std::cerr << transport << " router test: fork recorder failed\n";
        return 1;
    }

    const pid_t controller = shm
        ? spawn_child(kRouterClientBin, {"controller", "shm", kControllerLogPath})
        : uds
            ? spawn_child(kRouterClientBin,
                {"controller", "uds", kRouterUdsPath, kControllerUdsPath, kControllerLogPath})
            : spawn_child(kRouterClientBin,
                {"controller", "udp", router_port, controller_port, kControllerLogPath});
    if (controller < 0) {
        stop_all(recorder, 0, router);
        std::cerr << transport << " router test: fork controller failed\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const pid_t sensor = shm
        ? spawn_child(kRouterClientBin, {"sensor", "shm"})
        : uds
            ? spawn_child(kRouterClientBin,
                {"sensor", "uds", kRouterUdsPath, kSensorUdsPath})
            : spawn_child(kRouterClientBin,
                {"sensor", "udp", router_port, sensor_port});
    if (sensor < 0) {
        stop_all(recorder, controller, router);
        std::cerr << transport << " router test: fork sensor failed\n";
        return 1;
    }

    if (waitpid(sensor, nullptr, 0) < 0) {
        stop_all(recorder, controller, router);
        std::cerr << transport << " router test: wait sensor failed\n";
        return 1;
    }

    std::cout << "  " << transport << ": waiting for traffic..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(kScenarioMs));

    stop_all(recorder, controller, router);

    const int c_from_a = count_source(kRecorderLogPath, kEndpointSensor);
    const int c_from_b = count_source(kRecorderLogPath, kEndpointController);
    const int b_from_a = count_source(kControllerLogPath, kEndpointSensor);

    std::cout << transport << " router: recorder saw "
              << c_from_a << " sensor, " << c_from_b << " controller; "
              << "controller saw " << b_from_a << " sensor\n";

    if (c_from_a < 5 || c_from_b < 3 || b_from_a < 5) {
        std::cerr << transport << " router test failed\n";
        return 1;
    }

    std::cout << transport << " router test passed\n";
    return 0;
}

}  // namespace

int main() {
    setenv("ROUTER_TEST", "1", 1);
    const int uds_rc = run_scenario("uds");
    const int udp_rc = run_scenario("udp");
    const int shm_rc = run_scenario("shm");
    cleanup_paths();
    cleanup_shm_paths();
    return (uds_rc == 0 && udp_rc == 0 && shm_rc == 0) ? 0 : 1;
}
