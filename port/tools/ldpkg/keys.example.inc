// Copy this file to keys.inc (git-ignored) and fill in the two RSA-2048 private keys that fake-package (fPKG) tools
// use: the "fake" keyset that encrypts a fake package's IMAGE_KEY entry, and the package derived-key-3 keyset for
// ENTRY_KEYS key 3. Each is a big-endian modulus (N) and private exponent (D), 256 bytes. They are left out of this
// repository on purpose.
static const uint8_t kFakeN[256] = {/* 256 bytes */};
static const uint8_t kFakeD[256] = {/* 256 bytes */};
static const uint8_t kDk3N[256] = {/* 256 bytes */};
static const uint8_t kDk3D[256] = {/* 256 bytes */};
