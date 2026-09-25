// Reading and writing binary message captures (a flat concatenation of wire messages).
#pragma once

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "exsim/protocol.hpp"

namespace exsim {

inline std::vector<std::byte> encode_stream(const std::vector<Command>& cmds) {
  std::vector<std::byte> buf;
  buf.reserve(cmds.size() * wire::kMaxMessageSize);
  std::byte tmp[wire::kMaxMessageSize];
  for (const Command& c : cmds) {
    const std::size_t n = wire::encode(c, tmp);
    buf.insert(buf.end(), tmp, tmp + n);
  }
  return buf;
}

struct DecodeStats {
  std::uint64_t ok = 0, malformed = 0;
  bool truncated = false;
};

// Decodes an entire capture. Malformed messages are counted and skipped; lost framing stops decoding.
inline std::vector<Command> decode_stream(const std::vector<std::byte>& buf, DecodeStats* stats = nullptr) {
  std::vector<Command> out;
  DecodeStats st;
  std::size_t off = 0;
  while (off < buf.size()) {
    Command c;
    const auto r = wire::decode(buf.data() + off, buf.size() - off, c);
    if (r.status == wire::DecodeStatus::Ok) {
      out.push_back(c);
      ++st.ok;
    } else if (r.status == wire::DecodeStatus::Malformed && r.consumed != 0) {
      ++st.malformed;
    } else {
      st.truncated = true;
      break;
    }
    off += r.consumed;
  }
  if (stats != nullptr) *stats = st;
  return out;
}

inline void write_file(const std::string& path, const std::vector<std::byte>& buf) {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path + " for writing");
  f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  if (!f) throw std::runtime_error("write failed: " + path);
}

inline std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("cannot open " + path);
  const auto size = static_cast<std::size_t>(f.tellg());
  std::vector<std::byte> buf(size);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
  if (!f) throw std::runtime_error("read failed: " + path);
  return buf;
}

}  // namespace exsim
