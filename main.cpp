#include <boost/asio.hpp>
#include <cstdint>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

std::pair<std::string, std::string> GetHostInfo() {
  boost::asio::io_context io;

  std::string hostname = boost::asio::ip::host_name();
  std::string ip = "unknown";

  try {
    boost::asio::ip::udp::socket socket(io);

    socket.connect(boost::asio::ip::udp::endpoint(
        boost::asio::ip::make_address("8.8.8.8"), 80));

    ip = socket.local_endpoint().address().to_string();
  } catch (...) {
  }

  return {hostname, ip};
}

int main() {
  using namespace ftxui;
  namespace asio = boost::asio;

  auto screen = ScreenInteractive::Fullscreen();
  auto bg_color = Color::Palette256(16);

  auto [hostname, ip] = GetHostInfo();

  // -------------------------------------------------------------------------------------------------
  // STATUS BAR
  // -------------------------------------------------------------------------------------------------
  auto status_bar_renderer = Renderer([&] {
    auto dimText = [](std::string t) {
      return text(t) | dim /*| border*/;
    };

    std::string separator = "  ";

    return hbox({text("file-sync") | color(Color::White) | bold,
                 filler(),
                 dimText("no peer connected"),
                 dimText(separator),
                 dimText(hostname),
                 dimText(separator),
                 dimText(ip),
                 dimText(separator),
                 dimText("awaiting connection")}) |
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
      .on_click = []() {},
      .transform =
          [&](EntryState state) {
            if (state.focused) {
              return text("CONNECT") | center | borderDouble;
            }
            return text("CONNECT") | center | borderHeavy;
          },
  });

  std::string peer_ip, peer_port, peer_timeout;
  auto peer_ip_input = peer_input(&peer_ip, "IP Address");
  auto peer_port_input = peer_input(&peer_port, "Port");
  auto peer_timeout_input = peer_input(&peer_timeout, "Timeout");

  auto peer_container = Container::Vertical({
      peer_ip_input,
      Container::Horizontal({
          peer_port_input,
          peer_timeout_input,
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
  auto connected_peer_menu_entry = [&selected_peer, bg_color](
                                       std::string peer_name,
                                       asio::ip::address peer_address,
                                       uint16_t peer_port) {
    MenuEntryOption entry_option;
    entry_option.label = peer_name;
    std::string address =
        peer_address.to_string() + ":" + std::to_string(peer_port);
    entry_option.transform = [&selected_peer, peer_name, address, bg_color](
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

  auto connected_peer_container = Container::Vertical(
      {
          connected_peer_menu_entry(
              "Peer1", asio::ip::make_address("192.160.122.1"), 9000),
          connected_peer_menu_entry(
              "Peer2", asio::ip::make_address("192.160.122.2"), 9000),
          connected_peer_menu_entry(
              "Peer3", asio::ip::make_address("192.160.122.3"), 9000),
      },
      &selected_peer);

  auto connected_peer_renderer = Renderer(connected_peer_container, [&]() {
    return vbox({text("CONNECTED PEERS") | bold | dim | center,
                 separatorLight(),
                 connected_peer_container->Render() | frame}) |
           borderLight;
  });

  // -------------------------------------------------------------------------------------------------
  // MAIN UI
  // -------------------------------------------------------------------------------------------------

  auto ui = Container::Vertical(
                {status_bar_renderer,
                 Container::Horizontal(
                     {Container::Vertical(
                          {peer_renderer, connected_peer_renderer | flex}),
                      Renderer([&]() {
                        return vbox({text("IP is: " + peer_ip),
                                     text("Port is: " + peer_port),
                                     text("Timeout is: " + peer_timeout),
                                     text("Selected_peer: " +
                                          std::to_string(selected_peer))}) |
                               flex | border;
                      })}) |
                     flex}) |
            bgcolor(bg_color);

  screen.Loop(ui);
}
