#include <endian.h>
#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>
#include "include/fstree/fstree.hpp"

using boost::asio::ip::tcp;
namespace asio = boost::asio;

class Session : public std::enable_shared_from_this<Session> {
 public:
  using OnClose = std::function<void(std::shared_ptr<Session>)>;

  Session(tcp::socket socket, OnClose on_close)
      : socket_(std::move(socket)),
        strand_(asio::make_strand(socket_.get_executor())),
        on_close_(on_close) {}

  asio::awaitable<void> sendTree(const fstree::DirectoryTree& tree) {
    // Ensure strand entry
    co_await asio::dispatch(strand_, asio::use_awaitable);

    while (busy_.exchange(true)) {
      co_await asio::post(strand_, asio::use_awaitable);
    }

    // We own this session now
    try {
      buffer_ = fstree::serializeTree(tree);
      size_be_ = htobe64(buffer_.size());

      std::vector<asio::const_buffer> buffers{
          asio::buffer(&size_be_, sizeof(size_be_)), asio::buffer(buffer_)};

      co_await asio::async_write(socket_, buffers, asio::use_awaitable);
    } catch (...) {
      busy_.store(false);
      close();
      throw;
    }
    busy_.store(false);
  }

  asio::awaitable<fstree::DirectoryTree> receiveTree() {
    // Ensure strand entry
    co_await asio::dispatch(strand_, asio::use_awaitable);

    // If busy, wait by yielding into strand
    while (busy_.exchange(true)) {
      co_await asio::post(strand_, asio::use_awaitable);
    }

    try {
      // read size
      co_await asio::async_read(
          socket_,
          asio::buffer(&size_be_, sizeof(size_be_)),
          asio::bind_executor(strand_, asio::use_awaitable));

      buffer_.resize(be64toh(size_be_));

      // read payload
      co_await asio::async_read(
          socket_,
          asio::buffer(buffer_),
          asio::bind_executor(strand_, asio::use_awaitable));

      busy_.store(false);

      co_return fstree::deserializeTree(buffer_);
    } catch (...) {
      busy_.store(false);
      close();
      throw;
    }
  }

  void close() {
    boost::system::error_code ignored;
    socket_.close(ignored);
    if (on_close_)
      on_close_(shared_from_this());
  }

 private:
  OnClose on_close_;

  tcp::socket socket_;
  asio::strand<asio::any_io_executor> strand_;

  std::atomic<bool> busy_{false};
  std::vector<uint8_t> buffer_;
  uint64_t size_be_{0};
};

class Peer : public std::enable_shared_from_this<Peer> {
 public:
  using OnAccept = std::function<void(std::weak_ptr<Session>)>;
  using OnConnect = std::function<void(std::weak_ptr<Session>)>;

  Peer(uint16_t port) : io_(), acceptor_(io_), resolver_(io_) {
    tcp::endpoint ep(tcp::v6(), port);

    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::ip::v6_only(false));
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
  }

  // Expose execution context
  asio::any_io_executor getExecutor() {
    return io_.get_executor();
  }

  // Lifecycle control
  void run() {
    io_.run();
  }

  void stop() {
    io_.stop();
  }

  // Acceptor
  void doAccept(OnAccept on_accept) {
    acceptor_.async_accept(
        [self = shared_from_this(), on_accept = std::move(on_accept)](
            boost::system::error_code ec, tcp::socket socket) mutable {
          if (ec)
            return;
          auto session = self->createSession(std::move(socket));
          on_accept(std::weak_ptr<Session>(session));
        });
  }

  void closeAcceptor() {
    boost::system::error_code ignored;
    acceptor_.close(ignored);
  }

  // Resolver
  void doResolveAndConnect(const std::string& host,
                           uint16_t port,
                           OnConnect on_connect) {
    resolver_.async_resolve(
        host,
        std::to_string(port),
        [self = shared_from_this(), on_connect = std::move(on_connect)](
            boost::system::error_code ec, tcp::resolver::results_type results) {
          if (ec)
            return;
          auto socket = std::make_shared<tcp::socket>(self->io_);
          asio::async_connect(
              *socket,
              results,
              [self, socket, on_connect = std::move(on_connect)](
                  boost::system::error_code ec, auto) mutable {
                if (ec) {
                  return;
                }
                auto session = self->createSession(std::move(*socket));
                on_connect(std::weak_ptr<Session>(session));
              });
        });
  }

  // Session control
  void clearSessions() {
    // s->close() deletes s from sessions_ without copying we would be
    // deleting elements from sessions_ while iterating throught it, NOT SAFE
    auto copy = sessions_;
    for (auto& s : copy) {
      s->close();
    }
    sessions_.clear();
  }

 private:
  std::shared_ptr<Session> createSession(tcp::socket socket) {
    auto session = std::make_shared<Session>(
        std::move(socket),
        [self = shared_from_this()](std::shared_ptr<Session> s) {
          self->sessions_.erase(s);
        });
    sessions_.insert(session);
    return session;
  }

  boost::asio::io_context io_;
  tcp::acceptor acceptor_;
  tcp::resolver resolver_;
  std::unordered_set<std::shared_ptr<Session>> sessions_;
};
