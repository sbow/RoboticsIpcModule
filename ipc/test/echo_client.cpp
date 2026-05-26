#include "ipc.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr uint16_t kDefaultUdpPort = 19000;
constexpr const char* kDefaultUdpHost = "127.0.0.1";
constexpr const char* kDefaultUdsServerPath = "/tmp/cpp_tricks_echo_server.sock";
constexpr const char* kDefaultUdsClientPath = "/tmp/cpp_tricks_echo_client.sock";
constexpr int kDefaultRoundTrips = 1;
constexpr const char* kMessage = "ping";

void usage(const char* prog) {
    std::cerr << "usage: " << prog << " udp [host] [port] [count]\n"
              << "       " << prog << " uds [server_path] [client_path] [count]\n";
}

template<typename Transport, typename EchoClient>
void run_round_trips(typename Transport::SendParams send_params, int count) {
    EchoClient client;

    char reply[64];
    bool uds_client_bound = false;

    for (int i = 0; i < count; ++i) {
        send_params.payload = Buffer::read_only(kMessage, std::strlen(kMessage));
        if constexpr (std::is_same_v<Transport, Uds>) {
            if (!uds_client_bound) {
                uds_client_bound = true;
            } else {
                send_params.client_path.clear();
            }
        }

        Buffer response = client.exchange(
            send_params, Buffer::writable(reply, sizeof(reply)));

        if (count == 1) {
            std::cout.write(static_cast<const char*>(response.data),
                static_cast<std::streamsize>(response.size));
            std::cout << '\n';
        }
    }

    if (count != 1) {
        std::cout << count << " round trips completed\n";
    }
}

void run_udp(int argc, char* argv[]) {
    const char* host = (argc >= 3) ? argv[2] : kDefaultUdpHost;
    const uint16_t port = (argc >= 4)
        ? static_cast<uint16_t>(std::stoi(argv[3]))
        : kDefaultUdpPort;
    const int count = (argc >= 5) ? std::stoi(argv[4]) : kDefaultRoundTrips;

    run_round_trips<Udp, UdpEchoClient>(
        Udp::SendParams{
            .host = host,
            .port = port,
            .payload = Buffer::read_only(kMessage, std::strlen(kMessage)),
        },
        count);
}

void run_uds(int argc, char* argv[]) {
    const std::string server_path = (argc >= 3) ? argv[2] : kDefaultUdsServerPath;
    const std::string client_path = (argc >= 4) ? argv[3] : kDefaultUdsClientPath;
    const int count = (argc >= 5) ? std::stoi(argv[4]) : kDefaultRoundTrips;

    ::unlink(client_path.c_str());

    run_round_trips<Uds, UdsEchoClient>(
        Uds::SendParams{
            .server_path = server_path,
            .client_path = client_path,
            .payload = Buffer::read_only(kMessage, std::strlen(kMessage)),
        },
        count);

    ::unlink(client_path.c_str());
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
