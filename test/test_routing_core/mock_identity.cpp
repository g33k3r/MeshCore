// Crypto stubs: the 4 (and only) Identity symbols that link unresolved when
// Identity.cpp is excluded from the native test build. Routing characterization
// does not exercise real Ed25519 — sign is a no-op, verify always succeeds.
#include <Identity.h>

namespace mesh {

Identity::Identity() { memset(pub_key, 0, PUB_KEY_SIZE); }

LocalIdentity::LocalIdentity() { memset(prv_key, 0, PRV_KEY_SIZE); }

bool Identity::verify(const uint8_t*, const uint8_t*, int) const { return true; }

void LocalIdentity::sign(uint8_t* sig, const uint8_t* message, int msg_len) const {
  // Deterministic stand-in: leave no byte uninitialized (valgrind-gated),
  // and make signatures message-dependent.
  for (int i = 0; i < SIGNATURE_SIZE; i++) {
    sig[i] = (uint8_t)(i * 7) ^ (msg_len > 0 ? message[i % msg_len] : 0) ^ prv_key[i % PRV_KEY_SIZE];
  }
}

void LocalIdentity::calcSharedSecret(uint8_t* secret, const uint8_t*) const {
  memset(secret, 0, PUB_KEY_SIZE);
}

}  // namespace mesh
