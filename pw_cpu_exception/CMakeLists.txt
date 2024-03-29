# Copyright 2020 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

include($ENV{PW_ROOT}/pw_build/pigweed.cmake)

pw_add_facade(pw_cpu_exception.entry
  PUBLIC_DEPS
    pw_preprocessor
  HEADERS
    public/pw_cpu_exception/entry.h
    public/pw_cpu_exception/state.h
)

pw_add_facade(pw_cpu_exception.handler
  PUBLIC_DEPS
    pw_cpu_exception.entry
    pw_preprocessor
  SOURCES
    start_exception_handler.cc
  HEADERS
    public/pw_cpu_exception/handler.h
)

pw_add_facade(pw_cpu_exception.support
  PUBLIC_DEPS
    pw_cpu_exception.entry
  HEADERS
    public/pw_cpu_exception/support.h
)

pw_add_module_library(pw_cpu_exception.basic_handler
  PRIVATE_DEPS
    pw_cpu_exception.entry
  SOURCES
    basic_handler.cc
)
