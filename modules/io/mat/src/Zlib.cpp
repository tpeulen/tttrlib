// SPDX-License-Identifier: BSD-3-Clause
//
// zlib streams (RFC 1950) around DEFLATE (RFC 1951), both directions, so that
// MAT-files -- every variable MATLAB writes since R14 is an miCOMPRESSED zlib
// stream -- need no zlib library. The inflater decodes all three block types;
// the deflater emits one fixed-Huffman block after a hash-chain LZ77 search,
// which every inflater (MATLAB, Octave, zlib, this one) reads.

#include "MatFile.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace tttrlib {
namespace io {

namespace {

std::uint32_t adler32(const unsigned char* p, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    while (n > 0) {
        // 5552 is the largest block for which b cannot overflow 32 bits.
        std::size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

const unsigned short kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                        15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
const unsigned char kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const unsigned short kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                      33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                      1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const unsigned char kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// -------------------------------------------------------------------- inflate

struct BitReader {
    const unsigned char* p;
    std::size_t n;
    std::size_t pos = 0;   // byte position
    std::uint32_t buf = 0;
    int cnt = 0;

    int bits(int need) {
        while (cnt < need) {
            if (pos >= n) throw std::runtime_error("inflate: compressed data ends early");
            buf |= static_cast<std::uint32_t>(p[pos++]) << cnt;
            cnt += 8;
        }
        const int v = static_cast<int>(buf & ((1u << need) - 1u));
        buf >>= need;
        cnt -= need;
        return v;
    }
    void align() {
        buf = 0;
        cnt = 0;
    }
};

// Canonical Huffman decoding table, decoded bit by bit (RFC 1951 3.2.2): count
// of codes per length and the symbols in code order.
struct Huffman {
    std::array<short, 16> count{};
    std::vector<short> symbol;

    void build(const unsigned char* lengths, int n) {
        count.fill(0);
        symbol.assign(n, 0);
        for (int s = 0; s < n; ++s) count[lengths[s]]++;
        count[0] = 0;
        std::array<short, 16> offs{};
        for (int len = 1; len < 16; ++len) offs[len] = static_cast<short>(offs[len - 1] + count[len - 1]);
        for (int s = 0; s < n; ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = static_cast<short>(s);
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.bits(1);
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: invalid Huffman code");
    }
};

void inflate_codes(BitReader& br, std::vector<unsigned char>& out, const Huffman& lit,
                   const Huffman& dist) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 256) {
            out.push_back(static_cast<unsigned char>(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("inflate: invalid length symbol");
            const int len = kLengthBase[sym] + br.bits(kLengthExtra[sym]);
            const int ds = dist.decode(br);
            if (ds >= 30) throw std::runtime_error("inflate: invalid distance symbol");
            const std::size_t d = kDistBase[ds] + static_cast<std::size_t>(br.bits(kDistExtra[ds]));
            if (d > out.size()) throw std::runtime_error("inflate: distance before start of data");
            const std::size_t from = out.size() - d;
            for (int k = 0; k < len; ++k) out.push_back(out[from + k]);
        }
    }
}

} // namespace

std::vector<unsigned char> zlib_inflate(const unsigned char* in_data, std::size_t n_data) {
    if (n_data < 6) throw std::runtime_error("inflate: stream too short for a zlib header");
    const int cmf = in_data[0], flg = in_data[1];
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0)
        throw std::runtime_error("inflate: not a zlib stream");
    if (flg & 0x20) throw std::runtime_error("inflate: preset dictionaries are not supported");

    BitReader br{in_data + 2, n_data - 2};
    std::vector<unsigned char> out;
    out.reserve(n_data * 4);

    static Huffman fixed_lit, fixed_dist;
    static const bool fixed_ready = [] {
        unsigned char l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        fixed_lit.build(l, 288);
        unsigned char d[30];
        std::memset(d, 5, sizeof d);
        fixed_dist.build(d, 30);
        return true;
    }();
    (void)fixed_ready;

    int last;
    do {
        last = br.bits(1);
        const int type = br.bits(2);
        if (type == 0) {
            br.align();
            if (br.pos + 4 > br.n) throw std::runtime_error("inflate: stored block header truncated");
            const unsigned len = br.p[br.pos] | (br.p[br.pos + 1] << 8);
            const unsigned nlen = br.p[br.pos + 2] | (br.p[br.pos + 3] << 8);
            br.pos += 4;
            if ((len ^ 0xffffu) != nlen) throw std::runtime_error("inflate: stored block length check");
            if (br.pos + len > br.n) throw std::runtime_error("inflate: stored block truncated");
            out.insert(out.end(), br.p + br.pos, br.p + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            inflate_codes(br, out, fixed_lit, fixed_dist);
        } else if (type == 2) {
            const int nlen = br.bits(5) + 257, ndist = br.bits(5) + 1, ncode = br.bits(4) + 4;
            static const unsigned char order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
            unsigned char lengths[320] = {0};
            for (int k = 0; k < ncode; ++k) lengths[order[k]] = static_cast<unsigned char>(br.bits(3));
            Huffman lencode;
            lencode.build(lengths, 19);
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = lencode.decode(br);
                if (sym < 16) {
                    lengths[idx++] = static_cast<unsigned char>(sym);
                } else {
                    int rep, val = 0;
                    if (sym == 16) {
                        if (idx == 0) throw std::runtime_error("inflate: repeat with no previous length");
                        val = lengths[idx - 1];
                        rep = 3 + br.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (idx + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                    while (rep--) lengths[idx++] = static_cast<unsigned char>(val);
                }
            }
            if (lengths[256] == 0) throw std::runtime_error("inflate: no end-of-block code");
            Huffman lit, dist;
            lit.build(lengths, nlen);
            dist.build(lengths + nlen, ndist);
            inflate_codes(br, out, lit, dist);
        } else {
            throw std::runtime_error("inflate: invalid block type");
        }
    } while (!last);

    // Adler-32 of the uncompressed data, big-endian, on the next byte boundary.
    const std::size_t at = 2 + br.pos;
    if (at + 4 <= n_data) {
        const std::uint32_t want = (static_cast<std::uint32_t>(in_data[at]) << 24) |
                                   (static_cast<std::uint32_t>(in_data[at + 1]) << 16) |
                                   (static_cast<std::uint32_t>(in_data[at + 2]) << 8) |
                                   static_cast<std::uint32_t>(in_data[at + 3]);
        if (want != adler32(out.data(), out.size()))
            throw std::runtime_error("inflate: Adler-32 checksum mismatch (corrupt data)");
    }
    return out;
}

// -------------------------------------------------------------------- deflate

namespace {

struct BitWriter {
    std::vector<unsigned char>& out;
    std::uint32_t buf = 0;
    int cnt = 0;
    void put(std::uint32_t v, int n) {  // LSB-first
        buf |= v << cnt;
        cnt += n;
        while (cnt >= 8) {
            out.push_back(static_cast<unsigned char>(buf));
            buf >>= 8;
            cnt -= 8;
        }
    }
    void put_rev(std::uint32_t code, int n) {  // Huffman codes go MSB-first
        std::uint32_t r = 0;
        for (int k = 0; k < n; ++k) r |= ((code >> k) & 1u) << (n - 1 - k);
        put(r, n);
    }
    void flush() {
        if (cnt > 0) out.push_back(static_cast<unsigned char>(buf));
        buf = 0;
        cnt = 0;
    }
};

void put_literal(BitWriter& bw, int s) {
    if (s < 144) bw.put_rev(0x30 + s, 8);
    else if (s < 256) bw.put_rev(0x190 + (s - 144), 9);
    else if (s < 280) bw.put_rev(s - 256, 7);
    else bw.put_rev(0xc0 + (s - 280), 8);
}

void put_match(BitWriter& bw, int len, int dist) {
    int ls = 28;
    while (kLengthBase[ls] > len) --ls;
    put_literal(bw, 257 + ls);
    if (kLengthExtra[ls]) bw.put(len - kLengthBase[ls], kLengthExtra[ls]);
    int ds = 29;
    while (kDistBase[ds] > dist) --ds;
    bw.put_rev(ds, 5);
    if (kDistExtra[ds]) bw.put(dist - kDistBase[ds], kDistExtra[ds]);
}

} // namespace

std::vector<unsigned char> zlib_deflate(const unsigned char* in, std::size_t n) {
    std::vector<unsigned char> out;
    out.reserve(n / 2 + 64);
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter bw{out};
    bw.put(1, 1);  // final block
    bw.put(1, 2);  // fixed Huffman

    constexpr int kWindow = 32768, kHashBits = 15, kMaxChain = 64, kMaxLen = 258;
    std::vector<int> head(1 << kHashBits, -1), prev(n > 0 ? n : 1, -1);
    auto hash = [&](std::size_t i) {
        return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << kHashBits) - 1);
    };
    auto insert = [&](std::size_t i) {
        if (i + 2 < n) {
            const int h = hash(i);
            prev[i] = head[h];
            head[h] = static_cast<int>(i);
        }
    };

    std::size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < n) {
            int cand = head[hash(i)];
            int chain = kMaxChain;
            const int max_len = static_cast<int>(n - i < static_cast<std::size_t>(kMaxLen) ? n - i : kMaxLen);
            while (cand >= 0 && chain-- > 0 && static_cast<long>(i) - cand <= kWindow) {
                int l = 0;
                while (l < max_len && in[cand + l] == in[i + l]) ++l;
                if (l > best_len) {
                    best_len = l;
                    best_dist = static_cast<int>(i - cand);
                    if (l == max_len) break;
                }
                cand = prev[cand];
            }
        }
        if (best_len >= 3) {
            put_match(bw, best_len, best_dist);
            for (int k = 0; k < best_len; ++k) insert(i + k);
            i += best_len;
        } else {
            put_literal(bw, in[i]);
            insert(i);
            ++i;
        }
    }
    put_literal(bw, 256);
    bw.flush();
    const std::uint32_t a = adler32(in, n);
    out.push_back(static_cast<unsigned char>(a >> 24));
    out.push_back(static_cast<unsigned char>(a >> 16));
    out.push_back(static_cast<unsigned char>(a >> 8));
    out.push_back(static_cast<unsigned char>(a));
    return out;
}

} // namespace io
} // namespace tttrlib
