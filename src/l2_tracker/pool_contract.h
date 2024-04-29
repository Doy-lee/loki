#pragma once

#include <string>
#include <memory>
#include <vector>

#include <ethyl/provider.hpp>

class RewardRateResponse {
public:
    uint64_t timestamp;
    uint64_t reward;
    RewardRateResponse(uint64_t _timestamp, uint64_t _reward) : timestamp(_timestamp), reward(_reward) {}
};

class PoolContract {
public:
    PoolContract(const std::string& _contractAddress, std::shared_ptr<Provider> _provider);
    RewardRateResponse RewardRate(uint64_t timestamp);

private:
    std::string contractAddress;
    std::shared_ptr<Provider> provider;
};

