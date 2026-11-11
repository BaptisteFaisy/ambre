/*
 * ambre_sha256.cpp — SHA-256 (FIPS 180-4) implementation.
 *
 * Textbook implementation (64 rounds, 8 state words), standard library only.
 * Checked against the published vectors by tests/test_archive_cpp.cpp.
 */

#include "ambre_sha256.hpp"

#include <cstring>

namespace ambre {
namespace sha256 {

namespace {

const uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

uint32_t rotr(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

uint32_t load_be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(block + 4u * static_cast<size_t>(i));
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                            (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                            (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t v[8];
    std::memcpy(v, state, sizeof(v));
    for (int i = 0; i < 64; i++) {
        const uint32_t big_s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const uint32_t t1 = v[7] + big_s1 + ch + kK[i] + w[i];
        const uint32_t big_s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const uint32_t t2 = big_s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) {
        state[i] += v[i];
    }
}

}  // namespace

Context::Context()
{
    state_[0] = 0x6a09e667u; state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u; state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu; state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu; state_[7] = 0x5be0cd19u;
    pending_ = 0u;
    total_ = 0u;
}

void Context::feed(const uint8_t* data, size_t len)
{
    while (len > 0u) {
        const size_t room = 64u - pending_;
        const size_t take = (len < room) ? len : room;
        std::memcpy(buffer_ + pending_, data, take);
        pending_ += static_cast<uint32_t>(take);
        data += take;
        len -= take;
        if (pending_ == 64u) {
            compress(state_, buffer_);
            pending_ = 0u;
        }
    }
}

void Context::update(const void* data, size_t len)
{
    total_ += static_cast<uint64_t>(len);
    feed(static_cast<const uint8_t*>(data), len);
}

std::string Context::hex_digest()
{
    const uint64_t bits = total_ * 8u;
    /* Padding: 0x80, zeroes, then the 64-bit bit length, so that the last
     * block is exactly 64 bytes (FIPS 180-4 section 5.1.1). */
    uint8_t pad[128];
    std::memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;
    const size_t pad_len = (pending_ < 56u) ? (56u - pending_)
                                            : (120u - pending_);
    for (int i = 0; i < 8; i++) {
        pad[pad_len + static_cast<size_t>(i)] =
            static_cast<uint8_t>((bits >> (56 - 8 * i)) & 0xFFu);
    }
    feed(pad, pad_len + 8u);

    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64u);
    for (int i = 0; i < 8; i++) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(digits[(state_[i] >> shift) & 0xFu]);
        }
    }
    return out;
}

std::string hex_of(const void* data, size_t len)
{
    Context ctx;
    ctx.update(data, len);
    return ctx.hex_digest();
}

}  // namespace sha256
}  // namespace ambre
