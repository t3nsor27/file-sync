#include "../include/net/peer.hpp"
#include <endian.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

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

    co_await asio::async_write(
        socket_, buffers, asio::bind_executor(strand_, asio::use_awaitable));
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

asio::awaitable<void> Session::sendFile(const fstree::DirectoryTree& tree,
                                        const fstree::Node& node,
                                        uint32_t chunk_size) {
  // Ensure strand entry
  co_await asio::dispatch(strand_, asio::use_awaitable);

  if (chunk_size == 0 || chunk_size > MAX_FILE_CHUNK_SIZE)
    throw std::runtime_error("invalid chunk size");

  // Resolve absolute path
  fs::path file_path = tree.root_path / node.path;
  auto file_size = std::get<fstree::FileMeta>(node.data).size;

  std::ifstream file(file_path, std::ios::binary);
  if (!file)
    throw std::runtime_error("failed to open file");

  // Send header
  std::ostringstream header;
  fstree::wire::write_string(header, node.path.generic_string());
  fstree::wire::write_u64(header, file_size);

  auto header_buf = header.str();
  uint64_t header_size = header_buf.size();
  uint64_t header_size_be = htobe64(header_size);

  // --- Header Debug ---
  // std::cout << "\nSend header ->"
  //           << "\nSize: " << be64toh(header_size_be)
  //           << "\nrel_path: " << node.path.generic_string()
  //           << "\nfile_size: " << file_size << "\nBuffer: ";  // debug
  // for (auto& v : header_buf) {
  //   std::cout << (int)v << " ";
  // }
  // std::cout << "\n";

  co_await asio::async_write(
      socket_,
      asio::buffer(&header_size_be, sizeof(header_size_be)),
      asio::use_awaitable);

  co_await asio::async_write(
      socket_, asio::buffer(header_buf), asio::use_awaitable);

  // Send chunk
  std::vector<char> buffer(chunk_size);
  uint64_t remaining = file_size;

  while (remaining > 0) {
    uint32_t to_read =
        static_cast<uint32_t>(std::min<uint64_t>(remaining, chunk_size));

    file.read(buffer.data(), to_read);
    if (!file)
      throw std::runtime_error("file read failed");

    uint32_t be_size = htobe32(to_read);

    co_await asio::async_write(
        socket_, asio::buffer(&be_size, sizeof(be_size)), asio::use_awaitable);

    co_await asio::async_write(
        socket_, asio::buffer(buffer.data(), to_read), asio::use_awaitable);

    remaining -= to_read;
  }
}

asio::awaitable<void> Session::receiveFile(fstree::DirectoryTree& tree) {
  // Ensure strand entry
  co_await asio::dispatch(strand_, asio::use_awaitable);

  // Receive Header
  uint64_t hdr_size_be = 0;
  co_await asio::async_read(socket_,
                            asio::buffer(&hdr_size_be, sizeof(hdr_size_be)),
                            asio::use_awaitable);

  uint64_t hdr_size = be64toh(hdr_size_be);
  // std::cout << "Received header size: " << hdr_size << "\n";

  if (hdr_size > MAX_FILE_CHUNK_SIZE)
    throw std::runtime_error("header too large");

  std::vector<uint8_t> hdr_buf(hdr_size);
  co_await asio::async_read(
      socket_, asio::buffer(hdr_buf), asio::use_awaitable);

  std::istringstream hdr_stream(std::string(hdr_buf.begin(), hdr_buf.end()));

  fs::path rel_path = fstree::wire::read_string(hdr_stream);
  uint64_t file_size = fstree::wire::read_u64(hdr_stream);

  // --- Header Debug ---
  // std::cout << "\nReceive header ->"
  //           << "\nSize: " << hdr_size << "\nrel_path: " << rel_path
  //           << "\nfile_size: " << file_size << "\nBuffer: ";  // debug
  // for (auto& v : hdr_buf) {
  //   std::cout << (int)v << " ";
  // }
  // std::cout << "\n";  // debug

  // Resolve path
  fs::path abs_path = tree.root_path / rel_path;
  fs::create_directories(abs_path.parent_path());

  std::ofstream file(abs_path, std::ios::binary | std::ios::trunc);
  if (!file)
    throw std::runtime_error("failed to create file");

  // Receive chunk
  uint64_t received = 0;

  while (received < file_size) {
    uint32_t chunk_size_be = 0;
    co_await asio::async_read(
        socket_,
        asio::buffer(&chunk_size_be, sizeof(chunk_size_be)),
        asio::use_awaitable);
    uint32_t chunk_size = be32toh(chunk_size_be);

    if (chunk_size > MAX_FILE_CHUNK_SIZE || chunk_size == 0)
      throw std::runtime_error("chunk too large");

    std::vector<char> buffer(chunk_size);
    co_await asio::async_read(
        socket_, asio::buffer(buffer), asio::use_awaitable);

    file.write(buffer.data(), chunk_size);
    if (!file)
      throw std::runtime_error("file write failed");

    received += chunk_size;
  }

  file.close();
  tree = fstree::DirectoryTree(tree.root_path);
}

void Session::close() {
  if (!socket_.is_open())
    return;

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
        if (!ec) {
          auto session = self->createSession(std::move(socket));
          on_accept(std::weak_ptr<Session>(session));
        }
        self->doAccept(on_accept);
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
