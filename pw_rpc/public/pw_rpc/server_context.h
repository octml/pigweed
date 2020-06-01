// Copyright 2020 The Pigweed Authors
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
#pragma once

#include "pw_assert/assert.h"
#include "pw_rpc/channel.h"

namespace pw::rpc {
namespace internal {

class Method;
class Service;

}  // namespace internal

// The ServerContext collects context for an RPC being invoked on a server.
class ServerContext {
 public:
  constexpr ServerContext()
      : channel_(nullptr), service_(nullptr), method_(nullptr) {}

  uint32_t channel_id() const {
    PW_DCHECK_NOTNULL(channel_);
    return channel_->id();
  }

 private:
  friend class Server;

  constexpr ServerContext(Channel& channel,
                          internal::Service& service,
                          const internal::Method& method)
      : channel_(&channel), service_(&service), method_(&method) {
    // Temporarily silence unused private field warnings.
    (void)service_;
    (void)method_;
  }

  Channel* channel_;
  internal::Service* service_;
  const internal::Method* method_;
};

}  // namespace pw::rpc
