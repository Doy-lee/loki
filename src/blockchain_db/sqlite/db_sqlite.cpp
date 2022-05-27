// Copyright (c) 2021, The Oxen Project
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

#include "db_sqlite.h"

#include <sodium.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <sqlite3.h>

#include <string>

#include <iostream>

#include <cassert>

#include "cryptonote_config.h"
#include "cryptonote_core/blockchain.h"

#include "cryptonote_core/service_node_list.h"

#include "common/string_util.h"

#include "cryptonote_basic/hardfork.h"

#undef OXEN_DEFAULT_LOG_CATEGORY
#define OXEN_DEFAULT_LOG_CATEGORY "blockchain.db.sqlite"

namespace cryptonote {

  BlockchainSQLite::BlockchainSQLite(cryptonote::network_type nettype, fs::path db_path): db::Database(db_path, ""), m_nettype(nettype), filename {db_path.u8string()} {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);
    height = 0;

    if (!db.tableExists("batched_payments_accrued") || !db.tableExists("batched_payments_raw") || !db.tableExists("batch_db_info")) {
      create_schema();
    }

    SQLite::Statement st {
      db,
      "SELECT height FROM batch_db_info"
    };
    while (st.executeStep()) {
      height = st.getColumn(0).getInt64();
    }
  }

  void BlockchainSQLite::create_schema() {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    db.exec(R"(
      CREATE TABLE batched_payments_accrued(
        address VARCHAR NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY(address),
        CHECK(amount >= 0)
      );

      CREATE TRIGGER batch_payments_delete_empty AFTER UPDATE ON batched_payments_accrued FOR EACH ROW WHEN NEW.amount = 0 BEGIN DELETE FROM batched_payments_accrued WHERE address = NEW.address; END;

      CREATE TABLE batched_payments_raw(
        address VARCHAR NOT NULL,
        amount BIGINT NOT NULL,
        height_paid BIGINT NOT NULL,
        PRIMARY KEY(address, height_paid),
        CHECK(amount >= 0)
      );

      CREATE INDEX batched_payments_raw_height_idx ON batched_payments_raw(height_paid);

      CREATE TABLE batch_db_info(
        height BIGINT NOT NULL
      );

      INSERT INTO batch_db_info(height) VALUES(0);

      CREATE TRIGGER batch_payments_prune AFTER UPDATE ON batch_db_info FOR EACH ROW BEGIN DELETE FROM batched_payments_raw WHERE height_paid < (NEW.height - 10000); END;

      CREATE VIEW batched_payments_paid AS SELECT * FROM batched_payments_raw;

      CREATE TRIGGER make_payment INSTEAD OF INSERT ON batched_payments_paid FOR EACH ROW BEGIN UPDATE batched_payments_accrued SET amount = (amount - NEW.amount) WHERE address = NEW.address; SELECT RAISE(ABORT, 'Address not found') WHERE changes() = 0; INSERT INTO batched_payments_raw(address, amount, height_paid) VALUES(NEW.address, NEW.amount, NEW.height_paid); END;

      CREATE TRIGGER rollback_payment INSTEAD OF DELETE ON batched_payments_paid FOR EACH ROW BEGIN DELETE FROM batched_payments_raw WHERE address = OLD.address AND height_paid = OLD.height_paid; INSERT INTO batched_payments_accrued(address, amount) VALUES(OLD.address, OLD.amount) ON CONFLICT(address) DO UPDATE SET amount = (amount + excluded.amount); END;
    )");

    MDEBUG("Database setup complete");
  }

  void BlockchainSQLite::reset_database() {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    db.exec(R"(
      DROP TABLE IF EXISTS batched_payments_accrued;

      DROP VIEW IF EXISTS batched_payments_paid;

      DROP TABLE IF EXISTS batched_payments_raw;

      DROP TABLE IF EXISTS batch_db_info;
    )");

    create_schema();

    MDEBUG("Database reset complete");
  }

  void BlockchainSQLite::update_height(uint64_t new_height) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " Called with new height: " << new_height);
    height = new_height;
    SQLite::Statement update_height {
      db,
      "UPDATE batch_db_info SET height = ?"
    };
    db::exec_query(update_height, static_cast<int64_t>(height));
  }

  void BlockchainSQLite::increment_height() {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " Called with height: " << height + 1);
    update_height(height + 1);
  }

  void BlockchainSQLite::decrement_height() {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " Called with height: " << height - 1);
    update_height(height - 1);
  }

  bool BlockchainSQLite::add_sn_payments(std::vector<cryptonote::batch_sn_payment>& payments) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);
    SQLite::Statement insert_payment {
      db,
      "INSERT INTO batched_payments_accrued (address, amount) VALUES (?, ?) ON CONFLICT (address) DO UPDATE SET amount = amount + excluded.amount"
    };

    for (auto& payment: payments) {
      std::string address_str = cryptonote::get_account_address_as_str(m_nettype, 0, payment.address_info.address);
      MTRACE("Adding record for SN reward contributor " << address_str << "to database with amount " << static_cast<int64_t>(payment.amount));

      db::exec_query(insert_payment, address_str, static_cast<int64_t>(payment.amount * 1000));
      insert_payment.reset();
    };

    return true;
  }

  bool BlockchainSQLite::subtract_sn_payments(std::vector<cryptonote::batch_sn_payment>& payments) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);
    SQLite::Statement update_payment {
      db,
      "UPDATE batched_payments_accrued SET amount = (amount - ?) WHERE address = ?"
    };

    for (auto& payment: payments) {
      std::string address_str = cryptonote::get_account_address_as_str(m_nettype, 0, payment.address_info.address);
      auto result = db::exec_query(update_payment, static_cast<int64_t>(payment.amount * 1000), address_str);
      if (!result) {
        MERROR("tried to subtract payment from an address that doesnt exist: " << address_str);
        return false;
      }
      update_payment.reset();
    };

    return true;
  }

  std::optional<std::vector<cryptonote::batch_sn_payment>> BlockchainSQLite::get_sn_payments(uint64_t block_height) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    if (block_height == 0)
      return std::nullopt;

    const auto& conf = get_config(m_nettype);

    SQLite::Statement select_payments {
      db,
      "SELECT address, amount FROM batched_payments_accrued WHERE amount > ? ORDER BY address ASC"
    };

    select_payments.bind(1, static_cast<int64_t>(conf.MIN_BATCH_PAYMENT_AMOUNT * 1000));

    std::vector<cryptonote::batch_sn_payment> payments;

    std::string address;
    uint64_t amount;
    while (select_payments.executeStep()) {
      address = select_payments.getColumn(0).getString();
      amount = static_cast<uint64_t>(select_payments.getColumn(1).getInt64() / 1000);
      if (cryptonote::is_valid_address(address, m_nettype)) {
        cryptonote::address_parse_info addr_info {};
        cryptonote::get_account_address_from_str(addr_info, m_nettype, address);
        uint64_t next_payout_height = addr_info.address.next_payout_height(block_height - 1, conf.BATCHING_INTERVAL);
        if (block_height == next_payout_height) {
          payments.emplace_back(address, amount, m_nettype);
        }
      } else {
        MERROR("Invalid address returned from batching database: " << address);
        return std::nullopt;
      }
    }

    return payments;
  }


  uint64_t BlockchainSQLite::get_accrued_earnings(const std::string& address) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    SQLite::Statement select_earnings {
      db,
      "SELECT amount FROM batched_payments_accrued WHERE address = ?;"
    };
    select_earnings.bind(1, address);

    uint64_t amount{};
    while (select_earnings.executeStep()) {
      amount = static_cast<uint64_t>(select_earnings.getColumn(0).getInt64() / 1000);
    }

    return amount;
  }

  std::pair<std::vector<std::string>, std::vector<uint64_t>> BlockchainSQLite::get_all_accrued_earnings() {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    SQLite::Statement select_earnings {
      db,
      "SELECT address, amount FROM batched_payments_accrued;"
    };

    std::vector<uint64_t> amounts;
    std::vector<std::string> addresses;
    while (select_earnings.executeStep()) {
      addresses.emplace_back(select_earnings.getColumn(0).getString());
      amounts.emplace_back(static_cast<uint64_t>(select_earnings.getColumn(1).getInt64() / 1000));
    }

    return std::make_pair(addresses, amounts);
  }


  std::vector<cryptonote::batch_sn_payment> BlockchainSQLite::calculate_rewards(hf hf_version, uint64_t distribution_amount, service_nodes::service_node_info sn_info) {

    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    // Find out how much is due for the operator
    uint64_t operator_fee = 0;
    {
      // This calculates the operator fee using (operator_portion / max_operator_portion) * distribution_amount but using 128 bit integer math
      uint64_t hi, lo, resulthi, resultlo;
      lo = mul128(sn_info.portions_for_operator, distribution_amount, &hi);
      div128_64(hi, lo, old::STAKING_PORTIONS, &resulthi, &resultlo);
      if (resulthi > 0)
        throw std::logic_error("overflow from calculating sn operator fee");
      operator_fee = resultlo;
    }
    std::vector<cryptonote::batch_sn_payment> payments;
    // Pay the operator fee to the operator
    if (operator_fee > 0)
      payments.emplace_back(sn_info.operator_address, operator_fee, m_nettype);

    // Pay the balance to all the contributors (including the operator again)
    uint64_t total_contributed_to_sn = std::accumulate(sn_info.contributors.begin(), sn_info.contributors.end(), uint64_t(0), [](auto
      const a, auto
      const b) {
      return a + b.amount;
    });

    for (auto& contributor: sn_info.contributors) {
      // This calculates (contributor.amount / total_contributed_to_winner_sn) * (distribution_amount - operator_fee) but using 128 bit integer math
      uint64_t hi, lo, resulthi, resultlo;
      lo = mul128(contributor.amount, distribution_amount - operator_fee, &hi);
      div128_64(hi, lo, total_contributed_to_sn, &resulthi, &resultlo);
      if (resulthi > 0)
        throw std::logic_error("overflow from calculating sn contributor reward");
      if (resultlo > 0)
        payments.emplace_back(contributor.address, resultlo, m_nettype);
    }

    return payments;
  }

  bool BlockchainSQLite::add_block(const cryptonote::block& block,
    const service_nodes::service_node_list::state_t& service_nodes_state) {
    auto block_height = get_block_height(block);
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " called on height: " << block_height);

    auto hf_version = block.major_version;
    if (hf_version < hf::hf19) {
      update_height(block_height);
      print_database();
      return true;
    }

    auto fork_height = cryptonote::get_hard_fork_heights(m_nettype, hf::hf19);
    if (block_height == fork_height.first.value_or(0)) {
      MDEBUG("Batching of Service Node Rewards Begins");
      reset_database();
      update_height(block_height - 1);
    }

    if (block_height != height + 1) {
      MERROR("Block height out of sync with batching database. Block height: " << block_height << " batching db height: " << height);
      return false;
    }

    // We query our own database as a source of truth to verify the blocks payments against. The calculated_rewards
    // variable contains a known good list of who should have been paid in this block 
    auto calculated_rewards = get_sn_payments(block_height);

    // We iterate through the block's coinbase payments and build a copy of our own list of the payments
    // miner_tx_vouts this will be compared against calculated_rewards and if they match we know the block is
    // paying the correct people only.
    std::vector<std::tuple<crypto::public_key, uint64_t>> miner_tx_vouts;
    for (auto & vout: block.miner_tx.vout)
      miner_tx_vouts.emplace_back(var::get<txout_to_key>(vout.target).key, vout.amount);

    try {
      SQLite::Transaction transaction {
        db,
        SQLite::TransactionBehavior::IMMEDIATE
      };

      // Goes through the miner transactions vouts checks they are right and marks them as paid in the database
      if (!validate_batch_payment(miner_tx_vouts, * calculated_rewards, block_height)) {
        return false;
      }

      // Step 1: Pay out the block producer their fees
      uint64_t service_node_reward = cryptonote::service_node_reward_formula(0, block.major_version);
      if (block.reward - service_node_reward > 0) {
        if (block.service_node_winner_key // "service_node_winner_key" tracks the pulse winner; 0 if a mined block
          &&
          crypto_core_ed25519_is_valid_point(reinterpret_cast <
            const unsigned char * > (block.service_node_winner_key.data))) {
          auto service_node_winner = service_nodes_state.service_nodes_infos.find(block.service_node_winner_key);
          if (service_node_winner != service_nodes_state.service_nodes_infos.end()) {
            std::vector<cryptonote::batch_sn_payment> block_producer_fee_payments = calculate_rewards(block.major_version, block.reward - service_node_reward, * service_node_winner -> second);
            // Takes the block producer and adds its contributors to the batching database for the transaction fees
            if (!add_sn_payments(block_producer_fee_payments))
              return false;
          }
        }
      }

      // Step 2: Iterate over the whole service node list and pay each node 1/service_node_list fraction
      const auto payable_service_nodes = service_nodes_state.payable_service_nodes_infos(block_height, m_nettype);
      size_t total_service_nodes_payable = payable_service_nodes.size();
      for (const auto& [node_pubkey, node_info]: payable_service_nodes) {
        std::vector<cryptonote::batch_sn_payment> node_contributors;
        auto payable_service_node = service_nodes_state.service_nodes_infos.find(node_pubkey);
        if (payable_service_node == service_nodes_state.service_nodes_infos.end())
          continue;
        std::vector<cryptonote::batch_sn_payment> node_rewards = calculate_rewards(block.major_version, service_node_reward / total_service_nodes_payable, * payable_service_node -> second);
        // Takes the node and adds its contributors to the batching database
        if (!add_sn_payments(node_rewards))
          return false;
      }

      // Step 3: Add Governance reward to the list
      if (m_nettype != cryptonote::network_type::FAKECHAIN) {
        std::vector<cryptonote::batch_sn_payment> governance_rewards;
        cryptonote::address_parse_info governance_wallet_address;
        cryptonote::get_account_address_from_str(governance_wallet_address, m_nettype, cryptonote::get_config(m_nettype).governance_wallet_address(hf_version));
        uint64_t foundation_reward = cryptonote::governance_reward_formula(hf_version);
        governance_rewards.emplace_back(governance_wallet_address.address, foundation_reward, m_nettype);
        if (!add_sn_payments(governance_rewards))
          return false;
      }
      increment_height();

      transaction.commit();
    } catch (std::exception& e) {
      MFATAL("Exception: " << e.what());
      return false;
    }
    print_database();
    return true;
  }

  bool BlockchainSQLite::pop_block(const cryptonote::block& block,
    const service_nodes::service_node_list::state_t& service_nodes_state) {
    auto block_height = get_block_height(block);

    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " called on height: " << block_height);
    if (height < block_height) {
      MDEBUG("Block above batching DB height skipping pop");
      return true;
    }
    if (block_height != height) {
      MERROR("Block height out of sync with batching database");
      return false;
    }

    const auto& conf = get_config(m_nettype);
    auto hf_version = hf{block.major_version};
    if (hf_version < hf::hf19) {
      decrement_height();
      return true;
    }

    try {
      SQLite::Transaction transaction {
        db,
        SQLite::TransactionBehavior::IMMEDIATE
      };

      // Step 1: Remove the block producers txn fees
      uint64_t service_node_reward = cryptonote::service_node_reward_formula(0, block.major_version);
      if (block.reward - service_node_reward > 0) // If true then we have tx fees to roll back
      {
        if (block.service_node_winner_key // "service_node_winner_key" tracks the pulse winner; 0 if a mined block
          &&
          crypto_core_ed25519_is_valid_point(reinterpret_cast <
            const unsigned char * > (block.service_node_winner_key.data))) {
          auto service_node_winner = service_nodes_state.service_nodes_infos.find(block.service_node_winner_key);
          if (service_node_winner != service_nodes_state.service_nodes_infos.end()) {
            std::vector<cryptonote::batch_sn_payment> block_producer_fee_payments = calculate_rewards(block.major_version, block.reward - service_node_reward, * service_node_winner -> second);
            // Takes the block producer and adds its contributors to the batching database for the transaction fees
            if (!subtract_sn_payments(block_producer_fee_payments))
              return false;
          }
        }
      }

      // Step 2: Iterate over the whole service node list and subtract each node 1/service_node_list fraction
      const auto payable_service_nodes = service_nodes_state.payable_service_nodes_infos(block_height, m_nettype);
      size_t total_service_nodes_payable = payable_service_nodes.size();
      for (const auto& [node_pubkey, node_info]: payable_service_nodes) {
        std::vector<cryptonote::batch_sn_payment> node_contributors;
        auto payable_service_node = service_nodes_state.service_nodes_infos.find(node_pubkey);
        if (payable_service_node == service_nodes_state.service_nodes_infos.end())
          continue;
        std::vector<cryptonote::batch_sn_payment> node_rewards = calculate_rewards(block.major_version, service_node_reward / total_service_nodes_payable, * payable_service_node -> second);
        // Takes the node and adds its contributors to the batching database

        if (!subtract_sn_payments(node_rewards))
          return false;
      }
      // Step 3: Remove Governance reward
      if (m_nettype != cryptonote::network_type::FAKECHAIN) {
        std::vector<cryptonote::batch_sn_payment> governance_rewards;
        cryptonote::address_parse_info governance_wallet_address;
        cryptonote::get_account_address_from_str(governance_wallet_address, m_nettype, cryptonote::get_config(m_nettype).governance_wallet_address(hf_version));
        uint64_t foundation_reward = cryptonote::governance_reward_formula(hf_version);
        governance_rewards.emplace_back(governance_wallet_address.address, foundation_reward, m_nettype);
        if (!subtract_sn_payments(governance_rewards))
          return false;
      }

      // Add back to the database payments that had been made in this block
      delete_block_payments(block_height);

      decrement_height();
      transaction.commit();
    } catch (std::exception& e) {
      MFATAL("Exception: " << e.what());
      return false;
    }
    return true;
  }

  bool BlockchainSQLite::validate_batch_payment(std::vector<std::tuple<crypto::public_key, uint64_t>> miner_tx_vouts, std::vector<cryptonote::batch_sn_payment> calculated_payments_from_batching_db, uint64_t block_height) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);
    size_t length_miner_tx_vouts = miner_tx_vouts.size();
    size_t length_calculated_payments_from_batching_db = calculated_payments_from_batching_db.size();

    if (length_miner_tx_vouts != length_calculated_payments_from_batching_db) {
      MERROR("Length of batch paments does not match, block vouts: " << length_miner_tx_vouts << " batch size: " << length_calculated_payments_from_batching_db);
      return false;
    }

    int8_t vout_index = 0;
    uint64_t total_oxen_payout_in_our_db = std::accumulate(calculated_payments_from_batching_db.begin(), calculated_payments_from_batching_db.end(), uint64_t(0), [](auto
      const a, auto
      const b) {
      return a + b.amount;
    });
    uint64_t total_oxen_payout_in_vouts = 0;
    std::vector<batch_sn_payment> finalised_payments;
    cryptonote::keypair
    const deterministic_keypair = cryptonote::get_deterministic_keypair_from_height(block_height);
    for (auto& vout: miner_tx_vouts) {
      if (std::get<1>(vout) != calculated_payments_from_batching_db[vout_index].amount) {
        MERROR("Service node reward amount incorrect. Should be " << cryptonote::print_money(calculated_payments_from_batching_db[vout_index].amount) << ", is: " << cryptonote::print_money(std::get<1>(vout)));
        return false;
      }
      crypto::public_key out_eph_public_key {};
      if (!cryptonote::get_deterministic_output_key(calculated_payments_from_batching_db[vout_index].address_info.address, deterministic_keypair, vout_index, out_eph_public_key)) {
        MERROR("Failed to generate output one-time public key");
        return false;
      }
      if (tools::view_guts(std::get<0>(vout)) != tools::view_guts(out_eph_public_key)) {
        MERROR("Output ephemeral public key does not match");
        return false;
      }
      total_oxen_payout_in_vouts += std::get<1>(vout);
      finalised_payments.emplace_back(calculated_payments_from_batching_db[vout_index].address, std::get<1>(vout), m_nettype);
      vout_index++;
    }
    if (total_oxen_payout_in_vouts != total_oxen_payout_in_our_db) {
      MERROR("Total service node reward amount incorrect. Should be " << cryptonote::print_money(total_oxen_payout_in_our_db) << ", is: " << cryptonote::print_money(total_oxen_payout_in_vouts));
      return false;
    }

    return save_payments(block_height, finalised_payments);
  }

  bool BlockchainSQLite::save_payments(uint64_t block_height, std::vector<batch_sn_payment>paid_amounts) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);

    SQLite::Statement select_sum {
      db,
      "SELECT amount from batched_payments_accrued WHERE address = ?;"
    };

    SQLite::Statement update_paid {
      db,
      "INSERT INTO batched_payments_paid (address, amount, height_paid) VALUES (?,?,?);"
    };

    for (const auto& payment: paid_amounts) {
      select_sum.bind(1, payment.address);
      while (select_sum.executeStep()) {
        uint64_t amount = static_cast<uint64_t>(select_sum.getColumn(0).getInt64());
        if (amount != payment.amount * 1000) {
          MERROR("Invalid amounts passed in to save payments for address: " << payment.address << " received " << payment.amount << " expected " << amount);
          return false;
        }

        db::exec_query(update_paid, payment.address, static_cast<int64_t>(amount), static_cast<int64_t>(block_height));
        update_paid.reset();
      }
      select_sum.reset();
    };
    return true;
  }

  std::vector<cryptonote::batch_sn_payment> BlockchainSQLite::get_block_payments(uint64_t block_height) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " Called with height: " << block_height);

    std::vector<cryptonote::batch_sn_payment> payments_at_height;
    SQLite::Statement st {
      db,
      "SELECT address, amount FROM batched_payments_paid WHERE height_paid = ? ORDER BY address"
    };
    st.bind(1, static_cast<int64_t>(block_height));
    while (st.executeStep()) {
      payments_at_height.emplace_back(st.getColumn(0).getString(), st.getColumn(1).getInt64(), m_nettype);
    }
    return payments_at_height;
  }

  bool BlockchainSQLite::delete_block_payments(uint64_t block_height) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " Called with height: " << block_height);
    SQLite::Statement delete_payments {
      db,
      "DELETE FROM batched_payments_paid WHERE height_paid >= ?"
    };
    db::exec_query(delete_payments, static_cast<int64_t>(block_height));
    return true;
  }

  void BlockchainSQLite::print_database()
  {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__ << " Called with height: " << height);
    LOG_PRINT_L3("Print Database called with height: " << height);
    SQLite::Statement st{db, "SELECT address, amount FROM batched_payments_accrued ORDER BY address ASC"};
    while (st.executeStep()) {
      LOG_PRINT_L3(" Address: " << st.getColumn(0).getString() << " has amount: " << st.getColumn(1).getString() << " in the database");
    }
  }

  fs::path check_if_copy_filename(std::string_view db_path) {
    return (db_path != ":memory:") ? fs::path(std::string(db_path) + "-copy") : fs::path(std::string(db_path));
  }

  BlockchainSQLiteTest::BlockchainSQLiteTest(cryptonote::network_type nettype, fs::path db_path): BlockchainSQLite(nettype, db_path) {};

  BlockchainSQLiteTest::BlockchainSQLiteTest(BlockchainSQLiteTest& other): BlockchainSQLiteTest(other.m_nettype, check_if_copy_filename(other.filename)) {
    std::vector<std::tuple<std::string, int64_t>> all_payments_accrued;
    SQLite::Statement st {
      other.db, "SELECT address, amount FROM batched_payments_accrued"
    };
    while (st.executeStep())
      all_payments_accrued.emplace_back(st.getColumn(0).getString(), st.getColumn(1).getInt64());
    std::vector<std::tuple<std::string, int64_t, int64_t>> all_payments_paid;
    SQLite::Statement st2 {
      other.db, "SELECT address, amount, height_paid FROM batched_payments_raw"
    };
    while (st2.executeStep())
      all_payments_paid.emplace_back(st2.getColumn(0).getString(), st2.getColumn(1).getInt64(), st2.getColumn(2).getInt64());

    SQLite::Transaction transaction {
      db,
      SQLite::TransactionBehavior::IMMEDIATE
    };

    SQLite::Statement insert_payment_paid {
      db,
      "INSERT INTO batched_payments_raw (address, amount, height_paid) VALUES (?, ?, ?)"
    };

    for (auto& [address, amount, height_paid]: all_payments_paid) {
      db::exec_query(insert_payment_paid, address, amount, height_paid);
      insert_payment_paid.reset();
    };

    SQLite::Statement insert_payment_accrued {
      db,
      "INSERT INTO batched_payments_accrued (address, amount) VALUES (?, ?)"
    };

    for (auto& [address, amount]: all_payments_accrued) {
      db::exec_query(insert_payment_accrued, address, amount);
      insert_payment_accrued.reset();
    };

    transaction.commit();

    update_height(other.height);
  }

  uint64_t BlockchainSQLiteTest::batching_count() {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);
    SQLite::Statement st {
      db,
      "SELECT count(*) FROM batched_payments_accrued"
    };
    uint64_t count = 0;
    while (st.executeStep()) {
      count = st.getColumn(0).getInt64();
    }
    return count;
  }

  std::optional<uint64_t> BlockchainSQLiteTest::retrieve_amount_by_address(const std::string& address) {
    LOG_PRINT_L3("BlockchainDB_SQLITE::" << __func__);
    SQLite::Statement st {
      db,
      "SELECT amount FROM batched_payments_accrued WHERE address = ?"
    };
    st.bind(1, address);
    std::optional<uint64_t> amount = std::nullopt;
    while (st.executeStep()) {
      assert(!amount);
      amount.emplace(st.getColumn(0).getInt64());
    }
    return amount;
  }

} // namespace cryptonote