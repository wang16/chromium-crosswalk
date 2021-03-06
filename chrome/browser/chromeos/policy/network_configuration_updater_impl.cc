// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/policy/network_configuration_updater_impl.h"

#include <string>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/debug/stack_trace.h"
#include "chrome/browser/policy/policy_map.h"
#include "chromeos/network/managed_network_configuration_handler.h"
#include "chromeos/network/onc/onc_utils.h"
#include "policy/policy_constants.h"

namespace policy {

NetworkConfigurationUpdaterImpl::NetworkConfigurationUpdaterImpl(
    PolicyService* policy_service)
    : user_policy_initialized_(false),
      policy_change_registrar_(
          policy_service, PolicyNamespace(POLICY_DOMAIN_CHROME, std::string())),
      policy_service_(policy_service) {
  policy_change_registrar_.Observe(
      key::kDeviceOpenNetworkConfiguration,
      base::Bind(&NetworkConfigurationUpdaterImpl::OnPolicyChanged,
                 base::Unretained(this),
                 chromeos::onc::ONC_SOURCE_DEVICE_POLICY));
  policy_change_registrar_.Observe(
      key::kOpenNetworkConfiguration,
      base::Bind(&NetworkConfigurationUpdaterImpl::OnPolicyChanged,
                 base::Unretained(this),
                 chromeos::onc::ONC_SOURCE_USER_POLICY));

  ApplyNetworkConfiguration(chromeos::onc::ONC_SOURCE_DEVICE_POLICY);
}

NetworkConfigurationUpdaterImpl::~NetworkConfigurationUpdaterImpl() {
}

void NetworkConfigurationUpdaterImpl::OnUserPolicyInitialized() {
  VLOG(1) << "User policy initialized.";
  user_policy_initialized_ = true;
  ApplyNetworkConfiguration(chromeos::onc::ONC_SOURCE_USER_POLICY);
}

net::CertTrustAnchorProvider*
NetworkConfigurationUpdaterImpl::GetCertTrustAnchorProvider() {
  return NULL;
}

void NetworkConfigurationUpdaterImpl::OnPolicyChanged(
    chromeos::onc::ONCSource onc_source,
    const base::Value* previous,
    const base::Value* current) {
  VLOG(1) << "Policy for ONC source "
          << chromeos::onc::GetSourceAsString(onc_source) << " changed.";
  VLOG(2) << "User policy is " << (user_policy_initialized_ ? "" : "not ")
          << "initialized.";
  ApplyNetworkConfiguration(onc_source);
}

void NetworkConfigurationUpdaterImpl::ApplyNetworkConfiguration(
    chromeos::onc::ONCSource onc_source) {
  VLOG(1) << "Apply policy for ONC source "
          << chromeos::onc::GetSourceAsString(onc_source);

  std::string policy_key;
  if (onc_source == chromeos::onc::ONC_SOURCE_USER_POLICY)
    policy_key = key::kOpenNetworkConfiguration;
  else
    policy_key = key::kDeviceOpenNetworkConfiguration;

  const PolicyMap& policies = policy_service_->GetPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()));
  const base::Value* policy_value = policies.GetValue(policy_key);

  std::string onc_blob;
  if (policy_value) {
    if (!policy_value->GetAsString(&onc_blob))
      LOG(ERROR) << "ONC policy " << policy_key << " is not a string value.";
  } else {
    VLOG(2) << "The policy is not set.";
  }
  VLOG(2) << "The policy contains this ONC: " << onc_blob;

  if (onc_blob.empty())
    onc_blob = chromeos::onc::kEmptyUnencryptedConfiguration;

  scoped_ptr<base::DictionaryValue> onc_dict =
      chromeos::onc::ReadDictionaryFromJson(onc_blob);
  if (!onc_dict) {
    LOG(ERROR) << "ONC loaded from policy " << policy_key
               << " is not a valid JSON dictionary.";
    return;
  }

  chromeos::ManagedNetworkConfigurationHandler::Get()->SetPolicy(onc_source,
                                                                 *onc_dict);
}

}  // namespace policy
