#include "rewards_contract.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

#include "logging/oxen_logger.h"

static auto logcat = oxen::log::Cat("l2_tracker");

TransactionType RewardsLogEntry::getLogType() const {
    if (topics.empty()) {
        throw std::runtime_error("No topics in log entry");
    }
    // keccak256('NewServiceNode(uint64,address,(uint256,uint256),uint256,uint256)')
    if (topics[0] == "0x33f8d30fa2c44ed0b2147e4aa1e1d14e3ee4f96d7586e1fea20c3cfe67b40083") {
        return TransactionType::NewServiceNode;
    // keccak256('ServiceNodeRemovalRequest(uint64,address,(uint256,uint256))')
    } else if (topics[0] == "0x89477e9f4ddcb5eb9f30353ab22c31ef9a91ab33fd1ffef09aadb3458be7775d") {
        return TransactionType::ServiceNodeLeaveRequest;
    // keccak256('ServiceNodeLiquidated(uint64,address,(uint256,uint256))')
    } else if (topics[0] == "0x0bfb12191b00293af29126b1c5489f8daeb4a4af82db2960b7f8353c3105cd7c") {
        return TransactionType::ServiceNodeDeregister;
    }
    return TransactionType::Other;
}

std::optional<TransactionStateChangeVariant> RewardsLogEntry::getLogTransaction() const {
    TransactionType type = getLogType();
    switch (type) {
        case TransactionType::NewServiceNode: {
            // event NewServiceNode(uint64 indexed serviceNodeID, address recipient, BN256G1.G1Point pubkey, uint256 serviceNodePubkey, uint256 serviceNodeSignature);
            // service node id is a topic so only address and pubkey are in data
            // address is 32 bytes , pubkey is 64 bytes and serviceNodePubkey is 32 bytes
            //
            // pull 32 bytes from start
            std::string eth_address = data.substr(2, 64);
            // from position 64 (32 bytes -> 64 characters) + 2 for '0x' pull 64 bytes (128 characters)
            std::string bls_key = data.substr(64 + 2, 128);
            // pull 32 bytes (64 characters)
            std::string service_node_pubkey = data.substr(128 + 64 + 2, 64);
            // pull 32 bytes (64 characters)
            std::string signature = data.substr(128 + 64 + 64 + 2, 64);
            return NewServiceNodeTx(bls_key, eth_address, service_node_pubkey, signature);
        }
        case TransactionType::ServiceNodeLeaveRequest: {
            // event ServiceNodeRemovalRequest(uint64 indexed serviceNodeID, address recipient, BN256G1.G1Point pubkey);
            // service node id is a topic so only address and pubkey are in data
            // address is 32 bytes and pubkey is 64 bytes,
            //
            // from position 64 (32 bytes -> 64 characters) + 2 for '0x' pull 64 bytes (128 characters)
            std::string bls_key = data.substr(64 + 2, 128);
            return ServiceNodeLeaveRequestTx(bls_key);
        }
        case TransactionType::ServiceNodeDeregister: {
            // event ServiceNodeLiquidated(uint64 indexed serviceNodeID, address recipient, BN256G1.G1Point pubkey);
            // service node id is a topic so only address and pubkey are in data
            // address is 32 bytes and pubkey is 64 bytes,
            //
            // from position 64 (32 bytes -> 64 characters) + 2 for '0x' pull 64 bytes (128 characters)
            std::string bls_key = data.substr(64 + 2, 128);
            return ServiceNodeDeregisterTx(bls_key);
        }
        default:
            return std::nullopt;
    }
}

RewardsContract::RewardsContract(const std::string& _contractAddress, std::shared_ptr<Provider> _provider)
        : contractAddress(_contractAddress), provider(std::move(_provider)) {}

StateResponse RewardsContract::State() {
    return State(provider->getLatestHeight());
}

StateResponse RewardsContract::State(uint64_t height) {
    std::string blockHash = provider->getContractStorageRoot(contractAddress, height);
    // Check if blockHash starts with "0x" and remove it
    if (blockHash.size() >= 2 && blockHash[0] == '0' && blockHash[1] == 'x') {
        blockHash = blockHash.substr(2); // Skip the first two characters
    }
    return StateResponse{height, blockHash};
}

std::vector<RewardsLogEntry> RewardsContract::Logs(uint64_t height) {
    std::vector<RewardsLogEntry> logEntries;
    // Make the RPC call
    const auto logs = provider->getLogs(height, contractAddress);

    for (const auto& log : logs) {
        logEntries.emplace_back(RewardsLogEntry(log));
    }

    return logEntries;
}

