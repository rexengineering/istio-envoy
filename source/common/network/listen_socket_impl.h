#pragma once

#include <unistd.h>

#include <memory>
#include <string>

#include "envoy/network/connection.h"
#include "envoy/network/listen_socket.h"

#include "common/common/assert.h"

namespace Envoy {
namespace Network {

class SocketImpl : public virtual Socket {
public:
  ~SocketImpl() { close(); }

  // Network::Socket
  const Address::InstanceConstSharedPtr& localAddress() const override { return local_address_; }
  int fd() const override { return fd_; }
  void close() override {
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  void addOption(OptionPtr&& option) override {
    if (!options_) {
      options_ = std::make_shared<std::vector<OptionPtr>>();
    }
    options_->emplace_back(std::move(option));
  }
  const OptionsSharedPtr& options() const override { return options_; }

protected:
  SocketImpl(int fd, const Address::InstanceConstSharedPtr& local_address)
      : fd_(fd), local_address_(local_address) {}

  int fd_;
  Address::InstanceConstSharedPtr local_address_;
  OptionsSharedPtr options_;
};

class ListenSocketImpl : public SocketImpl {
protected:
  ListenSocketImpl(int fd, const Address::InstanceConstSharedPtr& local_address)
      : SocketImpl(fd, local_address) {}

  void doBind();
  void setListenSocketOptions(const Network::Socket::OptionsSharedPtr& options);
};

/**
 * Wraps a unix socket.
 */
class TcpListenSocket : public ListenSocketImpl {
public:
  TcpListenSocket(const Address::InstanceConstSharedPtr& address,
                  const Network::Socket::OptionsSharedPtr& options, bool bind_to_port);
  TcpListenSocket(int fd, const Address::InstanceConstSharedPtr& address,
                  const Network::Socket::OptionsSharedPtr& options);
};

typedef std::unique_ptr<TcpListenSocket> TcpListenSocketPtr;

class UdsListenSocket : public ListenSocketImpl {
public:
  UdsListenSocket(const Address::InstanceConstSharedPtr& address);
  UdsListenSocket(int fd, const Address::InstanceConstSharedPtr& address);
};

class ConnectionSocketImpl : public SocketImpl, public ConnectionSocket {
public:
  ConnectionSocketImpl(int fd, const Address::InstanceConstSharedPtr& local_address,
                       const Address::InstanceConstSharedPtr& remote_address)
      : SocketImpl(fd, local_address), remote_address_(remote_address) {}

  // Network::ConnectionSocket
  const Address::InstanceConstSharedPtr& remoteAddress() const override { return remote_address_; }
  void setLocalAddress(const Address::InstanceConstSharedPtr& local_address,
                       bool restored) override {
    ASSERT(!restored || *local_address != *local_address_);
    local_address_ = local_address;
    local_address_restored_ = restored;
  }
  void setRemoteAddress(const Address::InstanceConstSharedPtr& remote_address) override {
    remote_address_ = remote_address;
  }
  bool localAddressRestored() const override { return local_address_restored_; }

protected:
  Address::InstanceConstSharedPtr remote_address_;
  bool local_address_restored_{false};
};

// ConnectionSocket used with server connections.
class AcceptedSocketImpl : public ConnectionSocketImpl {
public:
  AcceptedSocketImpl(int fd, const Address::InstanceConstSharedPtr& local_address,
                     const Address::InstanceConstSharedPtr& remote_address)
      : ConnectionSocketImpl(fd, local_address, remote_address) {}
};

// ConnectionSocket used with client connections.
class ClientSocketImpl : public ConnectionSocketImpl {
public:
  ClientSocketImpl(const Address::InstanceConstSharedPtr& remote_address)
      : ConnectionSocketImpl(remote_address->socket(Address::SocketType::Stream), nullptr,
                             remote_address) {}
};

} // namespace Network
} // namespace Envoy
