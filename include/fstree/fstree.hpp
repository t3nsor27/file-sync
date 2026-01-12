#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace fstree {
namespace fs = std::filesystem;
using Hash = std::array<uint8_t, 32>;

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

  static Node file(fs::path file_path);
  static Node directory(fs::path dir_path);
  void generate_hash(const fs::path& root);

 private:
  Node(NodeType type, fs::path path, Data&& data);
};

const std::vector<std::unique_ptr<Node>>& children(const Node& n);
std::vector<std::unique_ptr<Node>>& children(Node& n);

struct DirectoryTree {
  fs::path root_path;
  std::unique_ptr<Node> root;
  std::unordered_map<fs::path, Node*> index;

  explicit DirectoryTree(fs::path dir_path);

 private:
  void buildIndex(Node& node);
};

enum class ChangeType : uint8_t { Added, Deleted, Modified };

struct NodeSnapshot {
  fs::path path;
  NodeType type;
  fs::file_time_type mtime;
  uint64_t size = 0;              // files only
  std::optional<Hash> file_hash;  // files only

  explicit NodeSnapshot(const Node& node);
};

struct NodeDiff {
  ChangeType type;

  std::optional<NodeSnapshot> old_node;
  std::optional<NodeSnapshot> new_node;

  static NodeDiff added(const Node& new_node);
  static NodeDiff deleted(const Node& old_node);
  static NodeDiff modified(const Node& old_node, const Node& new_node);
};

std::vector<NodeDiff> diffTree(DirectoryTree& old_tree,
                               DirectoryTree& new_tree);

void printTree(Node& node, std::string prefix = "");
void printHash(const Hash& hash);
}  // namespace fstree
