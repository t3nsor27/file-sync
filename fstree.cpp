#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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
  std::string name;
  NodeType type;
  fs::file_time_type mtime;

  using Data = std::variant<FileMeta, std::vector<std::unique_ptr<Node>>>;

  Data data;

 private:
  Node(std::string name, NodeType type, fs::file_time_type mtime, Data&& data)
      : name(std::move(name)),
        type(type),
        mtime(mtime),
        data(std::move(data)) {}

 public:
  static Node file(const fs::path& file_path) {
    if (!fs::directory_entry(file_path).is_regular_file())
      throw std::invalid_argument("Path must point to a file.");

    FileMeta meta;
    meta.size = fs::file_size(file_path);

    return Node(file_path.filename().string(),
                NodeType::File,
                fs::last_write_time(file_path),
                Data{std::move(meta)});
  }

  static Node directory(const fs::path& dir_path) {
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

    return Node(dir_path.filename().string(),
                NodeType::Directory,
                fs::last_write_time(dir_path),
                Data{std::move(children)});
  }
};

struct DirectoryTree {
  fs::path root_path;
  Node root;

  DirectoryTree(fs::path dir_path)
      : root_path(dir_path), root(Node::directory(dir_path)) {}
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

int main() {
  const std::string root_dir = "./test/100 Areas";
  fs::path root_path{root_dir};
  DirectoryTree root_dir_tree(root_path);

  printTree(root_dir_tree.root);
}

// TODO: Create hash function from openssl/sha.h
// TODO: Diff Strategy: First Pass: path + size + mtime; Second Pass: Hash
