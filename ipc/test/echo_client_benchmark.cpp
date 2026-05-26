#include "ipc.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <type_traits>

namespace {

constexpr uint16_t kDefaultUdpPort = 19000;
constexpr const char* kDefaultUdpHost = "127.0.0.1";
constexpr const char* kDefaultUdsServerPath = "/tmp/cpp_tricks_echo_server.sock";
constexpr const char* kDefaultUdsClientPath = "/tmp/cpp_tricks_echo_client.sock";
constexpr const char* kDefaultShmName = "/cpp_tricks_shm_echo";
constexpr int kDefaultDurationSec = 5;
constexpr int kRecvTimeoutMs = 200;

void usage(const char* prog) {
    std::cerr << "usage: " << prog << " udp [host] [port] [seconds]\n"
              << "       " << prog << " uds [server_path] [client_path] [seconds]\n"
              << "       " << prog << " shm [region_name] [seconds]\n";
}

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        throw std::runtime_error("setsockopt SO_RCVTIMEO failed");
    }
}

template<typename Transport, typename EchoClientType>
uint64_t run_benchmark(
    typename Transport::SendParams send_params,
    int duration_sec,
    const typename Transport::BindParams* client_bind = nullptr) {
    EchoClientType client = [&]() {
        if (client_bind) {
            return EchoClientType(*client_bind);
        }
        return EchoClientType();
    }();

    if constexpr (requires { client.fd(); }) {
        set_recv_timeout(client.fd(), kRecvTimeoutMs);
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(duration_sec);

    const char msg[] = "ping";
    char reply[64];
    uint64_t round_trips = 0;
    bool uds_client_bound = false;

    while (std::chrono::steady_clock::now() < deadline) {
        try {
            send_params.payload = Buffer::read_only(msg, sizeof(msg) - 1);
            if constexpr (std::is_same_v<Transport, Uds>) {
                if (!uds_client_bound) {
                    uds_client_bound = true;
                } else {
                    send_params.client_path.clear();
                }
            }

            Buffer response = client.exchange(
                send_params, Buffer::writable(reply, sizeof(reply)));

            if (response.size > 0) {
                ++round_trips;
            }
        } catch (const std::runtime_error&) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
    }

    return round_trips;
}

uint64_t run_udp(int argc, char* argv[]) {
    const char* host = (argc >= 3) ? argv[2] : kDefaultUdpHost;
    const uint16_t port = (argc >= 4)
        ? static_cast<uint16_t>(std::stoi(argv[3]))
        : kDefaultUdpPort;
    const int duration = (argc >= 5) ? std::stoi(argv[4]) : kDefaultDurationSec;

    const uint64_t trips = run_benchmark<Udp, UdpEchoClient>(
        Udp::SendParams{
            .host = host,
            .port = port,
            .payload = Buffer::read_only("ping", 3),
        },
        duration);

    std::cout << "UDP round trips in " << duration << "s: " << trips << '\n';
    return trips;
}

uint64_t run_uds(int argc, char* argv[]) {
    const std::string server_path = (argc >= 3) ? argv[2] : kDefaultUdsServerPath;
    const std::string client_path = (argc >= 4) ? argv[3] : kDefaultUdsClientPath;
    const int duration = (argc >= 5) ? std::stoi(argv[4]) : kDefaultDurationSec;

    ::unlink(client_path.c_str());

    const uint64_t trips = run_benchmark<Uds, UdsEchoClient>(
        Uds::SendParams{
            .server_path = server_path,
            .client_path = client_path,
            .payload = Buffer::read_only("ping", 3),
        },
        duration);

    ::unlink(client_path.c_str());

    std::cout << "UDS round trips in " << duration << "s: " << trips << '\n';
    return trips;
}

uint64_t run_shm(int argc, char* argv[]) {
    const char* name = (argc >= 3) ? argv[2] : kDefaultShmName;
    const int duration = (argc >= 4) ? std::stoi(argv[3]) : kDefaultDurationSec;

    const ShmSpsc::BindParams client_bind{.name = name, .create = false};
    const uint64_t trips = run_benchmark<ShmSpsc, ShmEchoClient>(
        ShmSpsc::SendParams{.payload = Buffer::read_only("ping", 3)},
        duration,
        &client_bind);

    std::cout << "SHM round trips in " << duration << "s: " << trips << '\n';
    return trips;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    try {
        if (std::strcmp(argv[1], "udp") == 0) {
            run_udp(argc, argv);
        } else if (std::strcmp(argv[1], "uds") == 0) {
            run_uds(argc, argv);
        } else if (std::strcmp(argv[1], "shm") == 0) {
            run_shm(argc, argv);
        } else {
            usage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
