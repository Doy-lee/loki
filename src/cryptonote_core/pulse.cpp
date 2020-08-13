#include <array>
#include <mutex>
#include <chrono>

#include "misc_log_ex.h"
#include "common/random.h"

#include "cryptonote_core.h"
#include "cryptonote_basic/hardfork.h"
#include "service_node_list.h"
#include "service_node_quorum_cop.h"
#include "service_node_rules.h"

#undef LOKI_DEFAULT_LOG_CATEGORY
#define LOKI_DEFAULT_LOG_CATEGORY "pulse"

enum struct round_state
{
  wait_for_next_block,

  prepare_for_round,
  wait_for_round,

  submit_handshakes,
  wait_for_handshakes,

  submit_handshake_bitset,
  wait_for_handshake_bitsets,

  submit_block_template,
  wait_for_block_template,

  submit_random_value_hash,
  wait_for_random_value_hashes,

  submit_random_value,
  wait_for_random_value,

  submit_signed_block,
  wait_for_signed_blocks,
};

constexpr std::string_view round_state_string(round_state state)
{
  switch(state)
  {
    case round_state::wait_for_next_block: return "Wait For Next Block"sv;

    case round_state::prepare_for_round: return "Prepare For Round"sv;
    case round_state::wait_for_round: return "Wait For Round"sv;

    case round_state::submit_handshakes: return "Submit Handshakes"sv;
    case round_state::wait_for_handshakes: return "Wait For Handshakes"sv;

    case round_state::submit_handshake_bitset: return "Submit Handshake Bitset"sv;
    case round_state::wait_for_handshake_bitsets: return "Wait For Validator Handshake Bitsets"sv;

    case round_state::submit_block_template: return "Submit Block Template"sv;
    case round_state::wait_for_block_template: return "Wait For Block Template"sv;

    case round_state::submit_random_value_hash: return "Submit Random Value Hash"sv;
    case round_state::wait_for_random_value_hashes: return "Wait For Random Value Hash"sv;

    case round_state::submit_random_value: return "Submit Random Value"sv;
    case round_state::wait_for_random_value: return "Wait For Random Value"sv;

    case round_state::submit_signed_block: return "Submit Signed Block"sv;
    case round_state::wait_for_signed_blocks: return "Wait For Signed Blocks"sv;
  }

  return "Invalid2"sv;
}

enum struct sn_type
{
  none,
  producer,
  validator,
};

enum struct queueing_state
{
  empty,
  received,
  processed,
};

struct message_queue
{
  std::array<std::pair<pulse::message, queueing_state>, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> buffer;
  size_t count;
};

struct pulse_wait_stage
{
  message_queue     queue;         // For messages from later stages that arrived before we reached that stage
  uint16_t          bitset;        // Bitset of validators that we received a message from for this stage
  uint16_t          msgs_received; // Number of unique messages received in the stage
  pulse::time_point end_time;      // Time at which the stage ends

  std::bitset<sizeof(uint16_t) * 8> bitset_view() const { return std::bitset<sizeof(bitset) * 8>(bitset); }
};

template <typename T>
using quorum_array = std::array<T, service_nodes::PULSE_QUORUM_NUM_VALIDATORS>;

struct round_context
{
  struct
  {
    uint64_t          height;
    crypto::hash      top_hash;
    uint64_t          top_block_timestamp;
    pulse::time_point round_0_start_time;
  } wait_for_next_block;

  struct
  {
    bool                  queue_for_next_round;
    uint8_t               round;
    service_nodes::quorum quorum;
    sn_type               participant;
    size_t                my_quorum_position;
    std::string           node_name;
    pulse::time_point     start_time;
  } prepare_for_round;

  struct
  {
    std::array<bool, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> data;
    pulse_wait_stage stage;
  } wait_for_handshakes;

  struct
  {
    std::array<std::pair<uint16_t, bool>, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> data;
    pulse_wait_stage stage;
  } wait_for_handshake_bitsets;

  struct
  {
    uint16_t validator_bitset;
    uint16_t validator_count;
  } submit_block_template;

  struct
  {
    cryptonote::block block;
    pulse_wait_stage stage;
  } wait_for_block_template;

  struct
  {
    cryptonote::pulse_random_value value;
  } submit_random_value_hash;

  struct
  {
    std::array<std::pair<crypto::hash, bool>, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> data;
    pulse_wait_stage stage;
  } wait_for_random_value_hashes;

  struct
  {
    std::array<std::pair<cryptonote::pulse_random_value, bool>, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> data;
    pulse_wait_stage stage;
  } wait_for_random_value;

  struct
  {
    std::string blob;
  } submit_signed_block;

  struct
  {
    std::array<std::pair<crypto::signature, bool>, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> data;
    pulse_wait_stage stage;
  } wait_for_signed_blocks;

  round_state state;
};

static round_context context;
namespace
{
std::string log_prefix(round_context const &context)
{
  std::stringstream result;
  result << "Pulse B" << context.wait_for_next_block.height << " R";
  if (context.state >= round_state::prepare_for_round)
    result << +context.prepare_for_round.round;
  else
    result << "0";
  result << ": ";

  if (context.prepare_for_round.node_name.size()) result << context.prepare_for_round.node_name << " ";
  result << "'" << round_state_string(context.state) << "' ";
  return result.str();
}

//
// NOTE: pulse::message Utiliities
//
// Generate the hash necessary for signing a message. All fields of the message
// must have been set for that message type except the signature.
crypto::hash msg_signature_hash(round_context const &context, pulse::message const &msg)
{
  assert(context.state >= round_state::wait_for_next_block);
  crypto::hash result = {};
  switch(msg.type)
  {
    case pulse::message_type::invalid:
      assert("Invalid Code Path" == nullptr);
    break;

    case pulse::message_type::handshake:
    {
      auto buf = tools::memcpy_le(context.wait_for_next_block.top_hash.data, msg.quorum_position);
      result   = crypto::cn_fast_hash(buf.data(), buf.size());
    }
    break;

    case pulse::message_type::handshake_bitset:
    {
      auto buf = tools::memcpy_le(msg.handshakes.validator_bitset, context.wait_for_next_block.top_hash.data, msg.quorum_position);
      result   = crypto::cn_fast_hash(buf.data(), buf.size());
    }
    break;

    case pulse::message_type::block_template:
      result = crypto::cn_fast_hash(msg.block_template.blob.data(), msg.block_template.blob.size());
    break;

    case pulse::message_type::random_value_hash:
    {
      auto buf = tools::memcpy_le(context.wait_for_next_block.top_hash.data, msg.quorum_position, msg.random_value_hash.hash.data);
      result   = crypto::cn_fast_hash(buf.data(), buf.size());
    }
    break;

    case pulse::message_type::random_value:
    {
      auto buf = tools::memcpy_le(context.wait_for_next_block.top_hash.data, msg.quorum_position, msg.random_value.value.data);
      result   = crypto::cn_fast_hash(buf.data(), buf.size());
    }
    break;

    case pulse::message_type::signed_block:
      result = crypto::cn_fast_hash(context.submit_signed_block.blob.data(), context.submit_signed_block.blob.size());
    break;
  }

  return result;
}

// Generate a helper string that describes the origin of the message, i.e.
// 'Signed Block' from 6:f9337ffc8bc30baf3fca92a13fa5a3a7ab7c93e69acb7136906e7feae9d3e769
//   or
// <Message Type> from <Validator Index>:<Validator Public Key>
std::string msg_source_string(round_context const &context, pulse::message const &msg)
{
  if (msg.quorum_position >= context.prepare_for_round.quorum.validators.size()) return "XX";
  assert(context.state >= round_state::prepare_for_round);

  crypto::public_key const &key = context.prepare_for_round.quorum.validators[msg.quorum_position];
  std::stringstream stream;
  stream << "'" << message_type_string(msg.type) << "' from " << msg.quorum_position;
  if (context.state >= round_state::prepare_for_round)
    stream << ":" << lokimq::to_hex(tools::view_guts(key));
  return stream.str();
}

bool msg_signature_check(pulse::message const &msg, service_nodes::quorum const &quorum)
{
  // Get Service Node Key
  crypto::public_key const *key = nullptr;
  switch (msg.type)
  {
    case pulse::message_type::invalid:
    {
      assert("Invalid Code Path" == nullptr);
      MERROR(log_prefix(context) << "Unhandled message type '" << pulse::message_type_string(msg.type) << "' can not verify signature.");
      return false;
    }
    break;

    case pulse::message_type::handshake: /* FALLTHRU */
    case pulse::message_type::handshake_bitset: /* FALLTHRU */
    case pulse::message_type::random_value_hash: /* FALLTHRU */
    case pulse::message_type::random_value: /* FALLTHRU */
    case pulse::message_type::signed_block:
    {
      if (msg.quorum_position >= static_cast<int>(quorum.validators.size()))
      {
        MERROR(log_prefix(context) << "Quorum position " << msg.quorum_position << " in Pulse message indexes oob");
        return false;
      }

      key = &quorum.validators[msg.quorum_position];
    }
    break;

    case pulse::message_type::block_template:
    {
      if (msg.quorum_position != 0)
      {
        MERROR(log_prefix(context) << "Quorum position " << msg.quorum_position << " in Pulse message indexes oob");
        return false;
      }

      key = &context.prepare_for_round.quorum.workers[0];
    }
    break;
  }

  if (!crypto::check_signature(msg_signature_hash(context, msg), *key, msg.signature))
  {
    MERROR(log_prefix(context) << "Signature for " << msg_source_string(context, msg) << " at height " << context.wait_for_next_block.height << "; is invalid");
    return false;
  }

  return true;
}

//
// NOTE: round_context Utilities
//
// Construct a pulse::message for sending the handshake bit or bitset.
void relay_validator_handshake_bit_or_bitset(round_context const &context, void *quorumnet_state, service_nodes::service_node_keys const &key, bool sending_bitset)
{
  assert(context.prepare_for_round.participant == sn_type::validator);

  // Message
  pulse::message msg  = {};
  msg.quorum_position = context.prepare_for_round.my_quorum_position;

  if (sending_bitset)
  {
    msg.type = pulse::message_type::handshake_bitset;

    // Generate the bitset from our received handshakes.
    auto const &quorum = context.wait_for_handshakes.data;
    for (size_t quorum_index = 0; quorum_index < quorum.size(); quorum_index++)
      if (bool received = quorum[quorum_index]; received)
        msg.handshakes.validator_bitset |= (1 << quorum_index);
  }
  else
  {
    msg.type = pulse::message_type::handshake;
  }
  crypto::generate_signature(msg_signature_hash(context, msg), key.pub, key.key, msg.signature);

  // Add our own handshake/bitset
  handle_message(nullptr, msg);

  // Send
  cryptonote::quorumnet_pulse_relay_message_to_quorum(quorumnet_state, msg, context.prepare_for_round.quorum, false /*block_producer*/);
}

// Check the stage's queue for any messages that we received early and process
// them if any. Any messages in the queue that we haven't received yet will also
// be relayed to the quorum.
void handle_messages_received_early_for(pulse_wait_stage &stage, void *quorumnet_state)
{
  if (!stage.queue.count)
    return;

  for (auto &[msg, queued] : stage.queue.buffer)
  {
    if (queued == queueing_state::received)
    {
      pulse::handle_message(quorumnet_state, msg);
      queued = queueing_state::processed;
    }
  }
}

// In Pulse, after the block template and validators are locked in, enforce that
// all participating validators are doing their job in the stage.
bool enforce_validator_participation_and_timeouts(round_context const &context,
                                                  pulse_wait_stage const &stage,
                                                  bool timed_out,
                                                  bool all_received)
{
  assert(context.state >= round_state::wait_for_block_template);
  uint16_t const validator_bitset = context.wait_for_block_template.block.pulse.validator_bitset;

  if (timed_out && !all_received)
  {
    MDEBUG(log_prefix(context) << "We timed out and there were insufficient hashes, required "
                               << service_nodes::PULSE_BLOCK_REQUIRED_SIGNATURES << ", received "
                               << stage.msgs_received << " from " << stage.bitset_view());
    return false;
  }

  // NOTE: This is not technically meant to hit, internal invariant checking
  // that should have been triggered earlier.
  bool unexpected_items = (stage.bitset | validator_bitset) != validator_bitset;
  if (stage.msgs_received == 0 || unexpected_items)
  {
    auto block_bitset = std::bitset<sizeof(validator_bitset) * 8>(validator_bitset);
    if (unexpected_items)
      MERROR(log_prefix(context) << "Internal error, unexpected block validator bitset is " << block_bitset << ", our bitset was " << stage.bitset_view());
    else
      MERROR(log_prefix(context) << "Internal error, unexpected empty bitset received, we expected " << block_bitset);

    return false;
  }

  return true;
}

} // anonymous namespace

void pulse::handle_message(void *quorumnet_state, pulse::message const &msg)
{
  if (msg.type == pulse::message_type::signed_block)
  {
    // Signed Block is the last message in the Pulse stage. This message
    // signs the final block blob, with the final random value inserted in
    // it.

    // To avoid re-sending the blob which we already agreed upon when
    // receiving the Block Template from the leader, this message's signature
    // signs the sender's Final Block Template blob.

    // To verify this signature we verify it against our version of the Final
    // Block Template. However, this message could be received by us, before
    // we're in the final Pulse stage, so we delay signature verification until
    // this is possible.

    // The other stages are unaffected by this because they are signing the
    // contents of the message itself, of which, these messages are processed
    // when we have reached that Pulse stage (where we have all the necessary
    // information to validate the contents).
  }
  else
  {
    if (!msg_signature_check(msg, context.prepare_for_round.quorum))
      return;
  }

  pulse_wait_stage *stage = nullptr;
  switch(msg.type)
  {
    case pulse::message_type::invalid:           assert("Invalid Code Path" != nullptr);              return;
    case pulse::message_type::handshake:         stage = &context.wait_for_handshakes.stage;          break;
    case pulse::message_type::handshake_bitset:  stage = &context.wait_for_handshake_bitsets.stage;   break;
    case pulse::message_type::block_template:    stage = &context.wait_for_block_template.stage;      break;
    case pulse::message_type::random_value_hash: stage = &context.wait_for_random_value_hashes.stage; break;
    case pulse::message_type::random_value:      stage = &context.wait_for_random_value.stage;        break;
    case pulse::message_type::signed_block:      stage = &context.wait_for_signed_blocks.stage;       break;
  }

  bool msg_received_early = false;
  switch(msg.type)
  {
    case pulse::message_type::invalid:           assert("Invalid Code Path" != nullptr); return;
    case pulse::message_type::handshake:         msg_received_early = (context.state < round_state::wait_for_handshakes);          break;
    case pulse::message_type::handshake_bitset:  msg_received_early = (context.state < round_state::wait_for_handshake_bitsets);   break;
    case pulse::message_type::block_template:    msg_received_early = (context.state < round_state::wait_for_block_template);      break;
    case pulse::message_type::random_value_hash: msg_received_early = (context.state < round_state::wait_for_random_value_hashes); break;
    case pulse::message_type::random_value:      msg_received_early = (context.state < round_state::wait_for_random_value);        break;
    case pulse::message_type::signed_block:      msg_received_early = (context.state < round_state::wait_for_signed_blocks);       break;
  }

  if (msg_received_early) // Enqueue the message until we're ready to process it
  {
    auto &[entry, queued] = stage->queue.buffer[msg.quorum_position];
    if (queued == queueing_state::empty)
    {
      MINFO(log_prefix(context) << "Message received early " << msg_source_string(context, msg) << ", queueing until we're ready.");
      stage->queue.count++;
      entry  = std::move(msg);
      queued = queueing_state::received;
    }

    return;
  }

  uint16_t const validator_bit = (1 << msg.quorum_position);
  if (context.state > round_state::wait_for_block_template)
  {
    // After the block template is received the partcipating validators are
    // locked in. Any stray messages from other validators are rejected.
    if ((validator_bit & context.wait_for_block_template.block.pulse.validator_bitset) == 0)
    {
      MINFO(log_prefix(context) << "Dropping " << msg_source_string(context, msg) << ". Not a locked in participant.");
      return;
    }
  }

  //
  // Add Message Data to Pulse Stage
  //
  assert(msg.quorum_position < service_nodes::PULSE_QUORUM_NUM_VALIDATORS);
  switch(msg.type)
  {
    case pulse::message_type::invalid:
      assert("Invalid Code Path" != nullptr);
      return;

    case pulse::message_type::handshake:
    {
      auto &quorum = context.wait_for_handshakes.data;
      if (quorum[msg.quorum_position]) return;
      quorum[msg.quorum_position] = true;

      auto const position_bitset = std::bitset<sizeof(validator_bit) * 8>(validator_bit);
      MINFO(log_prefix(context) << "Received handshake with quorum position bit (" << msg.quorum_position << ") "
                                << position_bitset << " saved to bitset " << stage->bitset_view());
    }
    break;

    case pulse::message_type::handshake_bitset:
    {
      auto &quorum             = context.wait_for_handshake_bitsets.data;
      auto &[bitset, received] = quorum[msg.quorum_position];
      if (received) return;
      received = true;
      bitset   = msg.handshakes.validator_bitset;
    }
    break;

    case pulse::message_type::block_template:
    {
      if (stage->msgs_received == 1)
        return;

      cryptonote::block block = {};
      if (!cryptonote::t_serializable_object_from_blob(block, msg.block_template.blob))
      {
        MINFO(log_prefix(context) << "Received unparsable pulse block template blob");
        return;
      }

      if (block.pulse.round != context.prepare_for_round.round)
      {
        MINFO(log_prefix(context) << "Received pulse block template specifying different round " << +block.pulse.round
                                  << ", expected " << +context.prepare_for_round.round);
        return;
      }

      context.wait_for_block_template.block = std::move(block);
    }
    break;

    case pulse::message_type::random_value_hash:
    {
      auto &quorum            = context.wait_for_random_value_hashes.data;
      auto &[value, received] = quorum[msg.quorum_position];
      if (received) return;
      value    = msg.random_value_hash.hash;
      received = true;
    }
    break;

    case pulse::message_type::random_value:
    {
      auto &quorum            = context.wait_for_random_value.data;
      auto &[value, received] = quorum[msg.quorum_position];
      if (received) return;

      if (auto const &[hash, hash_received] = context.wait_for_random_value_hashes.data[msg.quorum_position]; hash_received)
      {
        auto derived = crypto::cn_fast_hash(msg.random_value.value.data, sizeof(msg.random_value.value.data));
        if (derived != hash)
        {
          MINFO(log_prefix(context) << "Dropping " << msg_source_string(context, msg) << ". Rederived random value hash "
                                    << lokimq::to_hex(tools::view_guts(derived)) << " does not match original hash "
                                    << lokimq::to_hex(tools::view_guts(hash)));
          return;
        }
      }

      value    = msg.random_value.value;
      received = true;
    }
    break;

    case pulse::message_type::signed_block:
    {
      // Delayed signature verification because signature contents relies on us
      // have the Pulse data from the final stage
      if (!msg_signature_check(msg, context.prepare_for_round.quorum))
      {
        MDEBUG(log_prefix(context) << "Dropping " << msg_source_string(context, msg) << ". Sender's final block template signature does not match ours");
        return;
      }

      // Signature already verified in msg_signature_check(...)
      auto &quorum                = context.wait_for_signed_blocks.data;
      auto &[signature, received] = quorum[msg.quorum_position];
      if (received) return;

      signature = msg.signature;
      received  = true;
    }
    break;
  }

  stage->bitset |= validator_bit;
  stage->msgs_received++;

  if (quorumnet_state)
    cryptonote::quorumnet_pulse_relay_message_to_quorum(quorumnet_state, msg, context.prepare_for_round.quorum, context.prepare_for_round.participant == sn_type::producer);
}

/*
  Pulse progresses via a state-machine that is iterated through job submissions
  to 1 dedicated Pulse thread, started by LMQ.

  Iterating the state-machine is done by a periodic invocation of
  pulse::main(...) and messages received via Quorumnet for Pulse, which are
  queued in the thread's job queue.

  Using 1 dedicated thread via LMQ avoids any synchronization required in the
  user code when implementing Pulse.

  Skip control flow graph for textual description of stages.

          +---------------------+
          | Wait For Next Block |<--------+-------+
          +---------------------+         |       |
           |                              |       |
           +-[Blocks for round acquired]--+ No    |
           |                              |       |
           | Yes                          |       |
           |                              |       |
          +---------------------+         |       |
    +---->| Prepare For Round   |         |       |
    |     +---------------------+         |       |
    |      |                              |       |
    |     [Enough SN's for Pulse]---------+ No    |
    |      |                                      |
    |     Yes                                     |
    |      |                                      |
 No +-----[Participating in Quorum?]              |
    |      |                                      |
    |      | Yes                                  |
    |      |                                      |
    |     +---------------------+                 |
    |     | Wait For Round      |                 |
    |     +---------------------+                 |
    |      |                                      |
    |     [Block Height Changed?]-----------------+ Yes
    |      |
    |      | No
    |      |
    |     [Validator?]------------------+ No (We are Block Producer)
    |      |                            |
    |      | Yes                        |
    |      |                            |
    |     +---------------------+       |
    |     | Submit Handshakes   |       |
    |     +---------------------+       |
    |      |                            +-----------------+
Yes +-----[Quorumnet Comm Failure]                        |
    |      |                                              |
    |      | Yes                                          |
    |      |                                              |
    |     +---------------------+                         |
    |     | Wait For Handshakes |                         |
    |     +---------------------+                         |
    |      |                                              |
    |     +-------------------------+                     |
    |     | Submit Handshake Bitset |                     |
    |     +-------------------------+                     |
    |      |                                              |
Yes +-----[Quorumnet Comm Failure]                        |
    |      |                                              |
    |      | No                                           |
    |      |                                              |
    |     +----------------------------+                  |
    |     | Wait For Handshake Bitsets |<-----------------+
    |     +----------------------------+
    |      |
Yes +-----[Insufficient Bitsets]
    |      |
    |      | No
    |      |
    |     +-----------------------+
    |     | Submit Block Template |
    |     +-----------------------+
    |      |
 No +-----[Block Producer Passes SN List Checks]
           |
           | Yes
           |
          +-------------------------+
          | Wait For Block Template |
          +-------------------------+
           |
           | TODO(loki): TBD
           |
           V

  Wait For Next Block:
    - Checks for the next block in the blockchain to arrive. If it hasn't
      arrived yet, return to the caller.

    - Retrieves the blockchain metadata for starting a Pulse Round including the
      Genesis Pulse Block for the base timestamp and the top block hash and
      height for signatures.

    - // TODO(loki): After the Genesis Pulse Block is checkpointed, we can
      // remove it from the event loop. Right now we recheck every block incase
      // of (the very unlikely event) reorgs that might change the block at the
      // hardfork.

    - The next block timestamp is determined by

      G.Timestamp + (height * TARGET_BLOCK_TIME)

      Where 'G' is the base Pulse genesis block, i.e. the hardforking block
      activating Pulse (HF16).

      In case of the Service Node network failing, i.e. (pulse round > 255) or
      insufficient Service Nodes for Pulse, mining is re-activated and accepted
      as the next block in the blockchain.

      // TODO(loki): Activating mining on (Pulse Round > 255) needs to be
      // implemented.

  Prepare For Round:
    - Generate data for executing the round such as the Quorum and stage
      durations depending on the round Pulse is at by comparing the clock with
      the ideal block timestamp.

    - The state machine *always* reverts to 'Prepare For Round' when any
      subsequent stage fails, except in the cases where Pulse can not proceed
      because of an insufficient Service Node network.

  Wait For Round:
    - Checks clock against the next expected Pulse timestamps has elapsed,
      otherwise returns to caller.

    - If we are a validator we 'Submit Handshakes' with other Validators
      If we are a block producer we skip to 'Wait For Handshake Bitset' and
      await the final handshake bitsets from all the Validators

  Submit Handshakes:
    - Block Validators handshake to confirm participation in the round and collect other handshakes.

  Wait For Handshakes Then Submit Bitset:
    - Validators will each individually collect handshakes and build up a
      bitset of validators perceived to be participating.

    - When all handshakes are received we submit our bitset and progress to
      'Wait For Handshake Bitsets'

  Wait For Handshake Bitset:
    - Validators will each individually collect the handshake bitsets similar
      to Wait For Handshakes.

    - Upon receipt, the most common agreed upon bitset is used to lock in
      participation for the round. The round proceeds if more than 60% of the
      validators are participating, the round fails otherwise and reverts to
      'Prepare For Round'.

    - If we are a validator we go to 'Wait For Block Template'
    - If we are a block producer we go to 'Submit Block Template'

  Submit Block Template:
    - Block producer signs the block template with the validator bitset and
      pulse round applied to the block and submits it the Validators

  Wait For Block Template:
    - TODO(loki): TBD

*/

enum struct event_loop
{
  keep_running,
  return_to_caller,
};

event_loop goto_preparing_for_next_round(round_context &context)
{
  context.state                                  = round_state::prepare_for_round;
  context.prepare_for_round.queue_for_next_round = true;
  return event_loop::keep_running;
}

event_loop wait_for_next_block(uint64_t hf16_height, round_context &context, cryptonote::Blockchain const &blockchain)
{
  //
  // NOTE: If already processing pulse for height, wait for next height
  //
  uint64_t curr_height = blockchain.get_current_blockchain_height(true /*lock*/);
  if (context.wait_for_next_block.height == curr_height)
  {
    for (static uint64_t last_height = 0; last_height != curr_height; last_height = curr_height)
      MINFO(log_prefix(context) << "Network is currently producing block " << curr_height << ", waiting until next block");
    return event_loop::return_to_caller;
  }

  uint64_t top_height   = curr_height - 1;
  crypto::hash top_hash = blockchain.get_block_id_by_height(top_height);
  if (top_hash == crypto::null_hash)
  {
    for (static uint64_t last_height = 0; last_height != top_height; last_height = top_height)
      MERROR(log_prefix(context) << "Block hash for height " << top_height << " does not exist!");
    return event_loop::return_to_caller;
  }

  cryptonote::block top_block = {};
  if (bool orphan = false;
      !blockchain.get_block_by_hash(top_hash, top_block, &orphan) || orphan)
  {
    for (static uint64_t last_height = 0; last_height != top_height; last_height = top_height)
      MERROR(log_prefix(context) << "Failed to query previous block in blockchain at height " << top_height);
    return event_loop::return_to_caller;
  }

  //
  // NOTE: Query Pulse Genesis
  // TODO(loki): After HF16 genesis block is checkpointed, move this out of the loop/hardcode this as it can't change.
  //
  crypto::hash genesis_hash       = blockchain.get_block_id_by_height(hf16_height - 1);
  cryptonote::block genesis_block = {};
  if (bool orphaned = false; !blockchain.get_block_by_hash(genesis_hash, genesis_block, &orphaned) || orphaned)
  {
    for (static bool once = true; once; once = !once)
      MINFO(log_prefix(context) << "Failed to query the genesis block for Pulse at height " << hf16_height - 1);
    return event_loop::return_to_caller;
  }

  //
  // NOTE: Block Timing
  //
  uint64_t const delta_height = context.wait_for_next_block.height - cryptonote::get_block_height(genesis_block);
#if 0
  auto genesis_timestamp      = pulse::time_point(std::chrono::seconds(genesis_block.timestamp));
  pulse::time_point ideal_timestamp = genesis_timestamp + (TARGET_BLOCK_TIME * delta_height);
  pulse::time_point prev_timestamp  = pulse::time_point(std::chrono::seconds(top_block.timestamp));
  context.wait_for_next_block.round_0_start_time =
      std::clamp(ideal_timestamp,
                 prev_timestamp + service_nodes::PULSE_MIN_TARGET_BLOCK_TIME,
                 prev_timestamp + service_nodes::PULSE_MAX_TARGET_BLOCK_TIME);
#else // NOTE: Debug, make next block start relatively soon
  pulse::time_point prev_timestamp               = pulse::time_point(std::chrono::seconds(top_block.timestamp));
  context.wait_for_next_block.round_0_start_time = prev_timestamp + service_nodes::PULSE_ROUND_TIME;
#endif

  context.wait_for_next_block.height              = curr_height;
  context.wait_for_next_block.top_hash            = top_hash;
  context.wait_for_next_block.top_block_timestamp = top_block.timestamp;

  context.state             = round_state::prepare_for_round;
  context.prepare_for_round = {};

  return event_loop::keep_running;
}

event_loop prepare_for_round(round_context &context, service_nodes::service_node_keys const &key, cryptonote::Blockchain const &blockchain)
{
  context.wait_for_handshakes          = {};
  context.wait_for_handshake_bitsets   = {};
  context.submit_block_template        = {};
  context.wait_for_block_template      = {};
  context.submit_random_value_hash     = {};
  context.wait_for_random_value_hashes = {};
  context.wait_for_random_value        = {};

  if (context.prepare_for_round.queue_for_next_round)
  {
    // Set when an intermediate Pulse stage has failed and we wait on the
    // next round to occur.
    context.prepare_for_round.queue_for_next_round = false;
    context.prepare_for_round.round++; //TODO: Overflow check

    // Also check if the blockchain has changed, in which case we stop and
    // restart Pulse stages.
    if (context.wait_for_next_block.height != blockchain.get_current_blockchain_height(true /*lock*/))
      context.state = round_state::wait_for_next_block;
  }

  //
  // NOTE: Check Current Round
  //
  {
    auto now                     = pulse::clock::now();
    auto const time_since_block  = now <= context.wait_for_next_block.round_0_start_time ? std::chrono::seconds(0) : (now - context.wait_for_next_block.round_0_start_time);
    size_t round_usize           = time_since_block / service_nodes::PULSE_ROUND_TIME;
    uint8_t curr_round           = static_cast<uint8_t>(round_usize); // TODO: Overflow check

    if (curr_round > context.prepare_for_round.round)
      context.prepare_for_round.round = curr_round;
  }

  auto start_time = context.wait_for_next_block.round_0_start_time + (context.prepare_for_round.round * service_nodes::PULSE_ROUND_TIME);
  context.prepare_for_round.start_time                = start_time;
  context.wait_for_handshakes.stage.end_time          = context.prepare_for_round.start_time                + service_nodes::PULSE_WAIT_FOR_HANDSHAKES_DURATION;
  context.wait_for_handshake_bitsets.stage.end_time   = context.wait_for_handshakes.stage.end_time          + service_nodes::PULSE_WAIT_FOR_OTHER_VALIDATOR_HANDSHAKES_DURATION;
  context.wait_for_block_template.stage.end_time      = context.wait_for_handshake_bitsets.stage.end_time   + service_nodes::PULSE_WAIT_FOR_BLOCK_TEMPLATE_DURATION;
  context.wait_for_random_value_hashes.stage.end_time = context.wait_for_block_template.stage.end_time      + service_nodes::PULSE_WAIT_FOR_RANDOM_VALUE_HASH_DURATION;
  context.wait_for_random_value.stage.end_time        = context.wait_for_random_value_hashes.stage.end_time + service_nodes::PULSE_WAIT_FOR_RANDOM_VALUE_DURATION;
  context.wait_for_signed_blocks.stage.end_time       = context.wait_for_random_value.stage.end_time        + service_nodes::PULSE_WAIT_FOR_SIGNED_BLOCK_DURATION;

  context.prepare_for_round.quorum =
      service_nodes::generate_pulse_quorum(blockchain.nettype(),
                                           blockchain.get_db(),
                                           context.wait_for_next_block.height - 1,
                                           blockchain.get_service_node_list().get_block_leader().key,
                                           blockchain.get_current_hard_fork_version(),
                                           blockchain.get_service_node_list().active_service_nodes_infos(),
                                           context.prepare_for_round.round);

  if (!service_nodes::verify_pulse_quorum_sizes(context.prepare_for_round.quorum))
  {
    MINFO(log_prefix(context) << "Insufficient Service Nodes to execute Pulse on height " << context.wait_for_next_block.height << ", we require a PoW miner block. Sleeping until next block.");
    context.state = round_state::wait_for_next_block;
    return event_loop::keep_running;
  }

  //
  // NOTE: Quorum participation
  //
  if (key.pub == context.prepare_for_round.quorum.workers[0])
  {
    // NOTE: Producer doesn't send handshakes, they only collect the
    // handshake bitsets from the other validators to determine who to
    // lock in for this round in the block template.
    context.prepare_for_round.participant = sn_type::producer;
    context.prepare_for_round.node_name   = "W[0]";
  }
  else
  {
    for (size_t index = 0; index < context.prepare_for_round.quorum.validators.size(); index++)
    {
      auto const &validator_key = context.prepare_for_round.quorum.validators[index];
      if (validator_key == key.pub)
      {
        context.prepare_for_round.participant        = sn_type::validator;
        context.prepare_for_round.my_quorum_position = index;
        context.prepare_for_round.node_name = "V[" + std::to_string(context.prepare_for_round.my_quorum_position) + "]";
        break;
      }
    }
  }

  if (context.prepare_for_round.participant == sn_type::none)
  {
    MINFO(log_prefix(context) << "We are not a pulse validator. Waiting for next pulse round or block.");
    return goto_preparing_for_next_round(context);
  }

  context.state = round_state::wait_for_round;
  return event_loop::keep_running;
}

event_loop wait_for_round(round_context &context, cryptonote::Blockchain const &blockchain)
{
  if (context.wait_for_next_block.height != blockchain.get_current_blockchain_height(true /*lock*/))
  {
    MINFO(log_prefix(context) << "Block height changed whilst waiting for round " << +context.prepare_for_round.round << ", restarting Pulse stages");
    context.state = round_state::wait_for_next_block;
    return event_loop::keep_running;
  }

  auto start_time = context.wait_for_next_block.round_0_start_time + (context.prepare_for_round.round * service_nodes::PULSE_ROUND_TIME);
  if (auto now = pulse::clock::now(); now < start_time)
  {
    for (static uint64_t last_height = 0; last_height != context.wait_for_next_block.height; last_height = context.wait_for_next_block.height)
      MINFO(log_prefix(context) << "Waiting for Pulse round " << +context.prepare_for_round.round << " to start in " << tools::get_human_readable_timespan(start_time - now));
    return event_loop::return_to_caller;
  }

  if (context.prepare_for_round.participant == sn_type::validator)
  {
    MINFO(log_prefix(context) << "We are a pulse validator, sending handshake bit to quorum and collecting other validator handshakes.");
    context.state = round_state::submit_handshakes;
  }
  else
  {
    MINFO(log_prefix(context) << "We are the block producer for height " << context.wait_for_next_block.height << " in round " << +context.prepare_for_round.round << ", awaiting validator handshake bitsets.");
    context.state = round_state::wait_for_handshake_bitsets;
  }

  return event_loop::keep_running;
}

event_loop submit_handshakes(round_context &context, void *quorumnet_state, service_nodes::service_node_keys const &key)
{
  assert(context.prepare_for_round.participant == sn_type::validator);
  try
  {
    context.state = round_state::wait_for_handshakes;
    relay_validator_handshake_bit_or_bitset(context, quorumnet_state, key, false /*sending_bitset*/);
  }
  catch (std::exception const &e)
  {
    MERROR(log_prefix(context) << "Attempting to invoke and send a Pulse participation handshake unexpectedly failed. " << e.what());
    return goto_preparing_for_next_round(context);
  }

  return event_loop::return_to_caller;
}

event_loop wait_for_handshakes(round_context &context, void *quorumnet_state)
{
  handle_messages_received_early_for(context.wait_for_handshakes.stage, quorumnet_state);
  pulse_wait_stage const &stage = context.wait_for_handshakes.stage;

  auto const &quorum            = context.wait_for_handshakes.data;
  bool const timed_out          = pulse::clock::now() >= stage.end_time;
  bool const all_handshakes     = stage.msgs_received == quorum.size();

  assert(context.prepare_for_round.participant == sn_type::validator);
  assert(context.prepare_for_round.my_quorum_position < quorum.size());

  if (all_handshakes || timed_out)
  {
    bool missing_handshakes = timed_out && !all_handshakes;
    MINFO(log_prefix(context) << "Collected validator handshakes " << stage.bitset_view() << (missing_handshakes ? ", we timed out and some handshakes were not seen! " : ". ") << "Sending handshake bitset and collecting other validator bitsets.");
    context.state = round_state::submit_handshake_bitset;
    return event_loop::keep_running;
  }

  return event_loop::return_to_caller;
}

event_loop submit_handshake_bitset(round_context &context, void *quorumnet_state, service_nodes::service_node_keys const &key)
{
  assert(context.prepare_for_round.participant == sn_type::validator);
  try
  {
    context.state = round_state::wait_for_handshake_bitsets;
    relay_validator_handshake_bit_or_bitset(context, quorumnet_state, key, true /*sending_bitset*/);
  }
  catch(std::exception const &e)
  {
    MERROR(log_prefix(context) << "Attempting to invoke and send a Pulse validator bitset unexpectedly failed. " << e.what());
    return goto_preparing_for_next_round(context);
  }

  return event_loop::keep_running;
}

event_loop wait_for_handshake_bitsets(round_context &context, void *quorumnet_state)
{
  handle_messages_received_early_for(context.wait_for_handshake_bitsets.stage, quorumnet_state);
  pulse_wait_stage const &stage = context.wait_for_handshake_bitsets.stage;

  auto const &quorum            = context.wait_for_handshake_bitsets.data;
  bool const timed_out          = pulse::clock::now() >= stage.end_time;
  bool const all_bitsets        = stage.msgs_received == quorum.size();

  if (timed_out || all_bitsets)
  {
    bool missing_bitsets = timed_out && !all_bitsets;
    MINFO(log_prefix(context)
           << "Collected " << stage.msgs_received << "/" << quorum.size() << " handshake bitsets"
           << (missing_bitsets ? ", we timed out and some bitsets were not seen!" : ""));

    std::map<uint16_t, int> most_common_bitset;
    uint16_t best_bitset = 0;
    int count            = 0;
    for (size_t quorum_index = 0; quorum_index < quorum.size(); quorum_index++)
    {
      auto &[bitset, received] = quorum[quorum_index];
      uint16_t num             = ++most_common_bitset[bitset];
      if (received && num > count)
      {
        best_bitset = bitset;
        count       = num;
      }

      MINFO(log_prefix(context) << "Collected from V[" << quorum_index << "], handshake bitset " << std::bitset<8 * sizeof(bitset)>(bitset));
    }

    int count_threshold = (quorum.size() * 6 / 10);
    if (count < count_threshold || best_bitset == 0)
    {
      // Less than 60% of the validators can't come to agreement
      // about which validators are online, we wait until the
      // next round.
      if (best_bitset == 0)
      {
        MINFO(log_prefix(context) << count << "/" << quorum.size() << " validators did not send any handshake bitset or sent an empty handshake bitset");
      }
      else
      {
        MINFO(log_prefix(context) << "We heard back from less than " << count_threshold << " of the validators ("
                                  << count << "/" << quorum.size() << ", waiting for next round.");
      }

      return goto_preparing_for_next_round(context);
    }

    std::bitset<8 * sizeof(best_bitset)> bitset    = best_bitset;
    context.submit_block_template.validator_bitset = best_bitset;
    context.submit_block_template.validator_count  = count;

    MINFO(log_prefix(context) << count << "/" << quorum.size()
                              << " validators agreed on the participating nodes in the quorum " << bitset
                              << (context.prepare_for_round.participant == sn_type::producer
                                      ? ""
                                      : ". Awaiting block template from block producer"));

    if (context.prepare_for_round.participant == sn_type::producer)
      context.state = round_state::submit_block_template;
    else
      context.state = round_state::wait_for_block_template;

    return event_loop::keep_running;
  }

  return event_loop::return_to_caller;
}

event_loop submit_block_template(round_context &context, service_nodes::service_node_keys const &key, cryptonote::Blockchain &blockchain, void *quorumnet_state)
{
  assert(context.prepare_for_round.participant == sn_type::producer);
  std::vector<service_nodes::service_node_pubkey_info> list_state = blockchain.get_service_node_list().get_service_node_list_state({key.pub});

  // Invariants
  // TODO(doyle): These checks can be done earlier?
  if (list_state.empty())
  {
    MINFO(log_prefix(context) << "Block producer (us) is not available on the service node list, waiting until next round");
    return goto_preparing_for_next_round(context);
  }

  std::shared_ptr<const service_nodes::service_node_info> info = list_state[0].info;
  if (!info->is_active())
  {
    MINFO(log_prefix(context) << "Block producer (us) is not an active service node, waiting until next round");
    return goto_preparing_for_next_round(context);
  }

  // Block
  // TODO(doyle): Round and validator bitset should go into the create_next_pulse_block_template arguments
  cryptonote::block block = {};
  {
    uint64_t expected_reward = 0;
    service_nodes::payout block_producer_payouts = service_nodes::service_node_info_to_payout(key.pub, *info);
    blockchain.create_next_pulse_block_template(block, block_producer_payouts, context.wait_for_next_block.height, expected_reward);

    block.pulse.round            = context.prepare_for_round.round;
    block.pulse.validator_bitset = context.submit_block_template.validator_bitset;
  }

  // Message
  pulse::message msg      = {};
  msg.type                = pulse::message_type::block_template;
  msg.block_template.blob = cryptonote::t_serializable_object_to_blob(block);
  crypto::generate_signature(msg_signature_hash(context, msg), key.pub, key.key, msg.signature);

  // Send
  MINFO(log_prefix(context) << "Validators are handshaken and ready, sending block template from producer (us) to validators.\n" << cryptonote::obj_to_json_str(block));
  cryptonote::quorumnet_pulse_relay_message_to_quorum(quorumnet_state, msg, context.prepare_for_round.quorum, true /*block_producer*/);

  context.state = round_state::wait_for_next_block;
  return event_loop::keep_running;
}

event_loop wait_for_block_template(round_context &context, void *quorumnet_state)
{
  handle_messages_received_early_for(context.wait_for_block_template.stage, quorumnet_state);
  pulse_wait_stage const &stage = context.wait_for_block_template.stage;

  assert(context.prepare_for_round.participant == sn_type::validator);
  bool timed_out = pulse::clock::now() >= context.wait_for_block_template.stage.end_time;
  if (timed_out || context.wait_for_block_template.stage.msgs_received == 1)
  {
    if (context.wait_for_block_template.stage.msgs_received == 1)
    {
      // Check validator bitset after message is received incase we're abit
      // behind and still waiting to receive the bitsets from other
      // validators.
      cryptonote::block const &block = context.wait_for_block_template.block;
      if (block.pulse.validator_bitset == context.submit_block_template.validator_bitset)
      {
        MINFO(log_prefix(context) << "Valid block received: " << cryptonote::obj_to_json_str(context.wait_for_block_template.block));
      }
      else
      {
        auto block_bitset = std::bitset<sizeof(block.pulse.validator_bitset) * 8>(block.pulse.validator_bitset);
        auto our_bitset   = std::bitset<sizeof(block.pulse.validator_bitset) * 8>(context.submit_block_template.validator_bitset);
        MINFO(log_prefix(context) << "Received pulse block template specifying different validator handshake bitsets " << block_bitset << ", expected " << our_bitset);
      }
    }
    else
    {
      MINFO(log_prefix(context) << "Timed out, block template was not received");
    }

    context.state = round_state::submit_random_value_hash;
    return event_loop::keep_running;
  }

  return event_loop::return_to_caller;
}

event_loop submit_random_value_hash(round_context &context, void *quorumnet_state, service_nodes::service_node_keys const &key)
{
  assert(context.prepare_for_round.participant == sn_type::validator);

  // Random Value
  crypto::generate_random_bytes_thread_safe(sizeof(context.submit_random_value_hash.value.data), context.submit_random_value_hash.value.data);

  // Message
  pulse::message msg         = {};
  msg.type                   = pulse::message_type::random_value_hash;
  msg.quorum_position        = context.prepare_for_round.my_quorum_position;
  msg.random_value_hash.hash = crypto::cn_fast_hash(context.submit_random_value_hash.value.data, sizeof(context.submit_random_value_hash.value.data));
  crypto::generate_signature(msg_signature_hash(context, msg), key.pub, key.key, msg.signature);

  // Add Ourselves
  handle_message(nullptr /*quorumnet_state*/, msg);

  // Send
  cryptonote::quorumnet_pulse_relay_message_to_quorum(quorumnet_state, msg, context.prepare_for_round.quorum, false /*block_producer*/);
  context.state = round_state::wait_for_random_value_hashes;
  return event_loop::return_to_caller;
}

event_loop wait_for_random_value_hashes(round_context &context, void *quorumnet_state)
{
  handle_messages_received_early_for(context.wait_for_random_value_hashes.stage, quorumnet_state);
  pulse_wait_stage const &stage = context.wait_for_random_value_hashes.stage;

  auto const &quorum    = context.wait_for_random_value_hashes.data;
  bool const timed_out  = pulse::clock::now() >= stage.end_time;
  bool const all_hashes = stage.msgs_received == context.submit_block_template.validator_count;

  if (timed_out || all_hashes)
  {
    if (!enforce_validator_participation_and_timeouts(context, stage, timed_out, all_hashes))
      return goto_preparing_for_next_round(context);

    context.state         = round_state::submit_random_value;
    MINFO(log_prefix(context) << "Received " << stage.msgs_received << " random value hashes from " << stage.bitset_view() << (timed_out ? ". We timed out and some hashes are missing" : ""));
    return event_loop::keep_running;
  }

  return event_loop::return_to_caller;
}

event_loop submit_random_value(round_context &context, void *quorumnet_state, service_nodes::service_node_keys const &key)
{
  assert(context.prepare_for_round.participant == sn_type::validator);

  // Message
  pulse::message msg     = {};
  msg.type               = pulse::message_type::random_value;
  msg.quorum_position    = context.prepare_for_round.my_quorum_position;
  msg.random_value.value = context.submit_random_value_hash.value;
  crypto::generate_signature(msg_signature_hash(context, msg), key.pub, key.key, msg.signature);

  // Add Ourselves
  handle_message(nullptr /*quorumnet_state*/, msg);

  // Send
  context.state = round_state::wait_for_random_value;
  cryptonote::quorumnet_pulse_relay_message_to_quorum(quorumnet_state, msg, context.prepare_for_round.quorum, false /*block_producer*/);
  return event_loop::keep_running;
}

event_loop wait_for_random_value(round_context &context, void *quorumnet_state)
{
  handle_messages_received_early_for(context.wait_for_random_value.stage, quorumnet_state);
  pulse_wait_stage const &stage = context.wait_for_random_value.stage;

  auto const &quorum    = context.wait_for_random_value.data;
  bool const timed_out  = pulse::clock::now() >= stage.end_time;
  bool const all_values = stage.msgs_received == context.submit_block_template.validator_count;

  if (timed_out || all_values)
  {
    if (!enforce_validator_participation_and_timeouts(context, stage, timed_out, all_values))
      return goto_preparing_for_next_round(context);

    // Generate Final Random Value
    crypto::hash final_hash = {};
    for (size_t index = 0; index < quorum.size(); index++)
    {
      auto &[random_value, received] = quorum[index];
      if (received)
      {
        MDEBUG(log_prefix(context) << "Final random value seeding with V[" << index << "] " << lokimq::to_hex(tools::view_guts(random_value.data)));
        auto buf   = tools::memcpy_le(final_hash.data, random_value.data);
        final_hash = crypto::cn_fast_hash(buf.data(), buf.size());
      }
    }

    cryptonote::block &block                           = context.wait_for_block_template.block;
    cryptonote::pulse_random_value &final_random_value = block.pulse.random_value;
    std::memcpy(final_random_value.data, final_hash.data, sizeof(final_random_value.data));

    MINFO(log_prefix(context) << "Block final random value " << lokimq::to_hex(tools::view_guts(final_random_value.data)) << " generated from validators " << stage.bitset_view());
    context.submit_signed_block.blob = cryptonote::t_serializable_object_to_blob(block);
    context.state                    = round_state::submit_signed_block;
    return event_loop::keep_running;
  }

  return event_loop::return_to_caller;
}

event_loop submit_signed_block(round_context &context, void *quorumnet_state, service_nodes::service_node_keys const &key)
{
  assert(context.prepare_for_round.participant == sn_type::validator);

  // Message
  pulse::message msg  = {};
  msg.type            = pulse::message_type::signed_block;
  msg.quorum_position = context.prepare_for_round.my_quorum_position;
  crypto::generate_signature(msg_signature_hash(context, msg), key.pub, key.key, msg.signature);

  // Add Ourselves
  handle_message(nullptr /*quorumnet_state*/, msg);

  // Send
  context.state = round_state::wait_for_signed_blocks;
  cryptonote::quorumnet_pulse_relay_message_to_quorum(quorumnet_state, msg, context.prepare_for_round.quorum, false /*block_producer*/);
  return event_loop::keep_running;
}

event_loop wait_for_signed_blocks(round_context &context, void *quorumnet_state, cryptonote::core &core)
{
  handle_messages_received_early_for(context.wait_for_signed_blocks.stage, quorumnet_state);
  pulse_wait_stage const &stage = context.wait_for_signed_blocks.stage;

  auto const &quorum   = context.wait_for_signed_blocks.data;
  bool const timed_out = pulse::clock::now() >= stage.end_time;
  bool const enough    = stage.msgs_received >= context.submit_block_template.validator_count;

  if (timed_out || enough)
  {
    if (!enforce_validator_participation_and_timeouts(context, stage, timed_out, enough))
      return goto_preparing_for_next_round(context);

    // Select signatures randomly so we don't always just take the first N required signatures.
    // Then sort just the first N required signatures, so signatures are added
    // to the block in sorted order, but were chosen randomly.
    std::array<size_t, service_nodes::PULSE_QUORUM_NUM_VALIDATORS> indices = {};
    std::iota(indices.begin(), indices.end(), 0);
    tools::shuffle_portable(indices.begin(), indices.end(), tools::rng);
    std::sort(indices.begin(), indices.begin() + service_nodes::PULSE_BLOCK_REQUIRED_SIGNATURES);

    // Add Signatures
    cryptonote::block &final_block = context.wait_for_block_template.block;
    for (size_t index = 0; index < service_nodes::PULSE_BLOCK_REQUIRED_SIGNATURES; index++)
    {
      uint16_t validator_index          = indices[index];
      auto const &[signature, received] = quorum[validator_index];
      assert(received);
      final_block.signatures.emplace_back(validator_index, signature);
    }

    // Propagate Final Block
    MINFO(log_prefix(context) << "Final signed block received\n" << cryptonote::obj_to_json_str(final_block));
    cryptonote::block_verification_context bvc = {};
    core.handle_block_found(final_block, bvc);

    context.state = round_state::wait_for_next_block;
    return event_loop::keep_running;
  }

  return event_loop::return_to_caller;
}

void pulse::main(void *quorumnet_state, cryptonote::core &core)
{
  cryptonote::Blockchain &blockchain          = core.get_blockchain_storage();
  service_nodes::service_node_keys const &key = core.get_service_keys();

  //
  // NOTE: Early exit if too early
  //
  static uint64_t const hf16_height = cryptonote::HardFork::get_hardcoded_hard_fork_height(blockchain.nettype(), cryptonote::network_version_16);
  if (hf16_height == cryptonote::HardFork::INVALID_HF_VERSION_HEIGHT)
  {
    for (static bool once = true; once; once = !once)
      MERROR("Pulse: HF16 is not defined, pulse worker waiting");
    return;
  }

  if (uint64_t height = blockchain.get_current_blockchain_height(true /*lock*/); height < hf16_height)
  {
    for (static bool once = true; once; once = !once)
      MINFO("Pulse: Network at block " << height << " is not ready for Pulse until block " << hf16_height << ", waiting");
    return;
  }

  for (auto loop = event_loop::keep_running; loop == event_loop::keep_running;)
  {
    // TODO(doyle): Combine submit and wait stages. Submit goes straight to wait
    // stage, so instead of returning, looping in here again and
    // heading to the next state just execute the next state.

    // With that we can get rid of event_loop
    switch (context.state)
    {
      case round_state::wait_for_next_block:
        loop = wait_for_next_block(hf16_height, context, blockchain);
        break;

      case round_state::prepare_for_round:
        loop = prepare_for_round(context, key, blockchain);
        break;

      case round_state::wait_for_round:
        loop = wait_for_round(context, blockchain);
        break;

      case round_state::submit_handshakes:
        loop = submit_handshakes(context, quorumnet_state, key);
        break;

      case round_state::wait_for_handshakes:
        loop = wait_for_handshakes(context, quorumnet_state);
        break;

      case round_state::submit_handshake_bitset:
        loop = submit_handshake_bitset(context, quorumnet_state, key);
        break;

      case round_state::wait_for_handshake_bitsets:
        loop = wait_for_handshake_bitsets(context, quorumnet_state);
        break;

      case round_state::submit_block_template:
        loop = submit_block_template(context, key, blockchain, quorumnet_state);
        break;

      case round_state::wait_for_block_template:
        loop = wait_for_block_template(context, quorumnet_state);
        break;

      case round_state::submit_random_value_hash:
        loop = submit_random_value_hash(context, quorumnet_state, key);
        break;

      case round_state::wait_for_random_value_hashes:
        loop = wait_for_random_value_hashes(context, quorumnet_state);
        break;

      case round_state::submit_random_value:
        loop = submit_random_value(context, quorumnet_state, key);
        break;

      case round_state::wait_for_random_value:
        loop = wait_for_random_value(context, quorumnet_state);
        break;

      case round_state::submit_signed_block:
        loop = submit_signed_block(context, quorumnet_state, key);
        break;

      case round_state::wait_for_signed_blocks:
        loop = wait_for_signed_blocks(context, quorumnet_state, core);
        break;
    }
  }
}

