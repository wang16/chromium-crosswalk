// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/prefs/pref_service_syncable.h"

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/prefs/default_pref_store.h"
#include "base/prefs/overlay_user_pref_store.h"
#include "base/prefs/pref_notifier_impl.h"
#include "base/prefs/pref_registry.h"
#include "base/prefs/pref_value_store.h"
#include "base/strings/string_number_conversions.h"
#include "base/value_conversions.h"
#include "chrome/browser/prefs/pref_model_associator.h"
#include "chrome/browser/prefs/pref_service_syncable_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/prefs/prefs_tab_helper.h"
#include "components/user_prefs/pref_registry_syncable.h"

// static
PrefServiceSyncable* PrefServiceSyncable::FromProfile(Profile* profile) {
  return static_cast<PrefServiceSyncable*>(profile->GetPrefs());
}

// static
PrefServiceSyncable* PrefServiceSyncable::IncognitoFromProfile(
    Profile* profile) {
  return static_cast<PrefServiceSyncable*>(profile->GetOffTheRecordPrefs());
}

PrefServiceSyncable::PrefServiceSyncable(
    PrefNotifierImpl* pref_notifier,
    PrefValueStore* pref_value_store,
    PersistentPrefStore* user_prefs,
    PrefRegistrySyncable* pref_registry,
    base::Callback<void(PersistentPrefStore::PrefReadError)>
        read_error_callback,
    bool async)
  : PrefService(pref_notifier,
                pref_value_store,
                user_prefs,
                pref_registry,
                read_error_callback,
                async),
    pref_sync_associator_(syncer::PREFERENCES),
    priority_pref_sync_associator_(syncer::PRIORITY_PREFERENCES) {
  pref_sync_associator_.SetPrefService(this);
  priority_pref_sync_associator_.SetPrefService(this);

  // Let PrefModelAssociators know about changes to preference values.
  pref_value_store->set_callback(
      base::Bind(&PrefServiceSyncable::ProcessPrefChange,
                 base::Unretained(this)));

  // Add already-registered syncable preferences to PrefModelAssociator.
  const PrefRegistrySyncable::PrefToStatus& syncable_preferences =
      pref_registry->syncable_preferences();
  for (PrefRegistrySyncable::PrefToStatus::const_iterator it =
           syncable_preferences.begin();
       it != syncable_preferences.end();
       ++it) {
    AddRegisteredSyncablePreference(it->first.c_str(), it->second);
  }

  // Watch for syncable preferences registered after this point.
  pref_registry->SetSyncableRegistrationCallback(
      base::Bind(&PrefServiceSyncable::AddRegisteredSyncablePreference,
                 base::Unretained(this)));
}

PrefServiceSyncable::~PrefServiceSyncable() {
  // Remove our callback from the registry, since it may outlive us.
  PrefRegistrySyncable* registry =
      static_cast<PrefRegistrySyncable*>(pref_registry_.get());
  registry->SetSyncableRegistrationCallback(
      PrefRegistrySyncable::SyncableRegistrationCallback());
}

PrefServiceSyncable* PrefServiceSyncable::CreateIncognitoPrefService(
    PrefStore* incognito_extension_prefs) {
  pref_service_forked_ = true;
  PrefNotifierImpl* pref_notifier = new PrefNotifierImpl();
  OverlayUserPrefStore* incognito_pref_store =
      new OverlayUserPrefStore(user_pref_store_.get());
  PrefsTabHelper::InitIncognitoUserPrefStore(incognito_pref_store);

  scoped_refptr<PrefRegistrySyncable> forked_registry =
      static_cast<PrefRegistrySyncable*>(
          pref_registry_.get())->ForkForIncognito();
  PrefServiceSyncable* incognito_service = new PrefServiceSyncable(
      pref_notifier,
      pref_value_store_->CloneAndSpecialize(
          NULL,  // managed
          incognito_extension_prefs,
          NULL,  // command_line_prefs
          incognito_pref_store,
          NULL,  // recommended
          forked_registry->defaults(),
          pref_notifier),
      incognito_pref_store,
      forked_registry,
      read_error_callback_,
      false);
  return incognito_service;
}

bool PrefServiceSyncable::IsSyncing() {
  return pref_sync_associator_.models_associated();
}


bool PrefServiceSyncable::IsPrioritySyncing() {
  return priority_pref_sync_associator_.models_associated();
}

void PrefServiceSyncable::AddObserver(PrefServiceSyncableObserver* observer) {
  observer_list_.AddObserver(observer);
}

void PrefServiceSyncable::RemoveObserver(
    PrefServiceSyncableObserver* observer) {
  observer_list_.RemoveObserver(observer);
}

syncer::SyncableService* PrefServiceSyncable::GetSyncableService(
    const syncer::ModelType& type) {
  if (type == syncer::PREFERENCES) {
    return &pref_sync_associator_;
  } else if (type == syncer::PRIORITY_PREFERENCES) {
    return &priority_pref_sync_associator_;
  } else {
    NOTREACHED() << "invalid model type: " << type;
    return NULL;
  }
}

void PrefServiceSyncable::UpdateCommandLinePrefStore(
    PrefStore* cmd_line_store) {
  // If |pref_service_forked_| is true, then this PrefService and the forked
  // copies will be out of sync.
  DCHECK(!pref_service_forked_);
  PrefService::UpdateCommandLinePrefStore(cmd_line_store);
}

void PrefServiceSyncable::AddRegisteredSyncablePreference(
    const char* path,
    const PrefRegistrySyncable::PrefSyncStatus sync_status) {
  DCHECK(FindPreference(path));
  if (sync_status == PrefRegistrySyncable::SYNCABLE_PREF) {
    pref_sync_associator_.RegisterPref(path);
  } else if (sync_status == PrefRegistrySyncable::SYNCABLE_PRIORITY_PREF) {
    priority_pref_sync_associator_.RegisterPref(path);
  } else {
    NOTREACHED() << "invalid sync_status: " << sync_status;
  }
}

void PrefServiceSyncable::OnIsSyncingChanged() {
  FOR_EACH_OBSERVER(PrefServiceSyncableObserver, observer_list_,
                    OnIsSyncingChanged());
}

void PrefServiceSyncable::ProcessPrefChange(const std::string& name) {
  pref_sync_associator_.ProcessPrefChange(name);
  priority_pref_sync_associator_.ProcessPrefChange(name);
}
