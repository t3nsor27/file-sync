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
    buffer_ = fstree::serializeTree(tree);
    size_be_ = htobe64(buffer_.size());

    std::vector<asio::const_buffer> buffers{
        asio::buffer(&size_be_, sizeof(size_be_)), asio::buffer(buffer_)};

    co_await asio::async_write(socket_, buffers, asio::use_awaitable);

    busy_.store(false);
  }

  asio::awaitable<fstree::DirectoryTree> receiveTree() {
    // Ensure strand entry
    co_await asio::dispatch(strand_, asio::use_awaitable);

    // If busy, wait by yielding into strand
    while (busy_.exchange(true)) {
      co_await asio::post(strand_, asio::use_awaitable);
    }

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
  Peer(uint16_t port)
      : io_(),
        acceptor_(io_, tcp::endpoint(tcp::v4(), port)),
        resolver_(io_) {}

  // Lifecycle control
  void run() {
    io_.run();
  }

  void stop() {
    io_.stop();
  }

  // Acceptor
  void doAccept() {
    acceptor_.async_accept(
        [self = shared_from_this()](boost::system::error_code ec,
                                    tcp::socket socket) {
          if (!ec) {
            self->createSession(std::move(socket));
          }
        });
  }

  void closeAcceptor() {
    boost::system::error_code ignored;
    acceptor_.close(ignored);
  }

  // Resolver
  void doResolve(const std::string& host, uint16_t port) {
    resolver_.async_resolve(
        host,
        std::to_string(port),
        [self = shared_from_this()](boost::system::error_code ec,
                                    tcp::resolver::results_type results) {
          if (!ec) {
            tcp::socket socket = tcp::socket(self->io_);
            asio::async_connect(
                socket,
                results,
                [self, socket = std::move(socket)](boost::system::error_code ec,
                                                   auto) mutable {
                  if (!ec)
                    self->createSession(std::move(socket));
                });
          }
        });
  }

  // Session control
  void clearSessions() {
    for (auto& s : sessions_) {
      s->close();
    }
    sessions_.clear();
  }

 private:
  void createSession(tcp::socket socket) {
    auto session = std::make_shared<Session>(
        std::move(socket),
        [self = shared_from_this()](std::shared_ptr<Session> s) {
          self->sessions_.erase(s);
        });
    sessions_.insert(session);
  }

  boost::asio::io_context io_;
  tcp::acceptor acceptor_;
  tcp::resolver resolver_;
  std::unordered_set<std::shared_ptr<Session>> sessions_;
};
