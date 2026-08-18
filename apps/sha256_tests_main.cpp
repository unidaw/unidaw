// SHA-256 AGAINST PUBLISHED VECTORS, not against itself.
//
// The artifact inventory (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME) commits every stored byte
// with a SHA-256 and names each generation by the digest of its own sorted entries. An
// implementation that is subtly wrong does not fail loudly: it agrees with itself on every write
// and every read, so a save/load round trip is green, and it disagrees only with the rest of the
// world — with `shasum -a 256`, with a file somebody hashed elsewhere, with the next
// implementation anyone writes.
//
// So every expected value below comes from FIPS 180-4 / the NIST examples, not from running this
// code. The lengths are chosen to cross the places the padding gets written wrong:
//
//   55 bytes  — the last length whose 0x80 marker and 8-byte length still fit in ONE block
//   56 bytes  — the first that needs a SECOND block, the classic off-by-one
//   63, 64    — one short of a block, and exactly a block (which still needs a whole pad block)
//   1,000,000 — the standard long case, exercising the multi-block loop
//
// An implementation that pads correctly for short inputs and wrongly at 56..63 passes a naive test
// and corrupts a narrow band of real files.

#include <cstdio>
#include <string>
#include <vector>

#include "apps/sha256.h"

namespace {

int failures = 0;

void expect(const std::string& got, const std::string& want, const std::string& what) {
  if (got != want) {
    std::printf("sha256_tests: FAIL %s\n  got  %s\n  want %s\n", what.c_str(), got.c_str(),
                want.c_str());
    ++failures;
  }
}

}  // namespace

int main() {
  // FIPS 180-4, Appendix B.1: the empty string.
  expect(daw::sha256Hex(std::string("")),
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "the empty string");

  // FIPS 180-4, Appendix B.1: "abc" (one block, three bytes).
  expect(daw::sha256Hex(std::string("abc")),
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "abc");

  // FIPS 180-4, Appendix B.2: 56 bytes — the first length that needs a SECOND padded block.
  expect(daw::sha256Hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "the 56-byte two-block vector");

  // FIPS 180-4, Appendix B.3 / NIST: 112 bytes.
  expect(daw::sha256Hex(std::string(
             "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
             "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
         "the 112-byte vector");

  // 55 bytes: the LAST length that still fits one block. Its neighbour above needs two, and an
  // implementation with the boundary off by one gets exactly one of this pair wrong.
  expect(daw::sha256Hex(std::string(55, 'a')),
         "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
         "55 bytes of 'a' — the last single-block length");

  // 63 and 64: one short of a block, and exactly a block. 64 still needs a whole extra pad block,
  // which is the case that looks like it should need none.
  expect(daw::sha256Hex(std::string(63, 'a')),
         "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",
         "63 bytes of 'a'");
  expect(daw::sha256Hex(std::string(64, 'a')),
         "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
         "64 bytes of 'a' — exactly one block, and it still needs a pad block");

  // The standard long case: one million 'a'. FIPS 180-4 Appendix B.3.
  expect(daw::sha256Hex(std::string(1000000, 'a')),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
         "one million 'a'");

  // The byte-vector overload takes the same path — asserted rather than assumed, because a
  // wrapper that hashed the vector's SIZE or its pointer would still return a plausible digest.
  const std::vector<uint8_t> abc{'a', 'b', 'c'};
  expect(daw::sha256Hex(abc),
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "the byte-vector overload agrees with the string one");

  // AND A DISCRIMINATION CHECK: one flipped bit must change the digest. A stub returning a
  // constant would pass nothing above, but a stub that hashed only the first block would pass
  // several — this catches that shape.
  const std::string a(200, 'x');
  std::string b = a;
  b[199] = 'y';
  if (daw::sha256Hex(a) == daw::sha256Hex(b)) {
    std::printf("sha256_tests: FAIL a change in the LAST byte of a multi-block input did not "
                "change the digest\n");
    ++failures;
  }

  if (failures != 0) {
    std::printf("sha256_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("sha256_tests: PASS — every vector is from FIPS 180-4, not from this code\n");
  return 0;
}
