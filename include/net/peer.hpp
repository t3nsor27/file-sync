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
namespace fs   = std::filesystem;

constexpr uint64_t MAX_TREE_SIZE       = 64 * 1024 * 1024;  // 64MB
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
  asio::awaitable<void> receiveFile(fstree::DirectoryTree&,
                                    bool rebuild_tree = true);
  asio::awaitable<void> sendHello(const HelloPacket&);
  asio::awaitable<HelloPacket> receiveHello();

  // Packet type tag sent before each message after handshake
  enum class PacketType : uint8_t {
    Tree        = 0x01,
    TreeRequest = 0x02,
    SyncRequest = 0x03,  // requester sends this to kick off a sync
    FileData    = 0x04,  // sender streams one file per packet
    DeleteFile  = 0x05,  // sender tells requester to delete a path
    SyncDone    = 0x06,  // sender signals end of file stream
    SyncHeader  = 0x07,  // sender announces total op count before streaming
    CreateDir   = 0x08,  // sender tells requester to create an empty directory
    DisconnectRequest = 0x09,  // sender tells requester to disconnect
  };

  asio::awaitable<void> sendPacketType(PacketType);
  asio::awaitable<PacketType> receivePacketType();

  // Refresh (tree exchange only)
  asio::awaitable<void> sendTreeRequest();
  asio::awaitable<bool> receiveTreeRequest();

  // Sync helpers — called by the listener / sync coroutine
  asio::awaitable<void> sendTaggedFile(const fstree::DirectoryTree&,
                                       const fstree::Node&);
  asio::awaitable<void> sendDeleteNotice(const std::filesystem::path& rel_path);
  asio::awaitable<void> sendCreateDir(const std::filesystem::path& rel_path);
  asio::awaitable<void> sendSyncDone();
  asio::awaitable<void> sendSyncHeader(uint32_t total_ops);
  asio::awaitable<uint32_t> receiveSyncHeader();
  asio::awaitable<std::filesystem::path> receiveRelPath();
  asio::awaitable<void> sendDisconnectRequest();

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
  using OnAccept  = std::function<void(std::weak_ptr<Session>)>;
  using OnConnect = std::function<void(std::weak_ptr<Session>)>;
  using OnError   = std::function<void(const boost::system::error_code&)>;

  explicit Peer(uint16_t);

  asio::any_io_executor getExecutor();
  void run();
  void stop();

  void doAccept(OnAccept);
  void closeAcceptor();

  // Connect without error callback (legacy — silently drops errors)
  void doResolveAndConnect(const std::string&, uint16_t, OnConnect);
  // Connect with error callback — on_error is called on resolve or TCP failure
  void doResolveAndConnect(const std::string&, uint16_t, OnConnect, OnError);
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
