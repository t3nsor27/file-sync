#include "../include/net/peer.hpp"
#include <stdexcept>

namespace net {
Session::Session(tcp::socket socket, OnClose on_close)
    : socket_(std::move(socket)),
      strand_(asio::make_strand(socket_.get_executor())),
      on_close_(on_close) {}

asio::awaitable<void> Session::sendTree(const fstree::DirectoryTree& tree) {
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

asio::awaitable<fstree::DirectoryTree> Session::receiveTree() {
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

    auto size = be64toh(size_be_);
    if (size > MAX_TREE_SIZE)
      throw std::runtime_error("Tree payload too large.\n");
    buffer_.resize(size);

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

void Session::close() {
  boost::system::error_code ignored;
  socket_.close(ignored);
  if (on_close_)
    on_close_(shared_from_this());
}

Peer::Peer(uint16_t port) : io_(), acceptor_(io_), resolver_(io_) {
  tcp::endpoint ep(tcp::v6(), port);

  acceptor_.open(ep.protocol());
  acceptor_.set_option(asio::ip::v6_only(false));
  acceptor_.set_option(tcp::acceptor::reuse_address(true));
  acceptor_.bind(ep);
  acceptor_.listen();
}

// Expose execution context
asio::any_io_executor Peer::getExecutor() {
  return io_.get_executor();
}

// Lifecycle control
void Peer::run() {
  io_.run();
}

void Peer::stop() {
  io_.stop();
}

// Acceptor
void Peer::doAccept(OnAccept on_accept) {
  acceptor_.async_accept(
      [self = shared_from_this(), on_accept = std::move(on_accept)](
          boost::system::error_code ec, tcp::socket socket) mutable {
        if (ec)
          return;
        auto session = self->createSession(std::move(socket));
        on_accept(std::weak_ptr<Session>(session));
      });
}

void Peer::closeAcceptor() {
  boost::system::error_code ignored;
  acceptor_.close(ignored);
}

// Resolver
void Peer::doResolveAndConnect(const std::string& host,
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
        asio::async_connect(*socket,
                            results,
                            [self, socket, on_connect = std::move(on_connect)](
                                boost::system::error_code ec, auto) mutable {
                              if (ec) {
                                return;
                              }
                              auto session =
                                  self->createSession(std::move(*socket));
                              on_connect(std::weak_ptr<Session>(session));
                            });
      });
}

// Session control
void Peer::clearSessions() {
  // s->close() deletes s from sessions_ without copying we would be
  // deleting elements from sessions_ while iterating throught it, NOT SAFE
  auto copy = sessions_;
  for (auto& s : copy) {
    s->close();
  }
  sessions_.clear();
}

std::shared_ptr<Session> Peer::createSession(tcp::socket socket) {
  auto session = std::make_shared<Session>(
      std::move(socket),
      [self = shared_from_this()](std::shared_ptr<Session> s) {
        self->sessions_.erase(s);
      });
  sessions_.insert(session);
  return session;
}
}  // namespace net
