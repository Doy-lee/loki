#include "core_rpc_server_command_parser.h"
#include "oxenmq/bt_serialize.h"
#include "rpc/common/param_parser.hpp"

#include <chrono>
#include <oxenmq/base64.h>
#include <oxenmq/hex.h>
#include <type_traits>
#include <utility>

namespace cryptonote::rpc {

  using nlohmann::json;

  void parse_request(ONS_RESOLVE& ons, rpc_input in) {
    get_values(in,
        "name_hash", required{ons.request.name_hash},
        "type", required{ons.request.type});
  }

  void parse_request(GET_SERVICE_NODES& sns, rpc_input in) {
    // Remember: key access must be in sorted order (even across get_values() calls).
    get_values(in, "active_only", sns.request.active_only);
    bool fields_dict = false;
    if (auto* json_in = std::get_if<json>(&in)) {
        // Deprecated {"field":true, "field2":true, ...} handling:
      if (auto fit = json_in->find("fields"); fit != json_in->end() && fit->is_object()) {
        fields_dict = true;
        for (auto& [k, v] : fit->items()) {
          if (v.get<bool>()) {
            if (k == "all") {
              sns.request.fields.clear(); // Empty means all
              break; // The old behaviour just ignored everything else if you specified all
            }
            sns.request.fields.insert(k);
          }
        }
      }
    }
    if (!fields_dict) {
      std::vector<std::string_view> fields;
      get_values(in, "fields", fields);
      for (const auto& f : fields)
        sns.request.fields.emplace(f);
      // If the only thing given is "all" then just clear it (as a small optimization):
      if (sns.request.fields.size() == 1 && *sns.request.fields.begin() == "all")
        sns.request.fields.clear();
    }

    get_values(in,
        "limit", sns.request.limit,
        "poll_block_hash", sns.request.poll_block_hash,
        "service_node_pubkeys", sns.request.service_node_pubkeys);
  }
  void parse_request(START_MINING& start_mining, rpc_input in) {
    get_values(in,
        "miner_address", required{start_mining.request.miner_address},
        "num_blocks", start_mining.request.num_blocks,
        "slow_mining", start_mining.request.slow_mining,
        "threads_count", start_mining.request.threads_count);
  }
  void parse_request(GET_OUTPUTS& get_outputs, rpc_input in) {
    get_values(in,
        "as_tuple", get_outputs.request.as_tuple,
        "get_txid", get_outputs.request.get_txid);

    // "outputs" is trickier: for backwards compatibility we need to accept json of:
    //    [{"amount":0,"index":i1}, ...]
    // but that is incredibly wasteful and so we also want the more efficient (and we only accept
    // this for bt, since we don't have backwards compat to worry about):
    //    [i1, i2, ...]
    bool legacy_outputs = false;
    if (auto* json_in = std::get_if<json>(&in)) {
      if (auto outputs = json_in->find("outputs");
          outputs != json_in->end() && !outputs->empty() && outputs->is_array() && outputs->front().is_object()) {
        legacy_outputs = true;
        auto& reqoi = get_outputs.request.output_indices;
        reqoi.reserve(outputs->size());
        for (auto& o : *outputs)
          reqoi.push_back(o["index"].get<uint64_t>());
      }
    }
    if (!legacy_outputs)
      get_values(in, "outputs", get_outputs.request.output_indices);
  }

  void parse_request(GET_TRANSACTION_POOL_STATS& pstats, rpc_input in) {
    get_values(in, "include_unrelayed", pstats.request.include_unrelayed);
  }

  void parse_request(HARD_FORK_INFO& hfinfo, rpc_input in) {
    get_values(in,
        "height", hfinfo.request.height,
        "version", hfinfo.request.version);
    if (hfinfo.request.height && hfinfo.request.version)
      throw std::runtime_error{"Error: at most one of 'height'" + std::to_string(hfinfo.request.height) + "/" + std::to_string(hfinfo.request.version) + " and 'version' may be specified"};
  }

  void parse_request(GET_TRANSACTIONS& get, rpc_input in) {
    // Backwards compat for old stupid "txs_hashes" input name
    if (auto* json_in = std::get_if<json>(&in))
      if (auto it = json_in->find("txs_hashes"); it != json_in->end())
        (*json_in)["tx_hashes"] = std::move(*it);

    std::optional<bool> data;
    get_values(in,
      "data", data,
      "memory_pool", get.request.memory_pool,
      "prune", get.request.prune,
      "split", get.request.split,
      "tx_extra", get.request.tx_extra,
      "tx_hashes", get.request.tx_hashes);

    if (data)
        get.request.data = *data;
    else
        get.request.data = !(get.request.prune || get.request.split);

    if (get.request.memory_pool && !get.request.tx_hashes.empty())
      throw std::runtime_error{"Error: 'memory_pool' and 'tx_hashes' are mutually exclusive"};
  }

  void parse_request(SET_LIMIT& limit, rpc_input in) {
    get_values(in,
        "limit_down", limit.request.limit_down,
        "limit_up", limit.request.limit_up);
    if (limit.request.limit_down < -1)
      throw std::domain_error{"limit_down must be >= -1"};
    if (limit.request.limit_down < -1)
      throw std::domain_error{"limit_up must be >= -1"};
  }

  void parse_request(IS_KEY_IMAGE_SPENT& spent, rpc_input in) {
    get_values(in, "key_images", spent.request.key_images);
  }

  void parse_request(SUBMIT_TRANSACTION& tx, rpc_input in) {
    if (auto* json_in = std::get_if<json>(&in))
      if (auto it = json_in->find("tx_as_hex"); it != json_in->end())
        (*json_in)["tx"] = std::move(*it);

    auto& tx_data = tx.request.tx;
    get_values(in,
        "blink", tx.request.blink,
        "tx", required{tx_data});

    if (tx_data.empty()) // required above will make sure it's specified, but doesn't guarantee against an empty value
      throw std::domain_error{"Invalid 'tx' value: cannot be empty"};

    // tx can be specified as base64, hex, or binary, so try to figure out which one we have by
    // looking at the beginning.
    //
    // An encoded transaction always starts with the version byte, currently 0-4 (though 0 isn't
    // actually used), with higher future values possible.  That means in hex we get something like:
    // `04...` and in base64 we get `B` (because the first 6 bits are 000001, and the b64 alphabet
    // begins at `A` for 0).  Thus the first bytes, for tx versions 0 through 48, are thus:
    //
    // binary: (31 binary control characters through 0x1f ... )          (space) ! " # $ % & ' ( ) * + , - . / 0
    // base64: A A A A B B B B C C C C D D D D E E E E F F F F G G G G H H H H I I I I J J J J K K K K L L L L M
    // hex:    0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 2 3
    //
    // and so we run into the first ambiguity at version 48.  Since we are currently only at version
    // 4 (and Oxen started at version 2) this is likely to be sufficient for an extremely long time.
    //
    // Thus our heuristic:
    //     'A'-'L' => base64
    //     '0'-'2' => hex
    //     \x00-\x2f => bytes
    // anything else we reject as garbage.
    auto tx0 = tx_data.front();
    bool good = false;
    if (tx0 <= 0x2f) {
      good = true;
    } else if (tx0 >= 'A' && tx0 <= 'L') {
      if (oxenmq::is_base64(tx_data)) {
        auto end = oxenmq::from_base64(tx_data.begin(), tx_data.end(), tx_data.begin());
        tx_data.erase(end, tx_data.end());
        good = true;
      }
    } else if (tx0 >= '0' && tx0 <= '2') {
      if (oxenmq::is_hex(tx_data)) {
        auto end = oxenmq::from_hex(tx_data.begin(), tx_data.end(), tx_data.begin());
        tx_data.erase(end, tx_data.end());
        good = true;
      }
    }

    if (!good)
      throw std::domain_error{"Invalid 'tx' value: expected hex, base64, or bytes"};
  }

  void parse_request(GET_BLOCK_HASH& bh, rpc_input in) {
    get_values(in, "heights", bh.request.heights);
    if (bh.request.heights.size() > bh.MAX_HEIGHTS)
      throw std::domain_error{"Error: too many block heights requested at once"};
  }

  void parse_request(GET_PEER_LIST& pl, rpc_input in) {
    get_values(in, "public_only", pl.request.public_only);
  }

  void parse_request(SET_LOG_LEVEL& set_log_level, rpc_input in) {
    get_values(in, "level", set_log_level.request.level);
  }

  void parse_request(SET_LOG_CATEGORIES& set_log_categories, rpc_input in) {
    get_values(in, "categories", set_log_categories.request.categories);
  }

  void parse_request(BANNED& banned, rpc_input in) {
    get_values(in, "address", banned.request.address);
  }

  void parse_request(FLUSH_TRANSACTION_POOL& flush_transaction_pool, rpc_input in) {
    get_values(in, "txids", flush_transaction_pool.request.txids);
  }

  void parse_request(GET_COINBASE_TX_SUM& get_coinbase_tx_sum, rpc_input in) {
    get_values(in, "height", get_coinbase_tx_sum.request.height);
    get_values(in, "count", get_coinbase_tx_sum.request.count);
  }

  void parse_request(GET_BASE_FEE_ESTIMATE& get_base_fee_estimate, rpc_input in) {
    get_values(in, "grace_blocks", get_base_fee_estimate.request.grace_blocks);
  }

  void parse_request(OUT_PEERS& out_peers, rpc_input in){
    get_values(in, "set", out_peers.request.set);
    get_values(in, "out_peers", out_peers.request.out_peers);
  }

  void parse_request(IN_PEERS& in_peers, rpc_input in){
    get_values(in, "set", in_peers.request.set);
    get_values(in, "in_peers", in_peers.request.in_peers);
  }

  void parse_request(POP_BLOCKS& pop_blocks, rpc_input in){
    get_values(in, "nblocks", pop_blocks.request.nblocks);
  }

  void parse_request(LOKINET_PING& lokinet_ping, rpc_input in){
    get_values(in, "version", lokinet_ping.request.version);
    get_values(in, "ed25519_pubkey", lokinet_ping.request.ed25519_pubkey);
  }

  void parse_request(STORAGE_SERVER_PING& storage_server_ping, rpc_input in){
    get_values(in, "version", storage_server_ping.request.version);
    get_values(in, "https_port", storage_server_ping.request.https_port);
    get_values(in, "omq_port", storage_server_ping.request.omq_port);
    get_values(in, "ed25519_pubkey", storage_server_ping.request.ed25519_pubkey);
  }

  void parse_request(PRUNE_BLOCKCHAIN& prune_blockchain, rpc_input in){
    get_values(in, "check", prune_blockchain.request.check);
  }

  void parse_request(GET_SN_STATE_CHANGES& get_sn_state_changes, rpc_input in) {
    get_values(in, "start_height", get_sn_state_changes.request.start_height);
    get_values(in, "end_height", get_sn_state_changes.request.end_height);
  }

  void parse_request(REPORT_PEER_STATUS& report_peer_status, rpc_input in) {
    get_values(in, "type", report_peer_status.request.type);
    get_values(in, "pubkey", report_peer_status.request.pubkey);
    get_values(in, "passed", report_peer_status.request.passed);
  }

  void parse_request(FLUSH_CACHE& flush_cache, rpc_input in) {
    get_values(in, "bad_txs", flush_cache.request.bad_txs);
    get_values(in, "bad_blocks", flush_cache.request.bad_blocks);
  }

  void parse_request(GET_LAST_BLOCK_HEADER& get_last_block_header, rpc_input in) {
    get_values(in, "fill_pow_hash", get_last_block_header.request.fill_pow_hash);
    get_values(in, "get_tx_hashes", get_last_block_header.request.get_tx_hashes);
  }

  void parse_request(GET_BLOCK_HEADER_BY_HASH& get_block_header_by_hash, rpc_input in) {
    get_values(in, "hash", get_block_header_by_hash.request.hash);
    get_values(in, "hashes", get_block_header_by_hash.request.hashes);
    get_values(in, "fill_pow_hash", get_block_header_by_hash.request.fill_pow_hash);
    get_values(in, "get_tx_hashes", get_block_header_by_hash.request.get_tx_hashes);
  }

  void parse_request(SETBANS& set_bans, rpc_input in) {
    get_values(in, "host", set_bans.request.host);
    get_values(in, "ip", set_bans.request.ip);
    get_values(in, "seconds", set_bans.request.seconds);
    get_values(in, "ban", set_bans.request.ban);
  }

  void parse_request(GET_STAKING_REQUIREMENT& get_staking_requirement, rpc_input in) {
    get_values(in, "height", get_staking_requirement.request.height);
  }

  void parse_request(GET_BLOCK_HEADERS_RANGE& get_block_headers_range, rpc_input in) {
    get_values(in, "start_height", get_block_headers_range.request.start_height);
    get_values(in, "end_height", get_block_headers_range.request.end_height);
    get_values(in, "fill_pow_hash", get_block_headers_range.request.fill_pow_hash);
    get_values(in, "get_tx_hashes", get_block_headers_range.request.get_tx_hashes);
  }

  void parse_request(GET_BLOCK_HEADER_BY_HEIGHT& get_block_header_by_height, rpc_input in) {
    get_values(in, "height",        get_block_header_by_height.request.height);
    get_values(in, "heights",       get_block_header_by_height.request.heights);
    get_values(in, "fill_pow_hash", get_block_header_by_height.request.fill_pow_hash);
    get_values(in, "get_tx_hashes", get_block_header_by_height.request.get_tx_hashes);
  }

  void parse_request(GET_BLOCK& get_block, rpc_input in) {
    get_values(in, "hash",          get_block.request.hash);
    get_values(in, "height",        get_block.request.height);
    get_values(in, "fill_pow_hash", get_block.request.fill_pow_hash);
  }

  void parse_request(GET_OUTPUT_HISTOGRAM& get_output_histogram, rpc_input in) {
    get_values(in, "amounts", get_output_histogram.request.amounts);
    get_values(in, "min_count", get_output_histogram.request.min_count);
    get_values(in, "max_count", get_output_histogram.request.max_count);
    get_values(in, "unlocked", get_output_histogram.request.unlocked);
    get_values(in, "recent_cutoff", get_output_histogram.request.recent_cutoff);
  }

  void parse_request(GET_ACCRUED_BATCHED_EARNINGS& get_accrued_batched_earnings, rpc_input in) {
    get_values(in, "addresses", get_accrued_batched_earnings.request.addresses);
  }
}