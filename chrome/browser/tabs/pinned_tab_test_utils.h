// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_TABS_PINNED_TAB_TEST_UTILS_H_
#define CHROME_BROWSER_TABS_PINNED_TAB_TEST_UTILS_H_
#pragma once

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"

class PinnedTabTestUtils {
 public:
  // Converts a set of Tabs into a string. The format is a space separated list
  // of urls. If the tab is an app, ':app' is appended, and if the tab is
  // pinned, ':pinned' is appended.
  static std::string TabsToString(
      const std::vector<StartupBrowserCreator::LaunchWithProfile::Tab>& values);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(PinnedTabTestUtils);
};

#endif  // CHROME_BROWSER_TABS_PINNED_TAB_TEST_UTILS_H_
