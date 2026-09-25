// Data structures and infrastructure, each checked against a trusted model.

#include <atomic>
#include <cmath>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "exsim/latency_histogram.hpp"
#include "exsim/order_index.hpp"
#include "exsim/price_bitmap.hpp"
#include "exsim/protocol.hpp"
#include "exsim/spsc_queue.hpp"
#include "exsim/wire_file.hpp"
#include "exsim/workload.hpp"
#include "test_framework.hpp"

using namespace exsim;

TEST(bitmap_matches_std_set) {
  for (std::uint32_t n : {64u, 4096u, 1u << 16}) {
    PriceBitmap bm(n);
    std::set<std::int32_t> model;
    Rng rng(n);
    const auto sn = static_cast<std::int32_t>(n);
    for (int step = 0; step < 50'000; ++step) {
      const auto i = static_cast<std::int32_t>(rng.below(n));
      if (rng.chance(0.5)) {
        bm.set(i);
        model.insert(i);
      } else {
        bm.clear(i);
        model.erase(i);
      }
      const auto q = static_cast<std::int32_t>(rng.below(n + 2)) - 1;  // include out-of-range probes
      auto it = model.lower_bound(q);
      CHECK_EQ(bm.find_next(q), it == model.end() ? sn : *it);
      auto jt = model.upper_bound(q);
      CHECK_EQ(bm.find_prev(q), jt == model.begin() ? -1 : *std::prev(jt));
    }
  }
}

TEST(bitmap_rejects_bad_sizes) {
  bool threw = false;
  try {
    PriceBitmap bm(100);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
}

template <class Hash>
void index_matches_unordered_map() {
  // Small table + clustered keys: long probe chains and wraparound, the backward-shift worst case.
  // The index stores only fingerprints, so a side table plays the order store: owner[slot] = key.
  OrderIndex<Hash> idx(64);
  std::unordered_map<OrderId, std::uint32_t> model;
  std::vector<OrderId> owner(1'000'000, 0);
  auto key_of = [&](std::uint32_t slot) { return owner[slot]; };
  Rng rng(7);
  std::uint32_t next_slot = 0;
  for (int step = 0; step < 400'000; ++step) {
    const OrderId key = 1 + rng.below(200);
    if (rng.chance(0.5) && model.size() < 64 && model.count(key) == 0) {
      const std::uint32_t v = next_slot++ % 1'000'000;
      owner[v] = key;
      idx.insert(key, v);
      model[key] = v;
    } else if (rng.chance(0.5)) {
      const auto it = model.find(key);
      const std::uint32_t got = idx.erase(key, key_of);
      CHECK_EQ(got, it == model.end() ? kNil : it->second);
      if (it != model.end()) model.erase(it);
    } else if (!model.empty()) {  // erase by known slot, as the matcher does for filled makers
      const auto it = model.begin();
      idx.erase_known(it->first, it->second);
      model.erase(it);
    }
    CHECK_EQ(idx.size(), model.size());
    const OrderId probe = 1 + rng.below(200);
    const auto it = model.find(probe);
    CHECK_EQ(idx.find(probe, key_of), it == model.end() ? kNil : it->second);
  }
}
TEST(order_index_scatter_matches_unordered_map) { index_matches_unordered_map<ScatterHash>(); }
TEST(order_index_locality_matches_unordered_map) { index_matches_unordered_map<LocalityHash>(); }

TEST(order_index_locality_survives_strided_keys) {
  // Keys strided by the table size all share one home slot under LocalityHash: the documented
  // worst case. It must stay correct (just slower).
  OrderIndex<LocalityHash> idx(512);
  const std::uint64_t stride = idx.slot_count();
  auto key_of = [&](std::uint32_t slot) { return OrderId{1 + slot * stride}; };
  for (std::uint32_t i = 0; i < 512; ++i) idx.insert(1 + i * stride, i);
  for (std::uint32_t i = 0; i < 512; ++i) CHECK_EQ(idx.find(1 + i * stride, key_of), i);
  for (std::uint32_t i = 0; i < 512; i += 2) CHECK_EQ(idx.erase(1 + i * stride, key_of), i);
  for (std::uint32_t i = 1; i < 512; i += 2) CHECK_EQ(idx.find(1 + i * stride, key_of), i);
  CHECK_EQ(idx.find(3, key_of), kNil);
  CHECK_EQ(idx.size(), 256u);
}

TEST(spsc_single_thread_semantics) {
  SpscQueue<int> q(4);
  CHECK_EQ(q.capacity(), 4u);
  CHECK(q.front() == nullptr);
  for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
  CHECK(!q.try_push(99));  // full
  int v = -1;
  CHECK(q.try_pop(v));
  CHECK_EQ(v, 0);
  CHECK(q.try_push(4));  // wraps
  for (int want = 1; want <= 4; ++want) {
    REQUIRE(q.try_pop(v));
    CHECK_EQ(v, want);
  }
  CHECK(!q.try_pop(v));
}

TEST(spsc_two_threads_preserve_order) {
  constexpr std::uint64_t kItems = 5'000'000;
  SpscQueue<std::uint64_t> q(1024);
  std::atomic<bool> ok{true};
  std::thread consumer([&] {
    std::uint64_t expect = 0;
    while (expect < kItems) {
      if (std::uint64_t* p = q.front()) {
        if (*p != expect) ok = false;
        q.pop();
        ++expect;
      }
    }
  });
  for (std::uint64_t i = 0; i < kItems; ++i)
    while (!q.try_push(i)) {
    }
  consumer.join();
  CHECK(ok.load());
  CHECK(q.front() == nullptr);
}

TEST(histogram_percentiles_within_bucket_error) {
  LatencyHistogram h;
  std::vector<std::uint64_t> values;
  Rng rng(3);
  for (int i = 0; i < 200'000; ++i) {
    const auto v = static_cast<std::uint64_t>(std::exp(rng.uniform() * 14.0));  // 1 .. ~1.2M, log-uniform
    values.push_back(v);
    h.record(v);
  }
  std::sort(values.begin(), values.end());
  for (double p : {50.0, 90.0, 99.0, 99.9, 99.99}) {
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size())));
    const std::uint64_t exact = values[rank - 1];
    const std::uint64_t got = h.percentile(p);
    CHECK(got >= exact);                                           // reports bucket upper bound
    CHECK(static_cast<double>(got) <= static_cast<double>(exact) * (1.0 + 1.0 / 32) + 1);  // within one bucket
  }
  CHECK_EQ(h.max(), values.back());
  CHECK_EQ(h.min(), values.front());
  CHECK_EQ(h.count(), values.size());
}

TEST(histogram_bucket_boundaries_are_contiguous) {
  for (std::uint64_t v = 0; v < 100'000; ++v) {
    const auto i = LatencyHistogram::index(v);
    CHECK(v <= LatencyHistogram::upper_bound(i));
    if (i > 0) CHECK(v > LatencyHistogram::upper_bound(i - 1));
  }
  CHECK(LatencyHistogram::index(~0ull) < LatencyHistogram::kBuckets);
}

TEST(protocol_roundtrip_every_field) {
  WorkloadConfig wc;
  wc.messages = 100'000;
  const auto cmds = generate_workload(wc);
  DecodeStats st;
  const auto back = decode_stream(encode_stream(cmds), &st);
  CHECK_EQ(st.ok, cmds.size());
  CHECK_EQ(st.malformed, 0u);
  CHECK(!st.truncated);
  REQUIRE(back.size() == cmds.size());
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    // Cancel carries only id + symbol on the wire; compare the fields each type actually transmits.
    const Command &a = cmds[i], &b = back[i];
    bool same = a.type == b.type && a.order_id == b.order_id && a.symbol == b.symbol && a.seq == b.seq;
    if (a.type != MsgType::Cancel) same = same && a.price == b.price && a.qty == b.qty;
    if (a.type == MsgType::NewOrder)
      same = same && a.side == b.side && a.ord_type == b.ord_type && a.tif == b.tif && a.flags == b.flags &&
             a.owner == b.owner;
    if (!same) {
      CHECK(same);
      break;
    }
  }
}

TEST(protocol_rejects_malformed_input) {
  Command c{};
  c.type = MsgType::NewOrder;
  c.order_id = 5, c.qty = 1, c.price = 100;
  std::byte buf[wire::kMaxMessageSize];
  const std::size_t n = wire::encode(c, buf);
  Command out;

  CHECK(wire::decode(buf, n - 1, out).status == wire::DecodeStatus::Incomplete);
  CHECK(wire::decode(buf, 3, out).status == wire::DecodeStatus::Incomplete);

  auto corrupt = [&](std::size_t offset, std::uint8_t value) {
    std::byte copy[wire::kMaxMessageSize];
    std::memcpy(copy, buf, n);
    copy[offset] = std::byte{value};
    return wire::decode(copy, n, out);
  };
  auto r = corrupt(2, 99);  // unknown type: skippable
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == n);
  r = corrupt(3, 7);  // bad version
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == n);
  r = corrupt(offsetof(wire::NewOrder, side), 2);  // side out of range
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == n);
  r = corrupt(offsetof(wire::NewOrder, tif), 3);
  CHECK(r.status == wire::DecodeStatus::Malformed);
  r = corrupt(0, 4);  // length smaller than a header: framing is lost
  CHECK(r.status == wire::DecodeStatus::Malformed && r.consumed == 0);

  // A malformed message in the middle of a stream is skipped; its neighbours still decode.
  std::byte bad[wire::kMaxMessageSize];
  std::memcpy(bad, buf, n);
  bad[2] = std::byte{42};
  std::vector<std::byte> stream(buf, buf + n);
  stream.insert(stream.end(), bad, bad + n);
  stream.insert(stream.end(), buf, buf + n);
  DecodeStats st;
  CHECK_EQ(decode_stream(stream, &st).size(), 2u);
  CHECK_EQ(st.malformed, 1u);
  CHECK(!st.truncated);
}
