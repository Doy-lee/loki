#pragma once

#define BLS_ETH
#define MCLBN_FP_UNIT_SIZE 4
#define MCLBN_FR_UNIT_SIZE 4

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <bls/bls.hpp>
#include <mcl/bn.hpp>
#undef MCLBN_NO_AUTOLINK
#pragma GCC diagnostic pop

#include "crypto/base.h"
#include "crypto/crypto.h"
#include "cryptonote_config.h"

class BLSSigner {
  private:
    bls::SecretKey secretKey;
    cryptonote::network_type nettype;

  public:
    // Constructs a BLSSigner; if the `key` is nullptr, a key is generated; otherwise the key is
    // loaded from the given bls_secret_key data.
    explicit BLSSigner(
            const cryptonote::network_type nettype, const crypto::bls_secret_key* key = nullptr);

    bls::Signature signHashSig(const crypto::hash& hash);
    crypto::bls_signature signHash(const crypto::hash& hash);
    crypto::bls_signature proofOfPossession(
            crypto::eth_address sender, const crypto::public_key& serviceNodePubkey);
    std::string getPublicKeyHex();
    bls::PublicKey getPublicKey();

    // Gets the public key as our crypto::bls_public_key type
    crypto::bls_public_key getCryptoPubkey();
    // Gets the secret key as our crypto::bls_secret_key type
    crypto::bls_secret_key getCryptoSeckey();

    static std::string buildTagHex(std::string_view baseTag, cryptonote::network_type nettype);
    static crypto::hash buildTagHash(std::string_view baseTag, cryptonote::network_type nettype);
    std::string buildTagHex(std::string_view baseTag);
    crypto::hash buildTagHash(std::string_view baseTag);

    static crypto::hash hashModulus(std::string_view message);

    static constexpr inline std::string_view proofOfPossessionTag = "BLS_SIG_TRYANDINCREMENT_POP";
    static constexpr inline std::string_view rewardTag = "BLS_SIG_TRYANDINCREMENT_REWARD";
    static constexpr inline std::string_view removalTag = "BLS_SIG_TRYANDINCREMENT_REMOVE";
    static constexpr inline std::string_view liquidateTag = "BLS_SIG_TRYANDINCREMENT_LIQUIDATE";
};
