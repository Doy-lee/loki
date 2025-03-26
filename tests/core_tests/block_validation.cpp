// Copyright (c) 2014-2018, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include "chaingen.h"
#include "block_validation.h"
#include "common/util.h"

using namespace cryptonote;

static bool lift_up_difficulty(
        std::vector<test_event_entry>& events,
        std::vector<uint64_t>& timestamps,
        std::vector<difficulty_type>& cummulative_difficulties,
        oxen_chain_generator& gen,
        size_t block_count) {

    difficulty_type cummulative_diffic =
            cummulative_difficulties.empty() ? 0 : cummulative_difficulties.back();
    for (size_t i = 0; i < block_count; ++i) {

        // NOTE: Calc difficulty
        difficulty_type difficulty = next_difficulty_v2(
                timestamps,
                cummulative_difficulties,
                tools::to_seconds(
                        get_config(cryptonote::network_type::FAKECHAIN).TARGET_BLOCK_TIME),
                cryptonote::difficulty_calc_mode::normal);

        // NOTE: Construct block with custom difficulty
        oxen_create_block_params params = gen.next_block_params();
        params.timestamp = gen.blocks().back().block.timestamp;

        oxen_blockchain_entry entry = {};
        gen.block_begin(entry, params, /*tx_list*/ {});
        fill_nonce_with_oxen_generator(&gen, entry.block, difficulty, entry.block.get_height());
        gen.block_end(entry, params);
        gen.add_block(entry, /*can_be_added_to_blockchain*/ true);

        // NOTE: Append to timestamps and difficulty to window
        cummulative_diffic += difficulty;
        if (timestamps.size() == old::DIFFICULTY_WINDOW) {
            timestamps.erase(timestamps.begin());
            cummulative_difficulties.erase(cummulative_difficulties.begin());
        }
        timestamps.push_back(entry.block.timestamp);
        cummulative_difficulties.push_back(cummulative_diffic);
    }

    return true;
}

bool gen_block_big_major_version::generate(std::vector<test_event_entry>& events) const
{
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());
  uint64_t last_good_height = gen.chain_height();

  oxen_create_block_params params = gen.next_block_params();
  params.hf_version = cryptonote::hf::_next;

  oxen_blockchain_entry entry = {};
  gen.create_block(entry, params, /*tx_list*/ {});
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block with OOB major version cannot be accepted");

  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_big_minor_version::generate(std::vector<test_event_entry>& events) const
{
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());

  oxen_create_block_params params = gen.next_block_params();
  oxen_blockchain_entry entry = {};
  gen.create_block(entry, params, /*tx_list*/ {});

  entry.block.minor_version = 255;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ true);
  uint64_t last_good_height = gen.chain_height();

  oxen_register_callback(
          events,
          "check_block_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });


  return true;
}

bool gen_block_ts_not_checked::generate(std::vector<test_event_entry>& events) const
{
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());
  gen.add_n_blocks(BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW - 2);

  oxen_create_block_params params = gen.next_block_params();
  params.timestamp = gen.blocks().front().block.timestamp - 60 * 60;

  oxen_blockchain_entry entry = {};
  gen.create_block(entry, params, /*tx_list*/ {});
  gen.add_block(entry, /*can_be_added_to_blockchain*/ true);
  uint64_t last_good_height = gen.chain_height();

  oxen_register_callback(
          events,
          "check_block_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_ts_in_past::generate(std::vector<test_event_entry>& events) const
{
  // NOTE: Setup
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());
  gen.add_n_blocks(BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW - 1);
  uint64_t last_good_height = gen.chain_height();

  // NOTE: Construct block
  uint64_t ts_below_median = gen.blocks()[BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW / 2 - 1].block.timestamp;
  oxen_create_block_params params = gen.next_block_params();
  params.timestamp = ts_below_median;

  oxen_blockchain_entry entry = {};
  gen.create_block(entry, params, /*tx_list*/ {});
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false);

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });
  return true;
}

bool gen_block_ts_in_future::generate(std::vector<test_event_entry>& events) const
{
  // NOTE: Setup
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());
  gen.add_n_blocks(BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW - 1);
  uint64_t last_good_height = gen.chain_height();

  // NOTE: Construct block
  oxen_create_block_params params = gen.next_block_params();
  params.timestamp = time(nullptr) + (60 * 60) + old::BLOCK_FUTURE_TIME_LIMIT_V2;

  oxen_blockchain_entry entry = {};
  gen.create_block(entry, params, /*tx_list*/ {});
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false);

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_invalid_prev_id::generate(std::vector<test_event_entry>& events) const
{
  // NOTE: Setup
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());
  uint64_t last_good_height = gen.chain_height();

  // NOTE: Construct block
  oxen_create_block_params params = gen.next_block_params();

  oxen_blockchain_entry entry = {};
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.prev_id[0] ^= 1;

  gen.add_block(entry, /*can_be_added_to_blockchain*/ false);

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_invalid_nonce::generate(std::vector<test_event_entry>& events) const
{
  // NOTE: Setup
  oxen_chain_generator gen(events, oxen_generate_hard_fork_table());
  std::vector<uint64_t> timestamp_window;
  std::vector<difficulty_type> difficulty_window;
  if (!lift_up_difficulty(events, timestamp_window, difficulty_window, gen, 4))
    return false;
  uint64_t last_good_height = gen.chain_height();

  // NOTE: Construct the last block with the raised difficulty, and keep looking for a PoW solution
  // until the nonce is non-zero. Once we get that, we subtract 1 from the nonce to make it
  // insufficient.
  oxen_create_block_params params = gen.next_block_params();
  params.timestamp = gen.blocks().back().block.timestamp;

  difficulty_type difficulty = next_difficulty_v2(
          timestamp_window,
          difficulty_window,
          tools::to_seconds(get_config(cryptonote::network_type::FAKECHAIN).TARGET_BLOCK_TIME),
          cryptonote::difficulty_calc_mode::normal);
  assert(difficulty > 1);

  oxen_blockchain_entry entry = {};
  gen.block_begin(entry, params, /*tx_list*/ {});
  do {
      entry.block.timestamp++;
      fill_nonce_with_oxen_generator(&gen, entry.block, difficulty, entry.block.get_height());
  } while (entry.block.nonce == 0);
  entry.block.nonce--;
  gen.block_end(entry, params);

  gen.add_block(entry, /*can_be_added_to_blockchain*/ false);

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_no_miner_tx::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx = std::nullopt;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block without a miner TX cannot be added prior to HF21");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_unlock_time_is_low::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->unlock_time--;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block with too low unlock time");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });
  return true;
}

bool gen_block_unlock_time_is_high::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->unlock_time++;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has unlock time too high");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });
  return true;
}

bool gen_block_unlock_time_is_timestamp_in_past::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->unlock_time = gen.blocks().front().block.timestamp - 10 * 60;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has unlock timestamp in the past");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });
  return true;
}

bool gen_block_unlock_time_is_timestamp_in_future::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->unlock_time = gen.blocks().front().block.timestamp + 3 * MINED_MONEY_UNLOCK_WINDOW * tools::to_seconds(get_config(cryptonote::network_type::FAKECHAIN).TARGET_BLOCK_TIME);
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has unlock timestamp in the future");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_height_is_low::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  var::get<txin_gen>(entry.block.miner_tx->vin[0]).height--;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has bad height");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_height_is_high::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  var::get<txin_gen>(entry.block.miner_tx->vin[0]).height++;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has bad height");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_miner_tx_has_2_tx_gen_in::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});

  txin_gen in;
  in.height = gen.blocks().front().block.get_height() + 1;
  entry.block.miner_tx->vin.push_back(in);

  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has 2 txin gen which is not allowed");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_miner_tx_has_2_in::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  gen.add_n_blocks(10);
  gen.add_mined_money_unlock_blocks();
  uint64_t last_good_height = gen.chain_height();

  const cryptonote::block& blk_0 = gen.blocks().front().block;
  cryptonote::account_base miner = gen.first_miner();

  transaction tmp_tx;
  if (!oxen_tx_builder(
               events,
               tmp_tx,
               gen.blocks().back().block,
               miner,
               miner.get_keys().m_account_address,
               blk_0.miner_tx.value().vout[0].amount,
               cryptonote::hf::hf7)
               .build())
      return false;

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->vin.push_back(tmp_tx.vin[0]);
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has 2 txin which is not allowed");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_miner_tx_with_txin_to_key::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  gen.add_n_blocks(10);
  gen.add_mined_money_unlock_blocks();
  uint64_t last_good_height = gen.chain_height();

  const cryptonote::block& blk_0 = gen.blocks().front().block;
  cryptonote::account_base miner = gen.first_miner();

  transaction tmp_tx;
  if (!oxen_tx_builder(
               events,
               tmp_tx,
               gen.blocks().back().block,
               miner,
               miner.get_keys().m_account_address,
               blk_0.miner_tx.value().vout[0].amount,
               cryptonote::hf::hf7)
               .build())
      return false;

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->vin[0] = tmp_tx.vin[0];
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has account to account transfer as the miner tx input");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_miner_tx_out_is_big::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->vout[0].amount *= 2;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has bad amount");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_miner_tx_has_no_out::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.miner_tx->vout.clear();
  entry.block.miner_tx->version = txversion::v2_ringct;
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has bad amount");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

static crypto::public_key
get_output_key(const cryptonote::keypair &txkey, const cryptonote::account_public_address &addr, size_t output_index)
{
  crypto::key_derivation derivation;
  crypto::generate_key_derivation(addr.m_view_public_key, txkey.sec, derivation);
  crypto::public_key out_eph_public_key;
  crypto::derive_public_key(derivation, output_index, addr.m_spend_public_key, out_eph_public_key);
  return out_eph_public_key;
}

static bool construct_miner_tx_with_extra_output(cryptonote::transaction& tx,
                                                 const cryptonote::account_public_address& miner_address,
                                                 size_t height,
                                                 uint64_t already_generated_coins,
                                                 const cryptonote::account_public_address& extra_address)
{
    keypair txkey{hw::get_device("default")};
    add_tx_extra<tx_extra_pub_key>(tx, txkey.pub);

    keypair gov_key = get_deterministic_keypair_from_height(height);
    if (already_generated_coins != 0) {
        add_tx_extra<tx_extra_pub_key>(tx, gov_key.pub);
    }

    txin_gen in;
    in.height = height;
    tx.vin.push_back(in);

    // This will work, until size of constructed block is less then BLOCK_GRANTED_FULL_REWARD_ZONE
    const auto hard_fork_version = hf::hf7; // NOTE(oxen): We know this test doesn't need the new block reward formula
    uint64_t block_reward, block_reward_unpenalized;
    if (!get_base_block_reward(0, 0, already_generated_coins, block_reward, block_reward_unpenalized, hf::hf7, 0)) {
        oxen::log::warning(globallogcat, "Block is too big");
        return false;
    }

    uint64_t governance_reward = 0;
    if (already_generated_coins != 0) {
        governance_reward = governance_reward_formula(hard_fork_version, block_reward);
        block_reward -= governance_reward;
    }

    tx.version = txversion::v2_ringct;
    tx.unlock_time = height + MINED_MONEY_UNLOCK_WINDOW;

    /// half of the miner reward goes to the other account 
    const auto miner_reward = block_reward / 2;

    /// miner reward
    tx.vout.push_back({miner_reward, get_output_key(txkey, miner_address, 0)});

    /// extra reward
    tx.vout.push_back({miner_reward, get_output_key(txkey, extra_address, 1)});

    /// governance reward
    if (already_generated_coins != 0) {
        const cryptonote::network_type nettype = network_type::FAKECHAIN;
        cryptonote::address_parse_info governance_wallet_address;
        cryptonote::get_account_address_from_str(governance_wallet_address, nettype, cryptonote::get_config(nettype).governance_wallet_address(hard_fork_version));

        crypto::public_key out_eph_public_key{};

        if (!get_deterministic_output_key(
              governance_wallet_address.address, gov_key, tx.vout.size(), out_eph_public_key)) {
            oxen::log::error(globallogcat, "Failed to generate deterministic output key for governance wallet output creation");
            return false;
        }

        tx.vout.push_back({governance_reward, out_eph_public_key});
        tx.output_unlock_times.push_back(height + MINED_MONEY_UNLOCK_WINDOW);
    }

    return true;
}


bool gen_block_miner_tx_has_out_to_alice::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table(hf::hf7);
  oxen_chain_generator gen(events, hard_forks);

  cryptonote::account_base alice = gen.add_account();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.block_begin(entry, params, /*tx_list*/ {});
  {
      // NOTE: Get miner tx and halve the amount
      cryptonote::transaction& miner_tx = *entry.block.miner_tx;
      miner_tx.vin.clear();
      miner_tx.vout.clear();

      construct_miner_tx_with_extra_output(
              miner_tx,
              gen.first_miner().get_keys().m_account_address,
              gen.chain_height(),
              params.prev.already_generated_coins,
              alice.get_keys().m_account_address);
      fill_nonce_with_oxen_generator(
              &gen, entry.block, TEST_DEFAULT_DIFFICULTY, params.prev.block.get_height() + 1);
  }
  gen.block_end(entry, params);
  gen.add_block(entry, /*can_be_added_to_blockchain*/ true);
  uint64_t last_good_height = gen.chain_height();

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_has_invalid_tx::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.create_block(entry, params, /*tx_list*/ {});
  entry.block.tx_hashes.push_back({});
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has invalid TX hash");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_is_too_big::generate(std::vector<test_event_entry>& events) const
{
  auto hard_forks = oxen_generate_hard_fork_table(hf::hf9_service_nodes);
  oxen_chain_generator gen(events, hard_forks);
  uint64_t last_good_height = gen.chain_height();

  oxen_blockchain_entry entry = {};
  oxen_create_block_params params = gen.next_block_params();
  gen.block_begin(entry, params, /*tx_list*/ {});
  {
      // Creating a huge miner_tx, it will have a lot of outs
      cryptonote::transaction& miner_tx = *entry.block.miner_tx;
      miner_tx.version = txversion::v2_ringct;
      static const size_t tx_out_count = BLOCK_GRANTED_FULL_REWARD_ZONE_V1 / 2;
      uint64_t amount = miner_tx.vout[0].amount;
      uint64_t portion = amount / tx_out_count;
      uint64_t remainder = amount % tx_out_count;
      txout_target_v target = miner_tx.vout[0].target;
      miner_tx.vout.erase(miner_tx.vout.begin());
      for (size_t i = 0; i < tx_out_count; ++i) {
          tx_out o;
          o.amount = portion;
          o.target = target;
          miner_tx.vout.insert(miner_tx.vout.begin(), o);
      }
      if (0 < remainder) {
          tx_out o;
          o.amount = remainder;
          o.target = target;
          miner_tx.vout.insert(miner_tx.vout.begin(), o);
      }

      fill_nonce_with_oxen_generator(&gen, entry.block, TEST_DEFAULT_DIFFICULTY, entry.block.get_height());
  }
  // Block reward will be incorrect, as it must be reduced if cumulative block size is very big,
  // but in this test it doesn't matter
  gen.block_end(entry, params);
  gen.add_block(entry, /*can_be_added_to_blockchain*/ false, "Block has invalid TX hash");

  // NOTE: Verify block
  oxen_register_callback(
          events,
          "check_block_not_accepted",
          [=]([[maybe_unused]] cryptonote::core& c, [[maybe_unused]] size_t ev_index) {
              DEFINE_TESTS_ERROR_CONTEXT("check_block_not_accepted");
              CHECK_TEST_CONDITION(c.blockchain.get_current_blockchain_height() == last_good_height);
              return true;
          });

  return true;
}

bool gen_block_invalid_binary_format::generate(std::vector<test_event_entry>& events) const
{
#if 1
  auto hard_forks = oxen_generate_hard_fork_table();
  oxen_chain_generator gen(events, hard_forks);

  gen.add_blocks_until_version(hard_forks.back().version);
  gen.add_n_blocks(10);
  gen.add_mined_money_unlock_blocks();

  uint64_t last_valid_height = gen.height();
  cryptonote::transaction tx  = gen.create_and_add_tx(gen.first_miner_, gen.first_miner_.get_keys().m_account_address, MK_COINS(30));
  oxen_blockchain_entry entry = gen.create_next_block({tx});

  serialized_block block(t_serializable_object_to_blob(entry.block));
  // Generate some corrupt blocks
  {
    oxen_blockchain_addable<serialized_block> entry(block, false /*can_be_added_to_blockchain*/, "Corrupt block can't be added to blockchaain");
    serialized_block &corrupt_block = entry.data;
    for (size_t i = 0; i < corrupt_block.data.size() - 1; ++i)
      corrupt_block.data[i] ^= corrupt_block.data[i + 1];
    events.push_back(entry);
  }

  {
    oxen_blockchain_addable<serialized_block> entry(block, false /*can_be_added_to_blockchain*/, "Corrupt block can't be added to blockchaain");
    serialized_block &corrupt_block = entry.data;
    for (size_t i = 0; i < corrupt_block.data.size() - 2; ++i)
      corrupt_block.data[i] ^= corrupt_block.data[i + 2];
    events.push_back(entry);
  }

  {
    oxen_blockchain_addable<serialized_block> entry(block, false /*can_be_added_to_blockchain*/, "Corrupt block can't be added to blockchaain");
    serialized_block &corrupt_block = entry.data;
    for (size_t i = 0; i < corrupt_block.data.size() - 3; ++i)
      corrupt_block.data[i] ^= corrupt_block.data[i + 3];
    events.push_back(entry);
  }

  oxen_register_callback(events, "check_blocks_arent_accepted", [last_valid_height](cryptonote::core &c, size_t ev_index)
  {
    DEFINE_TESTS_ERROR_CONTEXT("check_blocks_arent_accepted");
    CHECK_EQ(c.mempool.get_transactions_count(), 1);
    CHECK_EQ(c.blockchain.get_current_blockchain_height(), last_valid_height + 1);
    return true;
  });

  // TODO(oxen): I don't know why difficulty has to be high for this test? Just generate some blocks and randomize the bytes???
#else
#define BLOCK_VALIDATION_INIT_GENERATE()                                                \
  GENERATE_ACCOUNT(miner_account);                                                      \
  MAKE_GENESIS_BLOCK(events, blk_0, miner_account, 1338224400);

  BLOCK_VALIDATION_INIT_GENERATE();

  std::vector<uint64_t> timestamps;
  std::vector<difficulty_type> cummulative_difficulties;
  difficulty_type cummulative_diff = 1;

  // Unlock blk_0 outputs
  block blk_last = blk_0;
  assert(MINED_MONEY_UNLOCK_WINDOW < DIFFICULTY_WINDOW);
  for (size_t i = 0; i < MINED_MONEY_UNLOCK_WINDOW; ++i)
  {
    MAKE_NEXT_BLOCK(events, blk_curr, blk_last, miner_account);
    timestamps.push_back(blk_curr.timestamp);
    cummulative_difficulties.push_back(++cummulative_diff);
    blk_last = blk_curr;
  }

  // Lifting up takes a while
  difficulty_type diffic;
  do
  {
    blk_last = var::get<block>(events.back());
    diffic = next_difficulty_v2(timestamps, cummulative_difficulties,tools::to_seconds(get_config(cryptonote::network_type::FAKECHAIN).TARGET_BLOCK_TIME), cryptonote::difficulty_calc_mode::normal);
    if (!lift_up_difficulty(events, timestamps, cummulative_difficulties, generator, 1, blk_last, miner_account))
      return false;
    std::cout << "Block #" << events.size() << ", difficulty: " << diffic << std::endl;
  }
  while (diffic < 1500);

  blk_last = var::get<block>(events.back());
  MAKE_TX(events, tx_0, miner_account, miner_account, MK_COINS(30), var::get<block>(events[1]));
  DO_CALLBACK(events, "corrupt_blocks_boundary");

  block blk_test;
  std::vector<crypto::hash> tx_hashes;
  tx_hashes.push_back(get_transaction_hash(tx_0));
  size_t txs_weight = get_transaction_weight(tx_0);
  diffic = next_difficulty_v2(timestamps, cummulative_difficulties,tools::to_seconds(get_config(cryptonote::network_type::FAKECHAIN).TARGET_BLOCK_TIME), cryptonote::difficulty_calc_mode::normal);
  if (!generator.construct_block_manually(blk_test, blk_last, miner_account,
    test_generator::bf_diffic | test_generator::bf_timestamp | test_generator::bf_tx_hashes, 0, 0, blk_last.timestamp,
    crypto::hash(), diffic, transaction(), tx_hashes, txs_weight))
    return false;

  std::string blob = t_serializable_object_to_blob(blk_test);
  for (size_t i = 0; i < blob.size(); ++i)
  {
    for (size_t bit_idx = 0; bit_idx < sizeof(std::string::value_type) * 8; ++bit_idx)
    {
      serialized_block sr_block(blob);
      std::string::value_type& ch = sr_block.data[i];
      ch ^= 1 << bit_idx;

      events.push_back(sr_block);
    }
  }

  DO_CALLBACK(events, "check_all_blocks_purged");
#endif

  return true;
}
