#include <boost/asio.hpp>
#include <cstdint>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
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

int main(int argc, char* argv[]) {
  using namespace ftxui;

  if (argc != 2) {
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
  PeerInfo local_peer{hostname, peer->id(), ip_addr, port};

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
            {
              std::lock_guard<std::mutex> lock(peer_mutex);
              peer_list.push_back(std::move(info));
            }

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
                          {
                            std::lock_guard<std::mutex> lock(peer_mutex);
                            peer_list.push_back(std::move(info));
                          }

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
      }) | size(WIDTH, GREATER_THAN, 30),
        Renderer([&]() {
          return vbox({
          text("IP is: " + peer_ip),
          text("Port is: " + peer_port),
          text("Timeout is: " + peer_timeout),
          text("Selected_peer: " +
              std::to_string(selected_peer))}) | flex | border;
        })
    }) | flex
  }) | bgcolor(bg_color);
  // clang-format on

  screen.Loop(ui);
  peer->stop();
  io_thread.join();
}
