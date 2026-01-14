#include "../include/fstree/fstree.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace fstree {

// ---------- Node ----------

Node::Node(NodeType type, fs::path path, Data&& data)
    : path(std::move(path)),
      name(this->path.filename().string()),
      type(type),
      mtime(fs::last_write_time(this->path)),
      data(std::move(data)) {}

Node::Node(fs::path path,
           std::string name,
           NodeType type,
           fs::file_time_type mtime,
           Data&& data)
    : path(std::move(path)),
      name(name),
      type(type),
      mtime(mtime),
      data(std::move(data)) {}

Node Node::file(fs::path file_path) {
  if (!fs::directory_entry(file_path).is_regular_file())
    throw std::invalid_argument("Path must point to a file.");

  FileMeta meta;
  meta.size = fs::file_size(file_path);

  return Node(NodeType::File, std::move(file_path), Data{std::move(meta)});
}

Node Node::directory(fs::path dir_path) {
  std::vector<std::unique_ptr<Node>> children;

  if (!fs::directory_entry(dir_path).is_directory())
    throw std::invalid_argument("Path must point a directory.");

  // Make node of each children
  for (auto const& dir_entry : fs::directory_iterator(dir_path)) {
    if (dir_entry.is_regular_file()) {
      children.push_back(std::make_unique<Node>(Node::file(dir_entry.path())));
    } else {
      children.push_back(
          std::make_unique<Node>(Node::directory(dir_entry.path())));
    }
  }

  // Sort childrens, giving priority to directories then name in
  // lexicographically increasing order
  std::sort(children.begin(),
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

void Node::generate_hash(const fs::path& root) {
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

// ---------- Helpers ----------

const std::vector<std::unique_ptr<Node>>& children(const Node& n) {
  return std::get<std::vector<std::unique_ptr<Node>>>(n.data);
}

std::vector<std::unique_ptr<Node>>& children(Node& n) {
  return std::get<std::vector<std::unique_ptr<Node>>>(n.data);
}

// ---------- Directory Tree ----------

DirectoryTree::DirectoryTree(fs::path dir_path)
    : root_path(dir_path),
      root(std::make_unique<Node>(Node::directory(dir_path))) {
  buildIndex(*root);
}

DirectoryTree::DirectoryTree(fs::path dir_path, std::unique_ptr<Node> node)
    : root_path(dir_path), root(std::move(node)) {
  buildIndex(*root);
}

void DirectoryTree::buildIndex(Node& node) {
  // Stores path relative to the DirectoryTree.root_path
  node.path = fs::relative(node.path, root_path);

  index[node.path] = &node;
  if (node.type == NodeType::Directory) {
    for (auto const& child : children(node)) {
      buildIndex(*child);
    }
  }
}

// ---------- Node Snapshot ----------

NodeSnapshot::NodeSnapshot(const Node& node)
    : path(node.path), type(node.type), mtime(node.mtime) {
  if (node.type == NodeType::File) {
    const FileMeta& meta = std::get<FileMeta>(node.data);
    size = meta.size;
    if (meta.file_hash.has_value()) {
      file_hash = meta.file_hash;
    }
  }
}

// ---------- Node Diff ----------

NodeDiff NodeDiff::added(const Node& new_node) {
  return {ChangeType::Added, std::nullopt, NodeSnapshot(new_node)};
}

NodeDiff NodeDiff::deleted(const Node& old_node) {
  return {ChangeType::Deleted, NodeSnapshot(old_node), std::nullopt};
}

NodeDiff NodeDiff::modified(const Node& old_node, const Node& new_node) {
  return {ChangeType::Modified, NodeSnapshot(old_node), NodeSnapshot(new_node)};
}

// ---------- Diff ----------

std::vector<NodeDiff> diffTree(DirectoryTree& old_tree,
                               DirectoryTree& new_tree) {
  std::vector<NodeDiff> nodeDiffVec;

  // Function to recursively loop through each node of DirectoryTree
  std::function<void(const Node*, const Node*)> diffLoop =
      [&](const Node* old_node, const Node* new_node) {
        const std::vector<std::unique_ptr<Node>>& old_vec = children(*old_node);
        const std::vector<std::unique_ptr<Node>>& new_vec = children(*new_node);
        auto old_it = old_vec.begin();
        auto new_it = new_vec.begin();

        // Two pointer approach assuming sorted children vector
        for (; old_it != old_vec.end() && new_it != new_vec.end();) {
          if ((*old_it)->path == (*new_it)->path) {
            if ((*old_it)->type !=
                (*new_it)->type) {  // Handles File <-> Directory
              nodeDiffVec.push_back(NodeDiff::modified(**old_it, **new_it));
            } else if ((*old_it)->type == NodeType::File) {
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

// ---------- Helpers ----------

// Prints each children of a node recursively through DFS
void printTree(Node& node, std::string prefix) {
  std::cout << prefix << "|--" << node.name << "\n";
  if (node.type == NodeType::Directory) {
    auto& childrens = children(node);
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

void serializeNode(std::ostream& os, const Node& node) {
  wire::write_u8(os, static_cast<uint8_t>(node.type));
  wire::write_u64(os, node.mtime.time_since_epoch().count());
  wire::write_string(os, node.name);
  wire::write_string(os, node.path.string());

  if (node.type == NodeType::File) {
    const auto& meta = std::get<FileMeta>(node.data);

    wire::write_u64(os, meta.size);

    wire::write_u8(os, meta.file_hash.has_value());
    if (meta.file_hash) {
      os.write(reinterpret_cast<const char*>(meta.file_hash->data()),
               meta.file_hash->size());
    }
  } else {
    const auto& kids = children(node);
    wire::write_u32(os, static_cast<uint32_t>(kids.size()));

    for (const auto& kid : kids) {
      serializeNode(os, *kid);
    }
  }
}

std::unique_ptr<Node> deserializeNode(std::istream& is) {
  NodeType type = static_cast<NodeType>(wire::read_u8(is));
  auto mtime =
      fs::file_time_type(fs::file_time_type::duration(wire::read_u64(is)));

  std::string name = wire::read_string(is);
  fs::path full_path = fs::path(wire::read_string(is));

  if (type == NodeType::File) {
    FileMeta meta;
    meta.size = wire::read_u64(is);

    bool has_hash = wire::read_u8(is);
    if (has_hash) {
      meta.file_hash.emplace();  // construct Hash inside optional
      is.read(reinterpret_cast<char*>(meta.file_hash->data()),
              meta.file_hash->size());
    }
    auto node = std::unique_ptr<Node>(
        new Node{full_path, name, type, mtime, Node::Data{meta}});
    return node;
  } else {
    uint32_t count = wire::read_u32(is);
    std::vector<std::unique_ptr<Node>> kids;

    for (uint32_t i = 0; i < count; i++) {
      kids.push_back(deserializeNode(is));
    }

    auto node = std::unique_ptr<Node>(
        new Node{full_path, name, type, mtime, Node::Data{std::move(kids)}});
    return node;
  }
}
}  // namespace fstree
