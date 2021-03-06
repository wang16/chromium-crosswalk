// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/loader/offline_policy.h"

#include "base/command_line.h"
#include "content/public/common/content_switches.h"
#include "net/base/load_flags.h"
#include "net/http/http_response_info.h"
#include "net/url_request/url_request.h"

namespace content {

OfflinePolicy::OfflinePolicy()
    : enabled_(CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableOfflineCacheAccess)),
      state_(INIT) {}

OfflinePolicy::~OfflinePolicy() {}

int OfflinePolicy::GetAdditionalLoadFlags(int current_flags,
                                          bool reset_state) {

  // Don't do anything if offline mode is disabled.
  if (!enabled_)
    return 0;

  if (reset_state)
    state_ = INIT;

  // If a consumer has requested something contradictory, it wins; we
  // don't modify the load flags.
  if (current_flags &
      (net::LOAD_BYPASS_CACHE | net::LOAD_PREFERRING_CACHE |
       net::LOAD_ONLY_FROM_CACHE | net::LOAD_FROM_CACHE_IF_OFFLINE |
       net::LOAD_DISABLE_CACHE)) {
    return 0;
  }

  switch(state_) {
    case INIT:
      return net::LOAD_FROM_CACHE_IF_OFFLINE;
    case ONLINE:
      return 0;
    case OFFLINE:
      return net::LOAD_ONLY_FROM_CACHE;
  }
  NOTREACHED();
  return 0;
}

void OfflinePolicy::UpdateStateForCompletedRequest(
    const net::HttpResponseInfo& response_info) {
  // Don't do anything if offline mode is disabled.
  if (!enabled_)
    return;

  if (state_ != INIT)
    // We've already made the decision for the rest of this set
    // of navigations.
    return;

  if (response_info.server_data_unavailable) {
    state_ = OFFLINE;
  } else if (response_info.network_accessed) {
    // If we got the response from the network or validated it as part
    // of this request, that means our connection to the host is working.
    state_ = ONLINE;
  }
}

}  // namespace content
