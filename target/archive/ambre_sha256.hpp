/*
 * ambre_sha256.hpp — SHA-256 (FIPS 180-4) used by the archive metadata
 * (EXP-04: configuration and image fingerprints).
 *
 * Standard library only: the target image ships no OpenSSL and the digest must
 * be reproducible on QNX 8.0. tests/test_archive_cpp.cpp checks it against the
 * published test vectors.
 */

#ifndef AMBRE_SHA256_HPP
#define AMBRE_SHA256_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace ambre {
namespace sha256 {

class Context {
 public:
    Context();
    void update(const void* data, size_t len);
    /* Pads the message, finalises it and returns the 64-character digest. */
    std::string hex_digest();

 private:
    void feed(const uint8_t* data, size_t len);

    uint32_t state_[8];
    uint8_t buffer_[64];
    uint32_t pending_;
    uint64_t total_;
};

/* One-shot digest of a memory block. */
std::string hex_of(const void* data, size_t len);

}  // namespace sha256
}  // namespace ambre

#endif /* AMBRE_SHA256_HPP */
