#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

  void generate_hash(const fs::path& root) {
    if (type != NodeType::File)
      return;

    auto& file_meta = std::get<FileMeta>(data);
    if (file_meta.file_hash.has_value())
      return;

    std::ifstream file(root / path, std::ios::binary | std::ios::ate);
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
    node.path = fs::relative(node.path, root_path);
    index[node.path] = &node;
    if (node.type == NodeType::Directory) {
      for (auto const& child : get_children(node.data)) {
        buildIndex(*child);
      }
    }
  }
};

enum class ChangeType : uint8_t { Added, Deleted, Modified };

struct NodeSnapshot {
  fs::path path;
  NodeType type;
  fs::file_time_type mtime;
  uint64_t size = 0;              // files only
  std::optional<Hash> file_hash;  // files only

  NodeSnapshot(const Node& node)
      : path(node.path), type(node.type), mtime(node.mtime) {
    if (node.type == NodeType::File) {
      const FileMeta& meta = std::get<FileMeta>(node.data);
      size = meta.size;
      if (meta.file_hash.has_value()) {
        file_hash = meta.file_hash;
      }
    }
  }
};

struct NodeDiff {
  ChangeType type;

  std::optional<NodeSnapshot> old_node;
  std::optional<NodeSnapshot> new_node;

  static NodeDiff added(const Node& new_node) {
    return {ChangeType::Added, std::nullopt, NodeSnapshot(new_node)};
  }

  static NodeDiff deleted(const Node& old_node) {
    return {ChangeType::Deleted, NodeSnapshot(old_node), std::nullopt};
  }

  static NodeDiff modified(const Node& old_node, const Node& new_node) {
    return {
        ChangeType::Modified, NodeSnapshot(old_node), NodeSnapshot(new_node)};
  }
};

std::vector<NodeDiff> diffTree(DirectoryTree& old_tree,
                               DirectoryTree& new_tree) {
  std::vector<NodeDiff> nodeDiffVec;

  std::function<void(const Node*, const Node*)> diffLoop =
      [&](const Node* old_node, const Node* new_node) {
        const std::vector<std::unique_ptr<Node>>& old_vec =
            get_children(old_node->data);
        const std::vector<std::unique_ptr<Node>>& new_vec =
            get_children(new_node->data);
        auto old_it = old_vec.begin();
        auto new_it = new_vec.begin();
        for (; old_it != old_vec.end() && new_it != new_vec.end();) {
          if ((*old_it)->path == (*new_it)->path) {
            if ((*old_it)->type == NodeType::File) {
              const auto& old_it_meta = std::get<FileMeta>((*old_it)->data);
              const auto& new_it_meta = std::get<FileMeta>((*new_it)->data);
              if (old_it_meta.size == new_it_meta.size) {
                (*old_it)->generate_hash(old_tree.root_path);
                (*new_it)->generate_hash(new_tree.root_path);
                if (old_it_meta.file_hash != new_it_meta.file_hash) {
                  nodeDiffVec.push_back(NodeDiff::modified(**old_it, **new_it));
                }
              } else {
                nodeDiffVec.push_back(NodeDiff::modified(**old_it, **new_it));
              }
            } else {
              diffLoop(old_it->get(), new_it->get());
            }
            old_it++;
            new_it++;
          } else if ((*old_it)->name < (*new_it)->name) {
            nodeDiffVec.push_back(NodeDiff::deleted(**old_it));
            old_it++;
          } else {
            nodeDiffVec.push_back(NodeDiff::added(**new_it));
            new_it++;
          }
        }
        while (old_it != old_vec.end()) {
          nodeDiffVec.push_back(NodeDiff::deleted(**old_it));
          ++old_it;
        }

        while (new_it != new_vec.end()) {
          nodeDiffVec.push_back(NodeDiff::added(**new_it));
          ++new_it;
        }
      };

  diffLoop(old_tree.root.get(), new_tree.root.get());

  return nodeDiffVec;
}

void printTree(Node& node, std::string prefix = "") {
  std::cout << prefix << "|--" << node.name << "\n";
  if (node.type == NodeType::Directory) {
    auto& childrens = get_children(node.data);
    for (auto const& children : childrens) {
      printTree(*children, prefix + "|  ");
    }
  }
}

void printHash(const Hash& hash) {
  std::cout << std::hex << std::setfill('0');
  for (auto& v : hash) {
    std::cout << std::setw(2) << static_cast<unsigned int>(v);
  }
  std::cout << std::dec;
}

int main() {}
