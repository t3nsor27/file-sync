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

  static Node file(fs::path);
  static Node directory(fs::path);
  void generate_hash(const fs::path&);

  Node(NodeType, fs::path, Data&&);
};

const std::vector<std::unique_ptr<Node>>& children(const Node&);
std::vector<std::unique_ptr<Node>>& children(Node&);

struct DirectoryTree {
  fs::path root_path;
  std::unique_ptr<Node> root;
  std::unordered_map<fs::path, Node*> index;

  explicit DirectoryTree(fs::path);

 private:
  void buildIndex(Node&);
};

enum class ChangeType : uint8_t { Added, Deleted, Modified };

struct NodeSnapshot {
  fs::path path;
  NodeType type;
  fs::file_time_type mtime;
  uint64_t size = 0;              // files only
  std::optional<Hash> file_hash;  // files only

  explicit NodeSnapshot(const Node&);
};

struct NodeDiff {
  ChangeType type;

  std::optional<NodeSnapshot> old_node;
  std::optional<NodeSnapshot> new_node;

  static NodeDiff added(const Node&);
  static NodeDiff deleted(const Node&);
  static NodeDiff modified(const Node&, const Node&);
};

std::vector<NodeDiff> diffTree(DirectoryTree&, DirectoryTree&);

void printTree(Node&, std::string prefix = "");
void printHash(const Hash&);

}  // namespace fstree
