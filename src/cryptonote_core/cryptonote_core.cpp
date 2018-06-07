// Copyright (c) 2014-2018, The Monero Project
// Copyright (c)      2018, The Loki Project
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

#include <boost/algorithm/string.hpp>

#include "include_base_utils.h"
#include "string_tools.h"
using namespace epee;

#include <unordered_set>
#include <random>

#include "cryptonote_core.h"
#include "common/command_line.h"
#include "common/util.h"
#include "common/updates.h"
#include "common/download.h"
#include "common/threadpool.h"
#include "common/command_line.h"
#include "warnings.h"
#include "crypto/crypto.h"
#include "cryptonote_config.h"
#include "cryptonote_tx_utils.h"
#include "misc_language.h"
#include "file_io_utils.h"
#include <csignal>
#include "checkpoints/checkpoints.h"
#include "ringct/rctTypes.h"
#include "blockchain_db/blockchain_db.h"
#include "ringct/rctSigs.h"
#include "version.h"

#undef LOKI_DEFAULT_LOG_CATEGORY
#define LOKI_DEFAULT_LOG_CATEGORY "cn"

DISABLE_VS_WARNINGS(4355)

#define MERROR_VER(x) MCERROR("verify", x)

#define BAD_SEMANTICS_TXES_MAX_SIZE 100

namespace cryptonote
{
  const command_line::arg_descriptor<bool, false> arg_testnet_on  = {
    "testnet"
  , "Run on testnet. The wallet must be launched with --testnet flag."
  , false
  };
  const command_line::arg_descriptor<bool, false> arg_stagenet_on  = {
    "stagenet"
  , "Run on stagenet. The wallet must be launched with --stagenet flag."
  , false
  };
  const command_line::arg_descriptor<std::string, false, true, 2> arg_data_dir = {
    "data-dir"
  , "Specify data directory"
  , tools::get_default_data_dir()
  , {{ &arg_testnet_on, &arg_stagenet_on }}
  , [](std::array<bool, 2> testnet_stagenet, bool defaulted, std::string val)->std::string {
      if (testnet_stagenet[0])
        return (boost::filesystem::path(val) / "testnet").string();
      else if (testnet_stagenet[1])
        return (boost::filesystem::path(val) / "stagenet").string();
      return val;
    }
  };
  const command_line::arg_descriptor<bool> arg_offline = {
    "offline"
  , "Do not listen for peers, nor connect to any"
  };
  const command_line::arg_descriptor<bool> arg_disable_dns_checkpoints = {
    "disable-dns-checkpoints"
  , "Do not retrieve checkpoints from DNS"
  };

  static const command_line::arg_descriptor<bool> arg_test_drop_download = {
    "test-drop-download"
  , "For net tests: in download, discard ALL blocks instead checking/saving them (very fast)"
  };
  static const command_line::arg_descriptor<uint64_t> arg_test_drop_download_height = {
    "test-drop-download-height"
  , "Like test-drop-download but discards only after around certain height"
  , 0
  };
  static const command_line::arg_descriptor<int> arg_test_dbg_lock_sleep = {
    "test-dbg-lock-sleep"
  , "Sleep time in ms, defaults to 0 (off), used to debug before/after locking mutex. Values 100 to 1000 are good for tests."
  , 0
  };
  static const command_line::arg_descriptor<bool> arg_dns_checkpoints  = {
    "enforce-dns-checkpointing"
  , "checkpoints from DNS server will be enforced"
  , false
  };
  static const command_line::arg_descriptor<uint64_t> arg_fast_block_sync = {
    "fast-block-sync"
  , "Sync up most of the way by using embedded, known block hashes."
  , 1
  };
  static const command_line::arg_descriptor<uint64_t> arg_prep_blocks_threads = {
    "prep-blocks-threads"
  , "Max number of threads to use when preparing block hashes in groups."
  , 4
  };
  static const command_line::arg_descriptor<uint64_t> arg_show_time_stats  = {
    "show-time-stats"
  , "Show time-stats when processing blocks/txs and disk synchronization."
  , 0
  };
  static const command_line::arg_descriptor<size_t> arg_block_sync_size  = {
    "block-sync-size"
  , "How many blocks to sync at once during chain synchronization (0 = adaptive)."
  , 0
  };
  static const command_line::arg_descriptor<std::string> arg_check_updates = {
    "check-updates"
  , "Check for new versions of loki: [disabled|notify|download|update]"
  , "notify"
  };
  static const command_line::arg_descriptor<bool> arg_fluffy_blocks  = {
    "fluffy-blocks"
  , "Relay blocks as fluffy blocks (obsolete, now default)"
  , true
  };
  static const command_line::arg_descriptor<bool> arg_no_fluffy_blocks  = {
    "no-fluffy-blocks"
  , "Relay blocks as normal blocks"
  , false
  };
  static const command_line::arg_descriptor<size_t> arg_max_txpool_size  = {
    "max-txpool-size"
  , "Set maximum txpool size in bytes."
  , DEFAULT_TXPOOL_MAX_SIZE
  };

  //-----------------------------------------------------------------------------------------------
  core::core(i_cryptonote_protocol* pprotocol):
              m_mempool(m_blockchain_storage),
              m_blockchain_storage(m_mempool),
              m_miner(this),
              m_miner_address(boost::value_initialized<account_public_address>()),
              m_starter_message_showed(false),
              m_target_blockchain_height(0),
              m_checkpoints_path(""),
              m_last_dns_checkpoints_update(0),
              m_last_json_checkpoints_update(0),
              m_disable_dns_checkpoints(false),
              m_threadpool(tools::threadpool::getInstance()),
              m_update_download(0),
              m_nettype(UNDEFINED)
  {
    m_checkpoints_updating.clear();
    set_cryptonote_protocol(pprotocol);
  }
  void core::set_cryptonote_protocol(i_cryptonote_protocol* pprotocol)
  {
    if(pprotocol)
      m_pprotocol = pprotocol;
    else
      m_pprotocol = &m_protocol_stub;
  }
  //-----------------------------------------------------------------------------------
  void core::set_checkpoints(checkpoints&& chk_pts)
  {
    m_blockchain_storage.set_checkpoints(std::move(chk_pts));
  }
  //-----------------------------------------------------------------------------------
  void core::set_checkpoints_file_path(const std::string& path)
  {
    m_checkpoints_path = path;
  }
  //-----------------------------------------------------------------------------------
  void core::set_enforce_dns_checkpoints(bool enforce_dns)
  {
    m_blockchain_storage.set_enforce_dns_checkpoints(enforce_dns);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::update_checkpoints()
  {
    if (m_nettype != MAINNET || m_disable_dns_checkpoints) return true;

    if (m_checkpoints_updating.test_and_set()) return true;

    bool res = true;
    if (time(NULL) - m_last_dns_checkpoints_update >= 3600)
    {
      res = m_blockchain_storage.update_checkpoints(m_checkpoints_path, true);
      m_last_dns_checkpoints_update = time(NULL);
      m_last_json_checkpoints_update = time(NULL);
    }
    else if (time(NULL) - m_last_json_checkpoints_update >= 600)
    {
      res = m_blockchain_storage.update_checkpoints(m_checkpoints_path, false);
      m_last_json_checkpoints_update = time(NULL);
    }

    m_checkpoints_updating.clear();

    // if anything fishy happened getting new checkpoints, bring down the house
    if (!res)
    {
      graceful_exit();
    }
    return res;
  }
  //-----------------------------------------------------------------------------------
  void core::stop()
  {
    m_blockchain_storage.cancel();

    tools::download_async_handle handle;
    {
      boost::lock_guard<boost::mutex> lock(m_update_mutex);
      handle = m_update_download;
      m_update_download = 0;
    }
    if (handle)
      tools::download_cancel(handle);
  }
  //-----------------------------------------------------------------------------------
  void core::init_options(boost::program_options::options_description& desc)
  {
    command_line::add_arg(desc, arg_data_dir);

    command_line::add_arg(desc, arg_test_drop_download);
    command_line::add_arg(desc, arg_test_drop_download_height);

    command_line::add_arg(desc, arg_testnet_on);
    command_line::add_arg(desc, arg_stagenet_on);
    command_line::add_arg(desc, arg_dns_checkpoints);
    command_line::add_arg(desc, arg_prep_blocks_threads);
    command_line::add_arg(desc, arg_fast_block_sync);
    command_line::add_arg(desc, arg_show_time_stats);
    command_line::add_arg(desc, arg_block_sync_size);
    command_line::add_arg(desc, arg_check_updates);
    command_line::add_arg(desc, arg_fluffy_blocks);
    command_line::add_arg(desc, arg_no_fluffy_blocks);
    command_line::add_arg(desc, arg_test_dbg_lock_sleep);
    command_line::add_arg(desc, arg_offline);
    command_line::add_arg(desc, arg_disable_dns_checkpoints);
    command_line::add_arg(desc, arg_max_txpool_size);

    miner::init_options(desc);
    BlockchainDB::init_options(desc);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_command_line(const boost::program_options::variables_map& vm)
  {
    if (m_nettype != FAKECHAIN)
    {
      const bool testnet = command_line::get_arg(vm, arg_testnet_on);
      const bool stagenet = command_line::get_arg(vm, arg_stagenet_on);
      m_nettype = testnet ? TESTNET : stagenet ? STAGENET : MAINNET;
    }

    m_config_folder = command_line::get_arg(vm, arg_data_dir);

    auto data_dir = boost::filesystem::path(m_config_folder);

    if (m_nettype == MAINNET)
    {
      cryptonote::checkpoints checkpoints;
      if (!checkpoints.init_default_checkpoints(m_nettype))
      {
        throw std::runtime_error("Failed to initialize checkpoints");
      }
      set_checkpoints(std::move(checkpoints));

      boost::filesystem::path json(JSON_HASH_FILE_NAME);
      boost::filesystem::path checkpoint_json_hashfile_fullpath = data_dir / json;

      set_checkpoints_file_path(checkpoint_json_hashfile_fullpath.string());
    }


    set_enforce_dns_checkpoints(command_line::get_arg(vm, arg_dns_checkpoints));
    test_drop_download_height(command_line::get_arg(vm, arg_test_drop_download_height));
    m_fluffy_blocks_enabled = !get_arg(vm, arg_no_fluffy_blocks);
    m_offline = get_arg(vm, arg_offline);
    m_disable_dns_checkpoints = get_arg(vm, arg_disable_dns_checkpoints);
    if (!command_line::is_arg_defaulted(vm, arg_fluffy_blocks))
      MWARNING(arg_fluffy_blocks.name << " is obsolete, it is now default");

    if (command_line::get_arg(vm, arg_test_drop_download) == true)
      test_drop_download();

    epee::debug::g_test_dbg_lock_sleep() = command_line::get_arg(vm, arg_test_dbg_lock_sleep);

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_current_blockchain_height() const
  {
    return m_blockchain_storage.get_current_blockchain_height();
  }
  //-----------------------------------------------------------------------------------------------
  void core::get_blockchain_top(uint64_t& height, crypto::hash& top_id) const
  {
    top_id = m_blockchain_storage.get_tail_id(height);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_blocks(uint64_t start_offset, size_t count, std::list<std::pair<cryptonote::blobdata,block>>& blocks, std::list<cryptonote::blobdata>& txs) const
  {
    return m_blockchain_storage.get_blocks(start_offset, count, blocks, txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_blocks(uint64_t start_offset, size_t count, std::list<std::pair<cryptonote::blobdata,block>>& blocks) const
  {
    return m_blockchain_storage.get_blocks(start_offset, count, blocks);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_blocks(uint64_t start_offset, size_t count, std::list<block>& blocks) const
  {
    std::list<std::pair<cryptonote::blobdata, cryptonote::block>> bs;
    if (!m_blockchain_storage.get_blocks(start_offset, count, bs))
      return false;
    for (const auto &b: bs)
      blocks.push_back(b.second);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_transactions(const std::vector<crypto::hash>& txs_ids, std::list<cryptonote::blobdata>& txs, std::list<crypto::hash>& missed_txs) const
  {
    return m_blockchain_storage.get_transactions_blobs(txs_ids, txs, missed_txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_txpool_backlog(std::vector<tx_backlog_entry>& backlog) const
  {
    m_mempool.get_transaction_backlog(backlog);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_transactions(const std::vector<crypto::hash>& txs_ids, std::list<transaction>& txs, std::list<crypto::hash>& missed_txs) const
  {
    return m_blockchain_storage.get_transactions(txs_ids, txs, missed_txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_alternative_blocks(std::list<block>& blocks) const
  {
    return m_blockchain_storage.get_alternative_blocks(blocks);
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_alternative_blocks_count() const
  {
    return m_blockchain_storage.get_alternative_blocks_count();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::init(const boost::program_options::variables_map& vm, const char *config_subdir, const cryptonote::test_options *test_options)
  {
    start_time = std::time(nullptr);

    if (test_options != NULL)
    {
      m_nettype = FAKECHAIN;
    }
    bool r = handle_command_line(vm);
    std::string m_config_folder_mempool = m_config_folder;

    if (config_subdir)
      m_config_folder_mempool = m_config_folder_mempool + "/" + config_subdir;

    std::string db_type = command_line::get_arg(vm, cryptonote::arg_db_type);
    std::string db_sync_mode = command_line::get_arg(vm, cryptonote::arg_db_sync_mode);
    bool db_salvage = command_line::get_arg(vm, cryptonote::arg_db_salvage) != 0;
    bool fast_sync = command_line::get_arg(vm, arg_fast_block_sync) != 0;
    uint64_t blocks_threads = command_line::get_arg(vm, arg_prep_blocks_threads);
    std::string check_updates_string = command_line::get_arg(vm, arg_check_updates);
    size_t max_txpool_size = command_line::get_arg(vm, arg_max_txpool_size);

    boost::filesystem::path folder(m_config_folder);
    if (m_nettype == FAKECHAIN)
      folder /= "fake";

    // make sure the data directory exists, and try to lock it
    CHECK_AND_ASSERT_MES (boost::filesystem::exists(folder) || boost::filesystem::create_directories(folder), false,
      std::string("Failed to create directory ").append(folder.string()).c_str());

    // check for blockchain.bin
    try
    {
      const boost::filesystem::path old_files = folder;
      if (boost::filesystem::exists(old_files / "blockchain.bin"))
      {
        MWARNING("Found old-style blockchain.bin in " << old_files.string());
        MWARNING("Loki now uses a new format. You can either remove blockchain.bin to start syncing");
        MWARNING("the blockchain anew, or use loki-blockchain-export and loki-blockchain-import to");
        MWARNING("convert your existing blockchain.bin to the new format. See README.md for instructions.");
        return false;
      }
    }
    // folder might not be a directory, etc, etc
    catch (...) { }

    std::unique_ptr<BlockchainDB> db(new_db(db_type));
    if (db == NULL)
    {
      LOG_ERROR("Attempted to use non-existent database type");
      return false;
    }

    folder /= db->get_db_name();
    MGINFO("Loading blockchain from folder " << folder.string() << " ...");

    const std::string filename = folder.string();
    // default to fast:async:1
    blockchain_db_sync_mode sync_mode = db_defaultsync;
    uint64_t blocks_per_sync = 1;

    try
    {
      uint64_t db_flags = 0;

      std::vector<std::string> options;
      boost::trim(db_sync_mode);
      boost::split(options, db_sync_mode, boost::is_any_of(" :"));
      const bool db_sync_mode_is_default = command_line::is_arg_defaulted(vm, cryptonote::arg_db_sync_mode);

      for(const auto &option : options)
        MDEBUG("option: " << option);

      // default to fast:async:1
      uint64_t DEFAULT_FLAGS = DBF_FAST;

      if(options.size() == 0)
      {
        // default to fast:async:1
        db_flags = DEFAULT_FLAGS;
      }

      bool safemode = false;
      if(options.size() >= 1)
      {
        if(options[0] == "safe")
        {
          safemode = true;
          db_flags = DBF_SAFE;
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_nosync;
        }
        else if(options[0] == "fast")
        {
          db_flags = DBF_FAST;
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_async;
        }
        else if(options[0] == "fastest")
        {
          db_flags = DBF_FASTEST;
          blocks_per_sync = 1000; // default to fastest:async:1000
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_async;
        }
        else
          db_flags = DEFAULT_FLAGS;
      }

      if(options.size() >= 2 && !safemode)
      {
        if(options[1] == "sync")
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_sync;
        else if(options[1] == "async")
          sync_mode = db_sync_mode_is_default ? db_defaultsync : db_async;
      }

      if(options.size() >= 3 && !safemode)
      {
        char *endptr;
        uint64_t bps = strtoull(options[2].c_str(), &endptr, 0);
        if (*endptr == '\0')
          blocks_per_sync = bps;
      }

      if (db_salvage)
        db_flags |= DBF_SALVAGE;

      db->open(filename, db_flags);
      if(!db->m_open)
        return false;
    }
    catch (const DB_ERROR& e)
    {
      LOG_ERROR("Error opening database: " << e.what());
      return false;
    }

    m_blockchain_storage.set_user_options(blocks_threads,
        blocks_per_sync, sync_mode, fast_sync);

    r = m_blockchain_storage.init(db.release(), m_nettype, m_offline, test_options);

    r = m_mempool.init(max_txpool_size);
    CHECK_AND_ASSERT_MES(r, false, "Failed to initialize memory pool");

    // now that we have a valid m_blockchain_storage, we can clean out any
    // transactions in the pool that do not conform to the current fork
    m_mempool.validate(m_blockchain_storage.get_current_hard_fork_version());

    bool show_time_stats = command_line::get_arg(vm, arg_show_time_stats) != 0;
    m_blockchain_storage.set_show_time_stats(show_time_stats);
    CHECK_AND_ASSERT_MES(r, false, "Failed to initialize blockchain storage");

    block_sync_size = command_line::get_arg(vm, arg_block_sync_size);

    MGINFO("Loading checkpoints");

    // load json & DNS checkpoints, and verify them
    // with respect to what blocks we already have
    CHECK_AND_ASSERT_MES(update_checkpoints(), false, "One or more checkpoints loaded from json or dns conflicted with existing checkpoints.");

   // DNS versions checking
    if (check_updates_string == "disabled")
      check_updates_level = UPDATES_DISABLED;
    else if (check_updates_string == "notify")
      check_updates_level = UPDATES_NOTIFY;
    else if (check_updates_string == "download")
      check_updates_level = UPDATES_DOWNLOAD;
    else if (check_updates_string == "update")
      check_updates_level = UPDATES_UPDATE;
    else {
      MERROR("Invalid argument to --dns-versions-check: " << check_updates_string);
      return false;
    }

    r = m_miner.init(vm, m_nettype);
    CHECK_AND_ASSERT_MES(r, false, "Failed to initialize miner instance");

    return load_state_data();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::set_genesis_block(const block& b)
  {
    return m_blockchain_storage.reset_and_set_genesis_block(b);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::load_state_data()
  {
    // may be some code later
    return true;
  }
  //-----------------------------------------------------------------------------------------------
    bool core::deinit()
  {
    m_miner.stop();
    m_mempool.deinit();
    m_blockchain_storage.deinit();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  void core::test_drop_download()
  {
    m_test_drop_download = false;
  }
  //-----------------------------------------------------------------------------------------------
  void core::test_drop_download_height(uint64_t height)
  {
    m_test_drop_download_height = height;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_test_drop_download() const
  {
    return m_test_drop_download;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_test_drop_download_height() const
  {
    if (m_test_drop_download_height == 0)
      return true;

    if (get_blockchain_storage().get_current_blockchain_height() <= m_test_drop_download_height)
      return true;

    return false;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx_pre(const blobdata& tx_blob, tx_verification_context& tvc, cryptonote::transaction &tx, crypto::hash &tx_hash, crypto::hash &tx_prefixt_hash, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    tvc = boost::value_initialized<tx_verification_context>();

    if(tx_blob.size() > get_max_tx_size())
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, too big size " << tx_blob.size() << ", rejected");
      tvc.m_verifivation_failed = true;
      tvc.m_too_big = true;
      return false;
    }

    tx_hash = crypto::null_hash;
    tx_prefixt_hash = crypto::null_hash;

    if(!parse_tx_from_blob(tx, tx_hash, tx_prefixt_hash, tx_blob))
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, Failed to parse, rejected");
      tvc.m_verifivation_failed = true;
      return false;
    }
    //std::cout << "!"<< tx.vin.size() << std::endl;

    bad_semantics_txes_lock.lock();
    for (int idx = 0; idx < 2; ++idx)
    {
      if (bad_semantics_txes[idx].find(tx_hash) != bad_semantics_txes[idx].end())
      {
        bad_semantics_txes_lock.unlock();
        LOG_PRINT_L1("Transaction already seen with bad semantics, rejected");
        tvc.m_verifivation_failed = true;
        return false;
      }
    }
    bad_semantics_txes_lock.unlock();

    int version = m_blockchain_storage.get_current_hard_fork_version();
    unsigned int max_tx_version = version == 1 ? 1 : version < 8 ? 2 : 3;

    if (tx.version == 0 || tx.version > max_tx_version)
    {
      // v3 is the latest one we know
      tvc.m_verifivation_failed = true;
      return false;
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx_post(const blobdata& tx_blob, tx_verification_context& tvc, cryptonote::transaction &tx, crypto::hash &tx_hash, crypto::hash &tx_prefixt_hash, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    if(!check_tx_syntax(tx))
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, Failed to check tx " << tx_hash << " syntax, rejected");
      tvc.m_verifivation_failed = true;
      return false;
    }

    if (keeped_by_block && get_blockchain_storage().is_within_compiled_block_hash_area())
    {
      MTRACE("Skipping semantics check for tx kept by block in embedded hash area");
    }
    else if(!check_tx_semantic(tx, keeped_by_block))
    {
      LOG_PRINT_L1("WRONG TRANSACTION BLOB, Failed to check tx " << tx_hash << " semantic, rejected");
      tvc.m_verifivation_failed = true;
      bad_semantics_txes_lock.lock();
      bad_semantics_txes[0].insert(tx_hash);
      if (bad_semantics_txes[0].size() >= BAD_SEMANTICS_TXES_MAX_SIZE)
      {
        std::swap(bad_semantics_txes[0], bad_semantics_txes[1]);
        bad_semantics_txes[0].clear();
      }
      bad_semantics_txes_lock.unlock();
      return false;
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_txs(const std::list<blobdata>& tx_blobs, std::vector<tx_verification_context>& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    TRY_ENTRY();

    struct result { bool res; cryptonote::transaction tx; crypto::hash hash; crypto::hash prefix_hash; bool in_txpool; bool in_blockchain; };
    std::vector<result> results(tx_blobs.size());

    tvc.resize(tx_blobs.size());
    tools::threadpool::waiter waiter;
    std::list<blobdata>::const_iterator it = tx_blobs.begin();
    for (size_t i = 0; i < tx_blobs.size(); i++, ++it) {
      m_threadpool.submit(&waiter, [&, i, it] {
        try
        {
          results[i].res = handle_incoming_tx_pre(*it, tvc[i], results[i].tx, results[i].hash, results[i].prefix_hash, keeped_by_block, relayed, do_not_relay);
        }
        catch (const std::exception &e)
        {
          MERROR_VER("Exception in handle_incoming_tx_pre: " << e.what());
          results[i].res = false;
        }
      });
    }
    waiter.wait();
    it = tx_blobs.begin();
    for (size_t i = 0; i < tx_blobs.size(); i++, ++it) {
      if (!results[i].res)
        continue;
      if(m_mempool.have_tx(results[i].hash))
      {
        LOG_PRINT_L2("tx " << results[i].hash << "already have transaction in tx_pool");
      }
      else if(m_blockchain_storage.have_tx(results[i].hash))
      {
        LOG_PRINT_L2("tx " << results[i].hash << " already have transaction in blockchain");
      }
      else
      {
        m_threadpool.submit(&waiter, [&, i, it] {
          try
          {
            results[i].res = handle_incoming_tx_post(*it, tvc[i], results[i].tx, results[i].hash, results[i].prefix_hash, keeped_by_block, relayed, do_not_relay);
          }
          catch (const std::exception &e)
          {
            MERROR_VER("Exception in handle_incoming_tx_post: " << e.what());
            results[i].res = false;
          }
        });
      }
    }
    waiter.wait();

    bool ok = true;
    it = tx_blobs.begin();
    for (size_t i = 0; i < tx_blobs.size(); i++, ++it) {
      if (!results[i].res)
      {
        ok = false;
        continue;
      }

      ok &= add_new_tx(results[i].tx, results[i].hash, results[i].prefix_hash, it->size(), tvc[i], keeped_by_block, relayed, do_not_relay);
      if(tvc[i].m_verifivation_failed)
      {MERROR_VER("Transaction verification failed: " << results[i].hash);}
      else if(tvc[i].m_verifivation_impossible)
      {MERROR_VER("Transaction verification impossible: " << results[i].hash);}

      if(tvc[i].m_added_to_pool)
        MDEBUG("tx added: " << results[i].hash);
    }
    return ok;

    CATCH_ENTRY_L0("core::handle_incoming_txs()", false);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_tx(const blobdata& tx_blob, tx_verification_context& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    std::list<cryptonote::blobdata> tx_blobs;
    tx_blobs.push_back(tx_blob);
    std::vector<tx_verification_context> tvcv(1);
    bool r = handle_incoming_txs(tx_blobs, tvcv, keeped_by_block, relayed, do_not_relay);
    tvc = tvcv[0];
    return r;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_stat_info(core_stat_info& st_inf) const
  {
    st_inf.mining_speed = m_miner.get_speed();
    st_inf.alternative_blocks = m_blockchain_storage.get_alternative_blocks_count();
    st_inf.blockchain_height = m_blockchain_storage.get_current_blockchain_height();
    st_inf.tx_pool_size = m_mempool.get_transactions_count();
    st_inf.top_block_id_str = epee::string_tools::pod_to_hex(m_blockchain_storage.get_tail_id());
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  static bool validate_deregistration_with_quorum(const tx_extra_service_node_deregister &deregistration, const std::vector<crypto::public_key> &quorum)
  {
    if (!(deregistration.voters_signatures.size() == 1 || deregistration.voters_signatures.size() == quorum.size()))
    {
      MERROR_VER("A full deregistration requires the number of voters to match: " << deregistration.voters_signatures.size() << ", which does not match quorum size: " << quorum.size());
      MERROR_VER("A partial deregistration must only have one vote associated.");
      return false;
    }

    // TODO(doyle): This needs better performance as quorums will grow to large amounts
    std::vector<crypto::hash> quorum_hashes  (quorum.size());
    {
      for (size_t i = 0; i < quorum.size(); i++)
      {
        const crypto::public_key& key = quorum[i];
        crypto::cn_fast_hash(key.data, sizeof(key.data), quorum_hashes[i]);
      }
    }

    std::vector<int> quorum_memoizer(quorum.size(), 0);
    bool matched = false;
    for (size_t i = 0; i < deregistration.voters_signatures.size(); i++, matched = false)
    {
      auto *signature
        = reinterpret_cast<const crypto::signature *>(&deregistration.voters_signatures[i]);

      for (size_t j = 0; j < quorum.size(); j++)
      {
        if (quorum_memoizer[j]) continue;
        const crypto::public_key& public_spend_key = quorum[j];
        const crypto::hash& hash  = quorum_hashes[i];

        if (crypto::check_signature(hash, public_spend_key, *signature))
        {
          quorum_memoizer[j] = true;
          matched            = true;
          break;
        }
      }

      if (!matched)
      {
        MERROR_VER("TX version 3 could not match deregistration key to the entries in the quorum");
        return false;
      }
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_semantic(const transaction& tx, bool keeped_by_block) const
  {
    if(!check_inputs_types_supported(tx))
    {
      MERROR_VER("unsupported input types for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if(!check_outs_valid(tx))
    {
      MERROR_VER("tx with invalid outputs, rejected for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if(!check_money_overflow(tx))
    {
      MERROR_VER("tx has money overflow, rejected for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if(!keeped_by_block && get_object_blobsize(tx) >= m_blockchain_storage.get_current_cumulative_blocksize_limit() - CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE)
    {
      MERROR_VER("tx is too large " << get_object_blobsize(tx) << ", expected not bigger than " << m_blockchain_storage.get_current_cumulative_blocksize_limit() - CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE);
      return false;
    }

    if(!check_tx_inputs_keyimages_diff(tx))
    {
      MERROR_VER("tx uses a single key image more than once");
      return false;
    }

    if (!check_tx_inputs_ring_members_diff(tx))
    {
      MERROR_VER("tx uses duplicate ring members");
      return false;
    }

    if (!check_tx_inputs_keyimages_domain(tx))
    {
      MERROR_VER("tx uses key image not in the valid domain");
      return false;
    }

    if (tx.version == 1)
    {
      uint64_t amount_in = 0;
      get_inputs_money_amount(tx, amount_in);
      uint64_t amount_out = get_outs_money_amount(tx);

      if(amount_in <= amount_out)
      {
        MERROR_VER("tx with wrong amounts: ins " << amount_in << ", outs " << amount_out << ", rejected for tx id= " << get_transaction_hash(tx));
        return false;
      }
    }

    if(tx.version <= 2 && !tx.vin.size())
    {
      MERROR_VER("tx with empty inputs, rejected for tx id= " << get_transaction_hash(tx));
      return false;
    }

    if (tx.version >= 2) // for version >= 2, ringct signatures check verifies amounts match
    {
      if (tx.rct_signatures.outPk.size() != tx.vout.size())
      {
        MERROR_VER("tx with mismatched vout/outPk count, rejected for tx id= " << get_transaction_hash(tx));
        return false;
      }

      if (tx.version == 2)
      {
        const rct::rctSig &rv = tx.rct_signatures;
        switch (rv.type) {
          case rct::RCTTypeNull:
            // coinbase should not come here, so we reject for all other types
            MERROR_VER("Unexpected Null rctSig type");
            return false;
          case rct::RCTTypeSimple:
          case rct::RCTTypeSimpleBulletproof:
            if (!rct::verRctSimple(rv, true))
            {
              MERROR_VER("rct signature semantics check failed");
              return false;
            }
            break;
          case rct::RCTTypeFull:
          case rct::RCTTypeFullBulletproof:
            if (!rct::verRct(rv, true))
            {
              MERROR_VER("rct signature semantics check failed");
              return false;
            }
            break;
          default:
            MERROR_VER("Unknown rct type: " << rv.type);
            return false;
        }
      }
      else if (tx.version == 3)
      {
        // TODO(doyle): Version 3 should only be valid from the hardfork height
        tx_extra_service_node_deregister deregistration;
        if (!get_service_node_deregister_from_tx_extra(tx.extra, deregistration))
        {
          MERROR_VER("TX version 3 did not contain deregistration data");
          return false;
        }

        // Check service node to deregister is valid
        {
          auto xx__is_service_node_registered = ([](const crypto::public_key &service_node_key) -> bool {
            return true;
          });

          if (!xx__is_service_node_registered(deregistration.service_node_key))
          {
            MERROR_VER("TX version 3 trying to deregister a non-active node");
            return false;
          }
        }

        // Match deregistration voters to quorum
        {
          std::vector<crypto::public_key> quorum;
          if (!get_quorum_list_for_height(deregistration.block_height, quorum))
          {
            MERROR_VER("TX version 3 could not get quorum for height: " << deregistration.block_height);
            return false;
          }

          if (!validate_deregistration_with_quorum(deregistration, quorum))
          {
            MERROR_VER("TX version 3 trying to deregister a non-active node");
            return false;
          }
        }
      }
    }

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::is_key_image_spent(const crypto::key_image &key_image) const
  {
    return m_blockchain_storage.have_tx_keyimg_as_spent(key_image);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::are_key_images_spent(const std::vector<crypto::key_image>& key_im, std::vector<bool> &spent) const
  {
    spent.clear();
    for(auto& ki: key_im)
    {
      spent.push_back(m_blockchain_storage.have_tx_keyimg_as_spent(ki));
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_block_sync_size(uint64_t height) const
  {
    static const uint64_t quick_height = m_nettype == TESTNET ? 801219 : m_nettype == MAINNET ? 1220516 : 0;
    if (block_sync_size > 0)
      return block_sync_size;
    if (height >= quick_height)
      return BLOCKS_SYNCHRONIZING_DEFAULT_COUNT;
    return BLOCKS_SYNCHRONIZING_DEFAULT_COUNT_PRE_V4;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::are_key_images_spent_in_pool(const std::vector<crypto::key_image>& key_im, std::vector<bool> &spent) const
  {
    spent.clear();

    return m_mempool.check_for_key_images(key_im, spent);
  }
  //-----------------------------------------------------------------------------------------------
  std::pair<uint64_t, uint64_t> core::get_coinbase_tx_sum(const uint64_t start_offset, const size_t count)
  {
    uint64_t emission_amount = 0;
    uint64_t total_fee_amount = 0;
    if (count)
    {
      const uint64_t end = start_offset + count - 1;
      m_blockchain_storage.for_blocks_range(start_offset, end,
        [this, &emission_amount, &total_fee_amount](uint64_t, const crypto::hash& hash, const block& b){
      std::list<transaction> txs;
      std::list<crypto::hash> missed_txs;
      uint64_t coinbase_amount = get_outs_money_amount(b.miner_tx);
      this->get_transactions(b.tx_hashes, txs, missed_txs);      
      uint64_t tx_fee_amount = 0;
      for(const auto& tx: txs)
      {
        tx_fee_amount += get_tx_fee(tx);
      }
      
      emission_amount += coinbase_amount - tx_fee_amount;
      total_fee_amount += tx_fee_amount;
      return true;
      });
    }

    return std::pair<uint64_t, uint64_t>(emission_amount, total_fee_amount);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_inputs_keyimages_diff(const transaction& tx) const
  {
    std::unordered_set<crypto::key_image> ki;
    for(const auto& in: tx.vin)
    {
      CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, tokey_in, false);
      if(!ki.insert(tokey_in.k_image).second)
        return false;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_inputs_ring_members_diff(const transaction& tx) const
  {
    const uint8_t version = m_blockchain_storage.get_current_hard_fork_version();
    if (version >= 6)
    {
      for(const auto& in: tx.vin)
      {
        CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, tokey_in, false);
        for (size_t n = 1; n < tokey_in.key_offsets.size(); ++n)
          if (tokey_in.key_offsets[n] == 0)
            return false;
      }
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_inputs_keyimages_domain(const transaction& tx) const
  {
    std::unordered_set<crypto::key_image> ki;
    for(const auto& in: tx.vin)
    {
      CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, tokey_in, false);
      if (!(rct::scalarmultKey(rct::ki2rct(tokey_in.k_image), rct::curveOrder()) == rct::identity()))
        return false;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_new_tx(transaction& tx, tx_verification_context& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    crypto::hash tx_hash = get_transaction_hash(tx);
    crypto::hash tx_prefix_hash = get_transaction_prefix_hash(tx);
    blobdata bl;
    t_serializable_object_to_blob(tx, bl);
    return add_new_tx(tx, tx_hash, tx_prefix_hash, bl.size(), tvc, keeped_by_block, relayed, do_not_relay);
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_blockchain_total_transactions() const
  {
    return m_blockchain_storage.get_total_transactions();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_new_tx(transaction& tx, const crypto::hash& tx_hash, const crypto::hash& tx_prefix_hash, size_t blob_size, tx_verification_context& tvc, bool keeped_by_block, bool relayed, bool do_not_relay)
  {
    if (keeped_by_block)
      get_blockchain_storage().on_new_tx_from_block(tx);

    if(m_mempool.have_tx(tx_hash))
    {
      LOG_PRINT_L2("tx " << tx_hash << "already have transaction in tx_pool");
      return true;
    }

    if(m_blockchain_storage.have_tx(tx_hash))
    {
      LOG_PRINT_L2("tx " << tx_hash << " already have transaction in blockchain");
      return true;
    }

    uint8_t version = m_blockchain_storage.get_current_hard_fork_version();
    return m_mempool.add_tx(tx, tx_hash, blob_size, tvc, keeped_by_block, relayed, do_not_relay, version);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::relay_txpool_transactions()
  {
    // we attempt to relay txes that should be relayed, but were not
    std::list<std::pair<crypto::hash, cryptonote::blobdata>> txs;
    if (m_mempool.get_relayable_transactions(txs) && !txs.empty())
    {
      cryptonote_connection_context fake_context = AUTO_VAL_INIT(fake_context);
      tx_verification_context tvc = AUTO_VAL_INIT(tvc);
      NOTIFY_NEW_TRANSACTIONS::request r;
      for (auto it = txs.begin(); it != txs.end(); ++it)
      {
        r.txs.push_back(it->second);
      }
      get_protocol()->relay_transactions(r, fake_context);
      m_mempool.set_relayed(txs);
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  void core::on_transaction_relayed(const cryptonote::blobdata& tx_blob)
  {
    std::list<std::pair<crypto::hash, cryptonote::blobdata>> txs;
    cryptonote::transaction tx;
    crypto::hash tx_hash, tx_prefix_hash;
    if (!parse_and_validate_tx_from_blob(tx_blob, tx, tx_hash, tx_prefix_hash))
    {
      LOG_ERROR("Failed to parse relayed transaction");
      return;
    }
    txs.push_back(std::make_pair(tx_hash, std::move(tx_blob)));
    m_mempool.set_relayed(txs);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_block_template(block& b, const account_public_address& adr, difficulty_type& diffic, uint64_t& height, uint64_t& expected_reward, const blobdata& ex_nonce)
  {
    return m_blockchain_storage.create_block_template(b, adr, diffic, height, expected_reward, ex_nonce);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::find_blockchain_supplement(const std::list<crypto::hash>& qblock_ids, NOTIFY_RESPONSE_CHAIN_ENTRY::request& resp) const
  {
    return m_blockchain_storage.find_blockchain_supplement(qblock_ids, resp);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::find_blockchain_supplement(const uint64_t req_start_block, const std::list<crypto::hash>& qblock_ids, std::list<std::pair<cryptonote::blobdata, std::list<cryptonote::blobdata> > >& blocks, uint64_t& total_height, uint64_t& start_height, bool pruned, size_t max_count) const
  {
    return m_blockchain_storage.find_blockchain_supplement(req_start_block, qblock_ids, blocks, total_height, start_height, pruned, max_count);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_random_outs_for_amounts(const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request& req, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response& res) const
  {
    return m_blockchain_storage.get_random_outs_for_amounts(req, res);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_outs(const COMMAND_RPC_GET_OUTPUTS_BIN::request& req, COMMAND_RPC_GET_OUTPUTS_BIN::response& res) const
  {
    return m_blockchain_storage.get_outs(req, res);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_random_rct_outs(const COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::request& req, COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::response& res) const
  {
    return m_blockchain_storage.get_random_rct_outs(req, res);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_output_distribution(uint64_t amount, uint64_t from_height, uint64_t to_height, uint64_t &start_height, std::vector<uint64_t> &distribution, uint64_t &base) const
  {
    return m_blockchain_storage.get_output_distribution(amount, from_height, to_height, start_height, distribution, base);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_tx_outputs_gindexs(const crypto::hash& tx_id, std::vector<uint64_t>& indexs) const
  {
    return m_blockchain_storage.get_tx_outputs_gindexs(tx_id, indexs);
  }
  //-----------------------------------------------------------------------------------------------
  void core::pause_mine()
  {
    m_miner.pause();
  }
  //-----------------------------------------------------------------------------------------------
  void core::resume_mine()
  {
    m_miner.resume();
  }
  //-----------------------------------------------------------------------------------------------
  block_complete_entry get_block_complete_entry(block& b, tx_memory_pool &pool)
  {
    block_complete_entry bce;
    bce.block = cryptonote::block_to_blob(b);
    for (const auto &tx_hash: b.tx_hashes)
    {
      cryptonote::blobdata txblob;
      CHECK_AND_ASSERT_THROW_MES(pool.get_transaction(tx_hash, txblob), "Transaction not found in pool");
      bce.txs.push_back(txblob);
    }
    return bce;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_block_found(block& b)
  {
    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    m_miner.pause();
    std::list<block_complete_entry> blocks;
    try
    {
      blocks.push_back(get_block_complete_entry(b, m_mempool));
    }
    catch (const std::exception &e)
    {
      m_miner.resume();
      return false;
    }
    prepare_handle_incoming_blocks(blocks);
    m_blockchain_storage.add_new_block(b, bvc);
    cleanup_handle_incoming_blocks(true);
    //anyway - update miner template
    update_miner_block_template();
    m_miner.resume();


    CHECK_AND_ASSERT_MES(!bvc.m_verifivation_failed, false, "mined block failed verification");
    if(bvc.m_added_to_main_chain)
    {
      cryptonote_connection_context exclude_context = boost::value_initialized<cryptonote_connection_context>();
      NOTIFY_NEW_BLOCK::request arg = AUTO_VAL_INIT(arg);
      arg.current_blockchain_height = m_blockchain_storage.get_current_blockchain_height();
      std::list<crypto::hash> missed_txs;
      std::list<cryptonote::blobdata> txs;
      m_blockchain_storage.get_transactions_blobs(b.tx_hashes, txs, missed_txs);
      if(missed_txs.size() &&  m_blockchain_storage.get_block_id_by_height(get_block_height(b)) != get_block_hash(b))
      {
        LOG_PRINT_L1("Block found but, seems that reorganize just happened after that, do not relay this block");
        return true;
      }
      CHECK_AND_ASSERT_MES(txs.size() == b.tx_hashes.size() && !missed_txs.size(), false, "can't find some transactions in found block:" << get_block_hash(b) << " txs.size()=" << txs.size()
        << ", b.tx_hashes.size()=" << b.tx_hashes.size() << ", missed_txs.size()" << missed_txs.size());

      block_to_blob(b, arg.b.block);
      //pack transactions
      for(auto& tx:  txs)
        arg.b.txs.push_back(tx);

      m_pprotocol->relay_block(arg, exclude_context);
    }
    return bvc.m_added_to_main_chain;
  }
  //-----------------------------------------------------------------------------------------------
  void core::on_synchronized()
  {
    m_miner.on_synchronized();
  }
  //-----------------------------------------------------------------------------------------------
  void core::safesyncmode(const bool onoff)
  {
    m_blockchain_storage.safesyncmode(onoff);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::add_new_block(const block& b, block_verification_context& bvc)
  {
    return m_blockchain_storage.add_new_block(b, bvc);
  }

  //-----------------------------------------------------------------------------------------------
  bool core::prepare_handle_incoming_blocks(const std::list<block_complete_entry> &blocks)
  {
    m_incoming_tx_lock.lock();
    m_blockchain_storage.prepare_handle_incoming_blocks(blocks);
    return true;
  }

  //-----------------------------------------------------------------------------------------------
  bool core::cleanup_handle_incoming_blocks(bool force_sync)
  {
    bool success = false;
    try {
      success = m_blockchain_storage.cleanup_handle_incoming_blocks(force_sync);
    }
    catch (...) {}
    m_incoming_tx_lock.unlock();
    return success;
  }

  //-----------------------------------------------------------------------------------------------
  bool core::handle_incoming_block(const blobdata& block_blob, block_verification_context& bvc, bool update_miner_blocktemplate)
  {
    TRY_ENTRY();

    // load json & DNS checkpoints every 10min/hour respectively,
    // and verify them with respect to what blocks we already have
    CHECK_AND_ASSERT_MES(update_checkpoints(), false, "One or more checkpoints loaded from json or dns conflicted with existing checkpoints.");

    bvc = boost::value_initialized<block_verification_context>();
    if(block_blob.size() > get_max_block_size())
    {
      LOG_PRINT_L1("WRONG BLOCK BLOB, too big size " << block_blob.size() << ", rejected");
      bvc.m_verifivation_failed = true;
      return false;
    }

    block b = AUTO_VAL_INIT(b);
    if(!parse_and_validate_block_from_blob(block_blob, b))
    {
      LOG_PRINT_L1("Failed to parse and validate new block");
      bvc.m_verifivation_failed = true;
      return false;
    }
    add_new_block(b, bvc);
    if(update_miner_blocktemplate && bvc.m_added_to_main_chain)
       update_miner_block_template();
    return true;

    CATCH_ENTRY_L0("core::handle_incoming_block()", false);
  }
  //-----------------------------------------------------------------------------------------------
  // Used by the RPC server to check the size of an incoming
  // block_blob
  bool core::check_incoming_block_size(const blobdata& block_blob) const
  {
    if(block_blob.size() > get_max_block_size())
    {
      LOG_PRINT_L1("WRONG BLOCK BLOB, too big size " << block_blob.size() << ", rejected");
      return false;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  crypto::hash core::get_tail_id() const
  {
    return m_blockchain_storage.get_tail_id();
  }
  //-----------------------------------------------------------------------------------------------
  difficulty_type core::get_block_cumulative_difficulty(uint64_t height) const
  {
    return m_blockchain_storage.get_db().get_block_cumulative_difficulty(height);
  }
  //-----------------------------------------------------------------------------------------------
  size_t core::get_pool_transactions_count() const
  {
    return m_mempool.get_transactions_count();
  }
  //-----------------------------------------------------------------------------------------------
  bool core::have_block(const crypto::hash& id) const
  {
    return m_blockchain_storage.have_block(id);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::parse_tx_from_blob(transaction& tx, crypto::hash& tx_hash, crypto::hash& tx_prefix_hash, const blobdata& blob) const
  {
    return parse_and_validate_tx_from_blob(blob, tx, tx_hash, tx_prefix_hash);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_tx_syntax(const transaction& tx) const
  {
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transactions(std::list<transaction>& txs, bool include_sensitive_data) const
  {
    m_mempool.get_transactions(txs, include_sensitive_data);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transaction_hashes(std::vector<crypto::hash>& txs, bool include_sensitive_data) const
  {
    m_mempool.get_transaction_hashes(txs, include_sensitive_data);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transaction_stats(struct txpool_stats& stats, bool include_sensitive_data) const
  {
    m_mempool.get_transaction_stats(stats, include_sensitive_data);
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transaction(const crypto::hash &id, cryptonote::blobdata& tx) const
  {
    return m_mempool.get_transaction(id, tx);
  }  
  //-----------------------------------------------------------------------------------------------
  bool core::pool_has_tx(const crypto::hash &id) const
  {
    return m_mempool.have_tx(id);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_transactions_and_spent_keys_info(std::vector<tx_info>& tx_infos, std::vector<spent_key_image_info>& key_image_infos, bool include_sensitive_data) const
  {
    return m_mempool.get_transactions_and_spent_keys_info(tx_infos, key_image_infos, include_sensitive_data);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_pool_for_rpc(std::vector<cryptonote::rpc::tx_in_pool>& tx_infos, cryptonote::rpc::key_images_with_tx_hashes& key_image_infos) const
  {
    return m_mempool.get_pool_for_rpc(tx_infos, key_image_infos);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_short_chain_history(std::list<crypto::hash>& ids) const
  {
    return m_blockchain_storage.get_short_chain_history(ids);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::handle_get_objects(NOTIFY_REQUEST_GET_OBJECTS::request& arg, NOTIFY_RESPONSE_GET_OBJECTS::request& rsp, cryptonote_connection_context& context)
  {
    return m_blockchain_storage.handle_get_objects(arg, rsp);
  }
  //-----------------------------------------------------------------------------------------------
  crypto::hash core::get_block_id_by_height(uint64_t height) const
  {
    return m_blockchain_storage.get_block_id_by_height(height);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_block_by_hash(const crypto::hash &h, block &blk, bool *orphan) const
  {
    return m_blockchain_storage.get_block_by_hash(h, blk, orphan);
  }
  //-----------------------------------------------------------------------------------------------
  std::string core::print_pool(bool short_format) const
  {
    return m_mempool.print_pool(short_format);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::update_miner_block_template()
  {
    m_miner.on_block_chain_update();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::on_idle()
  {
    if(!m_starter_message_showed)
    {
      std::string main_message;
      if (m_offline)
        main_message = "The daemon is running offline and will not attempt to sync to the Loki network.";
      else
        main_message = "The daemon will start synchronizing with the network. This may take a long time to complete.";
      MGINFO_YELLOW(ENDL << "**********************************************************************" << ENDL
        << main_message << ENDL
        << ENDL
        << "You can set the level of process detailization through \"set_log <level|categories>\" command," << ENDL
        << "where <level> is between 0 (no details) and 4 (very verbose), or custom category based levels (eg, *:WARNING)." << ENDL
        << ENDL
        << "Use the \"help\" command to see the list of available commands." << ENDL
        << "Use \"help <command>\" to see a command's documentation." << ENDL
        << "**********************************************************************" << ENDL);
      m_starter_message_showed = true;
    }

    m_fork_moaner.do_call(boost::bind(&core::check_fork_time, this));
    m_txpool_auto_relayer.do_call(boost::bind(&core::relay_txpool_transactions, this));
    m_check_updates_interval.do_call(boost::bind(&core::check_updates, this));
    m_check_disk_space_interval.do_call(boost::bind(&core::check_disk_space, this));
    m_miner.on_idle();
    m_mempool.on_idle();
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_fork_time()
  {
    HardFork::State state = m_blockchain_storage.get_hard_fork_state();
    const el::Level level = el::Level::Warning;
    switch (state) {
      case HardFork::LikelyForked:
        MCLOG_RED(level, "global", "**********************************************************************");
        MCLOG_RED(level, "global", "Last scheduled hard fork is too far in the past.");
        MCLOG_RED(level, "global", "We are most likely forked from the network. Daemon update needed now.");
        MCLOG_RED(level, "global", "**********************************************************************");
        break;
      case HardFork::UpdateNeeded:
        MCLOG_RED(level, "global", "**********************************************************************");
        MCLOG_RED(level, "global", "Last scheduled hard fork time shows a daemon update is needed soon.");
        MCLOG_RED(level, "global", "**********************************************************************");
        break;
      default:
        break;
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  uint8_t core::get_ideal_hard_fork_version() const
  {
    return get_blockchain_storage().get_ideal_hard_fork_version();
  }
  //-----------------------------------------------------------------------------------------------
  uint8_t core::get_ideal_hard_fork_version(uint64_t height) const
  {
    return get_blockchain_storage().get_ideal_hard_fork_version(height);
  }
  //-----------------------------------------------------------------------------------------------
  uint8_t core::get_hard_fork_version(uint64_t height) const
  {
    return get_blockchain_storage().get_hard_fork_version(height);
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_updates()
  {
    static const char software[] = "loki";
#ifdef BUILD_TAG
    static const char buildtag[] = BOOST_PP_STRINGIZE(BUILD_TAG);
    static const char subdir[] = "cli"; // because it can never be simple
#else
    static const char buildtag[] = "source";
    static const char subdir[] = "source"; // because it can never be simple
#endif

    if (m_offline)
      return true;

    if (check_updates_level == UPDATES_DISABLED)
      return true;

    std::string version, hash;
    MCDEBUG("updates", "Checking for a new " << software << " version for " << buildtag);
    if (!tools::check_updates(software, buildtag, version, hash))
      return false;

    if (tools::vercmp(version.c_str(), LOKI_VERSION) <= 0)
      return true;

    std::string url = tools::get_update_url(software, subdir, buildtag, version, true);
    MCLOG_CYAN(el::Level::Info, "global", "Version " << version << " of " << software << " for " << buildtag << " is available: " << url << ", SHA256 hash " << hash);

    if (check_updates_level == UPDATES_NOTIFY)
      return true;

    url = tools::get_update_url(software, subdir, buildtag, version, false);
    std::string filename;
    const char *slash = strrchr(url.c_str(), '/');
    if (slash)
      filename = slash + 1;
    else
      filename = std::string(software) + "-update-" + version;
    boost::filesystem::path path(epee::string_tools::get_current_module_folder());
    path /= filename;

    boost::unique_lock<boost::mutex> lock(m_update_mutex);

    if (m_update_download != 0)
    {
      MCDEBUG("updates", "Already downloading update");
      return true;
    }

    crypto::hash file_hash;
    if (!tools::sha256sum(path.string(), file_hash) || (hash != epee::string_tools::pod_to_hex(file_hash)))
    {
      MCDEBUG("updates", "We don't have that file already, downloading");
      const std::string tmppath = path.string() + ".tmp";
      if (epee::file_io_utils::is_file_exist(tmppath))
      {
        MCDEBUG("updates", "We have part of the file already, resuming download");
      }
      m_last_update_length = 0;
      m_update_download = tools::download_async(tmppath, url, [this, hash, path](const std::string &tmppath, const std::string &uri, bool success) {
        bool remove = false, good = true;
        if (success)
        {
          crypto::hash file_hash;
          if (!tools::sha256sum(tmppath, file_hash))
          {
            MCERROR("updates", "Failed to hash " << tmppath);
            remove = true;
            good = false;
          }
          else if (hash != epee::string_tools::pod_to_hex(file_hash))
          {
            MCERROR("updates", "Download from " << uri << " does not match the expected hash");
            remove = true;
            good = false;
          }
        }
        else
        {
          MCERROR("updates", "Failed to download " << uri);
          good = false;
        }
        boost::unique_lock<boost::mutex> lock(m_update_mutex);
        m_update_download = 0;
        if (success && !remove)
        {
          std::error_code e = tools::replace_file(tmppath, path.string());
          if (e)
          {
            MCERROR("updates", "Failed to rename downloaded file");
            good = false;
          }
        }
        else if (remove)
        {
          if (!boost::filesystem::remove(tmppath))
          {
            MCERROR("updates", "Failed to remove invalid downloaded file");
            good = false;
          }
        }
        if (good)
          MCLOG_CYAN(el::Level::Info, "updates", "New version downloaded to " << path.string());
      }, [this](const std::string &path, const std::string &uri, size_t length, ssize_t content_length) {
        if (length >= m_last_update_length + 1024 * 1024 * 10)
        {
          m_last_update_length = length;
          MCDEBUG("updates", "Downloaded " << length << "/" << (content_length ? std::to_string(content_length) : "unknown"));
        }
        return true;
      });
    }
    else
    {
      MCDEBUG("updates", "We already have " << path << " with expected hash");
    }

    lock.unlock();

    if (check_updates_level == UPDATES_DOWNLOAD)
      return true;

    MCERROR("updates", "Download/update not implemented yet");
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::check_disk_space()
  {
    uint64_t free_space = get_free_space();
    if (free_space < 1ull * 1024 * 1024 * 1024) // 1 GB
    {
      const el::Level level = el::Level::Warning;
      MCLOG_RED(level, "global", "Free space is below 1 GB on " << m_config_folder);
    }
    return true;
  }
  //-----------------------------------------------------------------------------------------------
  void core::set_target_blockchain_height(uint64_t target_blockchain_height)
  {
    m_target_blockchain_height = target_blockchain_height;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_target_blockchain_height() const
  {
    return m_target_blockchain_height;
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::prevalidate_block_hashes(uint64_t height, const std::list<crypto::hash> &hashes)
  {
    return get_blockchain_storage().prevalidate_block_hashes(height, hashes);
  }
  //-----------------------------------------------------------------------------------------------
  uint64_t core::get_free_space() const
  {
    boost::filesystem::path path(m_config_folder);
    boost::filesystem::space_info si = boost::filesystem::space(path);
    return si.available;
  }
  //-----------------------------------------------------------------------------------------------
  static std::vector<crypto::public_key> xx__get_service_nodes_pub_keys_for_height(uint64_t height)
  {
      (void)height; // TODO(doyle): Mock function needs to be implemented

      const char *xx__secret_spend_keys_str[] =
      {
        "42d0681beffac7e34f85dfc3b8fefd9ffb60854205f6068705c89eef43800903",
        "51256e8711c7d1ac06ac141f723aef280b98d46012d3a81a18c8dd8ce5f9a304",
        "66cba8ff989c3096fbfeb1dc505c982d4580566a6b22c50dc291906b3647ea04",
        "c4a6129b6846369f0161da99e71c9f39bf50d640aaaa3fe297819f1d269b3a0b",
        "165401ad072f5b2629766870d2de49cda6d09d97ba7bc2f7d820bb7ed8073f00",
        "f5fad9c4e9587826a882bd309aa4f2d1943a6ae916cf4f9971a833578eba360e",
        "e26a4d4386392d9a758e011ef9e29ae8fea5a1ab809a0fd4768674da2e36600e",
        "b5e8af1114fbc006823e6e025b99fe2f25f409d69d8ae74b35103621c00fca0c",
        "fe007f06f5eac4919ffefb45b358475fdfe541837f73d1f76118d44b42b10a01",
        "0da5ef9cf7c85d7d3d0464d0a80e1447a45e90c6664246d91a4691875ad33605",
        "49a57a4709d5cb8fb9fad3f1c4d93087eec78bc8e6a05e257ce50e71d049260b",
        "9aa18f77f49d22bb294cd3d32a652824882e5b71ffe1e13008d8fbe9ba16970c",
        "4a7f7bffd936f5b5dc4f0cdac1da52363a6dbd8d6ee5295f0a991f2592500e07",
        "118b5a3270358532e68bc69d097bd3c46d8c01a2183ee949d130e22f3e4a1604",
        "627cea57ea2215b31477e40b3dbf275b2b8e527b41ad66282321c9c1d34a0c06",
        "70c4ca12d1c12d617e000b2a90def96089497819b3da4a278506285c59c62905",
        "e395f75700e3288ea62b1734bf229ed56f40eb891b6e33a231ac1e1366502208",
        "9e6cd8667ef8f0e2b527678da80af2d34eb942da8877b1d21c3872e8f3e6b605",
        "e86dc61f76023c370feb9d086b3369dce764058ff4523c9c1d1a44957910f809",
        "d17c568d4e6662c67920b618436435a837241a8ccafad6684016f4371b4d6d07",
        "97c416757a07054d505e7ef2ebebc920e19f2a88b05dd0c58d39de5b9bf10405",
        "be5a0079f52509be69afadabd58a969fb439772ddf58ff3fedc3e6d9acbd0807",
        "cf43ebaacecc33591ddbd985ee7a637de665a275099d0777c84b358408a50d00",
        "6aa670f687da3026a836faecaaa2178442414e89dba8559600977ced60bda009",
        "2ad782d57e3d5e500cf9025ec981c4212c76662fb6a98fa1726438246dbb1602",
        "82a3b1d1ce260680a7ffe485f383fbd423d93e3f64232ef1c8ee40f05465540e",
        "627b6b28e2b89d1d28a6084fc8adf1f1859d299fd4507332a0bd410ace67860d",
        "a4092b42c1e8b08c86a7b6ed0719a25715d0fa3119464a7d400a414761d89e08",
        "3f47aee16a97cbfb8129ae211ff28ad89ffc0435545ae036414e52e2fa05060e",
        "691c003ce323c82404ee15405cdbc70e80763685fa5c46eda8c49205d735e60c",
        "df5c271474a07df63e2247f98f425a95136b5aae1fbd610fc1135c803185c503",
        "ad98c0a4ac23c3df8568480d55078970a1db42067f0ee32ac94f030f1df8f601",
        "ca09d289d9b5eff9ae78cdbbc17a374e66481d510bb6b4231466c5237a267a0b",
        "fee16aaeba4145e3dca7f3f8cc617609c16e1312a7c007dbdcec3a597ced5402",
        "238a7696ac0a36391b9910ad7626585fc229af593fa22701c8b4c8b18990b901",
        "9d3d4d20e35b7845df650fba04eb24602e142b89614462a780bc104d7defbd09",
        "729cd68e3b0a8326c2cbd98a1eece2c2af3f9aa92a71f41e2a140a7ffc6d220e",
        "cc99a1d53daaf60f4bec5c211047be10219cf414d7fe78b84a893dc970164d0a",
        "5704870bd6ab0ceff4ef50053396bd539ef1d54fe4e95ad6eec06d272acf360e",
        "0e62022d8f704d4f54f448fe31bcce7c4ea975a7a534b7036af87741f4a29700",
        "eedfff5f46958b1340044c13853d5dcf9cd7bf7f2c0b7fc9bcf507b2b29fdb0d",
        "a2e9c539b646957637897a93b7253e62955815c466474849287cf0b7270e6c00",
        "9627273e472b68e6f8c7a228146e65cc7da97e85a3bf9d0a10306d7ed8da2602",
        "efb04b20f01b9295f8066f84b90a795a0eda9ae714c8ca9be123eac678025d01",
        "6a622ec2c0210357c347b54d2b0fb846ef5894a1f2ae117817ae6effc8b1800d",
        "e31919c7f3596e625a98eb2407108c3760cffb5d0975c5354509752519316d08",
        "5e053ac6e0f7725b1238798f5d5a09047db89f0aaf9c87e6d265bdce97360203",
        "cfe967061830fa5277b8c7432ff9c2ef80159a1c201c63627cb88bf3ecd97f0b",
        "a69c06422128e7ab55f38795b700af760a1bc7d5a08386d0c5f6f7e4a0367606",
        "669b4227c299d9b615af5ea636b062c16fa2b1b12ebfe9a3d21c01ece7cd7207",
        "fd3c1b477688cee1affe423772a5d20c2b6c99f1dc836f082d67f030ddf5330e",
        "739ad42ba90d3f6ad7bf58d6471f9dc3a3a70c335497d27eb84866360396b70f",
        "7ede7663967b311665d0dd4b93bb4eb6e0dbddbc1dd4f45d0d0668fdc1bf9602",
        "eb52bf69cd99e55bf4b6a6c150108a7f9262b96c43e349e429f573bddc3cab03",
        "56374dbe767e0bc5bb36c9b320884bb2b14978903964ca196054c4b63d76ab02",
        "2d3a6bc00151bec3f6802db502c4994eef16df5224fe9004dccedb6542990b04",
        "25291317fb6b7004086036d4c2afb93d17357b8927c05a86abf507c7066a3900",
        "368e6263aec214dfed4f9cda9d61dd6ad5354df89ede955d5bf11aacb608be08",
        "267ebbf081917b31073e4d24cf979db4761c1aab5972bc6f3fc14027b33d220a",
        "f4e1c1221ecb5fe301ed9a6852a8bf32be8a171f4bf4eaf77fea6aa698cf6a08",
        "2f11eab4c3c7625f1102b8af46bec768f70085bb3b358fc5e3d5c124c1a6960a",
        "9e9e42e1a8cdf9329389ea727e091bd33de97bc39fd18d355a0e6c58346b6d07",
        "3ae8b143ff3cb1c06b6623e0d4bb542ed4c777703b07e9c6c69a707297f51708",
        "dd4486a5cc44ca4d1f8fb452f387727682eacd68fdf342fae72af4d7dbd48f06",
        "b16e9260836c8a19b4bc082abbf096f184c24528f213b0cd137897914949ce0b",
        "e139ea1f4d2f86d4b3fc30962769a0bc961205ec6a65bb31117d034561ceee03",
        "fb5c840ad686c9ec2256f75ebbc9fcece5f2dfa8f02bfac587e56b052a687709",
        "e1c199e70a77a7141e5be996ab887b6eaf865166b2d5b77a23581903dd211b0d",
        "9813c6e6ea9a20a0ce9ceb9c3c153471bd54497ca5cf30348b791a7b2fc24f05",
        "d2731a9f2c4406a3ed0507ae5b390ddef234c59f7d0eaa52e7f56e0c43770206",
        "b61c77620ff596a7fa4f7562491bc3a77056acdfbe148507a8ebfab070b3c60a",
        "b44172f3e97b65c55fc8b89dba70d5b1fe4d947e5877cb6fbc402a36500b910d",
        "7566074ceb7da410088d0e40e10314831d2bf2ef2c347605ac00b9b5c1f22d07",
        "aa34be55d15a3e9b8715dfa5dbb09c40beefe21c12eee541893b13998e95a901",
        "13ead1a2c33c1db4814d55f015d177ad64fbf2481bdd24f5a709b92108f26f0b",
        "7082548d52798708301b8b7235950d4c1a92804ae56ae449ca6a88abe30f3707",
        "277f6f333a80796f352c39fba1a5da2e1b1c090c38170aa2d70f3127909dc009",
        "664726ebc833a5bf99057247d067451fe5ceef71b73db8636829a9dcb737fe01",
        "1300421958cfcf97fecccca4293abdc51d1032fadd09c41b84cd0a19615ef205",
        "cd994d117edc22e7eb6cd285681852026423cafcacc52f05d0e1b71f1bf99202",
        "8026fb72b2af35229a84f14badffbdd898d490c0c469d0c445322d2a07ee260d",
        "2b7be54ace38dee69733a5cea14de60de61aa7504ba93d7a4591a53ef7947005",
        "8816aa86e4e4d94f4785ef873810876bec9cba5488be17f653c1384e5d539f04",
        "a92aeb6be2833ba4acc81985df8038da898998c243ad61bd2e91b58468d15708",
        "9c14135fd4f93836b91348f051ce655d89b127dd25f9747fb903f7781c052302",
        "9c85976687edb01c63bfe10b5bd5da5542e3f91484be66969c50aea01d2d2402",
        "3a0aed78125f6a33a6ca342a33b3b0b8bd4f9fa9fed62058ac553af6080eb604",
        "58857677e6619e89e3d7688a83eafdce0a93c9e0b6ff1baaadc7ad2229d2bf01",
        "16c0b921ef146f0edb0a3617a222657ef3dfcaa855de0ff2b20fd35efe210d01",
        "18c7f0a125f35fc2bf9b066ea44a428c7613c05eb08c21f06d151e4d2a0abc03",
        "92f639ea36294071df576a5df3a6477617361543754440f2f036146d2f77130a",
        "d672c81f36f51b20ba2c1fae33a0b10f06615bc1464d84904a3d749a6a9b2c0e",
        "6439873d731d24fc1737e28914ed7b2dd919520699323f3c86a6a94827c13201",
        "52261c788b9dd1982f2615848577f403611563f891400c43c21cfe8534ab2f09",
        "64334da023fbb0cdeaeed9205c5cea14647c37c6d1178c5401795f12e7540708",
        "eb6c797d2a135edf704d03053e71a0b734de22e157806db4726e9a3fa6de9401",
        "3f41c8c2312302deb89669c8d59776173b0265ee058ebbcaa3ef55e82a474f03",
        "fe09c62ed7e4b100a10cf4eb6be1f17acd6b7dbf64d15bb956a48a36f915e704",
        "5b41d546ccd37e0b44bbd2f9cfe093fb3833f2b808c50b18aa8d4015193c5805",
        "ec7a3e53f86bd14c756f852ab4772a6dda38f88b4f2bca702a7c4b2dae857f0c",
      };

      size_t const size = sizeof(xx__secret_spend_keys_str) / sizeof(xx__secret_spend_keys_str[0]);
      std::vector<crypto::public_key> result;
      result.reserve(size);

      for (size_t i = 0; i < size; i++)
      {
        crypto::secret_key secret_key;
        assert(epee::string_tools::hex_to_pod(xx__secret_spend_keys_str[i], secret_key));

        crypto::public_key public_key;
        assert(crypto::secret_key_to_public_key(secret_key, public_key));
        result.push_back(public_key);
      }

      return result;
  }
  //-----------------------------------------------------------------------------------------------
  bool core::get_quorum_list_for_height(uint64_t height, std::vector<crypto::public_key>& quorum) const
  {
    const std::vector<crypto::public_key> pub_keys   = xx__get_service_nodes_pub_keys_for_height(height);
    const crypto::hash                    block_hash = get_block_id_by_height(height);

    if (block_hash == crypto::null_hash)
    {
      LOG_ERROR("Block height: " << height << " returned null hash");
      return false;
    }

    // Generate index mapping to pub_keys
    std::vector<size_t> pub_keys_indexes;
    {
      pub_keys_indexes.reserve(pub_keys.size());
      for (size_t i = 0; i < pub_keys.size(); i++) pub_keys_indexes.push_back(i);
    }

    // Swap first N (size of quorum) indexes randomly
    const int xx__quorum_size = 10;
    quorum.resize(xx__quorum_size);
    if (0)
    {
      // TODO(doyle): We should use more of the data from the hash
      uint64_t seed = 0;
      std::memcpy(&seed, block_hash.data, std::min(sizeof(seed), sizeof(block_hash.data)));

      std::mt19937_64 mersenne_twister(seed);
      std::uniform_int_distribution<size_t> rng(0, pub_keys.size() - 1);

      for (size_t i = 0; i < quorum.size(); i++)
      {
        size_t swap_index = rng(mersenne_twister);
        std::swap(pub_keys_indexes[i], pub_keys_indexes[swap_index]);
      }
    }

    for (size_t i = 0; i < quorum.size(); i++)
      quorum[i] = pub_keys[pub_keys_indexes[i]];

    return true;
  }
  //-----------------------------------------------------------------------------------------------
  std::time_t core::get_start_time() const
  {
    return start_time;
  }
  //-----------------------------------------------------------------------------------------------
  void core::graceful_exit()
  {
    raise(SIGTERM);
  }
}
