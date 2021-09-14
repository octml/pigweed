// Copyright 2021 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_log_rpc/log_service.h"

#include <array>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "pw_containers/vector.h"
#include "pw_log/log.h"
#include "pw_log/proto/log.pwpb.h"
#include "pw_log/proto_utils.h"
#include "pw_protobuf/decoder.h"
#include "pw_result/result.h"
#include "pw_rpc/raw/test_method_context.h"
#include "pw_string/string_builder.h"
#include "pw_sync/mutex.h"

namespace pw::log_rpc {
namespace {

#define LOG_SERVICE_METHOD_CONTEXT \
  PW_RAW_TEST_METHOD_CONTEXT(LogService, Listen)

constexpr size_t kMaxMessageSize = 50;
static_assert(RpcLogDrain::kMaxDropMessageSize < kMaxMessageSize);
constexpr size_t kMaxLogEntrySize =
    RpcLogDrain::kMinEntrySizeWithoutPayload + kMaxMessageSize;
constexpr size_t kMultiSinkBufferSize = kMaxLogEntrySize * 10;
constexpr size_t kMaxDrains = 3;
constexpr char kMessage[] = "message";
// A message small enough to fit encoded in LogServiceTest::entry_encode_buffer_
// but large enough to not fit in LogServiceTest::small_buffer_.
constexpr char kLongMessage[] =
    "This is a long log message that will be dropped.";
static_assert(sizeof(kLongMessage) < kMaxMessageSize);
static_assert(sizeof(kLongMessage) > RpcLogDrain::kMaxDropMessageSize);
std::array<std::byte, 1> rpc_request_buffer;

// `LogServiceTest` sets up a logging environment for testing with a `MultiSink`
// for log entries, and multiple `RpcLogDrain`s for consuming such log entries.
// It includes methods to add log entries to the `MultiSink`, and buffers for
// encoding and retrieving log entries. Tests can choose how many entries to
// add to the multisink, and which drain to use.
class LogServiceTest : public ::testing::Test {
 public:
  LogServiceTest() : multisink_(multisink_buffer_), drain_map_(drains_) {
    for (auto& drain : drain_map_.drains()) {
      multisink_.AttachDrain(drain);
    }
  }

  void AddLogEntries(size_t log_count, std::string_view message) {
    for (size_t i = 0; i < log_count; ++i) {
      AddLogEntry(message);
    }
  }

  void AddLogEntry(std::string_view message) {
    auto metadata =
        log_tokenized::Metadata::Set<PW_LOG_LEVEL_WARN, __LINE__, 0, 0>();
    Result<ConstByteSpan> encoded_log_result =
        log::EncodeTokenizedLog(metadata,
                                std::as_bytes(std::span(message)),
                                /*ticks_since_epoch=*/0,
                                entry_encode_buffer_);
    EXPECT_EQ(encoded_log_result.status(), OkStatus());
    multisink_.HandleEntry(encoded_log_result.value());
  }

 protected:
  std::array<std::byte, kMultiSinkBufferSize> multisink_buffer_;
  multisink::MultiSink multisink_;
  RpcLogDrainMap drain_map_;
  std::array<std::byte, kMaxLogEntrySize> entry_encode_buffer_;

  // Drain Buffers
  std::array<std::byte, kMaxLogEntrySize> drain_buffer1_;
  std::array<std::byte, kMaxLogEntrySize> drain_buffer2_;
  std::array<std::byte, RpcLogDrain::kMinEntryBufferSize> small_buffer_;
  static constexpr uint32_t kSmallBufferDrainId = 3;
  sync::Mutex shared_mutex_;
  std::array<RpcLogDrain, kMaxDrains> drains_{
      RpcLogDrain(1,
                  drain_buffer1_,
                  shared_mutex_,
                  RpcLogDrain::LogDrainErrorHandling::kIgnoreWriterErrors),
      RpcLogDrain(
          2,
          drain_buffer2_,
          shared_mutex_,
          RpcLogDrain::LogDrainErrorHandling::kCloseStreamOnWriterError),
      RpcLogDrain(kSmallBufferDrainId,
                  small_buffer_,
                  shared_mutex_,
                  RpcLogDrain::LogDrainErrorHandling::kIgnoreWriterErrors),
  };
};

// Unpacks a `LogEntry` proto buffer and compares it with the expected data.
void VerifyLogEntry(protobuf::Decoder& entry_decoder,
                    log_tokenized::Metadata expected_metadata,
                    ConstByteSpan expected_tokenized_data,
                    const int64_t expected_timestamp) {
  ConstByteSpan tokenized_data;
  EXPECT_TRUE(entry_decoder.Next().ok());  // message [tokenized]
  EXPECT_EQ(1U, entry_decoder.FieldNumber());
  EXPECT_TRUE(entry_decoder.ReadBytes(&tokenized_data).ok());
  EXPECT_EQ(tokenized_data.size(), expected_tokenized_data.size());
  EXPECT_EQ(std::memcmp(tokenized_data.begin(),
                        expected_tokenized_data.begin(),
                        expected_tokenized_data.size()),
            0);

  uint32_t line_level;
  EXPECT_TRUE(entry_decoder.Next().ok());  // line_level
  EXPECT_EQ(2U, entry_decoder.FieldNumber());
  EXPECT_TRUE(entry_decoder.ReadUint32(&line_level).ok());
  EXPECT_EQ(expected_metadata.level(), line_level & PW_LOG_LEVEL_BITMASK);
  EXPECT_EQ(expected_metadata.line_number(),
            (line_level & ~PW_LOG_LEVEL_BITMASK) >> PW_LOG_LEVEL_BITS);

  if (expected_metadata.flags() != 0) {
    uint32_t flags;
    EXPECT_TRUE(entry_decoder.Next().ok());  // flags
    EXPECT_EQ(3U, entry_decoder.FieldNumber());
    EXPECT_TRUE(entry_decoder.ReadUint32(&flags).ok());
    EXPECT_EQ(expected_metadata.flags(), flags);
  }

  const bool has_timestamp = entry_decoder.Next().ok();  // timestamp
  if (expected_timestamp == 0 && !has_timestamp) {
    return;
  }
  int64_t timestamp;
  EXPECT_TRUE(has_timestamp);
  EXPECT_EQ(4U, entry_decoder.FieldNumber());
  EXPECT_TRUE(entry_decoder.ReadInt64(&timestamp).ok());
  EXPECT_EQ(expected_timestamp, timestamp);
}

// Verifies a stream of log entries, returning the total count found.
size_t VerifyLogEntries(protobuf::Decoder& entries_decoder,
                        Vector<ConstByteSpan>& message_stack) {
  size_t entries_found = 0;
  while (entries_decoder.Next().ok()) {
    ConstByteSpan entry;
    EXPECT_TRUE(entries_decoder.ReadBytes(&entry).ok());
    protobuf::Decoder entry_decoder(entry);
    auto expected_metadata =
        log_tokenized::Metadata::Set<PW_LOG_LEVEL_WARN, __LINE__, 0, 0>();
    if (message_stack.empty()) {
      break;
    }
    ConstByteSpan expected_message = message_stack.back();
    VerifyLogEntry(entry_decoder,
                   expected_metadata,
                   expected_message,
                   /*expected_timestamp=*/0);
    message_stack.pop_back();
    ++entries_found;
  }
  return entries_found;
}

TEST_F(LogServiceTest, AssignWriter) {
  // Drains don't have writers.
  for (auto& drain : drain_map_.drains()) {
    EXPECT_EQ(drain.Flush(), Status::Unavailable());
  }

  // Create context directed to drain with ID 1.
  RpcLogDrain& active_drain = drains_[0];
  const uint32_t drain_channel_id = active_drain.channel_id();
  LOG_SERVICE_METHOD_CONTEXT context(drain_map_);
  context.set_channel_id(drain_channel_id);

  // Call RPC, which sets the drain's writer.
  context.call(rpc_request_buffer);
  EXPECT_EQ(active_drain.Flush(), OkStatus());

  // Other drains are still missing writers.
  for (auto& drain : drain_map_.drains()) {
    if (drain.channel_id() != drain_channel_id) {
      EXPECT_EQ(drain.Flush(), Status::Unavailable());
    }
  }

  // Calling an ongoing log stream must not change the active drain's
  // writer, and the second writer must not get any responses.
  LOG_SERVICE_METHOD_CONTEXT second_call_context(drain_map_);
  second_call_context.set_channel_id(drain_channel_id);
  second_call_context.call(rpc_request_buffer);
  EXPECT_EQ(active_drain.Flush(), OkStatus());
  ASSERT_TRUE(second_call_context.done());
  EXPECT_EQ(second_call_context.responses().size(), 0u);

  // Setting a new writer on a closed stream is allowed.
  ASSERT_EQ(active_drain.Close(), OkStatus());
  LOG_SERVICE_METHOD_CONTEXT third_call_context(drain_map_);
  third_call_context.set_channel_id(drain_channel_id);
  third_call_context.call(rpc_request_buffer);
  EXPECT_EQ(active_drain.Flush(), OkStatus());
  ASSERT_FALSE(third_call_context.done());
  EXPECT_EQ(third_call_context.responses().size(), 1u);
  EXPECT_EQ(active_drain.Close(), OkStatus());
}

TEST_F(LogServiceTest, StartAndEndStream) {
  RpcLogDrain& active_drain = drains_[2];
  const uint32_t drain_channel_id = active_drain.channel_id();
  LOG_SERVICE_METHOD_CONTEXT context(drain_map_);
  context.set_channel_id(drain_channel_id);

  // Add log entries.
  const size_t total_entries = 10;
  AddLogEntries(total_entries, kMessage);
  // Request logs.
  context.call(rpc_request_buffer);
  EXPECT_EQ(active_drain.Flush(), OkStatus());

  // Not done until the stream is finished.
  ASSERT_FALSE(context.done());
  active_drain.Close();
  ASSERT_TRUE(context.done());

  EXPECT_EQ(context.status(), OkStatus());
  // There is at least 1 response with multiple log entries packed.
  EXPECT_GE(context.responses().size(), 1u);

  // Verify data in responses.
  Vector<ConstByteSpan, total_entries> message_stack;
  for (size_t i = 0; i < total_entries; ++i) {
    message_stack.push_back(
        std::as_bytes(std::span(std::string_view(kMessage))));
  }
  size_t entries_found = 0;
  for (auto& response : context.responses()) {
    protobuf::Decoder entry_decoder(response);
    entries_found += VerifyLogEntries(entry_decoder, message_stack);
  }
  EXPECT_EQ(entries_found, total_entries);
}

TEST_F(LogServiceTest, HandleDropped) {
  RpcLogDrain& active_drain = drains_[0];
  const uint32_t drain_channel_id = active_drain.channel_id();
  LOG_SERVICE_METHOD_CONTEXT context(drain_map_);
  context.set_channel_id(drain_channel_id);

  // Add log entries.
  const size_t total_entries = 5;
  const uint32_t total_drop_count = 2;
  AddLogEntries(total_entries, kMessage);
  multisink_.HandleDropped(total_drop_count);

  // Request logs.
  context.call(rpc_request_buffer);
  EXPECT_EQ(active_drain.Flush(), OkStatus());
  active_drain.Close();
  ASSERT_EQ(context.status(), OkStatus());
  // There is at least 1 response with multiple log entries packed.
  ASSERT_GE(context.responses().size(), 1u);

  // Add create expected messages in a stack to match the order they arrive in.
  Vector<ConstByteSpan, total_entries + 1> message_stack;
  StringBuffer<32> message;
  message.Format("Dropped %u", static_cast<unsigned int>(total_drop_count));
  message_stack.push_back(std::as_bytes(std::span(std::string_view(message))));
  for (size_t i = 0; i < total_entries; ++i) {
    message_stack.push_back(
        std::as_bytes(std::span(std::string_view(kMessage))));
  }

  // Verify data in responses.
  size_t entries_found = 0;
  for (auto& response : context.responses()) {
    protobuf::Decoder entry_decoder(response);
    entries_found += VerifyLogEntries(entry_decoder, message_stack);
  }
  // Expect an extra message with the drop count.
  EXPECT_EQ(entries_found, total_entries + 1);
}

TEST_F(LogServiceTest, HandleSmallBuffer) {
  LOG_SERVICE_METHOD_CONTEXT context(drain_map_);
  context.set_channel_id(kSmallBufferDrainId);
  auto small_buffer_drain =
      drain_map_.GetDrainFromChannelId(kSmallBufferDrainId);
  ASSERT_TRUE(small_buffer_drain.ok());

  // Add log entries.
  const size_t total_entries = 5;
  const uint32_t total_drop_count = total_entries;
  AddLogEntries(total_entries, kLongMessage);
  // Request logs.
  context.call(rpc_request_buffer);
  EXPECT_EQ(small_buffer_drain.value()->Flush(), OkStatus());
  EXPECT_EQ(small_buffer_drain.value()->Close(), OkStatus());
  ASSERT_EQ(context.status(), OkStatus());
  ASSERT_GE(context.responses().size(), 1u);

  Vector<ConstByteSpan, total_entries + 1> message_stack;
  StringBuffer<32> message;
  message.Format("Dropped %u", static_cast<unsigned int>(total_drop_count));
  message_stack.push_back(std::as_bytes(std::span(std::string_view(message))));

  // Verify data in responses.
  size_t entries_found = 0;
  for (auto& response : context.responses()) {
    protobuf::Decoder entry_decoder(response);
    entries_found += VerifyLogEntries(entry_decoder, message_stack);
  }
  // No messages fit the buffer, expect a drop message.
  EXPECT_EQ(entries_found, 1u);
}

TEST_F(LogServiceTest, FlushDrainWithoutMultisink) {
  auto& detached_drain = drains_[0];
  multisink_.DetachDrain(detached_drain);
  LOG_SERVICE_METHOD_CONTEXT context(drain_map_);
  context.set_channel_id(detached_drain.channel_id());

  // Add log entries.
  const size_t total_entries = 5;
  AddLogEntries(total_entries, kMessage);
  // Request logs.
  context.call(rpc_request_buffer);
  EXPECT_EQ(detached_drain.Close(), OkStatus());
  ASSERT_EQ(context.status(), OkStatus());
  EXPECT_EQ(context.responses().size(), 0u);
}

TEST_F(LogServiceTest, LargeLogEntry) {
  const auto expected_metadata =
      log_tokenized::Metadata::Set<PW_LOG_LEVEL_WARN,
                                   (1 << PW_LOG_TOKENIZED_MODULE_BITS) - 1,
                                   (1 << PW_LOG_TOKENIZED_FLAG_BITS) - 1,
                                   (1 << PW_LOG_TOKENIZED_LINE_BITS) - 1>();
  ConstByteSpan expected_message = std::as_bytes(std::span(kMessage));
  const int64_t expected_timestamp = std::numeric_limits<int64_t>::max();

  // Add entry to multisink.
  log::LogEntry::MemoryEncoder encoder(entry_encode_buffer_);
  encoder.WriteMessage(expected_message);
  encoder.WriteLineLevel(
      (expected_metadata.level() & PW_LOG_LEVEL_BITMASK) |
      ((expected_metadata.line_number() << PW_LOG_LEVEL_BITS) &
       ~PW_LOG_LEVEL_BITMASK));
  encoder.WriteFlags(expected_metadata.flags());
  encoder.WriteTimestamp(expected_timestamp);
  ASSERT_EQ(encoder.status(), OkStatus());
  multisink_.HandleEntry(encoder);

  // Start log stream.
  RpcLogDrain& active_drain = drains_[0];
  const uint32_t drain_channel_id = active_drain.channel_id();
  LOG_SERVICE_METHOD_CONTEXT context(drain_map_);
  context.set_channel_id(drain_channel_id);
  context.call(rpc_request_buffer);
  ASSERT_EQ(active_drain.Flush(), OkStatus());
  active_drain.Close();
  ASSERT_EQ(context.status(), OkStatus());
  ASSERT_EQ(context.responses().size(), 1u);

  // Verify message.
  protobuf::Decoder entries_decoder(context.responses()[0]);
  ASSERT_TRUE(entries_decoder.Next().ok());
  ConstByteSpan entry;
  EXPECT_TRUE(entries_decoder.ReadBytes(&entry).ok());
  protobuf::Decoder entry_decoder(entry);
  VerifyLogEntry(
      entry_decoder, expected_metadata, expected_message, expected_timestamp);
}

// TODO(pwbug/469): add tests for an open RawServerWriter that closes or fails
// while flushing, then on re-open the drain sends a counts. The drain mus have
// ignore_writer_error disabled.

}  // namespace
}  // namespace pw::log_rpc