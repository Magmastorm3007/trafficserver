/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#pragma once

#include "HttpTransact.h"
#include "tscpp/util/ts_bw.h"

class HttpDebugNames
{
public:
  static const char *get_event_name(int event);
  static const char *get_action_name(HttpTransact::StateMachineAction_t e);
  static const char *get_method_name(const char *method);
  static const char *get_cache_action_name(HttpTransact::CacheAction_t t);
  static const char *get_api_hook_name(TSHttpHookID t);
  static const char *get_server_state_name(HttpTransact::ServerState_t state);
};

swoc::BufferWriter &bwformat(swoc::BufferWriter &w, swoc::bwf::Spec const &spec, HttpTransact::ServerState_t state);
swoc::BufferWriter &bwformat(swoc::BufferWriter &w, swoc::bwf::Spec const &spec, HttpTransact::CacheAction_t state);
swoc::BufferWriter &bwformat(swoc::BufferWriter &w, swoc::bwf::Spec const &spec, HttpTransact::StateMachineAction_t state);
swoc::BufferWriter &bwformat(swoc::BufferWriter &w, swoc::bwf::Spec const &spec, TSHttpHookID id);
