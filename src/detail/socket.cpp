// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "detail/socket.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "black_start_manager/limits.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace black_start_manager::detail {
namespace {

#if defined(_WIN32)
constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(INVALID_SOCKET);
using NativeSocket = SOCKET;

[[nodiscard]] int last_socket_error() { return ::WSAGetLastError(); }

void close_native(std::uintptr_t handle) {
  ::closesocket(static_cast<SOCKET>(handle));
}

[[nodiscard]] std::string describe_socket_error(int code) {
  return "windows socket error " + std::to_string(code);
}
#else
constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(-1);
using NativeSocket = int;

[[nodiscard]] int last_socket_error() { return errno; }

void close_native(std::uintptr_t handle) {
  ::close(static_cast<int>(handle));
}

[[nodiscard]] std::string describe_socket_error(int code) {
  return "socket error " + std::to_string(code);
}
#endif

std::once_flag g_runtime_once;
bool g_runtime_ready = false;

}  // namespace

Result<Unit> ensure_socket_runtime() {
#if defined(_WIN32)
  std::call_once(g_runtime_once, []() {
    WSADATA data{};
    g_runtime_ready = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  if (!g_runtime_ready) {
    return Error(ErrorCode::ControllerUnavailable,
                 "the socket runtime could not be initialized");
  }
#else
  g_runtime_ready = true;
#endif
  return Unit{};
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

Socket::~Socket() { close(); }

bool Socket::valid() const noexcept { return handle_ != kInvalidSocket; }

void Socket::close() noexcept {
  if (valid()) {
    close_native(handle_);
    handle_ = kInvalidSocket;
  }
}

Result<Unit> Socket::send_line(std::string_view line) {
  if (!valid()) {
    return Error(ErrorCode::ControllerUnavailable, "socket is not connected");
  }
  if (line.size() > limits::kMaxControllerReplyBytes) {
    return Error(ErrorCode::LimitExceeded, "protocol line exceeds the supported size");
  }
  std::string payload(line);
  payload.push_back('\n');
  std::size_t sent = 0;
  while (sent < payload.size()) {
#if defined(_WIN32)
    const int chunk = static_cast<int>(
        std::min<std::size_t>(payload.size() - sent, 1u << 20));
    const int produced =
        ::send(static_cast<SOCKET>(handle_), payload.data() + sent, chunk, 0);
#else
    const std::size_t chunk = std::min<std::size_t>(payload.size() - sent, 1u << 20);
    const ssize_t produced =
        ::send(static_cast<int>(handle_), payload.data() + sent, chunk, 0);
#endif
    if (produced <= 0) {
      return Error(ErrorCode::ControllerUnavailable,
                   "the socket connection failed while sending (" +
                       describe_socket_error(last_socket_error()) + ")");
    }
    sent += static_cast<std::size_t>(produced);
  }
  return Unit{};
}

Result<std::string> Socket::receive_line(std::size_t max_bytes) {
  if (!valid()) {
    return Error(ErrorCode::ControllerUnavailable, "socket is not connected");
  }
  std::string line;
  char buffer[4096];
  while (true) {
#if defined(_WIN32)
    const int produced = ::recv(static_cast<SOCKET>(handle_), buffer,
                                static_cast<int>(sizeof(buffer)), 0);
#else
    const ssize_t produced = ::recv(static_cast<int>(handle_), buffer, sizeof(buffer), 0);
#endif
    if (produced == 0) {
      return Error(ErrorCode::TruncatedInput,
                   "the peer closed the connection before the line was complete");
    }
    if (produced < 0) {
      const std::string failure = describe_socket_error(last_socket_error());
      const int code = last_socket_error();
#if defined(_WIN32)
      const bool reset = code == WSAECONNRESET || code == WSAECONNABORTED;
#else
      const bool reset = code == ECONNRESET || code == ECONNABORTED;
#endif
      if (reset) {
        // A peer that was killed resets the connection; that is a closed connection rather
        // than an unavailable owner.
        return Error(ErrorCode::TruncatedInput,
                     "the peer abandoned the connection before the line was complete");
      }
      return Error(ErrorCode::ControllerUnavailable,
                   "the socket connection failed while receiving (" + failure + ")");
    }
    for (int index = 0; index < produced; ++index) {
      if (buffer[index] == '\n') {
        return line;
      }
      line.push_back(buffer[index]);
      if (line.size() > max_bytes) {
        return Error(ErrorCode::LimitExceeded,
                     "protocol line exceeds the supported size");
      }
    }
  }
}

Listener::Listener(Listener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
  other.handle_ = kInvalidSocket;
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = kInvalidSocket;
    other.port_ = 0;
  }
  return *this;
}

Listener::~Listener() { close(); }

void Listener::close() noexcept {
  if (handle_ != kInvalidSocket) {
    close_native(handle_);
    handle_ = kInvalidSocket;
  }
}

Result<Listener> Listener::bind_loopback(std::uint16_t port) {
  BSM_RETURN_IF_ERROR(ensure_socket_runtime());
  const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
  if (handle == INVALID_SOCKET) {
#else
  if (handle < 0) {
#endif
    return Error(ErrorCode::ControllerUnavailable,
                 "a listening socket could not be created (" +
                     describe_socket_error(last_socket_error()) + ")");
  }
  int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(static_cast<std::uintptr_t>(handle));
    return Error(ErrorCode::ControllerUnavailable,
                 "the loopback port could not be bound (" +
                     describe_socket_error(last_socket_error()) + ")");
  }
  if (::listen(handle, 8) != 0) {
    close_native(static_cast<std::uintptr_t>(handle));
    return Error(ErrorCode::ControllerUnavailable,
                 "the listening socket could not accept connections (" +
                     describe_socket_error(last_socket_error()) + ")");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int bound_length = sizeof(bound);
#else
  socklen_t bound_length = sizeof(bound);
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    close_native(static_cast<std::uintptr_t>(handle));
    return Error(ErrorCode::ControllerUnavailable,
                 "the bound port could not be read back");
  }
  Listener listener;
  listener.handle_ = static_cast<std::uintptr_t>(handle);
  listener.port_ = ntohs(bound.sin_port);
  return listener;
}

Result<Socket> Listener::accept_one() {
  if (handle_ == kInvalidSocket) {
    return Error(ErrorCode::ControllerUnavailable, "the listener is closed");
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int peer_length = sizeof(peer);
#else
  socklen_t peer_length = sizeof(peer);
#endif
  const NativeSocket connection = ::accept(
      static_cast<NativeSocket>(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
#if defined(_WIN32)
  if (connection == INVALID_SOCKET) {
#else
  if (connection < 0) {
#endif
    return Error(ErrorCode::ControllerUnavailable,
                 "accept failed (" + describe_socket_error(last_socket_error()) + ")");
  }
  int no_delay = 1;
  ::setsockopt(connection, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
  Socket socket;
  socket.handle_ = static_cast<std::uintptr_t>(connection);
  return socket;
}

Result<Socket> connect_loopback(std::uint16_t port) {
  BSM_RETURN_IF_ERROR(ensure_socket_runtime());
  const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
  if (handle == INVALID_SOCKET) {
#else
  if (handle < 0) {
#endif
    return Error(ErrorCode::ControllerUnavailable,
                 "a client socket could not be created (" +
                     describe_socket_error(last_socket_error()) + ")");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(static_cast<std::uintptr_t>(handle));
    return Error(ErrorCode::ControllerUnavailable,
                 "the adjacent owner is not reachable on loopback port " +
                     std::to_string(port) + " (" +
                     describe_socket_error(last_socket_error()) + ")");
  }
  Socket socket;
  socket.handle_ = static_cast<std::uintptr_t>(handle);
  return socket;
}

}  // namespace black_start_manager::detail
