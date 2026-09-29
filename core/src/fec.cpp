#include "hyperlink/fec.h"

#include <cstring>

namespace hl::fec {
namespace {

struct Tables {
    uint8_t exp[512];
    uint8_t log[256];
    uint8_t mul[256][256];

    Tables() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = (uint8_t)x;
            log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
        for (int a = 0; a < 256; a++)
            for (int b = 0; b < 256; b++)
                mul[a][b] = (a && b) ? exp[log[a] + log[b]] : 0;
    }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};

const Tables& T() {
    static const Tables t;
    return t;
}

// Parity row j, data column i. x_j = k + j and y_i = i are all distinct, so x ^ y != 0.
inline uint8_t coef(int k, int j, int i) { return T().inv((uint8_t)((k + j) ^ i)); }

// dst ^= c * src
inline void mulAdd(uint8_t* dst, const uint8_t* src, uint8_t c, size_t n) {
    if (c == 0) return;
    if (c == 1) {
        for (size_t b = 0; b < n; b++) dst[b] ^= src[b];
        return;
    }
    const uint8_t* row = T().mul[c];
    for (size_t b = 0; b < n; b++) dst[b] ^= row[src[b]];
}

// In-place Gauss-Jordan inversion of an n x n matrix. Returns false if singular.
bool invert(std::vector<uint8_t>& a, int n) {
    std::vector<uint8_t> inv(n * n, 0);
    for (int i = 0; i < n; i++) inv[i * n + i] = 1;
    const Tables& t = T();
    for (int col = 0; col < n; col++) {
        int pivot = -1;
        for (int r = col; r < n; r++)
            if (a[r * n + col]) { pivot = r; break; }
        if (pivot < 0) return false;
        if (pivot != col)
            for (int c = 0; c < n; c++) {
                std::swap(a[pivot * n + c], a[col * n + c]);
                std::swap(inv[pivot * n + c], inv[col * n + c]);
            }
        uint8_t s = t.inv(a[col * n + col]);
        for (int c = 0; c < n; c++) {
            a[col * n + c] = t.mul[s][a[col * n + c]];
            inv[col * n + c] = t.mul[s][inv[col * n + c]];
        }
        for (int r = 0; r < n; r++) {
            if (r == col) continue;
            uint8_t f = a[r * n + col];
            if (!f) continue;
            for (int c = 0; c < n; c++) {
                a[r * n + c] ^= t.mul[f][a[col * n + c]];
                inv[r * n + c] ^= t.mul[f][inv[col * n + c]];
            }
        }
    }
    a.swap(inv);
    return true;
}

}  // namespace

void encode(const uint8_t* const* data, int k, uint8_t* const* parity, int m, size_t shardSize) {
    for (int j = 0; j < m; j++) {
        std::memset(parity[j], 0, shardSize);
        for (int i = 0; i < k; i++) mulAdd(parity[j], data[i], coef(k, j, i), shardSize);
    }
}

bool decode(uint8_t* const* shards, const uint8_t* present, int k, int m, size_t shardSize) {
    std::vector<int> missing;
    for (int i = 0; i < k; i++)
        if (!present[i]) missing.push_back(i);
    if (missing.empty()) return true;

    // Pick k present shards: every present data shard, then parity shards to fill up.
    std::vector<int> rows;
    for (int i = 0; i < k; i++)
        if (present[i]) rows.push_back(i);
    for (int j = 0; j < m && (int)rows.size() < k; j++)
        if (present[k + j]) rows.push_back(k + j);
    if ((int)rows.size() < k) return false;

    std::vector<uint8_t> a(k * k, 0);
    for (int r = 0; r < k; r++) {
        int s = rows[r];
        if (s < k) a[r * k + s] = 1;
        else
            for (int c = 0; c < k; c++) a[r * k + c] = coef(k, s - k, c);
    }
    if (!invert(a, k)) return false;

    for (int i : missing) {
        std::memset(shards[i], 0, shardSize);
        for (int r = 0; r < k; r++) mulAdd(shards[i], shards[rows[r]], a[i * k + r], shardSize);
    }
    return true;
}

}  // namespace hl::fec
