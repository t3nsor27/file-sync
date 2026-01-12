#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

namespace fstree::wire {

void write_u8(std::ostream&, uint8_t);
void write_u32(std::ostream&, uint32_t);
void write_u64(std::ostream&, uint64_t);

uint8_t read_u8(std::istream&);
uint32_t read_u32(std::istream&);
uint64_t read_u64(std::istream&);

void write_string(std::ostream&, const std::string&);
std::string read_string(std::istream&);
}  // namespace fstree::wire
