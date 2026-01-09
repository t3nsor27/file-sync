#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fs = std::filesystem;
using Hash = std::array<uint8_t, 32>;
#define get_children std::get<std::vector<std::unique_ptr<Node>>>

enum class NodeType : uint8_t { File, Directory };

struct FileMeta {
  uint64_t size;
  std::optional<Hash> file_hash;
};

struct Node {
  fs::path path;
  std::string name;
  NodeType type;
  fs::file_time_type mtime;

  using Data = std::variant<FileMeta, std::vector<std::unique_ptr<Node>>>;

  Data data;

 private:
  Node(NodeType type, fs::path path, Data&& data)
      : path(std::move(path)),
        name(this->path.filename().string()),
        type(type),
        mtime(fs::last_write_time(this->path)),
        data(std::move(data)) {}

 public:
  static Node file(fs::path file_path) {
    if (!fs::directory_entry(file_path).is_regular_file())
      throw std::invalid_argument("Path must point to a file.");

    FileMeta meta;
    meta.size = fs::file_size(file_path);

    return Node(NodeType::File, std::move(file_path), Data{std::move(meta)});
  }

  static Node directory(fs::path dir_path) {
    std::vector<std::unique_ptr<Node>> children;

    if (!fs::directory_entry(dir_path).is_directory())
      throw std::invalid_argument("Path must point a directory.");

    for (auto const& dir_entry : fs::directory_iterator(dir_path)) {
      if (dir_entry.is_regular_file()) {
        children.push_back(
            std::make_unique<Node>(Node::file(dir_entry.path())));
      } else {
        children.push_back(
            std::make_unique<Node>(Node::directory(dir_entry.path())));
      }
    }

    std::sort(
        children.begin(),
        children.end(),
        [](const std::unique_ptr<Node>& a, const std::unique_ptr<Node>& b) {
          if (a->type != b->type)
            return a->type == NodeType::Directory;
          else
            return a->name < b->name;
        });

    return Node(
        NodeType::Directory, std::move(dir_path), Data{std::move(children)});
  }

  void generate_hash() {
    if (type != NodeType::File)
      return;

    auto& file_meta = std::get<FileMeta>(data);
    if (file_meta.file_hash.has_value())
      return;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("Failed to open file.");

    auto size = file.tellg();
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.seekg(0);

    if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
      throw std::runtime_error("Failed to read file.");

    file_meta.file_hash.emplace();
    SHA256(buffer.data(), buffer.size(), file_meta.file_hash->data());
  }
};

struct DirectoryTree {
  fs::path root_path;
  std::unique_ptr<Node> root;
  std::unordered_map<fs::path, Node*> index;

  DirectoryTree(fs::path dir_path)
      : root_path(dir_path),
        root(std::make_unique<Node>(Node::directory(dir_path))) {
    buildIndex(*root);
  }

 private:
  void buildIndex(Node& node) {
    index[node.path] = &node;
    if (node.type == NodeType::Directory) {
      for (auto const& children : get_children(node.data)) {
        buildIndex(*children);
      }
    }
  }
};

enum class ChangeType : uint8_t { Added, Deleted, Modified };

struct NodeSnapshot {
  fs::path path;
  NodeType type;
  fs::file_time_type mtime;
  uint64_t size = 0;         // files only
  std::optional<Hash> hash;  // files only
};

struct NodeDiff {
  fs::path path;
  ChangeType type;

  std::optional<NodeSnapshot> old_node;
  std::optional<NodeSnapshot> new_node;
};

void printTree(Node& node, std::string prefix = "") {
  std::cout << prefix << "|--" << node.name << "\n";
  if (node.type == NodeType::Directory) {
    auto& childrens = get_children(node.data);
    for (auto const& children : childrens) {
      printTree(*children, prefix + "|  ");
    }
  }
}

// TODO: Diff Strategy: First Pass: path + size + mtime; Second Pass: Hash

int main() {}
