#pragma once
#include <ethyl/logs.hpp>
#include <ethyl/provider.hpp>
#include <string>
#include <unordered_set>
#include <variant>

#include "crypto/crypto.h"
#include "crypto/eth.h"

namespace eth {

enum class TransactionType {
    NewServiceNode,
    ServiceNodeLeaveRequest,
    ServiceNodeDeregister,
    ServiceNodeExit,
    Other
};

struct Contributor {
    eth::address addr;
    uint64_t amount;
};

struct NewServiceNodeTx {
    bls_public_key bls_pubkey;
    eth::address eth_address;
    crypto::public_key sn_pubkey;
    crypto::ed25519_signature sn_signature;
    uint64_t fee;
    std::vector<Contributor> contributors;
};

struct ServiceNodeLeaveRequestTx {
    bls_public_key bls_pubkey;
};

struct ServiceNodeDeregisterTx {
    bls_public_key bls_pubkey;
};

struct ServiceNodeExitTx {
    eth::address eth_address;
    uint64_t amount;
    bls_public_key bls_pubkey;
};

using TransactionStateChangeVariant = std::variant<
        std::monostate,
        NewServiceNodeTx,
        ServiceNodeLeaveRequestTx,
        ServiceNodeDeregisterTx,
        ServiceNodeExitTx>;

TransactionType getLogType(const ethyl::LogEntry& log);
TransactionStateChangeVariant getLogTransaction(const ethyl::LogEntry& log);

struct StateResponse {
    uint64_t height;
    crypto::hash block_hash;
};

struct ContractServiceNode {
    bool good;
    uint64_t next;
    uint64_t prev;
    eth::address operatorAddr;
    bls_public_key pubkey;
    uint64_t leaveRequestTimestamp;
    uint64_t deposit;
    std::array<Contributor, 10> contributors;
    size_t contributorsSize;
};

class RewardsContract {
  public:
    // Constructor
    RewardsContract(const std::string& _contractAddress, ethyl::Provider& provider);

    StateResponse State();
    StateResponse State(uint64_t height);

    std::vector<ethyl::LogEntry> Logs(uint64_t height);
    ContractServiceNode serviceNodes(
            uint64_t index, std::optional<uint64_t> blockNumber = std::nullopt);
    std::vector<uint64_t> getNonSigners(const std::unordered_set<bls_public_key>& bls_public_keys);
    std::vector<bls_public_key> getAllBLSPubkeys(uint64_t blockNumber);

  private:
    std::string contractAddress;
    ethyl::Provider& provider;
};

}  // namespace eth
