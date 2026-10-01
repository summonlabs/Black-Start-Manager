#pragma once

// Loopback socket transport for the out-of-process adjacent owner.
//
// The transport is deliberately small: a listening socket on the loopback interface, a
// blocking line protocol, and no timeouts. A timeout would turn an unresponsive owner
// into an authority decision, which this boundary refuses to do; an owner that stops
// answering leaves the attempt unresolved, and unresolved is a state the manager can
// explain and recover from.

#include <cstdint>
#include <string>

#include "black_start_manager/error.hpp"

namespace black_start_manager::detail {

// Reference-counted process-wide socket runtime initialisation.
[[nodiscard]] Result<Unit> ensure_socket_runtime();

class Socket {
 public:
  Socket() = default;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  ~Socket();

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Result<Unit> send_line(std::string_view line);
  // Blocks until one line arrives. End of file is reported as TruncatedInput with the
  // exact reason, so a caller can distinguish "closed" from "malformed".
  [[nodiscard]] Result<std::string> receive_line(std::size_t max_bytes);
  void close() noexcept;

 private:
  friend class Listener;
  friend Result<Socket> connect_loopback(std::uint16_t port);
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
};

class Listener {
 public:
  Listener() = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  ~Listener();

  // Binds the loopback interface. Port zero asks the operating system for an ephemeral
  // port, which the caller reads back with port().
  [[nodiscard]] static Result<Listener> bind_loopback(std::uint16_t port);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] Result<Socket> accept_one();
  void close() noexcept;

 private:
  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
  std::uint16_t port_ = 0;
};

[[nodiscard]] Result<Socket> connect_loopback(std::uint16_t port);

}  // namespace black_start_manager::detail
