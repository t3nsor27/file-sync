#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "./include/fstree/fstree.hpp"
#include "./include/net/peer.hpp"

namespace asio = boost::asio;
using namespace asio::experimental::awaitable_operators;
std::pair<std::string, asio::ip::address> GetHostInfo() {
  boost::asio::io_context io;

  std::string hostname = boost::asio::ip::host_name();
  asio::ip::address ip;

  try {
    boost::asio::ip::udp::socket socket(io);

    socket.connect(boost::asio::ip::udp::endpoint(
        boost::asio::ip::make_address("8.8.8.8"), 80));

    ip = socket.local_endpoint().address();
  } catch (...) {
  }

  return {hostname, ip};
}

struct PeerInfo {
  std::string name;
  uint64_t peer_id;
  asio::ip::address address;
  uint16_t port;
  std::shared_ptr<fstree::DirectoryTree> tree;
  std::weak_ptr<net::Session> session;
};

PeerInfo ExtractPeerInfo(std::shared_ptr<net::Session> session,
                         const net::Session::HelloPacket& hello) {
  auto& socket  = session->socket();
  auto endpoint = socket.remote_endpoint();

  PeerInfo info;
  info.address = endpoint.address();
  info.port    = endpoint.port();
  info.session = session;

  info.peer_id = hello.peer_id;
  info.name    = hello.hostname;

  return info;
}

// TODO: Separate sync_state for each peer
// TODO: Implement parallel sending and receiving files
// TODO: Change endian.h to boost-endian
int main(int argc, char* argv[]) {
  using namespace ftxui;

  if (argc != 3) {
    return 0;
  }

  auto screen   = ScreenInteractive::Fullscreen();
  auto bg_color = Color::Palette256(16);

  auto [hostname, ip_addr] = GetHostInfo();
  std::string ip           = ip_addr.to_string();
  std::string peer_ip, peer_port, peer_timeout;
  std::mutex peer_mutex;
  std::vector<PeerInfo> peer_list;
  int selected_peer = 0;

  // Must be called with peer_mutex already held OR from UI thread after lock.
  auto remove_peer_by_session = [&](std::shared_ptr<net::Session> s) -> bool {
    for (auto it = peer_list.begin(); it != peer_list.end(); ++it) {
      if (it->session.lock().get() == s.get()) {
        peer_list.erase(it);
        // Clamp selected_peer so it stays in range.
        if (selected_peer >= static_cast<int>(peer_list.size()))
          selected_peer = std::max(0, static_cast<int>(peer_list.size()) - 1);
        return true;
      }
    }
    return false;
  };
  uint16_t port = std::stoi(argv[1]);
  auto peer     = std::make_shared<net::Peer>(port);
  PeerInfo local_peer{
      hostname,
      peer->id(),
      ip_addr,
      port,
      std::make_shared<fstree::DirectoryTree>(
          fstree::DirectoryTree(std::filesystem::path(argv[2])))};

  // NOTE: Use this string for debugging
  std::string debug_str;

  // -------------------------------------------------------------------------------------------------
  // SYNC STATE  (written from IO thread, read by UI thread)
  // -------------------------------------------------------------------------------------------------
  struct SyncState {
    enum class Phase { Idle, SyncingTrees, SyncingFiles, Done, Error };
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<int> files_done{0};
    std::atomic<int> files_total{0};
  };
  SyncState sync_state;

  // -------------------------------------------------------------------------------------------------
  // ERROR STATE  (written from IO thread, shown as modal in UI thread)
  // -------------------------------------------------------------------------------------------------
  struct ErrorState {
    std::mutex mtx;
    bool visible      = false;
    std::string title = "Error";
    std::string message;
  };
  auto error_state = std::make_shared<ErrorState>();

  // Post an error from any thread — shows the modal and triggers a UI refresh.
  auto post_error = [&](std::string title, std::string msg) {
    {
      std::lock_guard<std::mutex> lk(error_state->mtx);
      error_state->visible = true;
      error_state->title   = std::move(title);
      error_state->message = std::move(msg);
    }
    screen.PostEvent(Event::Custom);
  };

  std::function<void(std::size_t, std::shared_ptr<net::Session>)>
      start_refresh_listener;  // Forward declaration

  // -------------------------------------------------------------------------------------------------
  // ACCEPT LOOP
  // -------------------------------------------------------------------------------------------------

  peer->doAccept([&](std::weak_ptr<net::Session> weak_session) {
    if (auto session = weak_session.lock()) {
      asio::co_spawn(
          peer->getExecutor(),
          [&, session]() -> asio::awaitable<void> {
            try {
              co_await session->sendHello({peer->id(), hostname});
              auto hello = co_await session->receiveHello();
              auto info  = ExtractPeerInfo(session, hello);

              {
                std::lock_guard<std::mutex> lock(peer_mutex);
                for (auto& existing : peer_list) {
                  if (existing.peer_id == hello.peer_id) {
                    session->close();
                    co_return;
                  }
                }
              }

              co_await session->sendTree(*local_peer.tree);
              info.tree = std::make_shared<fstree::DirectoryTree>(
                  co_await session->receiveTree());

              {
                std::lock_guard<std::mutex> lock(peer_mutex);
                peer_list.push_back(std::move(info));
              }

              start_refresh_listener(peer_list.size() - 1, session);
              screen.PostEvent(Event::Custom);
            } catch (const boost::system::system_error& e) {
              session->close();
              post_error("Incoming Connection Error",
                         std::string("Handshake with incoming peer failed: ") +
                             e.what());
            } catch (const std::exception& e) {
              session->close();
              post_error("Incoming Connection Error", e.what());
            } catch (...) {
              session->close();
              // Silently drop truly unknown errors on accept side
            }
          },
          asio::detached);
    }
  });

  std::thread io_thread([peer]() {
    peer->run();
  });

  // -------------------------------------------------------------------------------------------------
  // STATUS BAR
  // -------------------------------------------------------------------------------------------------
  auto status_bar_renderer = Renderer([&] {
    auto dimText = [](std::string t) {
      return text(t) | dim /*| border*/;
    };

    std::string separator = "  ";
    std::string peer_no =
        peer_list.size() > 0 ? std::to_string(peer_list.size()) : "no";

    return hbox({
               text("file-sync") | color(Color::White) | bold,
               filler(),
               dimText(peer_no + " peer connected"),
               dimText(separator),
               dimText(hostname),
               dimText(separator),
               dimText(ip + ':' + std::to_string(port)),
           }) |
           xflex | bgcolor(Color::Black) | borderHeavy;
  });

  // -------------------------------------------------------------------------------------------------
  // PEER PANEL
  // -------------------------------------------------------------------------------------------------

  // ---- Validation helpers ----

  enum class FieldState { Empty, Valid, Invalid };

  // Validate an IP address string using Boost.Asio's parser.
  auto validateIP = [](const std::string& s) -> FieldState {
    if (s.empty())
      return FieldState::Empty;
    boost::system::error_code ec;
    boost::asio::ip::make_address(s, ec);
    return ec ? FieldState::Invalid : FieldState::Valid;
  };

  // Validate a port string: must be a non-empty integer in [1, 65535].
  auto validatePort = [](const std::string& s) -> FieldState {
    if (s.empty())
      return FieldState::Empty;
    for (char c : s)
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return FieldState::Invalid;
    try {
      long v = std::stol(s);
      return (v >= 1 && v <= 65535) ? FieldState::Valid : FieldState::Invalid;
    } catch (...) {
      return FieldState::Invalid;
    }
  };

  auto fieldBorderColor = [](FieldState fs, bool focused) -> Color {
    if (fs == FieldState::Invalid)
      return Color::Red;
    if (fs == FieldState::Valid)
      return focused ? Color::Green : Color::GreenLight;
    // Empty
    return focused ? Color::White : Color::GrayDark;
  };

  auto peer_input_validated = [&](StringRef s,
                                  std::string label,
                                  std::function<FieldState()> get_state) {
    InputOption opt = InputOption::Default();
    opt.transform   = [](InputState is) {
      return is.element;
    };
    auto input = Input(s, "", opt);

    return Renderer(input, [=]() mutable {
      bool focused     = input->Focused();
      FieldState fs    = get_state();
      Color border_col = fieldBorderColor(fs, focused);

      std::string icon;
      Color icon_col = Color::White;
      if (fs == FieldState::Valid) {
        icon     = " ✓";
        icon_col = Color::Green;
      } else if (fs == FieldState::Invalid) {
        icon     = " ✗";
        icon_col = Color::Red;
      }

      // clang-format off
      return vbox({
          hbox({
              text(label) | bold,
              text(icon) | color(icon_col),
          }),
          input->Render() | borderStyled(LIGHT, border_col) |
              size(WIDTH, GREATER_THAN, 10),
      });
      // clang-format on
    });
  };

  auto isDuplicate = [&](const std::string& check_ip,
                         uint16_t check_port) -> bool {
    // Self-connect guard
    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(check_ip, ec);
    if (!ec && addr == ip_addr && check_port == port)
      return true;
    for (auto& p : peer_list) {
      if (p.address == addr && p.port == check_port)
        return true;
    }
    return false;
  };

  auto connectReady = [&]() -> bool {
    if (validateIP(peer_ip) != FieldState::Valid)
      return false;
    if (validatePort(peer_port) != FieldState::Valid)
      return false;
    if (!peer_timeout.empty()) {
      bool valid_digits =
          std::all_of(peer_timeout.begin(), peer_timeout.end(), [](char c) {
            return std::isdigit((unsigned char)c);
          });
      if (!valid_digits)
        return false;
      try {
        long v = std::stol(peer_timeout);
        if (v < 1 || v > 3600)
          return false;
      } catch (...) {
        return false;
      }
    }
    uint16_t p = static_cast<uint16_t>(std::stoi(peer_port));
    std::lock_guard<std::mutex> lock(peer_mutex);
    return !isDuplicate(peer_ip, p);
  };

  auto connectHint = [&]() -> std::string {
    auto ip_st   = validateIP(peer_ip);
    auto port_st = validatePort(peer_port);
    if (ip_st == FieldState::Invalid)
      return "  Invalid IP address";
    if (port_st == FieldState::Invalid)
      return "  Port must be 1 – 65535";
    if (!peer_timeout.empty()) {
      bool valid_digits =
          std::all_of(peer_timeout.begin(), peer_timeout.end(), [](char c) {
            return std::isdigit((unsigned char)c);
          });
      if (!valid_digits)
        return "  Timeout must be a number (seconds)";
      try {
        long v = std::stol(peer_timeout);
        if (v < 1 || v > 3600)
          return "  Timeout must be 1 – 3600 s";
      } catch (...) {
        return "  Timeout must be a number (seconds)";
      }
    }
    if (ip_st == FieldState::Valid && port_st == FieldState::Valid) {
      uint16_t p = static_cast<uint16_t>(std::stoi(peer_port));
      std::lock_guard<std::mutex> lock(peer_mutex);
      if (isDuplicate(peer_ip, p))
        return "  Already connected to this peer";
    }
    return "";
  };

  // ---- Connect button ----
  auto peer_connect_button = Button({
      .label = "CONNECT",
      .on_click =
          [&, peer]() {
            if (!connectReady())
              return;
            uint16_t p = static_cast<uint16_t>(std::stoi(peer_port));
            // Resolve timeout — default to 5 s if field is empty
            int timeout_secs = 5;
            if (!peer_timeout.empty()) {
              try {
                timeout_secs = std::stoi(peer_timeout);
              } catch (...) {
              }
            }

            peer->doResolveAndConnect(
                peer_ip,
                p,
                [&, timeout_secs](std::weak_ptr<net::Session> ws) {
                  if (auto session = ws.lock()) {
                    asio::co_spawn(
                        peer->getExecutor(),
                        [&, session, timeout_secs]() -> asio::awaitable<void> {
                          // Helper: run the handshake coroutine, optionally
                          // racing it against a steady_timer deadline.
                          auto do_handshake =
                              [&, session]() -> asio::awaitable<void> {
                            auto hello = co_await session->receiveHello();
                            co_await session->sendHello({peer->id(), hostname});
                            auto info = ExtractPeerInfo(session, hello);

                            {
                              std::lock_guard<std::mutex> lock(peer_mutex);
                              for (auto& existing : peer_list) {
                                if (existing.peer_id == hello.peer_id) {
                                  session->close();
                                  co_return;
                                }
                              }
                            }

                            info.tree = std::make_shared<fstree::DirectoryTree>(
                                co_await session->receiveTree());
                            co_await session->sendTree(*local_peer.tree);

                            {
                              std::lock_guard<std::mutex> lock(peer_mutex);
                              peer_list.push_back(std::move(info));
                            }

                            start_refresh_listener(peer_list.size() - 1,
                                                   session);
                            screen.PostEvent(Event::Custom);
                          };

                          try {
                            // Always race the handshake against the timeout.
                            // Snapshot target address before we potentially
                            // close the socket on timeout.
                            std::string target_addr;
                            try {
                              target_addr = session->socket()
                                                .remote_endpoint()
                                                .address()
                                                .to_string();
                            } catch (...) {
                              target_addr = peer_ip;
                            }

                            asio::steady_timer timer(
                                co_await asio::this_coro::executor);
                            timer.expires_after(
                                std::chrono::seconds(timeout_secs));

                            auto result = co_await (
                                do_handshake() ||
                                timer.async_wait(asio::use_awaitable));

                            if (result.index() == 1) {
                              // Timer won — timeout
                              session->close();
                              post_error("Connection Timed Out",
                                         "Could not complete handshake with " +
                                             target_addr + " within " +
                                             std::to_string(timeout_secs) +
                                             " s.");
                            }
                            // index == 0 means handshake completed normally
                          } catch (const boost::system::system_error& e) {
                            session->close();
                            post_error("Connection Error",
                                       std::string("Network error during "
                                                   "handshake: ") +
                                           e.what());
                          } catch (const std::exception& e) {
                            session->close();
                            post_error("Handshake Failed", e.what());
                          } catch (...) {
                            session->close();
                            post_error("Connection Error",
                                       "An unknown error occurred while "
                                       "connecting.");
                          }
                        },
                        asio::detached);
                  }
                },
                // on_error: called when resolve or TCP connect itself fails
                [&](const boost::system::error_code& ec) {
                  post_error("Connection Failed",
                             "Could not connect to " + peer_ip + ":" +
                                 peer_port + ".\n" + ec.message());
                });
          },
      .transform =
          [&](EntryState state) {
            bool ready = connectReady();
            auto btn   = text(" CONNECT ") | center;
            if (!ready)
              return btn | borderLight | dim | color(Color::GrayDark);
            return state.focused ? btn | borderDouble | color(Color::Green)
                                 : btn | borderHeavy | color(Color::Green);
          },
  });

  auto peer_ip_input   = peer_input_validated(&peer_ip, "IP Address", [&]() {
    return validateIP(peer_ip);
  });
  auto peer_port_input = peer_input_validated(&peer_port, "Port", [&]() {
    return validatePort(peer_port);
  });
  auto peer_timeout_input =
      peer_input_validated(&peer_timeout, "Timeout (s)", [&]() {
        if (peer_timeout.empty())
          return FieldState::Empty;  // empty = default 5 s
        for (char c : peer_timeout)
          if (!std::isdigit(static_cast<unsigned char>(c)))
            return FieldState::Invalid;
        try {
          long v = std::stol(peer_timeout);
          return (v >= 1 && v <= 3600) ? FieldState::Valid
                                       : FieldState::Invalid;
        } catch (...) {
          return FieldState::Invalid;
        }
      });

  auto peer_container = Container::Vertical({
      peer_ip_input,
      Container::Horizontal({
          peer_port_input | flex,
          peer_timeout_input | flex,
      }),
      peer_connect_button,
  });

  auto peer_renderer = Renderer(peer_container, [&]() {
    std::string hint = connectHint();
    Elements body;
    body.push_back(peer_container->Render());
    if (!hint.empty())
      body.push_back(text(hint) | color(Color::Red) | dim);

    return vbox({text("CONNECT TO PEER") | bold | dim | center,
                 separatorLight(),
                 vbox(std::move(body))}) |
           borderLight;
  });

  // -------------------------------------------------------------------------------------------------
  // CONNECTED PEER PANEL
  // -------------------------------------------------------------------------------------------------

  // NOTE: Try to implement a Vertical underline menu
  auto connected_peer_menu_entry = [&selected_peer,
                                    bg_color](const PeerInfo& info) {
    MenuEntryOption entry_option;
    std::string peer_name = info.name;
    entry_option.label    = peer_name;
    std::string address =
        info.address.to_v4().to_string() + ":" + std::to_string(info.port);
    entry_option.transform = [&selected_peer, address, peer_name, &bg_color](
                                 EntryState state) {
      // clang-format off
      auto peer_info = vbox({
        text(address),
        text(peer_name) | dim
      });

      if(state.index == selected_peer) {
        return hbox({
          separatorLight(),
          text(" "),
          peer_info
        }) | bgcolor(Color::Palette256(0));
      } else if (state.active){
        return hbox({
          separatorLight() | color(Color::White),
          text(" ") | color(Color::Black),
          peer_info | color(Color::Black)
        }) | bgcolor(Color::White);
      } else {
        return hbox({
          separatorLight() | color(bg_color),
          text(" "),
          peer_info
        });
      }
      // clang-format on
    };
    auto entry = MenuEntry(entry_option);
    return entry;
  };

  auto connected_peer_container = Container::Vertical({}, &selected_peer);

  auto rebuild_peer_list = [&]() {
    connected_peer_container->DetachAllChildren();

    std::lock_guard<std::mutex> lock(peer_mutex);
    for (auto& info : peer_list) {
      connected_peer_container->Add(connected_peer_menu_entry(info));
    }
  };

  auto connected_peer_renderer = Renderer(connected_peer_container, [&]() {
    rebuild_peer_list();
    return vbox({text("CONNECTED PEERS") | bold | dim | center,
                 separatorLight(),
                 connected_peer_container->ChildCount() != 0
                     ? connected_peer_container->Render() | frame
                     : text("NO PEER CONNECTED")}) |
           borderLight;
  });

  // NOTE:: Change Design of Diff - view

  // -------------------------------------------------------------------------------------------------
  // DIFF VIEW
  // -------------------------------------------------------------------------------------------------

  // ---- Helpers ----

  auto fmt_size = [](uint64_t sz) -> std::string {
    if (sz < 1024)
      return std::to_string(sz) + " B";
    if (sz < 1024 * 1024)
      return std::to_string(sz / 1024) + " KB";
    return std::to_string(sz / (1024 * 1024)) + " MB";
  };

  auto diff_row = [&](const fstree::NodeDiff& d) -> Element {
    std::string tag, path_str;
    Color tag_color = Color::White;
    bool is_dir     = false;

    if (d.type == fstree::ChangeType::Added) {
      tag       = " + ";
      tag_color = Color::Green;
      path_str  = d.new_node->path.generic_string();
      is_dir    = (d.new_node->type == fstree::NodeType::Directory);
    } else if (d.type == fstree::ChangeType::Deleted) {
      tag       = " - ";
      tag_color = Color::Red;
      path_str  = d.old_node->path.generic_string();
      is_dir    = (d.old_node->type == fstree::NodeType::Directory);
    } else {
      tag       = " ~ ";
      tag_color = Color::Yellow;
      path_str  = d.new_node->path.generic_string();
    }

    std::string right_str;
    if (is_dir) {
      right_str = "dir/";
    } else if (d.type == fstree::ChangeType::Added) {
      right_str = fmt_size(d.new_node->size);
    } else if (d.type == fstree::ChangeType::Deleted) {
      right_str = fmt_size(d.old_node->size);
    } else if (d.type == fstree::ChangeType::Modified) {
      right_str =
          fmt_size(d.old_node->size) + " -> " + fmt_size(d.new_node->size);
    }

    return hbox({
        text(tag) | color(tag_color) | bold,
        text(path_str) | flex,
        text(right_str) | color(Color::GrayDark) | dim,
    });
  };

  // ---- Refresh button ----
  auto refresh_button = Button({
      .label = "REFRESH",
      .on_click =
          [&]() {
            local_peer.tree = std::make_shared<fstree::DirectoryTree>(
                local_peer.tree->root_path);
            sync_state.phase.store(SyncState::Phase::Idle);
            std::shared_ptr<net::Session> session;
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              if (peer_list.empty() ||
                  selected_peer >= static_cast<int>(peer_list.size()))
                return;
              session = peer_list[selected_peer].session.lock();
              if (!session)
                return;
            }
            asio::co_spawn(
                peer->getExecutor(),
                [&, session]() -> asio::awaitable<void> {
                  co_await session->sendTreeRequest();
                  co_await session->sendTaggedTree(*local_peer.tree);
                },
                asio::detached);
          },
      .transform =
          [](EntryState state) {
            auto btn = text(" Refresh ") | center;
            return state.focused ? btn | borderDouble : btn | borderLight;
          },
  });

  // ---- Sync button ----
  // Sends SyncRequest + our tree. The listener on the remote side handles it:
  // it computes the diff (from the requester's perspective), streams files /
  // delete notices, then sends SyncDone. Our listener receives all of that and
  // updates sync_state as it goes, posting events so the UI re-renders.
  auto sync_button = Button({
      .label = "SYNC",
      .on_click =
          [&]() {
            using Phase = SyncState::Phase;
            auto ph     = sync_state.phase.load();
            if (ph == Phase::SyncingTrees || ph == Phase::SyncingFiles)
              return;

            std::shared_ptr<net::Session> session;
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              if (peer_list.empty() ||
                  selected_peer >= static_cast<int>(peer_list.size()))
                return;
              session = peer_list[selected_peer].session.lock();
              if (!session)
                return;
            }

            sync_state.phase.store(Phase::SyncingTrees);
            sync_state.files_done.store(0);
            sync_state.files_total.store(0);
            screen.PostEvent(Event::Custom);

            asio::co_spawn(
                peer->getExecutor(),
                [&, session]() -> asio::awaitable<void> {
                  // Send SyncRequest tag + our current tree.
                  // The remote listener will:
                  //   1. receive our tree
                  //   2. compute diffs (what WE are missing)
                  //   3. stream FileData / DeleteFile packets
                  //   4. send SyncDone
                  // Our listener picks all of that up and updates sync_state.
                  try {
                    co_await session->sendPacketType(
                        net::Session::PacketType::SyncRequest);
                    co_await session->sendTaggedTree(*local_peer.tree);
                  } catch (const boost::system::system_error& e) {
                    sync_state.phase.store(Phase::Error);
                    post_error("Sync Failed",
                               std::string("Failed to send sync request: ") +
                                   e.what());
                  } catch (const std::exception& e) {
                    sync_state.phase.store(Phase::Error);
                    post_error("Sync Failed", e.what());
                  } catch (...) {
                    sync_state.phase.store(Phase::Error);
                    post_error(
                        "Sync Failed",
                        "An unknown error occurred while starting sync.");
                    screen.PostEvent(Event::Custom);
                  }
                },
                asio::detached);
          },
      .transform =
          [&](EntryState state) {
            using Phase = SyncState::Phase;
            auto ph     = sync_state.phase.load();
            bool busy = ph == Phase::SyncingTrees || ph == Phase::SyncingFiles;
            auto btn  = text(busy ? " Syncing... " : " Sync ") | center;
            if (busy)
              return btn | borderLight | dim;
            return state.focused ? btn | borderDouble : btn | borderLight;
          },
  });

  // ---- Disconnect button ----
  auto disconnect_button = Button({
      .label = "DISCONNECT",
      .on_click =
          [&]() {
            using Phase = SyncState::Phase;
            auto ph     = sync_state.phase.load();
            if (ph == Phase::SyncingTrees || ph == Phase::SyncingFiles)
              return;

            std::shared_ptr<net::Session> session;
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              if (peer_list.empty() ||
                  selected_peer >= static_cast<int>(peer_list.size()))
                return;
              session = peer_list[selected_peer].session.lock();
              if (!session)
                return;
              remove_peer_by_session(session);
            }

            sync_state.phase.store(Phase::Idle);
            screen.PostEvent(Event::Custom);

            // Notify the remote peer, then close the socket
            asio::co_spawn(
                peer->getExecutor(),
                [&, session]() -> asio::awaitable<void> {
                  try {
                    co_await session->sendDisconnectRequest();
                  } catch (...) {
                    // Best-effort: ignore send errors on disconnect
                  }
                  session->close();
                },
                asio::detached);
          },
      .transform =
          [&](EntryState state) {
            using Phase = SyncState::Phase;
            auto ph     = sync_state.phase.load();
            bool busy = ph == Phase::SyncingTrees || ph == Phase::SyncingFiles;
            bool no_peer = peer_list.empty() ||
                           selected_peer >= static_cast<int>(peer_list.size());
            auto btn     = text(" Disconnect ") | center;
            if (busy || no_peer)
              return btn | borderLight | dim | color(Color::GrayDark);
            return state.focused ? btn | borderDouble | color(Color::Red)
                                 : btn | borderLight | color(Color::Red);
          },
  });

  // ---- Diff renderer ----
  auto diff_renderer = Renderer(
      Container::Horizontal({refresh_button, sync_button, disconnect_button}),
      [&]() -> Element {
        std::string peer_name;
        std::vector<fstree::NodeDiff> diffs;
        bool has_peer = false;
        bool has_tree = false;

        {
          std::lock_guard<std::mutex> lock(peer_mutex);
          has_peer = !peer_list.empty() &&
                     selected_peer < static_cast<int>(peer_list.size());
          if (has_peer) {
            peer_name = peer_list[selected_peer].name + "  (" +
                        peer_list[selected_peer].address.to_string() + ":" +
                        std::to_string(peer_list[selected_peer].port) + ")";
            has_tree  = peer_list[selected_peer].tree != nullptr;
            if (has_tree)
              diffs = fstree::diffTree(*local_peer.tree,
                                       *peer_list[selected_peer].tree);
          }
        }

        // -- Header row --
        Element header = hbox({
            text(" DIFF  ") | bold | dim,
            separatorLight(),
            text("  "),
            has_peer ? (text(peer_name) | color(Color::Cyan))
                     : (text("select a peer") | dim),
            filler(),
            refresh_button->Render(),
            text(" "),
            sync_button->Render(),
            text(" "),
            disconnect_button->Render(),
            text(" "),
        });

        // -- Progress bar (shown while syncing) --
        using Phase = SyncState::Phase;
        auto phase  = sync_state.phase.load();
        int f_done  = sync_state.files_done.load();
        int f_total = sync_state.files_total.load();

        Elements progress_row;
        if (phase == Phase::SyncingTrees || phase == Phase::SyncingFiles ||
            phase == Phase::Done || phase == Phase::Error) {
          std::string status_text;
          float bar_fraction = 0.0f;
          Color status_color = Color::White;

          if (phase == Phase::SyncingTrees) {
            status_text  = "STATUS: SYNCING TREES";
            bar_fraction = 0.0f;
            status_color = Color::Yellow;
          } else if (phase == Phase::SyncingFiles) {
            int total    = f_total > 0 ? f_total : 1;
            status_text  = "STATUS: SYNCING FILES [" + std::to_string(f_done) +
                           "/" + std::to_string(f_total) + "]";
            bar_fraction = static_cast<float>(f_done) / total;
            status_color = Color::Cyan;
          } else if (phase == Phase::Done) {
            status_text  = "STATUS: SYNC COMPLETE";
            bar_fraction = 1.0f;
            status_color = Color::Green;
          } else {
            status_text  = "STATUS: SYNC ERROR";
            bar_fraction = 0.0f;
            status_color = Color::Red;
          }

          progress_row.push_back(separatorLight());
          progress_row.push_back(hbox({
              text("  "),
              text(status_text) | color(status_color) | bold,
              text("  "),
              gauge(bar_fraction) | flex | color(status_color),
              text("  "),
          }));
        }

        // -- Diff body --
        Elements rows;
        rows.push_back(separatorLight());
        if (!has_peer) {
          rows.push_back(text("  No peer selected.") | dim | center);
        } else if (!has_tree) {
          rows.push_back(text("  Waiting for tree from peer...") | dim |
                         center);
        } else if (diffs.empty()) {
          rows.push_back(text("  Trees are identical.") | color(Color::Green) |
                         center);
        } else {
          int added = 0, deleted = 0, modified = 0;
          for (auto& d : diffs) {
            if (d.type == fstree::ChangeType::Added)
              ++added;
            else if (d.type == fstree::ChangeType::Deleted)
              ++deleted;
            else
              ++modified;
          }
          rows.push_back(hbox({
              text("  "),
              text("+" + std::to_string(added)) | color(Color::Green) | bold,
              text("  "),
              text("-" + std::to_string(deleted)) | color(Color::Red) | bold,
              text("  "),
              text("~" + std::to_string(modified)) | color(Color::Yellow) |
                  bold,
              text("  changes") | dim,
          }));
          rows.push_back(separatorLight());
          for (auto& d : diffs)
            rows.push_back(diff_row(d));
        }

        return vbox({
                   header,
                   vbox(std::move(progress_row)),
                   vbox(std::move(rows)) | frame | flex,
               }) |
               flex | borderLight;
      });

  // -------------------------------------------------------------------------------------------------
  // LISTENER  (sole reader on each session post-handshake)
  // -------------------------------------------------------------------------------------------------

  start_refresh_listener = [&](std::size_t peer_idx,
                               std::shared_ptr<net::Session> session) {
    asio::co_spawn(
        peer->getExecutor(),
        [&, peer_idx, session]() -> asio::awaitable<void> {
          try {
            while (true) {
              auto pkt = co_await session->receivePacketType();

              // ---- TreeRequest: plain refresh (tree exchange only) ----
              if (pkt == net::Session::PacketType::TreeRequest) {
                // Requester sends: TreeRequest | Tree tag + payload (their
                // tree) We reply with: Tree tag + payload (our tree)
                auto pt2 = co_await session->receivePacketType();
                if (pt2 != net::Session::PacketType::Tree)
                  break;
                auto new_tree   = co_await session->receiveTreePayload();
                local_peer.tree = std::make_shared<fstree::DirectoryTree>(
                    local_peer.tree->root_path);
                co_await session->sendTaggedTree(*local_peer.tree);

                {
                  std::lock_guard<std::mutex> lock(peer_mutex);
                  if (peer_idx < peer_list.size())
                    peer_list[peer_idx].tree =
                        std::make_shared<fstree::DirectoryTree>(
                            std::move(new_tree));
                }
                screen.PostEvent(Event::Custom);

                // ---- Tree: unsolicited push ----
              } else if (pkt == net::Session::PacketType::Tree) {
                auto new_tree = co_await session->receiveTreePayload();
                {
                  std::lock_guard<std::mutex> lock(peer_mutex);
                  if (peer_idx < peer_list.size())
                    peer_list[peer_idx].tree =
                        std::make_shared<fstree::DirectoryTree>(
                            std::move(new_tree));
                }
                screen.PostEvent(Event::Custom);

                // ---- SyncRequest: remote wants us to send them our files ----
              } else if (pkt == net::Session::PacketType::SyncRequest) {
                // 1. Receive requester's current tree
                auto pt2 = co_await session->receivePacketType();
                if (pt2 != net::Session::PacketType::Tree)
                  break;
                auto requester_tree = co_await session->receiveTreePayload();

                // 2. Compute what the requester is missing (diff from their
                // POV)
                //    local_peer.tree = "new" (ours), requester_tree = "old"
                auto diffs = fstree::diffTree(requester_tree, *local_peer.tree);

                std::function<int(const fstree::Node&)> countOps =
                    [&](const fstree::Node& node) -> int {
                  if (node.type == fstree::NodeType::File)
                    return 1;
                  const auto& kids = fstree::children(node);
                  if (kids.empty())
                    return 1;
                  int n = 0;
                  for (auto& c : kids)
                    n += countOps(*c);
                  return n;
                };

                // Helper: recursively send every file inside an added subtree,
                // or send a CreateDir for an empty directory.
                std::function<asio::awaitable<void>(const fstree::Node&)>
                    sendSubtree;
                sendSubtree =
                    [&](const fstree::Node& node) -> asio::awaitable<void> {
                  if (node.type == fstree::NodeType::File) {
                    auto it = local_peer.tree->index.find(node.path);
                    if (it != local_peer.tree->index.end())
                      co_await session->sendTaggedFile(*local_peer.tree,
                                                       *it->second);
                  } else {
                    const auto& kids = fstree::children(node);
                    if (kids.empty()) {
                      co_await session->sendCreateDir(node.path);
                    } else {
                      for (auto& child : kids)
                        co_await sendSubtree(*child);
                    }
                  }
                };

                // Count all operations
                int total_ops = 0;
                for (auto& d : diffs) {
                  if (d.type == fstree::ChangeType::Added) {
                    auto it = local_peer.tree->index.find(d.new_node->path);
                    if (it != local_peer.tree->index.end())
                      total_ops += countOps(*it->second);
                  } else if (d.type == fstree::ChangeType::Deleted) {
                    ++total_ops;  // one remove_all
                  } else if (d.type == fstree::ChangeType::Modified &&
                             d.new_node->type == fstree::NodeType::File) {
                    ++total_ops;
                  }
                }

                // 3. Tell the requester how many operations to expect
                co_await session->sendSyncHeader(
                    static_cast<uint32_t>(total_ops));

                // 4. Stream files / deletes to requester
                for (auto& d : diffs) {
                  if (d.type == fstree::ChangeType::Added) {
                    // Added file or directory subtree
                    auto it = local_peer.tree->index.find(d.new_node->path);
                    if (it != local_peer.tree->index.end())
                      co_await sendSubtree(*it->second);

                  } else if (d.type == fstree::ChangeType::Deleted) {
                    // Deleted file or directory — remove_all handles recursion
                    co_await session->sendDeleteNotice(d.old_node->path);

                  } else if (d.type == fstree::ChangeType::Modified &&
                             d.new_node->type == fstree::NodeType::File) {
                    auto it = local_peer.tree->index.find(d.new_node->path);
                    if (it != local_peer.tree->index.end())
                      co_await session->sendTaggedFile(*local_peer.tree,
                                                       *it->second);
                  }
                }

                // 5. Send our own tree so the requester's diff view updates,
                //    then signal end of sync
                co_await session->sendTaggedTree(*local_peer.tree);
                co_await session->sendSyncDone();

                // The requester now mirrors our tree — store a fresh snapshot
                // so our diff view reflects the post-sync state immediately.
                {
                  std::lock_guard<std::mutex> lock(peer_mutex);
                  if (peer_idx < peer_list.size())
                    peer_list[peer_idx].tree =
                        std::make_shared<fstree::DirectoryTree>(
                            local_peer.tree->root_path);
                }
                screen.PostEvent(Event::Custom);

                // ---- SyncHeader: total op count from sender ----
              } else if (pkt == net::Session::PacketType::SyncHeader) {
                uint32_t total = co_await session->receiveSyncHeader();
                sync_state.files_total.store(static_cast<int>(total));
                sync_state.phase.store(SyncState::Phase::SyncingFiles);
                screen.PostEvent(Event::Custom);

                // ---- FileData: we are the requester, receiving a file ----
              } else if (pkt == net::Session::PacketType::FileData) {
                co_await session->receiveFile(*local_peer.tree, false);
                sync_state.files_done.fetch_add(1);
                screen.PostEvent(Event::Custom);

                // ---- DeleteFile: we are the requester, delete a path ----
              } else if (pkt == net::Session::PacketType::DeleteFile) {
                auto rel_path = co_await session->receiveRelPath();
                auto abs_path = local_peer.tree->root_path / rel_path;
                std::error_code ec;
                std::filesystem::remove_all(abs_path, ec);
                // defer tree rebuild to SyncDone
                sync_state.files_done.fetch_add(1);
                screen.PostEvent(Event::Custom);

                // ---- CreateDir: create an empty directory ----
              } else if (pkt == net::Session::PacketType::CreateDir) {
                auto rel_path = co_await session->receiveRelPath();
                auto abs_path = local_peer.tree->root_path / rel_path;
                std::error_code ec;
                std::filesystem::create_directories(abs_path, ec);
                // defer tree rebuild to SyncDone
                sync_state.files_done.fetch_add(1);
                screen.PostEvent(Event::Custom);

                // ---- SyncHeader: total op count from sender ----
              } else if (pkt == net::Session::PacketType::SyncHeader) {
                uint32_t total = co_await session->receiveSyncHeader();
                sync_state.files_total.store(static_cast<int>(total));
                sync_state.phase.store(SyncState::Phase::SyncingFiles);
                screen.PostEvent(Event::Custom);

                // ---- SyncDone: all operations received ----
              } else if (pkt == net::Session::PacketType::SyncDone) {
                // Single rebuild covering all received files, deletes, and dirs
                *local_peer.tree =
                    fstree::DirectoryTree(local_peer.tree->root_path);
                sync_state.phase.store(SyncState::Phase::Done);
                screen.PostEvent(Event::Custom);

                // ---- DisconnectRequest: remote peer is leaving ----
              } else if (pkt == net::Session::PacketType::DisconnectRequest) {
                {
                  std::lock_guard<std::mutex> lock(peer_mutex);
                  remove_peer_by_session(session);
                }
                sync_state.phase.store(SyncState::Phase::Idle);
                session->close();
                screen.PostEvent(Event::Custom);
                co_return;
              }
              // Unknown tags silently skipped — forward-compatible
            }
          } catch (const boost::system::system_error& e) {
            // Session closed or I/O error — exit listener cleanly.
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              remove_peer_by_session(session);
            }
            auto ph = sync_state.phase.load();
            if (ph == SyncState::Phase::SyncingTrees ||
                ph == SyncState::Phase::SyncingFiles) {
              sync_state.phase.store(SyncState::Phase::Error);
              post_error(
                  "Sync Interrupted",
                  std::string("Connection lost during sync: ") + e.what());
            } else if (ph != SyncState::Phase::Idle &&
                       ph != SyncState::Phase::Done) {
              post_error(
                  "Connection Lost",
                  std::string("Peer disconnected unexpectedly: ") + e.what());
            }
            screen.PostEvent(Event::Custom);
          } catch (const std::exception& e) {
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              remove_peer_by_session(session);
            }
            auto ph = sync_state.phase.load();
            if (ph == SyncState::Phase::SyncingTrees ||
                ph == SyncState::Phase::SyncingFiles) {
              sync_state.phase.store(SyncState::Phase::Error);
              post_error("Sync Error", e.what());
            }
            screen.PostEvent(Event::Custom);
          } catch (...) {
            // Truly unknown error — remove peer silently
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              remove_peer_by_session(session);
            }
            auto ph = sync_state.phase.load();
            if (ph == SyncState::Phase::SyncingTrees ||
                ph == SyncState::Phase::SyncingFiles)
              sync_state.phase.store(SyncState::Phase::Error);
            screen.PostEvent(Event::Custom);
          }
        },
        asio::detached);
  };

  // -------------------------------------------------------------------------------------------------
  // ERROR MODAL
  // -------------------------------------------------------------------------------------------------

  auto error_modal_component = [&]() -> Component {
    auto dismiss_button = Button({
        .label = "  OK  ",
        .on_click =
            [&]() {
              std::lock_guard<std::mutex> lk(error_state->mtx);
              error_state->visible = false;
            },
        .transform =
            [](EntryState state) {
              auto btn = text("  OK  ") | center | bold;
              return state.focused ? btn | borderDouble | color(Color::White)
                                   : btn | borderLight | color(Color::White);
            },
    });

    return Renderer(dismiss_button, [&, dismiss_button]() -> Element {
      std::string title, message;
      {
        std::lock_guard<std::mutex> lk(error_state->mtx);
        title   = error_state->title;
        message = error_state->message;
      }

      // Wrap long message lines manually for terminal display
      Elements msg_lines;
      std::istringstream ss(message);
      std::string line;
      while (std::getline(ss, line))
        msg_lines.push_back(text(line));

      return vbox({
                 hbox({
                     text(" ⚠  ") | color(Color::Red) | bold,
                     text(title) | bold | color(Color::Red),
                 }) | center,
                 separatorLight(),
                 vbox(std::move(msg_lines)) | color(Color::White),
                 separator(),
                 dismiss_button->Render() | center,
             }) |
             borderHeavy | color(Color::Red) | size(WIDTH, GREATER_THAN, 40) |
             bgcolor(Color::Palette256(16));
    });
  }();

  // -------------------------------------------------------------------------------------------------
  // MAIN UI
  // -------------------------------------------------------------------------------------------------

  // clang-format off
  auto base_ui = Container::Vertical({
    status_bar_renderer,
    Container::Horizontal({
      Container::Vertical({
        peer_renderer,
        connected_peer_renderer | flex
      }) | size(WIDTH, GREATER_THAN, 32),
      diff_renderer | flex,
    }) | flex
  }) | bgcolor(bg_color);

  auto ui = Modal(base_ui, error_modal_component, &error_state->visible);
  // clang-format on

  screen.Loop(ui);
  peer->stop();
  io_thread.join();
}
