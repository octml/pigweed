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

#include "pw_protobuf/stream_decoder.h"

#include "pw_assert/check.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_status/try.h"
#include "pw_varint/stream.h"
#include "pw_varint/varint.h"

namespace pw::protobuf {

Status StreamDecoder::BytesReader::DoSeek(ptrdiff_t offset, Whence origin) {
  PW_TRY(status_);
  if (!decoder_.reader_.seekable()) {
    return Status::Unimplemented();
  }

  ptrdiff_t absolute_position = std::numeric_limits<ptrdiff_t>::min();

  // Convert from the position within the bytes field to the position within the
  // proto stream.
  switch (origin) {
    case Whence::kBeginning:
      absolute_position = start_offset_ + offset;
      break;

    case Whence::kCurrent:
      absolute_position = decoder_.position_ + offset;
      break;

    case Whence::kEnd:
      absolute_position = end_offset_ + offset;
      break;
  }

  if (absolute_position < 0) {
    return Status::InvalidArgument();
  }

  if (static_cast<size_t>(absolute_position) < start_offset_ ||
      static_cast<size_t>(absolute_position) >= end_offset_) {
    return Status::OutOfRange();
  }

  PW_TRY(decoder_.reader_.Seek(absolute_position, Whence::kBeginning));
  decoder_.position_ = absolute_position;
  return OkStatus();
}

StatusWithSize StreamDecoder::BytesReader::DoRead(ByteSpan destination) {
  if (!status_.ok()) {
    return StatusWithSize(status_, 0);
  }

  // Bound the read buffer to the size of the bytes field.
  size_t max_length = end_offset_ - decoder_.position_;
  if (destination.size() > max_length) {
    destination = destination.first(max_length);
  }

  Result<ByteSpan> result = decoder_.reader_.Read(destination);
  if (!result.ok()) {
    return StatusWithSize(result.status(), 0);
  }

  decoder_.position_ += result.value().size();
  return StatusWithSize(result.value().size());
}

StreamDecoder::~StreamDecoder() {
  if (parent_ != nullptr) {
    parent_->CloseNestedDecoder(*this);
  }
}

Status StreamDecoder::Next() {
  PW_CHECK(!nested_reader_open_,
           "Cannot use parent decoder while a nested one is open");

  PW_TRY(status_);

  if (!field_consumed_) {
    PW_TRY(SkipField());
  }

  if (position_ >= stream_bounds_.high) {
    return Status::OutOfRange();
  }

  status_ = ReadFieldKey();
  return status_;
}

Result<int32_t> StreamDecoder::ReadInt32() {
  uint64_t varint = 0;
  PW_TRY(ReadVarintField(&varint));

  int64_t signed_value = static_cast<int64_t>(varint);
  if (signed_value > std::numeric_limits<int32_t>::max() ||
      signed_value < std::numeric_limits<int32_t>::min()) {
    return Status::OutOfRange();
  }

  return signed_value;
}

Result<uint32_t> StreamDecoder::ReadUint32() {
  uint64_t varint = 0;
  PW_TRY(ReadVarintField(&varint));

  if (varint > std::numeric_limits<uint32_t>::max()) {
    return Status::OutOfRange();
  }
  return varint;
}

Result<int64_t> StreamDecoder::ReadInt64() {
  uint64_t varint = 0;
  PW_TRY(ReadVarintField(&varint));
  return varint;
}

Result<int32_t> StreamDecoder::ReadSint32() {
  uint64_t varint = 0;
  PW_TRY(ReadVarintField(&varint));

  int64_t signed_value = varint::ZigZagDecode(varint);
  if (signed_value > std::numeric_limits<int32_t>::max() ||
      signed_value < std::numeric_limits<int32_t>::min()) {
    return Status::OutOfRange();
  }
  return signed_value;
}

Result<int64_t> StreamDecoder::ReadSint64() {
  uint64_t varint = 0;
  PW_TRY(ReadVarintField(&varint));
  return varint::ZigZagDecode(varint);
}

Result<bool> StreamDecoder::ReadBool() {
  uint64_t varint = 0;
  PW_TRY(ReadVarintField(&varint));
  return varint;
}

StreamDecoder::BytesReader StreamDecoder::GetBytesReader() {
  Status status = CheckOkToRead(WireType::kDelimited);

  if (reader_.ConservativeReadLimit() < delimited_field_size_) {
    status.Update(Status::DataLoss());
  }

  nested_reader_open_ = true;

  if (!status.ok()) {
    return BytesReader(*this, status);
  }

  size_t low = position_;
  size_t high = low + delimited_field_size_;

  return BytesReader(*this, low, high);
}

StreamDecoder StreamDecoder::GetNestedDecoder() {
  Status status = CheckOkToRead(WireType::kDelimited);

  if (reader_.ConservativeReadLimit() < delimited_field_size_) {
    status.Update(Status::DataLoss());
  }

  nested_reader_open_ = true;

  if (!status.ok()) {
    return StreamDecoder(reader_, this, status);
  }

  size_t low = position_;
  size_t high = low + delimited_field_size_;

  return StreamDecoder(reader_, this, low, high);
}

Status StreamDecoder::Advance(size_t end_position) {
  if (reader_.seekable()) {
    PW_TRY(reader_.Seek(end_position - position_, stream::Stream::kCurrent));
    position_ = end_position;
    return OkStatus();
  }

  while (position_ < end_position) {
    std::byte b;
    PW_TRY(reader_.Read(std::span(&b, 1)));
    position_++;
  }
  return OkStatus();
}

void StreamDecoder::CloseBytesReader(BytesReader& reader) {
  status_ = reader.status_;
  if (status_.ok()) {
    // Advance the stream to the end of the bytes field.
    // The BytesReader already updated our position_ field as bytes were read.
    PW_CHECK(Advance(reader.end_offset_).ok());
  }

  field_consumed_ = true;
  nested_reader_open_ = false;
}

void StreamDecoder::CloseNestedDecoder(StreamDecoder& nested) {
  PW_CHECK_PTR_EQ(nested.parent_, this);

  nested.nested_reader_open_ = true;
  nested.parent_ = nullptr;

  status_ = nested.status_;
  position_ = nested.position_;
  if (status_.ok()) {
    // Advance the stream to the end of the nested message field.
    PW_CHECK(Advance(nested.stream_bounds_.high).ok());
  }

  field_consumed_ = true;
  nested_reader_open_ = false;
}

Status StreamDecoder::ReadFieldKey() {
  PW_DCHECK(field_consumed_);

  uint64_t varint = 0;
  PW_TRY_ASSIGN(size_t bytes_read, varint::Read(reader_, &varint));
  position_ += bytes_read;

  if (!FieldKey::IsValidKey(varint)) {
    return Status::DataLoss();
  }

  current_field_ = FieldKey(varint);

  if (current_field_.wire_type() == WireType::kDelimited) {
    // Read the length varint of length-delimited fields immediately to simplify
    // later processing of the field.
    PW_TRY_ASSIGN(bytes_read, varint::Read(reader_, &varint));
    position_ += bytes_read;

    if (varint > std::numeric_limits<uint32_t>::max()) {
      return Status::DataLoss();
    }

    delimited_field_size_ = varint;
    delimited_field_offset_ = position_;
  }

  field_consumed_ = false;
  return OkStatus();
}

Result<StreamDecoder::Bounds> StreamDecoder::GetLengthDelimitedPayloadBounds() {
  PW_TRY(CheckOkToRead(WireType::kDelimited));
  return StreamDecoder::Bounds{delimited_field_offset_,
                               delimited_field_size_ + delimited_field_offset_};
}

// Consumes the current protobuf field, advancing the stream to the key of the
// next field (if one exists).
Status StreamDecoder::SkipField() {
  PW_DCHECK(!field_consumed_);

  size_t bytes_to_skip = 0;
  uint64_t value = 0;

  switch (current_field_.wire_type()) {
    case WireType::kVarint: {
      // Consume the varint field; nothing more to skip afterward.
      PW_TRY_ASSIGN(size_t bytes_read, varint::Read(reader_, &value));
      position_ += bytes_read;
      break;
    }
    case WireType::kDelimited:
      bytes_to_skip = delimited_field_size_;
      break;

    case WireType::kFixed32:
      bytes_to_skip = sizeof(uint32_t);
      break;

    case WireType::kFixed64:
      bytes_to_skip = sizeof(uint64_t);
      break;
  }

  if (bytes_to_skip > 0) {
    // Check if the stream has the field available. If not, report it as a
    // DATA_LOSS since the proto is invalid (as opposed to OUT_OF_BOUNDS if we
    // just tried to seek beyond the end).
    if (reader_.ConservativeReadLimit() < bytes_to_skip) {
      status_ = Status::DataLoss();
      return status_;
    }

    PW_TRY(Advance(position_ + bytes_to_skip));
  }

  field_consumed_ = true;
  return OkStatus();
}

Status StreamDecoder::ReadVarintField(uint64_t* out) {
  PW_TRY(CheckOkToRead(WireType::kVarint));

  uint64_t value;
  StatusWithSize sws = varint::Read(reader_, &value);
  if (sws.IsOutOfRange()) {
    // Out of range indicates the end of the stream. As a value is expected
    // here, report it as a data loss and terminate the decode operation.
    status_ = Status::DataLoss();
    return status_;
  }
  PW_TRY(sws);

  position_ += sws.size();
  field_consumed_ = true;
  *out = value;

  return OkStatus();
}

Status StreamDecoder::ReadFixedField(std::span<std::byte> out) {
  WireType expected_wire_type =
      out.size() == sizeof(uint32_t) ? WireType::kFixed32 : WireType::kFixed64;
  PW_TRY(CheckOkToRead(expected_wire_type));

  if (reader_.ConservativeReadLimit() < out.size()) {
    status_ = Status::DataLoss();
    return status_;
  }

  PW_TRY(reader_.Read(out));
  position_ += out.size();
  field_consumed_ = true;

  return OkStatus();
}

StatusWithSize StreamDecoder::ReadDelimitedField(std::span<std::byte> out) {
  if (Status status = CheckOkToRead(WireType::kDelimited); !status.ok()) {
    return StatusWithSize(status, 0);
  }

  if (reader_.ConservativeReadLimit() < delimited_field_size_) {
    status_ = Status::DataLoss();
    return StatusWithSize(status_, 0);
  }

  if (out.size() < delimited_field_size_) {
    // Value can't fit into the provided buffer. Don't advance the cursor so
    // that the field can be re-read with a larger buffer or through the stream
    // API.
    return StatusWithSize::ResourceExhausted();
  }

  Result<ByteSpan> result = reader_.Read(out.first(delimited_field_size_));
  if (!result.ok()) {
    return StatusWithSize(result.status(), 0);
  }

  position_ += result.value().size();
  field_consumed_ = true;
  return StatusWithSize(result.value().size());
}

Status StreamDecoder::CheckOkToRead(WireType type) {
  PW_CHECK(!nested_reader_open_,
           "Cannot read from a decoder while a nested decoder is open");
  PW_CHECK(
      !field_consumed_,
      "Attempting to read from protobuf decoder without first calling Next()");

  // Attempting to read the wrong type is typically a programmer error; however,
  // it could also occur due to data corruption. As we don't want to crash on
  // bad data, return NOT_FOUND here to distinguish it from other corruption
  // cases.
  if (current_field_.wire_type() != type) {
    status_ = Status::NotFound();
  }

  return status_;
}

}  // namespace pw::protobuf
