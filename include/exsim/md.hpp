// Reader for the binary market-data capture produced by scripts/mdconv.py.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace exsim::md {

enum class Kind : std::uint8_t { Diff = 1, Trade = 2, Snapshot = 3 };

struct LevelUpdate {
  std::int64_t px;   // ticks
  std::uint64_t qty; // lots; 0 deletes the level
};

// One capture record. `levels` points into the loaded file buffer (bids first, then asks).
struct Record {
  Kind kind;
  std::uint64_t rx_ns;  // local receive time: the single clock used for everything
  // Diff: [first_id, last_id]; Snapshot: last_id = lastUpdateId
  std::uint64_t first_id = 0, last_id = 0;
  std::uint32_t n_bids = 0, n_asks = 0;
  const LevelUpdate* levels = nullptr;  // may be unaligned in the buffer: use level(i)
  // Trade
  std::int64_t trade_px = 0;
  std::uint64_t trade_qty = 0;
  bool buyer_is_maker = false;  // true: an aggressive SELL hit a resting bid

  LevelUpdate level(std::size_t i) const {
    LevelUpdate l;
    std::memcpy(&l, reinterpret_cast<const char*>(levels) + i * sizeof(LevelUpdate), sizeof l);
    return l;
  }
  LevelUpdate bid(std::size_t i) const { return level(i); }
  LevelUpdate ask(std::size_t i) const { return level(n_bids + i); }
};

class Capture {
 public:
  explicit Capture(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    buf_.resize(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf_.data()), static_cast<std::streamsize>(buf_.size()));
    if (buf_.size() < 16 || std::memcmp(buf_.data(), "EXMD", 4) != 0) throw std::runtime_error("not an EXMD file");
    std::uint32_t version;
    std::memcpy(&version, buf_.data() + 4, 4);
    if (version != 1) throw std::runtime_error("unsupported EXMD version");
    std::memcpy(&price_dec_, buf_.data() + 8, 4);
    std::memcpy(&qty_dec_, buf_.data() + 12, 4);

    std::size_t off = 16;
    while (off + 4 <= buf_.size()) {
      std::uint32_t len;
      std::memcpy(&len, buf_.data() + off, 4);
      if (off + 4 + len > buf_.size() || len < 12) throw std::runtime_error("truncated or corrupt record");
      records_.push_back(parse(buf_.data() + off + 4, len));
      off += 4 + len;
    }
  }

  const std::vector<Record>& records() const { return records_; }
  std::uint32_t price_decimals() const { return price_dec_; }
  std::uint32_t qty_decimals() const { return qty_dec_; }

 private:
  static Record parse(const std::byte* p, std::uint32_t len) {
    Record r{};
    r.kind = static_cast<Kind>(p[0]);
    std::memcpy(&r.rx_ns, p + 4, 8);
    const std::byte* q = p + 12;
    switch (r.kind) {
      case Kind::Diff:
        std::memcpy(&r.first_id, q, 8), std::memcpy(&r.last_id, q + 8, 8);
        std::memcpy(&r.n_bids, q + 16, 4), std::memcpy(&r.n_asks, q + 20, 4);
        r.levels = reinterpret_cast<const LevelUpdate*>(q + 24);
        check(len, 12 + 24 + (std::size_t{r.n_bids} + r.n_asks) * sizeof(LevelUpdate));
        break;
      case Kind::Snapshot:
        std::memcpy(&r.last_id, q, 8);
        std::memcpy(&r.n_bids, q + 8, 4), std::memcpy(&r.n_asks, q + 12, 4);
        r.levels = reinterpret_cast<const LevelUpdate*>(q + 16);
        check(len, 12 + 16 + (std::size_t{r.n_bids} + r.n_asks) * sizeof(LevelUpdate));
        break;
      case Kind::Trade: {
        std::memcpy(&r.trade_px, q, 8), std::memcpy(&r.trade_qty, q + 8, 8);
        r.buyer_is_maker = static_cast<std::uint8_t>(q[16]) != 0;
        check(len, 12 + 24);
        break;
      }
      default:
        throw std::runtime_error("unknown record kind");
    }
    return r;
  }
  static void check(std::uint32_t len, std::size_t want) {
    if (len != want) throw std::runtime_error("record length mismatch");
  }

  std::vector<std::byte> buf_;
  std::vector<Record> records_;
  std::uint32_t price_dec_ = 0, qty_dec_ = 0;
};

// Where to start a replay: a snapshot plus the first diff that straddles it.
struct Seed {
  std::size_t snap_rec;  // index into Capture::records()
  std::size_t diff_rec;  // first diff to apply
};

// Binance's local-book procedure: the first applied diff must satisfy U <= lastUpdateId+1 <= u.
// Picks the first snapshot (file order) that some diff at or after position `from_diff_pos` (an index
// into `diffs`) straddles. Returns false if none does.
inline bool find_seed(const std::vector<Record>& recs, const std::vector<std::size_t>& diffs,
                      const std::vector<std::size_t>& snaps, std::size_t from_diff_pos, Seed& out) {
  for (std::size_t s : snaps) {
    const std::uint64_t want = recs[s].last_id + 1;
    for (std::size_t d = from_diff_pos; d < diffs.size(); ++d) {
      const Record& df = recs[diffs[d]];
      if (df.last_id < want) continue;  // diff entirely before the snapshot
      if (df.first_id > want) break;    // gap: this snapshot is too old for the stream
      out = {s, diffs[d]};
      return true;
    }
  }
  return false;
}

}  // namespace exsim::md
