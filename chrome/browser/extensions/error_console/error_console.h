// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_ERROR_CONSOLE_ERROR_CONSOLE_H_
#define CHROME_BROWSER_EXTENSIONS_ERROR_CONSOLE_ERROR_CONSOLE_H_

#include <vector>

#include "base/gtest_prod_util.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/scoped_vector.h"
#include "base/observer_list.h"
#include "base/strings/string16.h"
#include "base/threading/thread_checker.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"

namespace content {
class NotificationDetails;
class NotificationSource;
class RenderViewHost;
}

class Profile;

namespace extensions {
class ErrorConsoleUnitTest;
class ExtensionError;

// The ErrorConsole is a central object to which all extension errors are
// reported. This includes errors detected in extensions core, as well as
// runtime Javascript errors.
// This class is owned by ExtensionSystem, making it, in effect, a
// BrowserContext-keyed service.
class ErrorConsole : content::NotificationObserver {
 public:
  typedef ScopedVector<ExtensionError> ErrorList;
  typedef std::vector<const ExtensionError*> WeakErrorList;

  class Observer {
   public:
    // Sent when a new error is reported to the error console.
    virtual void OnErrorAdded(const ExtensionError* error) = 0;

    // Sent upon destruction to allow any observers to invalidate any references
    // they have to the error console.
    virtual void OnErrorConsoleDestroyed();
  };

  explicit ErrorConsole(Profile* profile);
  virtual ~ErrorConsole();

  // Convenience method to return the ErrorConsole for a given profile.
  static ErrorConsole* Get(Profile* profile);

  // Report an extension error, and add it to the list.
  void ReportError(scoped_ptr<ExtensionError> error);

  // Get a collection of weak pointers to all errors relating to the extension
  // with the given |extension_id|.
  WeakErrorList GetErrorsForExtension(const std::string& extension_id) const;

  // Remove an error from the list of observed errors.
  void RemoveError(const ExtensionError* error);

  // Remove all errors from the list of observed errors.
  void RemoveAllErrors();

  // Add or remove observers of the ErrorConsole to be notified of any errors
  // added.
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  const ErrorList& errors() { return errors_; }

 private:
  FRIEND_TEST_ALL_PREFIXES(ErrorConsoleUnitTest, AddAndRemoveErrors);

  // Remove all errors which happened while incognito; we have to do this once
  // the incognito profile is destroyed.
  void RemoveIncognitoErrors();

  // content::NotificationObserver implementation.
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

  // Needed because ObserverList is not thread-safe.
  base::ThreadChecker thread_checker_;

  // The list of all observers for the ErrorConsole.
  ObserverList<Observer> observers_;

  // The errors which we have received so far.
  ErrorList errors_;

  // The profile with which the ErrorConsole is associated. Only collect errors
  // from extensions and RenderViews associated with this Profile (and it's
  // incognito fellow).
  Profile* profile_;

  content::NotificationRegistrar registrar_;

  DISALLOW_COPY_AND_ASSIGN(ErrorConsole);
};

}  // namespace extensions

#endif  // CHROME_BROWSER_EXTENSIONS_ERROR_CONSOLE_ERROR_CONSOLE_H_
