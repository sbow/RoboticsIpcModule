#pragma once

#include "endpoint.hpp"
#include "shm_spsc.hpp"
#include "transport.hpp"

#include <csignal>
#include <thread>

template<Transport T>
class EchoServer : public IpcEndpoint<T> {
public:
    explicit EchoServer(const typename T::BindParams& bind_params) {
        this->bind(bind_params);
    }

    void echo(const typename T::RecvResult& recv, const Buffer& payload) {
        T::echo(this->handle(), recv, payload);
    }

    void run() {
        char storage[1024];
        Buffer buf = Buffer::writable(storage, sizeof(storage));
        typename T::RecvResult recv{};

        while (true) {
            this->recv(buf, recv);
            echo(recv, buf);
        }
    }

    void run_until(
        volatile sig_atomic_t* stop,
        int poll_timeout_ms = 200)
        requires InterruptibleTransport<T>
    {
        char storage[1024];
        Buffer buf = Buffer::writable(storage, sizeof(storage));
        typename T::RecvResult recv{};

        if constexpr (requires {
            T::try_recv(this->handle(), buf, recv);
        }) {
            while (!*stop) {
                if (T::try_recv(this->handle(), buf, recv)) {
                    echo(recv, buf);
                } else {
                    std::this_thread::yield();
                }
            }
            return;
        }

        this->set_recv_timeout_ms(poll_timeout_ms);
        while (!*stop) {
            try {
                this->recv(buf, recv);
                echo(recv, buf);
            } catch (const std::runtime_error&) {
            }
        }
    }
};

template<Transport T>
class EchoClient : public IpcEndpoint<T> {
public:
    EchoClient() : IpcEndpoint<T>() {}

    explicit EchoClient(const typename T::BindParams& bind_params) : IpcEndpoint<T>() {
        this->bind(bind_params);
    }

    Buffer exchange(const typename T::SendParams& send_params, Buffer recv_buf) {
        T::connect_or_send(this->handle(), send_params);
        typename T::RecvResult recv{};
        this->recv(recv_buf, recv);
        return recv_buf;
    }
};

using UdpEchoClient = EchoClient<Udp>;
using UdsEchoClient = EchoClient<Uds>;
using ShmEchoClient = EchoClient<ShmSpsc>;
using UdpEchoServer = EchoServer<Udp>;
using UdsEchoServer = EchoServer<Uds>;
using ShmEchoServer = EchoServer<ShmSpsc>;
