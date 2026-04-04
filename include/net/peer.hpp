#pragma once

#include <endian.h>
#include <atomic>
#include <boost/asio.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>
#include "../fstree/fstree.hpp"

namespace net {
using boost::asio::ip::tcp;
namespace asio = boost::asio;
namespace fs = std::filesystem;

constexpr uint64_t MAX_TREE_SIZE = 64 * 1024 * 1024;        // 64MB
constexpr uint32_t MAX_FILE_CHUNK_SIZE = 64 * 1024 * 1024;  // 64 MB

class Session : public std::enable_shared_from_this<Session> {
 public:
  struct HelloPacket {
    uint64_t peer_id;
    std::string hostname;
  };
  using OnClose = std::function<void(std::shared_ptr<Session>)>;

  explicit Session(tcp::socket, OnClose);

  // Traffic
  asio::awaitable<void> sendTree(
      const fstree::DirectoryTree&);  // handshake: no tag prefix
  asio::awaitable<void> sendTaggedTree(
      const fstree::DirectoryTree&);  // post-handshake: Tree tag + payload
  asio::awaitable<fstree::DirectoryTree> receiveTree();  // handshake: no tag
  asio::awaitable<fstree::DirectoryTree> receiveTreePayload();

  asio::awaitable<void> sendFile(const fstree::DirectoryTree&,
                                 const fstree::Node&,
                                 uint32_t chunk_size = MAX_FILE_CHUNK_SIZE);
  asio::awaitable<void> receiveFile(fstree::DirectoryTree&);
  asio::awaitable<void> sendHello(const HelloPacket&);
  asio::awaitable<HelloPacket> receiveHello();

  // Packet type tag sent before each message after handshake
  enum class PacketType : uint8_t {
    Tree = 0x01,
    TreeRequest = 0x02,
  };

  asio::awaitable<void> sendPacketType(PacketType);
  asio::awaitable<PacketType> receivePacketType();

  // Request the remote side to re-send its tree (refresh)
  asio::awaitable<void> sendTreeRequest();
  // Returns true when a TreeRequest arrived (caller should send back its tree)
  asio::awaitable<bool> receiveTreeRequest();

  // Utlilities
  tcp::socket& socket();
  void close();

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

  explicit Peer(uint16_t);

  asio::any_io_executor getExecutor();
  void run();
  void stop();

  void doAccept(OnAccept);
  void closeAcceptor();

  void doResolveAndConnect(const std::string&, uint16_t, OnConnect);
  void clearSessions();

  uint64_t id();

 private:
  uint64_t id_;
  std::shared_ptr<Session> createSession(tcp::socket);
  boost::asio::io_context io_;
  tcp::acceptor acceptor_;
  tcp::resolver resolver_;
  std::unordered_set<std::shared_ptr<Session>> sessions_;
};
}  // namespace net
