// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_APPS_CLIENT_H_
#define APPS_APPS_CLIENT_H_

#include <vector>

namespace content {
class BrowserContext;
}

namespace extensions {
class Extension;
}

namespace apps {

class AppWindow;

// Sets up global state for the apps system. Should be Set() once in each
// process. This should be implemented by the client of the apps system.
class AppsClient {
 public:
  // Get all loaded browser contexts.
  virtual std::vector<content::BrowserContext*> GetLoadedBrowserContexts() = 0;

  // Do any pre app launch checks. Returns true if the app launch should proceed
  // or false if the launch should be prevented.
  virtual bool CheckAppLaunch(content::BrowserContext* context,
                              const extensions::Extension* extension) = 0;

  // Creates a new apps::AppWindow for the app in |extension| for |context|.
  // Caller takes ownership.
  virtual AppWindow* CreateAppWindow(
      content::BrowserContext* context,
      const extensions::Extension* extension) = 0;

  // Tells the embedding application to stay running. The application may close
  // after a matching number of calls to EndKeepAlive() are made.
  virtual void StartKeepAlive() = 0;
  virtual void EndKeepAlive() = 0;

  // Return the apps client.
  static AppsClient* Get();

  // Initialize the apps system with this apps client.
  static void Set(AppsClient* client);
};

}  // namespace apps

#endif  // APPS_APPS_CLIENT_H_
