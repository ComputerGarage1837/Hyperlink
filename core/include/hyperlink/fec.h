// Systematic Reed-Solomon erasure coding over GF(2^8) with a Cauchy matrix.
//
// A block of k data shards gets m parity shards (k + m <= 255). Any k of the k + m
// shards are enough to rebuild the data, so up to m lost packets per block are
// repaired on the receiver without a retransmission.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hl::fec {

// Computes the m parity shards for k data shards. Every shard is shardSize bytes.
void encode(const uint8_t* const* data, int k, uint8_t* const* parity, int m, size_t shardSize);

// shards has k + m entries (data first, then parity); present[i] says which arrived.
// Missing data shards are rebuilt in place, so shards[0..k) must point at writable
// buffers of shardSize bytes even when absent. Returns false if fewer than k are present.
bool decode(uint8_t* const* shards, const uint8_t* present, int k, int m, size_t shardSize);

}  // namespace hl::fec
