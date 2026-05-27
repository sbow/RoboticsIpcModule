#include "ipc.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace {

constexpr uint16_t kUdpPort = 19000;
constexpr const char* kUdsServerPath = "/tmp/rim_echo_server.sock";
constexpr const char* kUdsClientPath = "/tmp/rim_echo_client.sock";
constexpr const char* kShmEchoName = "/rim_shm_echo";
constexpr auto kTestDuration = std::chrono::seconds(5);
constexpr int kRecvTimeoutMs = 200;

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        throw std::runtime_error("setsockopt SO_RCVTIMEO failed");
    }
}

template<typename Transport, typename EchoServer, typename EchoClient>
uint64_t run_echo_benchmark(const typename Transport::BindParams& bind_params,
                            typename Transport::SendParams send_params) {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> round_trips{0};

    EchoServer server(bind_params);

    const auto server_fn = [&]() {
        if constexpr (requires { server.fd(); }) {
            set_recv_timeout(server.fd(), kRecvTimeoutMs);
        }

        char storage[1024];
        Buffer buf = Buffer::writable(storage, sizeof(storage));
        typename Transport::RecvResult recv{};

        while (!stop.load(std::memory_order_acquire)) {
            if constexpr (requires {
                ShmSpsc::try_recv(server.handle(), buf, recv);
            }) {
                if (ShmSpsc::try_recv(server.handle(), buf, recv)) {
                    server.echo(recv, buf);
                } else {
                    std::this_thread::yield();
                }
            } else {
                try {
                    server.recv_from(buf, recv);
                    server.echo(recv, buf);
                } catch (const std::runtime_error&) {
                }
            }
        }
    };

    const auto client_fn = [&]() {
        EchoClient client = [&]() {
            if constexpr (std::is_same_v<Transport, ShmSpsc>) {
                typename Transport::BindParams client_bind = bind_params;
                client_bind.create = false;
                return EchoClient(client_bind);
            }
            return EchoClient();
        }();

        if constexpr (requires { client.fd(); }) {
            set_recv_timeout(client.fd(), kRecvTimeoutMs);
        }

        const char msg[] = "ping";
        char reply[64];
        bool uds_client_bound = false;

        while (!stop.load(std::memory_order_acquire)) {
            try {
                send_params.payload = Buffer::read_only(msg, sizeof(msg) - 1);
                if constexpr (std::is_same_v<Transport, Uds>) {
                    if (!uds_client_bound) {
                        uds_client_bound = true;
                    } else {
                        send_params.client_path.clear();
                    }
                }

                if constexpr (std::is_same_v<Transport, ShmSpsc>) {
                    // Phase C: interruptible SHM exchange. try_send + try_recv
                    // with periodic stop checks so a full ring / dead server
                    // cannot wedge this thread on shutdown. Matches the
                    // server's existing try_recv loop above; the two together
                    // let `IPC_SKIP_SHM=1` be retired (see ADR 0006).
                    Buffer rbuf = Buffer::writable(reply, sizeof(reply));
                    typename Transport::RecvResult rr{};

                    while (!stop.load(std::memory_order_acquire)
                           && ShmSpsc::try_send(client.handle(), send_params,
                                                send_params.payload)
                              != ShmSendResult::Ok) {
                        std::this_thread::yield();
                    }
                    if (stop.load(std::memory_order_acquire)) {
                        break;
                    }
                    while (!stop.load(std::memory_order_acquire)
                           && !ShmSpsc::try_recv(client.handle(), rbuf, rr)) {
                        std::this_thread::yield();
                    }
                    if (stop.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (rbuf.size > 0) {
                        round_trips.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    Buffer response = client.exchange(
                        send_params, Buffer::writable(reply, sizeof(reply)));

                    if (response.size > 0) {
                        round_trips.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (const std::runtime_error&) {
                if (stop.load(std::memory_order_acquire)) {
                    break;
                }
            }
        }
    };

    std::thread server_thread(server_fn);
    std::thread client_thread(client_fn);
    std::thread timer_thread([]() {
        std::this_thread::sleep_for(kTestDuration);
    });

    timer_thread.join();
    stop.store(true, std::memory_order_release);

    client_thread.join();
    server_thread.join();

    return round_trips.load(std::memory_order_relaxed);
}

uint64_t run_udp_benchmark() {
    return run_echo_benchmark<Udp, UdpEchoServer, UdpEchoClient>(
        Udp::BindParams{.port = kUdpPort},
        Udp::SendParams{
            .host = "127.0.0.1",
            .port = kUdpPort,
            .payload = Buffer::read_only("ping", 3),
        });
}

uint64_t run_uds_benchmark() {
    ::unlink(kUdsServerPath);
    ::unlink(kUdsClientPath);

    const uint64_t trips = run_echo_benchmark<Uds, UdsEchoServer, UdsEchoClient>(
        Uds::BindParams{.path = kUdsServerPath},
        Uds::SendParams{
            .server_path = kUdsServerPath,
            .client_path = kUdsClientPath,
            .payload = Buffer::read_only("ping", 3),
        });

    ::unlink(kUdsServerPath);
    ::unlink(kUdsClientPath);
    return trips;
}

uint64_t run_shm_benchmark() {
    return run_echo_benchmark<ShmSpsc, ShmEchoServer, ShmEchoClient>(
        ShmSpsc::BindParams{.name = kShmEchoName, .create = true},
        ShmSpsc::SendParams{.payload = Buffer::read_only("ping", 3)});
}

}  // namespace

int main() {
    const uint64_t udp_trips = run_udp_benchmark();
    std::cout << "UDP round trips in 5s: " << udp_trips << '\n' << std::flush;

    const uint64_t uds_trips = run_uds_benchmark();
    std::cout << "UDS round trips in 5s: " << uds_trips << '\n' << std::flush;

    // Phase C lifts the IPC_SKIP_SHM escape hatch: the SHM echo client now
    // uses try_send + try_recv with periodic stop checks, so shutdown is
    // bounded even when the ring is full or the server has stopped. The
    // environment variable is still honored (in case CI wants to skip SHM
    // for kernel / sysctl reasons), but it is no longer required.
    if (std::getenv("IPC_SKIP_SHM") != nullptr) {
        std::cout << "SHM round trips in 5s: SKIPPED (IPC_SKIP_SHM set; "
                     "Phase C made this optional)\n"
                  << std::flush;
        return 0;
    }

    const uint64_t shm_trips = run_shm_benchmark();
    std::cout << "SHM round trips in 5s: " << shm_trips << '\n' << std::flush;
    return 0;
}
