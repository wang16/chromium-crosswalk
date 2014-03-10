// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/signin/fake_signin_manager.h"

#include "base/callback_helpers.h"
#include "base/prefs/pref_service.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/signin/chrome_signin_client.h"
#include "chrome/browser/signin/signin_global_error.h"
#include "chrome/browser/signin/signin_manager_factory.h"
#include "chrome/browser/ui/global_error/global_error_service.h"
#include "chrome/browser/ui/global_error/global_error_service_factory.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/notification_service.h"

FakeSigninManagerBase::FakeSigninManagerBase() {
}

FakeSigninManagerBase::~FakeSigninManagerBase() {
}

// static
BrowserContextKeyedService* FakeSigninManagerBase::Build(
    content::BrowserContext* context) {
  SigninManagerBase* manager;
  Profile* profile = static_cast<Profile*>(context);
#if defined(OS_CHROMEOS)
  manager = new FakeSigninManagerBase();
#else
  manager = new FakeSigninManager(profile);
#endif
  manager->Initialize(profile, NULL);
  SigninManagerFactory::GetInstance()
      ->NotifyObserversOfSigninManagerCreationForTesting(manager);
  return manager;
}

#if !defined (OS_CHROMEOS)

FakeSigninManager::FakeSigninManager(Profile* profile)
    : SigninManager(scoped_ptr<SigninClient>(new ChromeSigninClient(profile))) {
}

FakeSigninManager::~FakeSigninManager() {
}

void FakeSigninManager::StartSignInWithCredentials(
    const std::string& session_index,
    const std::string& username,
    const std::string& password,
    const OAuthTokenFetchedCallback& oauth_fetched_callback) {
  set_auth_in_progress(username);
  set_password(password);
  if (!oauth_fetched_callback.is_null())
    oauth_fetched_callback.Run("fake_oauth_token");
}

void FakeSigninManager::CompletePendingSignin() {
  SetAuthenticatedUsername(GetUsernameForAuthInProgress());
  set_auth_in_progress(std::string());
  FOR_EACH_OBSERVER(Observer,
                    observer_list_,
                    GoogleSigninSucceeded(authenticated_username_, password_));
}

void FakeSigninManager::SignIn(const std::string& username,
                               const std::string& password) {
  StartSignInWithCredentials(
      std::string(), username, password, OAuthTokenFetchedCallback());
  CompletePendingSignin();
}

void FakeSigninManager::FailSignin(const GoogleServiceAuthError& error) {
  FOR_EACH_OBSERVER(Observer, observer_list_, GoogleSigninFailed(error));
}

void FakeSigninManager::SignOut() {
  if (IsSignoutProhibited())
    return;
  set_auth_in_progress(std::string());
  set_password(std::string());
  const std::string username = authenticated_username_;
  authenticated_username_.clear();

  // TODO(blundell): Eliminate this notification send once crbug.com/333997 is
  // fixed.
  GoogleServiceSignoutDetails details(username);
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_GOOGLE_SIGNED_OUT,
      content::Source<Profile>(profile_),
      content::Details<const GoogleServiceSignoutDetails>(&details));

  FOR_EACH_OBSERVER(SigninManagerBase::Observer, observer_list_,
                    GoogleSignedOut(username));
}

#endif  // !defined (OS_CHROMEOS)
