#include "../include/fstree/wire.hpp"

namespace fstree::wire {

void write_u8(std::ostream& os, uint8_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void write_u32(std::ostream& os, uint32_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void write_u64(std::ostream& os, uint64_t v) {
  os.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

uint8_t read_u8(std::istream& is) {
  uint8_t v;
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return v;
}

uint32_t read_u32(std::istream& is) {
  uint32_t v;
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return v;
}

uint64_t read_u64(std::istream& is) {
  uint64_t v;
  is.read(reinterpret_cast<char*>(&v), sizeof(v));
  return v;
}

void write_string(std::ostream& os, const std::string& str) {
  write_u32(os, static_cast<uint32_t>(str.size()));
  os.write(str.data(), str.size());
}

std::string read_string(std::istream& is) {
  uint32_t len = read_u32(is);
  std::string str(len, '\0');
  is.read(str.data(), len);
  return str;
}
}  // namespace fstree::wire
