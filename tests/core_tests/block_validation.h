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

#pragma once
#include "chaingen.h"

struct gen_block_big_major_version : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_big_minor_version : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_ts_not_checked : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_ts_in_past : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_ts_in_future : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_invalid_prev_id : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_invalid_nonce : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_no_miner_tx : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_unlock_time_is_low : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_unlock_time_is_high : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_unlock_time_is_timestamp_in_past : test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_unlock_time_is_timestamp_in_future : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_height_is_low : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_height_is_high : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_miner_tx_has_2_tx_gen_in : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_miner_tx_has_2_in : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_miner_tx_with_txin_to_key : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_miner_tx_out_is_big : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_miner_tx_has_no_out : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_miner_tx_has_out_to_alice : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_has_invalid_tx : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_is_too_big : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};

struct gen_block_invalid_binary_format : public test_chain_unit_base
{
  bool generate(std::vector<test_event_entry>& events) const;
};
