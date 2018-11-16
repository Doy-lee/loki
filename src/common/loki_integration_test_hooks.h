#define LOKI_ENABLE_INTEGRATION_TEST_HOOKS
#if defined(LOKI_ENABLE_INTEGRATION_TEST_HOOKS)

#ifndef LOKI_INTEGRATION_TEST_HOOKS_H
#define LOKI_INTEGRATION_TEST_HOOKS_H

//
// Header
//
#include <stdint.h>
#include <sstream>
#include <iostream>

namespace loki
{
struct fixed_buffer
{
  static const int SIZE = 8192;
  char data[SIZE];
  int  len;
};

void         use_standard_cout();
void         use_redirected_cout();

enum struct shared_mem_type { default_type, wallet, daemon };
void         init_integration_test_context        (shared_mem_type type);
void         write_to_stdout_shared_mem           (char const *buf, int buf_len, shared_mem_type type = shared_mem_type::default_type);
void         write_to_stdout_shared_mem           (std::string const &input, shared_mem_type type = shared_mem_type::default_type);
fixed_buffer read_from_stdin_shared_mem           (shared_mem_type type = shared_mem_type::default_type);
void         write_redirected_stdout_to_shared_mem(shared_mem_type type = shared_mem_type::default_type);
}; // namespace loki

#endif // LOKI_INTEGRATION_TEST_HOOKS_H

//
// CPP Implementation
//
#ifdef LOKI_INTEGRATION_TEST_HOOKS_IMPLEMENTATION
#include <string.h>
#include <assert.h>
#include <chrono>
#include <thread>

#define SHOOM_IMPLEMENTATION
#include "shoom.h"

static std::ostringstream     global_redirected_cout;
static std::streambuf        *global_std_cout;
static loki::shared_mem_type  global_default_type;
static shoom::Shm             global_wallet_stdout_shared_mem{"loki_integration_testing_wallet_stdout", 8192};
static shoom::Shm             global_wallet_stdin_shared_mem {"loki_integration_testing_wallet_stdin",  8192};
static shoom::Shm             global_daemon_stdout_shared_mem{"loki_integration_testing_daemon_stdout", 8192};
static shoom::Shm             global_daemon_stdin_shared_mem {"loki_integration_testing_daemon_stdin",  8192};

enum struct shared_mem_create { yes, no };
void loki::use_standard_cout()   { if (!global_std_cout) { global_std_cout = std::cout.rdbuf(); } std::cout.rdbuf(global_std_cout); }
void loki::use_redirected_cout() { if (!global_std_cout) { global_std_cout = std::cout.rdbuf(); } std::cout.rdbuf(global_redirected_cout.rdbuf()); }

void loki::init_integration_test_context(shared_mem_type type)
{
  static bool init = false;
  if (init)
    return;

  assert(type != shared_mem_type::default_type);

  init                = true;
  global_default_type = type;

  if (type == shared_mem_type::daemon)
  {
    global_daemon_stdout_shared_mem.Create(shoom::Flag::create);
    while (global_daemon_stdin_shared_mem.Open() != 0)
    {
      static bool once_only = true;
      if (once_only)
      {
        once_only = false;
        printf("Loki Integration Test: Shared memory %s has not been created yet, blocking ...\n", global_daemon_stdin_shared_mem.Path().c_str());
      }
    }
  }
  else
  {
    global_wallet_stdout_shared_mem.Create(shoom::Flag::create | shoom::Flag::clear_on_create);
    while (global_wallet_stdin_shared_mem.Open() != 0)
    {
      static bool once_only = true;
      if (once_only)
      {
        once_only = false;
        printf("Loki Integration Test: Shared memory %s has not been created yet, blocking ...\n", global_wallet_stdin_shared_mem.Path().c_str());
      }
    }
  }
  global_std_cout = std::cout.rdbuf();

  printf("Loki Integration Test: Hooks initialised into shared memory stdin/stdout\n");
}

uint32_t const MSG_MAGIC_BYTES = 0x7428da3f;
static void make_message(char *msg_buf, int msg_buf_len, char const *msg_data, int msg_data_len)
{
  uint64_t timestamp = time(nullptr);
  int total_len      = static_cast<int>(sizeof(timestamp) + sizeof(MSG_MAGIC_BYTES) + msg_data_len);
  assert(total_len < msg_buf_len);

  char *ptr = msg_buf;
  memcpy(ptr, &timestamp, sizeof(timestamp));
  ptr += sizeof(timestamp);

  memcpy(ptr, (char *)&MSG_MAGIC_BYTES, sizeof(MSG_MAGIC_BYTES));
  ptr += sizeof(MSG_MAGIC_BYTES);

  memcpy(ptr, msg_data, msg_data_len);
  ptr += sizeof(msg_data);

  msg_buf[total_len] = 0;
}

static char const *parse_message(char const *msg_buf, int msg_buf_len, uint64_t *timestamp)
{
  char const *ptr = msg_buf;
  *timestamp = *((uint64_t const *)ptr);
  ptr += sizeof(*timestamp);

  if ((*(uint32_t const *)ptr) != MSG_MAGIC_BYTES)
    return nullptr;

  ptr += sizeof(MSG_MAGIC_BYTES);
  assert(ptr < msg_buf + msg_buf_len);
  return ptr;
}

enum struct stdin_or_out { in, out };
static shoom::Shm *get_shared_mem(loki::shared_mem_type type, stdin_or_out in_out)
{
  if (type == loki::shared_mem_type::default_type)
    type = global_default_type;

  shoom::Shm *result = nullptr;
  if (type == loki::shared_mem_type::wallet)
    result = (in_out == stdin_or_out::in) ? &global_wallet_stdin_shared_mem : &global_wallet_stdout_shared_mem;
  else
    result = (in_out == stdin_or_out::in) ? &global_daemon_stdin_shared_mem : &global_daemon_stdout_shared_mem;

  return result;
}

void loki::write_to_stdout_shared_mem(char const *buf, int buf_len, shared_mem_type type)
{
  shoom::Shm *shared_mem = get_shared_mem(type, stdin_or_out::out);
  if (shared_mem)
  {
    make_message(reinterpret_cast<char *>(shared_mem->Data()), shared_mem->Size(), buf, buf_len);
  }
}

void loki::write_to_stdout_shared_mem(std::string const &input, shared_mem_type type)
{
  write_to_stdout_shared_mem(input.c_str(), input.size(), type);
}

loki::fixed_buffer loki::read_from_stdin_shared_mem(shared_mem_type type)
{
  uint64_t timestamp       = 0;
  uint64_t *last_timestamp = nullptr;
  char const *input        = nullptr;

  static uint64_t wallet_last_timestamp = 0;
  static uint64_t daemon_last_timestamp = 0;

  if (type == shared_mem_type::default_type)
    type = global_default_type;

  shoom::Shm *shared_mem = get_shared_mem(type, stdin_or_out::in);
  if (type == shared_mem_type::wallet) last_timestamp = &wallet_last_timestamp;
  else                                 last_timestamp = &daemon_last_timestamp;

  for (;;)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    shared_mem->Open();
    char const *data = reinterpret_cast<char const *>(shared_mem->Data());
    input = parse_message(data, shared_mem->Size(), &timestamp);

    if (input && (*last_timestamp) != timestamp)
    {
      *last_timestamp = timestamp;
      break;
    }
  }

  fixed_buffer result = {};
  result.len = strlen(input);
  assert(result.len <= fixed_buffer::SIZE);
  memcpy(result.data, input, result.len);
  return result;
}

void loki::write_redirected_stdout_to_shared_mem(shared_mem_type type)
{
  std::string output = global_redirected_cout.str();
  global_redirected_cout.flush();
  global_redirected_cout.str("");
  global_redirected_cout.clear();
  loki::write_to_stdout_shared_mem(output, type);

  loki::use_standard_cout();
  std::cout << output << std::endl;
  loki::use_redirected_cout();
}
#endif // LOKI_INTEGRATION_TEST_HOOKS_IMPLEMENTATION
#endif // LOKI_ENABLE_INTEGRATION_TEST_HOOKS

