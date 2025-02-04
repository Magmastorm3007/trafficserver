/** @file

  Plugin Traffic Dump captures traffic on a per session basis. A sampling ratio can be set via plugin.config or traffic_ctl to dump
  one out of n sessions. The dump file schema can be found
  https://github.com/apache/trafficserver/tree/master/tests/tools/lib/replay_schema.json

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

#include <getopt.h>

#include "global_variables.h"
#include "session_data.h"
#include "transaction_data.h"

namespace traffic_dump
{
/// Handle LIFECYCLE_MSG from traffic_ctl.
static int
global_message_handler(TSCont contp, TSEvent event, void *edata)
{
  switch (event) {
  case TS_EVENT_LIFECYCLE_MSG: {
    TSPluginMsg *msg = static_cast<TSPluginMsg *>(edata);
    static constexpr std::string_view PLUGIN_PREFIX("traffic_dump.");

    std::string_view tag(msg->tag, strlen(msg->tag));
    if (tag.substr(0, PLUGIN_PREFIX.size()) == PLUGIN_PREFIX) {
      tag.remove_prefix(PLUGIN_PREFIX.size());
      if (tag == "sample" && msg->data_size) {
        const auto new_sample_size = static_cast<int64_t>(strtol(static_cast<char const *>(msg->data), nullptr, 0));
        TSDebug(debug_tag, "TS_EVENT_LIFECYCLE_MSG: Received Msg to change sample size to %" PRId64 "bytes", new_sample_size);
        SessionData::set_sample_pool_size(new_sample_size);
      } else if (tag == "reset") {
        TSDebug(debug_tag, "TS_EVENT_LIFECYCLE_MSG: Received Msg to reset disk usage counter");
        SessionData::reset_disk_usage();
      } else if (tag == "unlimit") {
        TSDebug(debug_tag, "TS_EVENT_LIFECYCLE_MSG: Received Msg to disable disk usage limit enforcement");
        SessionData::disable_disk_limit_enforcement();
      } else if (tag == "limit" && msg->data_size) {
        const auto new_max_disk_usage = static_cast<int64_t>(strtol(static_cast<char const *>(msg->data), nullptr, 0));
        TSDebug(debug_tag, "TS_EVENT_LIFECYCLE_MSG: Received Msg to change max disk usage to %" PRId64 "bytes", new_max_disk_usage);
        SessionData::set_max_disk_usage(new_max_disk_usage);
      }
    }
    return TS_SUCCESS;
  }
  default:
    TSDebug(debug_tag, "session_aio_handler(): unhandled events %d", event);
    return TS_ERROR;
  }
}

} // namespace traffic_dump

void
TSPluginInit(int argc, char const *argv[])
{
  TSDebug(traffic_dump::debug_tag, "initializing plugin");
  TSPluginRegistrationInfo info;

  info.plugin_name   = "traffic_dump";
  info.vendor_name   = "Apache Software Foundation";
  info.support_email = "dev@trafficserver.apache.org";

  if (TS_SUCCESS != TSPluginRegister(&info)) {
    TSError("[%s] Unable to initialize plugin (disabled). Failed to register plugin.", traffic_dump::debug_tag);
    return;
  }

  bool dump_body                       = false;
  bool sensitive_fields_were_specified = false;
  traffic_dump::sensitive_fields_t user_specified_fields;
  swoc::file::path log_dir{traffic_dump::SessionData::default_log_directory};
  int64_t sample_pool_size = traffic_dump::SessionData::default_sample_pool_size;
  int64_t max_disk_usage   = traffic_dump::SessionData::default_max_disk_usage;
  bool enforce_disk_limit  = traffic_dump::SessionData::default_enforce_disk_limit;
  std::string sni_filter;
  std::string client_ip_filter;

  /// Commandline options
  static const struct option longopts[] = {
    {"dump_body",        no_argument,       nullptr, 'b'},
    {"logdir",           required_argument, nullptr, 'l'},
    {"sample",           required_argument, nullptr, 's'},
    {"limit",            required_argument, nullptr, 'm'},
    {"sensitive-fields", required_argument, nullptr, 'f'},
    {"sni-filter",       required_argument, nullptr, 'n'},
    {"client_ipv4",      required_argument, nullptr, '4'},
    {"client_ipv6",      required_argument, nullptr, '6'},
    {nullptr,            no_argument,       nullptr, 0  }
  };
  int opt = 0;
  while (opt >= 0) {
    opt = getopt_long(argc, const_cast<char *const *>(argv), "bf:l:s:m:n:4:6", longopts, nullptr);
    switch (opt) {
    case 'b': {
      dump_body = true;
      break;
    }
    case 'f': {
      // --sensitive-fields takes a comma-separated list of HTTP fields that
      // are sensitive.  The field values for these fields will be replaced
      // with generic traffic_dump generated data.
      //
      // If this option is not used, then the default values in
      // default_sensitive_fields is used. If this option is used, then it
      // replaced the default sensitive fields with the user-supplied list of
      // sensitive fields.
      sensitive_fields_were_specified = true;
      swoc::TextView input_filter_fields{std::string_view{optarg}};
      swoc::TextView filter_field;
      while (!(filter_field = input_filter_fields.take_prefix_at(',')).empty()) {
        filter_field.trim_if(&isspace);
        if (filter_field.empty()) {
          continue;
        }
        user_specified_fields.emplace(filter_field);
      }
      break;
    }
    case 'n': {
      // --sni-filter is used to filter sessions based upon an SNI.
      sni_filter = std::string(optarg);
      break;
    }
    case 'l': {
      log_dir = swoc::file::path{optarg};
      break;
    }
    case 's': {
      sample_pool_size = static_cast<int64_t>(std::strtol(optarg, nullptr, 0));
      break;
    }
    case 'm': {
      max_disk_usage     = static_cast<int64_t>(std::strtol(optarg, nullptr, 0));
      enforce_disk_limit = true;
      break;
    }
    case '4':
    case '6': {
      client_ip_filter = std::string(optarg);
      break;
    }
    case -1:
    case '?':
      break;

    default:
      TSDebug(traffic_dump::debug_tag, "Unexpected options.");
      TSError("[%s] Unexpected options error.", traffic_dump::debug_tag);
      return;
    }
  }
  if (!log_dir.is_absolute()) {
    log_dir = swoc::file::path(TSInstallDirGet()) / log_dir;
  }
  if (sni_filter.empty()) {
    if (!traffic_dump::SessionData::init(log_dir.view(), enforce_disk_limit, max_disk_usage, sample_pool_size, client_ip_filter)) {
      TSError("[%s] Failed to initialize session state.", traffic_dump::debug_tag);
      return;
    }
  } else {
    if (!traffic_dump::SessionData::init(log_dir.view(), enforce_disk_limit, max_disk_usage, sample_pool_size, client_ip_filter,
                                         sni_filter)) {
      TSError("[%s] Failed to initialize session state with an SNI filter.", traffic_dump::debug_tag);
      return;
    }
  }

  if (sensitive_fields_were_specified) {
    if (!traffic_dump::TransactionData::init(dump_body, std::move(user_specified_fields))) {
      TSError("[%s] Failed to initialize transaction state with user-specified fields.", traffic_dump::debug_tag);
      return;
    }
  } else {
    // The user did not provide their own list of sensitive fields. Use the
    // default.
    if (!traffic_dump::TransactionData::init(dump_body)) {
      TSError("[%s] Failed to initialize transaction state.", traffic_dump::debug_tag);
      return;
    }
  }

  TSCont message_continuation = TSContCreate(traffic_dump::global_message_handler, nullptr);
  TSLifecycleHookAdd(TS_LIFECYCLE_MSG_HOOK, message_continuation);
  return;
}
