#include <boost/asio.hpp>
#include <cstdint>
#include <filesystem>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "./include/fstree/fstree.hpp"
#include "./include/net/peer.hpp"

namespace asio = boost::asio;
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
  auto& socket = session->socket();
  auto endpoint = socket.remote_endpoint();

  PeerInfo info;
  info.address = endpoint.address();
  info.port = endpoint.port();
  info.session = session;

  info.peer_id = hello.peer_id;
  info.name = hello.hostname;

  return info;
}

// TODO: implement send and recv diff-ed files
int main(int argc, char* argv[]) {
  using namespace ftxui;

  if (argc != 3) {
    return 0;
  }

  auto screen = ScreenInteractive::Fullscreen();
  auto bg_color = Color::Palette256(16);

  auto [hostname, ip_addr] = GetHostInfo();
  std::string ip = ip_addr.to_string();
  std::string peer_ip, peer_port, peer_timeout;
  std::mutex peer_mutex;
  std::vector<PeerInfo> peer_list;
  uint16_t port = std::stoi(argv[1]);
  auto peer = std::make_shared<net::Peer>(port);
  PeerInfo local_peer{
      hostname,
      peer->id(),
      ip_addr,
      port,
      std::make_shared<fstree::DirectoryTree>(
          fstree::DirectoryTree(std::filesystem::path(argv[2])))};

  // NOTE: Use this string for debugging
  std::string debug_str;

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
            co_await session->sendHello({peer->id(), hostname});
            auto hello = co_await session->receiveHello();
            auto info = ExtractPeerInfo(session, hello);

            co_await session->sendTree(*local_peer.tree);
            info.tree = std::make_shared<fstree::DirectoryTree>(
                co_await session->receiveTree());

            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              peer_list.push_back(std::move(info));
            }

            start_refresh_listener(peer_list.size() - 1, session);
            screen.PostEvent(Event::Custom);
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
  auto peer_input = [](StringRef s, std::string placeholder) {
    InputOption input_style = InputOption::Default();
    input_style.transform = [](InputState s) {
      if (s.focused) {
        s.element |= borderStyled(LIGHT, Color::White);
      } else {
        s.element |= borderStyled(LIGHT, Color::Black);
      }
      return s.element;
    };
    auto input = Input(s, "", input_style);

    // clang-format off
    return Renderer(input, [=] {
      return vbox({
        text(placeholder) | bold,
        input->Render() | flex | size(WIDTH, GREATER_THAN, 10)
      });
    });
    // clang-format on
  };

  auto peer_connect_button = Button({
      .label = "CONNECT",
      .on_click =
          [&, peer]() {
            if (peer_ip.empty() || peer_port.empty())
              return;
            uint16_t port = std::stoi(peer_port);
            peer->doResolveAndConnect(
                peer_ip, port, [&](std::weak_ptr<net::Session> ws) {
                  if (auto session = ws.lock()) {
                    asio::co_spawn(
                        peer->getExecutor(),
                        [&, session]() -> asio::awaitable<void> {
                          auto hello = co_await session->receiveHello();
                          co_await session->sendHello({peer->id(), hostname});
                          auto info = ExtractPeerInfo(session, hello);

                          info.tree = std::make_shared<fstree::DirectoryTree>(
                              co_await session->receiveTree());
                          co_await session->sendTree(*local_peer.tree);

                          {
                            std::lock_guard<std::mutex> lock(peer_mutex);
                            peer_list.push_back(std::move(info));
                          }

                          start_refresh_listener(peer_list.size() - 1, session);
                          screen.PostEvent(Event::Custom);
                        },
                        asio::detached);
                  }
                });
          },
      .transform =
          [&](EntryState state) {
            auto button = text("CONNECT") | center;
            if (state.focused) {
              return button | borderDouble;
            }
            return button | borderHeavy;
          },
  });

  auto peer_ip_input = peer_input(&peer_ip, "IP Address");
  auto peer_port_input = peer_input(&peer_port, "Port");
  auto peer_timeout_input = peer_input(&peer_timeout, "Timeout");

  auto peer_container = Container::Vertical({
      peer_ip_input,
      Container::Horizontal({
          peer_port_input | flex,
          peer_timeout_input | flex,
      }),
      peer_connect_button,
  });

  auto peer_renderer = Renderer(peer_container, [&]() {
    return vbox({text("CONNECT TO PEER") | bold | dim | center,
                 separatorLight(),
                 peer_container->Render()}) |
           borderLight;
  });

  // -------------------------------------------------------------------------------------------------
  // CONNECTED PEER PANEL
  // -------------------------------------------------------------------------------------------------

  // NOTE: Try to implement a Vertical underline menu
  int selected_peer = 0;
  auto connected_peer_menu_entry = [&selected_peer,
                                    bg_color](const PeerInfo& info) {
    MenuEntryOption entry_option;
    std::string peer_name = info.name;
    entry_option.label = peer_name;
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

    if (d.type == fstree::ChangeType::Added) {
      tag = " + ";
      tag_color = Color::Green;
      path_str = d.new_node->path.generic_string();
    } else if (d.type == fstree::ChangeType::Deleted) {
      tag = " - ";
      tag_color = Color::Red;
      path_str = d.old_node->path.generic_string();
    } else {
      tag = " ~ ";
      tag_color = Color::Yellow;
      path_str = d.new_node->path.generic_string();
    }

    std::string size_str;
    if (d.type == fstree::ChangeType::Added &&
        d.new_node->type == fstree::NodeType::File)
      size_str = fmt_size(d.new_node->size);
    else if (d.type == fstree::ChangeType::Deleted &&
             d.old_node->type == fstree::NodeType::File)
      size_str = fmt_size(d.old_node->size);
    else if (d.type == fstree::ChangeType::Modified)
      size_str =
          fmt_size(d.old_node->size) + " -> " + fmt_size(d.new_node->size);

    return hbox({
        text(tag) | color(tag_color) | bold,
        text(path_str) | flex,
        text(size_str) | color(Color::GrayDark),
    });
  };

  auto refresh_button = Button({
      .label = "REFRESH",
      .on_click =
          [&]() {
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

  // ---- Diff renderer ----
  auto diff_renderer = Renderer(refresh_button, [&]() -> Element {
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
        has_tree = peer_list[selected_peer].tree != nullptr;
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
    });

    // -- Body --
    Elements rows;
    if (!has_peer) {
      rows.push_back(text("  No peer selected.") | dim | center);
    } else if (!has_tree) {
      rows.push_back(text("  Waiting for tree from peer...") | dim | center);
    } else if (diffs.empty()) {
      rows.push_back(separatorLight());
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

      rows.push_back(separatorLight());
      rows.push_back(hbox({
          text("  "),
          text("+" + std::to_string(added)) | color(Color::Green) | bold,
          text("  "),
          text("-" + std::to_string(deleted)) | color(Color::Red) | bold,
          text("  "),
          text("~" + std::to_string(modified)) | color(Color::Yellow) | bold,
          text("  changes") | dim,
      }));
      rows.push_back(separatorLight());

      for (auto& d : diffs)
        rows.push_back(diff_row(d));
    }

    return vbox({
               header,
               vbox(std::move(rows)) | frame | flex,
           }) |
           flex | borderLight;
  });

  start_refresh_listener = [&](std::size_t peer_idx,
                               std::shared_ptr<net::Session> session) {
    asio::co_spawn(
        peer->getExecutor(),
        [&, peer_idx, session]() -> asio::awaitable<void> {
          try {
            while (true) {
              auto pkt = co_await session->receivePacketType();

              if (pkt == net::Session::PacketType::TreeRequest) {
                auto pt2 = co_await session->receivePacketType();
                if (pt2 != net::Session::PacketType::Tree)
                  break;
                auto new_tree = co_await session->receiveTreePayload();

                co_await session->sendTaggedTree(*local_peer.tree);

                {
                  std::lock_guard<std::mutex> lock(peer_mutex);
                  if (peer_idx < peer_list.size())
                    peer_list[peer_idx].tree =
                        std::make_shared<fstree::DirectoryTree>(
                            std::move(new_tree));
                }
                screen.PostEvent(Event::Custom);

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
              }
              // Unknown tags are silently skipped — forward-compatible
            }
          } catch (...) {
            // Session closed or I/O error — exit listener cleanly
          }
        },
        asio::detached);
  };

  // -------------------------------------------------------------------------------------------------
  // MAIN UI
  // -------------------------------------------------------------------------------------------------

  // clang-format off
  auto ui = Container::Vertical({
    status_bar_renderer,
    Container::Horizontal({
      Container::Vertical({
        peer_renderer,
        connected_peer_renderer | flex
      }) | size(WIDTH, GREATER_THAN, 32),
      diff_renderer | flex,
    }) | flex
  }) | bgcolor(bg_color);
  // clang-format on

  screen.Loop(ui);
  peer->stop();
  io_thread.join();
}
