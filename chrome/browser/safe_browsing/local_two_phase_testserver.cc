// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/local_two_phase_testserver.h"

#include "base/command_line.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "net/test/python_utils.h"
#include "net/test/test_server.h"

LocalTwoPhaseTestServer::LocalTwoPhaseTestServer()
    : net::LocalTestServer(net::TestServer::TYPE_HTTP,
                           net::TestServer::kLocalhost,
                           base::FilePath()) {
}

LocalTwoPhaseTestServer::~LocalTwoPhaseTestServer() {}

bool LocalTwoPhaseTestServer::GetTestServerPath(
    base::FilePath* testserver_path) const {
  base::FilePath testserver_dir;
  if (!PathService::Get(base::DIR_SOURCE_ROOT, &testserver_dir)) {
    LOG(ERROR) << "Failed to get DIR_SOURCE_ROOT";
    return false;
  }

  testserver_dir = testserver_dir
      .Append(FILE_PATH_LITERAL("chrome"))
      .Append(FILE_PATH_LITERAL("browser"))
      .Append(FILE_PATH_LITERAL("safe_browsing"));

  *testserver_path = testserver_dir.Append(FILE_PATH_LITERAL(
      "two_phase_testserver.py"));
  return true;
}
