// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_session.h"

#include <algorithm>
#include <map>

#include "base/basictypes.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/message_loop.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram.h"
#include "base/metrics/sparse_histogram.h"
#include "base/metrics/stats_counters.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "crypto/ec_private_key.h"
#include "crypto/ec_signature_creator.h"
#include "net/base/connection_type_histograms.h"
#include "net/base/net_log.h"
#include "net/base/net_util.h"
#include "net/cert/asn1_util.h"
#include "net/http/http_network_session.h"
#include "net/http/http_server_properties.h"
#include "net/spdy/spdy_buffer_producer.h"
#include "net/spdy/spdy_credential_builder.h"
#include "net/spdy/spdy_frame_builder.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_protocol.h"
#include "net/spdy/spdy_session_pool.h"
#include "net/spdy/spdy_stream.h"
#include "net/ssl/server_bound_cert_service.h"

namespace net {

namespace {

const int kReadBufferSize = 8 * 1024;
const int kDefaultConnectionAtRiskOfLossSeconds = 10;
const int kHungIntervalSeconds = 10;

// Always start at 1 for the first stream id.
const SpdyStreamId kFirstStreamId = 1;

// Minimum seconds that unclaimed pushed streams will be kept in memory.
const int kMinPushedStreamLifetimeSeconds = 300;

SpdyMajorVersion NPNToSpdyVersion(NextProto next_proto) {
  switch (next_proto) {
    case kProtoSPDY2:
    case kProtoSPDY21:
      return SPDY2;
    case kProtoSPDY3:
    case kProtoSPDY31:
      return SPDY3;
    case kProtoSPDY4a2:
      return SPDY4;
    default:
      NOTREACHED();
  }
  return SPDY2;
}

base::Value* NetLogSpdySynCallback(const SpdyHeaderBlock* headers,
                                   bool fin,
                                   bool unidirectional,
                                   SpdyStreamId stream_id,
                                   SpdyStreamId associated_stream,
                                   NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  base::ListValue* headers_list = new base::ListValue();
  for (SpdyHeaderBlock::const_iterator it = headers->begin();
       it != headers->end(); ++it) {
    headers_list->Append(new base::StringValue(base::StringPrintf(
        "%s: %s", it->first.c_str(),
        (ShouldShowHttpHeaderValue(
            it->first) ? it->second : "[elided]").c_str())));
  }
  dict->SetBoolean("fin", fin);
  dict->SetBoolean("unidirectional", unidirectional);
  dict->Set("headers", headers_list);
  dict->SetInteger("stream_id", stream_id);
  if (associated_stream)
    dict->SetInteger("associated_stream", associated_stream);
  return dict;
}

base::Value* NetLogSpdyCredentialCallback(size_t slot,
                                          const std::string* origin,
                                          NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("slot", slot);
  dict->SetString("origin", *origin);
  return dict;
}

base::Value* NetLogSpdySessionCloseCallback(int net_error,
                                            const std::string* description,
                                            NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("net_error", net_error);
  dict->SetString("description", *description);
  return dict;
}

base::Value* NetLogSpdySessionCallback(const HostPortProxyPair* host_pair,
                                       NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetString("host", host_pair->first.ToString());
  dict->SetString("proxy", host_pair->second.ToPacString());
  return dict;
}

base::Value* NetLogSpdySettingsCallback(const HostPortPair& host_port_pair,
                                        bool clear_persisted,
                                        NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetString("host", host_port_pair.ToString());
  dict->SetBoolean("clear_persisted", clear_persisted);
  return dict;
}

base::Value* NetLogSpdySettingCallback(SpdySettingsIds id,
                                       SpdySettingsFlags flags,
                                       uint32 value,
                                       NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("id", id);
  dict->SetInteger("flags", flags);
  dict->SetInteger("value", value);
  return dict;
}

base::Value* NetLogSpdySendSettingsCallback(const SettingsMap* settings,
                                            NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  base::ListValue* settings_list = new base::ListValue();
  for (SettingsMap::const_iterator it = settings->begin();
       it != settings->end(); ++it) {
    const SpdySettingsIds id = it->first;
    const SpdySettingsFlags flags = it->second.first;
    const uint32 value = it->second.second;
    settings_list->Append(new base::StringValue(
        base::StringPrintf("[id:%u flags:%u value:%u]", id, flags, value)));
  }
  dict->Set("settings", settings_list);
  return dict;
}

base::Value* NetLogSpdyWindowUpdateFrameCallback(
    SpdyStreamId stream_id,
    uint32 delta,
    NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("stream_id", static_cast<int>(stream_id));
  dict->SetInteger("delta", delta);
  return dict;
}

base::Value* NetLogSpdySessionWindowUpdateCallback(
    int32 delta,
    int32 window_size,
    NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("delta", delta);
  dict->SetInteger("window_size", window_size);
  return dict;
}

base::Value* NetLogSpdyDataCallback(SpdyStreamId stream_id,
                                    int size,
                                    bool fin,
                                    NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("stream_id", static_cast<int>(stream_id));
  dict->SetInteger("size", size);
  dict->SetBoolean("fin", fin);
  return dict;
}

base::Value* NetLogSpdyRstCallback(SpdyStreamId stream_id,
                                   int status,
                                   const std::string* description,
                                   NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("stream_id", static_cast<int>(stream_id));
  dict->SetInteger("status", status);
  dict->SetString("description", *description);
  return dict;
}

base::Value* NetLogSpdyPingCallback(uint32 unique_id,
                                    const char* type,
                                    NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("unique_id", unique_id);
  dict->SetString("type", type);
  return dict;
}

base::Value* NetLogSpdyGoAwayCallback(SpdyStreamId last_stream_id,
                                      int active_streams,
                                      int unclaimed_streams,
                                      SpdyGoAwayStatus status,
                                      NetLog::LogLevel /* log_level */) {
  base::DictionaryValue* dict = new base::DictionaryValue();
  dict->SetInteger("last_accepted_stream_id",
                   static_cast<int>(last_stream_id));
  dict->SetInteger("active_streams", active_streams);
  dict->SetInteger("unclaimed_streams", unclaimed_streams);
  dict->SetInteger("status", static_cast<int>(status));
  return dict;
}

// Maximum number of concurrent streams we will create, unless the server
// sends a SETTINGS frame with a different value.
const size_t kInitialMaxConcurrentStreams = 100;
// The maximum number of concurrent streams we will ever create.  Even if
// the server permits more, we will never exceed this limit.
const size_t kMaxConcurrentStreamLimit = 256;

}  // namespace

SpdyStreamRequest::SpdyStreamRequest() {
  Reset();
}

SpdyStreamRequest::~SpdyStreamRequest() {
  CancelRequest();
}

int SpdyStreamRequest::StartRequest(
    SpdyStreamType type,
    const scoped_refptr<SpdySession>& session,
    const GURL& url,
    RequestPriority priority,
    const BoundNetLog& net_log,
    const CompletionCallback& callback) {
  DCHECK(session.get());
  DCHECK(!session_.get());
  DCHECK(!stream_.get());
  DCHECK(callback_.is_null());

  type_ = type;
  session_ = session;
  url_ = url;
  priority_ = priority;
  net_log_ = net_log;
  callback_ = callback;

  base::WeakPtr<SpdyStream> stream;
  int rv = session->TryCreateStream(this, &stream);
  if (rv == OK) {
    Reset();
    stream_ = stream;
  }
  return rv;
}

void SpdyStreamRequest::CancelRequest() {
  if (session_.get())
    session_->CancelStreamRequest(this);
  Reset();
}

base::WeakPtr<SpdyStream> SpdyStreamRequest::ReleaseStream() {
  DCHECK(!session_.get());
  base::WeakPtr<SpdyStream> stream = stream_;
  DCHECK(stream.get());
  Reset();
  return stream;
}

void SpdyStreamRequest::OnRequestCompleteSuccess(
    base::WeakPtr<SpdyStream>* stream) {
  DCHECK(session_.get());
  DCHECK(!stream_.get());
  DCHECK(!callback_.is_null());
  CompletionCallback callback = callback_;
  Reset();
  DCHECK(stream->get());
  stream_ = *stream;
  callback.Run(OK);
}

void SpdyStreamRequest::OnRequestCompleteFailure(int rv) {
  DCHECK(session_.get());
  DCHECK(!stream_.get());
  DCHECK(!callback_.is_null());
  CompletionCallback callback = callback_;
  Reset();
  DCHECK_NE(rv, OK);
  callback.Run(rv);
}

void SpdyStreamRequest::Reset() {
  type_ = SPDY_BIDIRECTIONAL_STREAM;
  session_ = NULL;
  stream_.reset();
  url_ = GURL();
  priority_ = MINIMUM_PRIORITY;
  net_log_ = BoundNetLog();
  callback_.Reset();
}

SpdySession::ActiveStreamInfo::ActiveStreamInfo()
    : stream(NULL),
      waiting_for_syn_reply(false) {}

SpdySession::ActiveStreamInfo::ActiveStreamInfo(SpdyStream* stream)
    : stream(stream),
      waiting_for_syn_reply(stream->type() != SPDY_PUSH_STREAM) {}

SpdySession::ActiveStreamInfo::~ActiveStreamInfo() {}

SpdySession::PushedStreamInfo::PushedStreamInfo() : stream_id(0) {}

SpdySession::PushedStreamInfo::PushedStreamInfo(
    SpdyStreamId stream_id,
    base::TimeTicks creation_time)
    : stream_id(stream_id),
      creation_time(creation_time) {}

SpdySession::PushedStreamInfo::~PushedStreamInfo() {}

SpdySession::SpdySession(const SpdySessionKey& spdy_session_key,
                         HttpServerProperties* http_server_properties,
                         bool verify_domain_authentication,
                         bool enable_sending_initial_settings,
                         bool enable_credential_frames,
                         bool enable_compression,
                         bool enable_ping_based_connection_checking,
                         NextProto default_protocol,
                         size_t stream_initial_recv_window_size,
                         size_t initial_max_concurrent_streams,
                         size_t max_concurrent_streams_limit,
                         TimeFunc time_func,
                         const HostPortPair& trusted_spdy_proxy,
                         NetLog* net_log)
    : weak_factory_(this),
      spdy_session_key_(spdy_session_key),
      spdy_session_pool_(NULL),
      http_server_properties_(http_server_properties),
      read_buffer_(new IOBuffer(kReadBufferSize)),
      stream_hi_water_mark_(kFirstStreamId),
      in_flight_write_frame_type_(DATA),
      in_flight_write_frame_size_(0),
      is_secure_(false),
      certificate_error_code_(OK),
      availability_state_(STATE_AVAILABLE),
      read_state_(READ_STATE_DO_READ),
      write_state_(WRITE_STATE_DO_WRITE),
      error_on_close_(OK),
      max_concurrent_streams_(initial_max_concurrent_streams == 0 ?
                              kInitialMaxConcurrentStreams :
                              initial_max_concurrent_streams),
      max_concurrent_streams_limit_(max_concurrent_streams_limit == 0 ?
                                    kMaxConcurrentStreamLimit :
                                    max_concurrent_streams_limit),
      streams_initiated_count_(0),
      streams_pushed_count_(0),
      streams_pushed_and_claimed_count_(0),
      streams_abandoned_count_(0),
      total_bytes_received_(0),
      sent_settings_(false),
      received_settings_(false),
      stalled_streams_(0),
      pings_in_flight_(0),
      next_ping_id_(1),
      last_activity_time_(base::TimeTicks::Now()),
      check_ping_status_pending_(false),
      flow_control_state_(FLOW_CONTROL_NONE),
      stream_initial_send_window_size_(kSpdyStreamInitialWindowSize),
      stream_initial_recv_window_size_(stream_initial_recv_window_size == 0 ?
                                       kDefaultInitialRecvWindowSize :
                                       stream_initial_recv_window_size),
      session_send_window_size_(0),
      session_recv_window_size_(0),
      session_unacked_recv_window_bytes_(0),
      net_log_(BoundNetLog::Make(net_log, NetLog::SOURCE_SPDY_SESSION)),
      verify_domain_authentication_(verify_domain_authentication),
      enable_sending_initial_settings_(enable_sending_initial_settings),
      enable_credential_frames_(enable_credential_frames),
      enable_compression_(enable_compression),
      enable_ping_based_connection_checking_(
          enable_ping_based_connection_checking),
      default_protocol_(default_protocol),
      credential_state_(SpdyCredentialState::kDefaultNumSlots),
      connection_at_risk_of_loss_time_(
          base::TimeDelta::FromSeconds(kDefaultConnectionAtRiskOfLossSeconds)),
      hung_interval_(
          base::TimeDelta::FromSeconds(kHungIntervalSeconds)),
      trusted_spdy_proxy_(trusted_spdy_proxy),
      time_func_(time_func) {
  DCHECK(HttpStreamFactory::spdy_enabled());
  net_log_.BeginEvent(
      NetLog::TYPE_SPDY_SESSION,
      base::Bind(&NetLogSpdySessionCallback, &host_port_proxy_pair()));
  next_unclaimed_push_stream_sweep_time_ = time_func_() +
      base::TimeDelta::FromSeconds(kMinPushedStreamLifetimeSeconds);
  // TODO(mbelshe): consider randomization of the stream_hi_water_mark.
}

SpdySession::~SpdySession() {
  if (availability_state_ != STATE_CLOSED) {
    availability_state_ = STATE_CLOSED;
    error_on_close_ = ERR_ABORTED;

    // Cleanup all the streams.
    CloseAllStreams(ERR_ABORTED);
  }

  // TODO(akalin): Check connection->is_initialized() instead. This
  // requires re-working CreateFakeSpdySession(), though.
  DCHECK(connection_->socket());
  // With SPDY we can't recycle sockets.
  connection_->socket()->Disconnect();

  // Streams should all be gone now.
  DCHECK_EQ(0u, num_active_streams());
  DCHECK_EQ(0u, num_unclaimed_pushed_streams());

  for (int i = 0; i < NUM_PRIORITIES; ++i) {
    DCHECK(pending_create_stream_queues_[i].empty());
  }
  DCHECK(pending_stream_request_completions_.empty());

  RecordHistograms();

  net_log_.EndEvent(NetLog::TYPE_SPDY_SESSION);
}

Error SpdySession::InitializeWithSocket(
    scoped_ptr<ClientSocketHandle> connection,
    SpdySessionPool* spdy_session_pool,
    bool is_secure,
    int certificate_error_code) {
  DCHECK(!connection_);
  // TODO(akalin): Check connection->is_initialized() instead. This
  // requires re-working CreateFakeSpdySession(), though.
  DCHECK(connection->socket());
  base::StatsCounter spdy_sessions("spdy.sessions");
  spdy_sessions.Increment();

  DCHECK_EQ(availability_state_, STATE_AVAILABLE);
  DCHECK_EQ(read_state_, READ_STATE_DO_READ);
  DCHECK_EQ(write_state_, WRITE_STATE_DO_WRITE);

  connection_ = connection.Pass();
  is_secure_ = is_secure;
  certificate_error_code_ = certificate_error_code;

  NextProto protocol = default_protocol_;
  NextProto protocol_negotiated =
      connection_->socket()->GetNegotiatedProtocol();
  if (protocol_negotiated != kProtoUnknown) {
    protocol = protocol_negotiated;
  }

  SSLClientSocket* ssl_socket = GetSSLClientSocket();
  if (ssl_socket && ssl_socket->WasChannelIDSent()) {
    // According to the SPDY spec, the credential associated with the TLS
    // connection is stored in slot[1].
    credential_state_.SetHasCredential(GURL("https://" +
                                            host_port_pair().ToString()));
  }

  // TODO(akalin): Change this to kProtoSPDYMinimumVersion once we
  // stop supporting SPDY/1.
  DCHECK_GE(protocol, kProtoSPDY2);
  DCHECK_LE(protocol, kProtoSPDYMaximumVersion);
  if (protocol >= kProtoSPDY31) {
    flow_control_state_ = FLOW_CONTROL_STREAM_AND_SESSION;
    session_send_window_size_ = kSpdySessionInitialWindowSize;
    session_recv_window_size_ = kSpdySessionInitialWindowSize;
  } else if (protocol >= kProtoSPDY3) {
    flow_control_state_ = FLOW_CONTROL_STREAM;
  } else {
    flow_control_state_ = FLOW_CONTROL_NONE;
  }

  buffered_spdy_framer_.reset(
      new BufferedSpdyFramer(NPNToSpdyVersion(protocol), enable_compression_));
  buffered_spdy_framer_->set_visitor(this);
  buffered_spdy_framer_->set_debug_visitor(this);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdyVersion", protocol, kProtoMaximumVersion);
#if defined(SPDY_PROXY_AUTH_ORIGIN)
  UMA_HISTOGRAM_BOOLEAN("Net.SpdySessions_DataReductionProxy",
                        host_port_pair().Equals(HostPortPair::FromURL(
                            GURL(SPDY_PROXY_AUTH_ORIGIN))));
#endif

  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_INITIALIZED,
      connection_->socket()->NetLog().source().ToEventParametersCallback());

  int error = DoReadLoop(READ_STATE_DO_READ, OK);
  if (error == ERR_IO_PENDING)
    error = OK;
  if (error == OK) {
    connection_->AddLayeredPool(this);
    SendInitialSettings();
    spdy_session_pool_ = spdy_session_pool;
  }
  return static_cast<Error>(error);
}

bool SpdySession::VerifyDomainAuthentication(const std::string& domain) {
  if (!verify_domain_authentication_)
    return true;

  if (availability_state_ == STATE_CLOSED)
    return false;

  SSLInfo ssl_info;
  bool was_npn_negotiated;
  NextProto protocol_negotiated = kProtoUnknown;
  if (!GetSSLInfo(&ssl_info, &was_npn_negotiated, &protocol_negotiated))
    return true;   // This is not a secure session, so all domains are okay.

  return !ssl_info.client_cert_sent &&
      (enable_credential_frames_ || !ssl_info.channel_id_sent ||
       ServerBoundCertService::GetDomainForHost(domain) ==
       ServerBoundCertService::GetDomainForHost(host_port_pair().host())) &&
       ssl_info.cert->VerifyNameMatch(domain);
}

int SpdySession::GetPushStream(
    const GURL& url,
    base::WeakPtr<SpdyStream>* stream,
    const BoundNetLog& stream_net_log) {
  if (availability_state_ == STATE_CLOSED)
    return ERR_CONNECTION_CLOSED;

  stream->reset();

  // Don't allow access to secure push streams over an unauthenticated, but
  // encrypted SSL socket.
  if (is_secure_ && certificate_error_code_ != OK &&
      (url.SchemeIs("https") || url.SchemeIs("wss"))) {
    RecordProtocolErrorHistogram(
        PROTOCOL_ERROR_REQUEST_FOR_SECURE_CONTENT_OVER_INSECURE_SESSION);
    CloseSessionOnError(
        static_cast<Error>(certificate_error_code_),
        "Tried to get SPDY stream for secure content over an unauthenticated "
        "session.");
    return ERR_SPDY_PROTOCOL_ERROR;
  }

  *stream = GetActivePushStream(url.spec());
  if (stream->get()) {
    DCHECK(streams_pushed_and_claimed_count_ < streams_pushed_count_);
    streams_pushed_and_claimed_count_++;
  }
  return OK;
}

int SpdySession::TryCreateStream(SpdyStreamRequest* request,
                                 base::WeakPtr<SpdyStream>* stream) {
  CHECK(request);

  // TODO(akalin): Also refuse to create the stream when
  // |availability_state_| == STATE_GOING_AWAY.
  if (availability_state_ == STATE_CLOSED)
    return ERR_CONNECTION_CLOSED;

  if (!max_concurrent_streams_ ||
      (active_streams_.size() + created_streams_.size() <
       max_concurrent_streams_)) {
    return CreateStream(*request, stream);
  }

  stalled_streams_++;
  net_log().AddEvent(NetLog::TYPE_SPDY_SESSION_STALLED_MAX_STREAMS);
  pending_create_stream_queues_[request->priority()].push_back(request);
  return ERR_IO_PENDING;
}

int SpdySession::CreateStream(const SpdyStreamRequest& request,
                              base::WeakPtr<SpdyStream>* stream) {
  DCHECK_GE(request.priority(), MINIMUM_PRIORITY);
  DCHECK_LT(request.priority(), NUM_PRIORITIES);

  // TODO(akalin): Also refuse to create the stream when
  // |availability_state_| == STATE_GOING_AWAY.
  if (availability_state_ == STATE_CLOSED)
    return ERR_CONNECTION_CLOSED;

  // Make sure that we don't try to send https/wss over an unauthenticated, but
  // encrypted SSL socket.
  if (is_secure_ && certificate_error_code_ != OK &&
      (request.url().SchemeIs("https") || request.url().SchemeIs("wss"))) {
    RecordProtocolErrorHistogram(
        PROTOCOL_ERROR_REQUEST_FOR_SECURE_CONTENT_OVER_INSECURE_SESSION);
    CloseSessionOnError(
        static_cast<Error>(certificate_error_code_),
        "Tried to create SPDY stream for secure content over an "
        "unauthenticated session.");
    return ERR_SPDY_PROTOCOL_ERROR;
  }

  DCHECK(connection_->socket());
  DCHECK(connection_->socket()->IsConnected());
  if (connection_->socket()) {
    UMA_HISTOGRAM_BOOLEAN("Net.SpdySession.CreateStreamWithSocketConnected",
                          connection_->socket()->IsConnected());
    if (!connection_->socket()->IsConnected()) {
      CloseSessionOnError(
          ERR_CONNECTION_CLOSED,
          "Tried to create SPDY stream for a closed socket connection.");
      return ERR_CONNECTION_CLOSED;
    }
  }

  const std::string& path = request.url().PathForRequest();
  scoped_ptr<SpdyStream> new_stream(
      new SpdyStream(request.type(), this, path, request.priority(),
                     stream_initial_send_window_size_,
                     stream_initial_recv_window_size_,
                     request.net_log()));
  *stream = new_stream->GetWeakPtr();
  InsertCreatedStream(new_stream.Pass());

  UMA_HISTOGRAM_CUSTOM_COUNTS(
      "Net.SpdyPriorityCount",
      static_cast<int>(request.priority()), 0, 10, 11);

  return OK;
}

void SpdySession::CancelStreamRequest(SpdyStreamRequest* request) {
  CHECK(request);

  if (DCHECK_IS_ON()) {
    // |request| should not be in a queue not matching its priority.
    for (int i = 0; i < NUM_PRIORITIES; ++i) {
      if (request->priority() == i)
        continue;
      PendingStreamRequestQueue* queue = &pending_create_stream_queues_[i];
      DCHECK(std::find(queue->begin(), queue->end(), request) == queue->end());
    }
  }

  PendingStreamRequestQueue* queue =
      &pending_create_stream_queues_[request->priority()];
  // Remove |request| from |queue| while preserving the order of the
  // other elements.
  PendingStreamRequestQueue::iterator it =
      std::find(queue->begin(), queue->end(), request);
  if (it != queue->end()) {
    it = queue->erase(it);
    // |request| should be in the queue at most once, and if it is
    // present, should not be pending completion.
    DCHECK(std::find(it, queue->end(), request) == queue->end());
    DCHECK(!ContainsKey(pending_stream_request_completions_,
                        request));
    return;
  }

  pending_stream_request_completions_.erase(request);
}

void SpdySession::ProcessPendingStreamRequests() {
  while (!max_concurrent_streams_ ||
         (active_streams_.size() + created_streams_.size() <
          max_concurrent_streams_)) {
    bool no_pending_create_streams = true;
    for (int i = NUM_PRIORITIES - 1; i >= MINIMUM_PRIORITY; --i) {
      if (!pending_create_stream_queues_[i].empty()) {
        SpdyStreamRequest* pending_request =
            pending_create_stream_queues_[i].front();
        CHECK(pending_request);
        pending_create_stream_queues_[i].pop_front();
        no_pending_create_streams = false;
        DCHECK(!ContainsKey(pending_stream_request_completions_,
                            pending_request));
        pending_stream_request_completions_.insert(pending_request);
        base::MessageLoop::current()->PostTask(
            FROM_HERE,
            base::Bind(&SpdySession::CompleteStreamRequest,
                       weak_factory_.GetWeakPtr(), pending_request));
        break;
      }
    }
    if (no_pending_create_streams)
      return;  // there were no streams in any queue
  }
}

bool SpdySession::NeedsCredentials() const {
  if (!is_secure_)
    return false;
  SSLClientSocket* ssl_socket = GetSSLClientSocket();
  if (ssl_socket->GetNegotiatedProtocol() < kProtoSPDY3)
    return false;
  return ssl_socket->WasChannelIDSent();
}

void SpdySession::AddPooledAlias(const SpdySessionKey& alias_key) {
  pooled_aliases_.insert(alias_key);
}

int SpdySession::GetProtocolVersion() const {
  DCHECK(buffered_spdy_framer_.get());
  return buffered_spdy_framer_->protocol_version();
}

bool SpdySession::CloseOneIdleConnection() {
  DCHECK(spdy_session_pool_);
  if (num_active_streams() > 0)
    return false;
  base::WeakPtr<SpdySession> weak_ptr = weak_factory_.GetWeakPtr();
  // Will remove a reference to this.
  RemoveFromPool();
  // Since the underlying socket is only returned when |this| is destroyed,
  // we should only return true if |this| no longer exists.
  return !weak_ptr;
}

void SpdySession::EnqueueStreamWrite(
    const base::WeakPtr<SpdyStream>& stream,
    SpdyFrameType frame_type,
    scoped_ptr<SpdyBufferProducer> producer) {
  DCHECK(frame_type == HEADERS ||
         frame_type == DATA ||
         frame_type == CREDENTIAL ||
         frame_type == SYN_STREAM);
  EnqueueWrite(stream->priority(), frame_type, producer.Pass(), stream);
}

scoped_ptr<SpdyFrame> SpdySession::CreateSynStream(
    SpdyStreamId stream_id,
    RequestPriority priority,
    uint8 credential_slot,
    SpdyControlFlags flags,
    const SpdyHeaderBlock& headers) {
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  CHECK(it != active_streams_.end());
  CHECK_EQ(it->second.stream->stream_id(), stream_id);

  SendPrefacePingIfNoneInFlight();

  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> syn_frame(
      buffered_spdy_framer_->CreateSynStream(
          stream_id, 0,
          ConvertRequestPriorityToSpdyPriority(priority, GetProtocolVersion()),
          credential_slot, flags, enable_compression_, &headers));

  base::StatsCounter spdy_requests("spdy.requests");
  spdy_requests.Increment();
  streams_initiated_count_++;

  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_SYN_STREAM,
        base::Bind(&NetLogSpdySynCallback, &headers,
                   (flags & CONTROL_FLAG_FIN) != 0,
                   (flags & CONTROL_FLAG_UNIDIRECTIONAL) != 0,
                   stream_id, 0));
  }

  return syn_frame.Pass();
}

int SpdySession::CreateCredentialFrame(
    const std::string& origin,
    SSLClientCertType type,
    const std::string& key,
    const std::string& cert,
    RequestPriority priority,
    scoped_ptr<SpdyFrame>* credential_frame) {
  DCHECK(is_secure_);
  SSLClientSocket* ssl_socket = GetSSLClientSocket();
  DCHECK(ssl_socket);
  DCHECK(ssl_socket->WasChannelIDSent());

  SpdyCredential credential;
  std::string tls_unique;
  ssl_socket->GetTLSUniqueChannelBinding(&tls_unique);
  size_t slot = credential_state_.SetHasCredential(GURL(origin));
  int rv = SpdyCredentialBuilder::Build(tls_unique, type, key, cert, slot,
                                        &credential);
  DCHECK_NE(rv, ERR_IO_PENDING);
  if (rv != OK)
    return rv;

  DCHECK(buffered_spdy_framer_.get());
  credential_frame->reset(
      buffered_spdy_framer_->CreateCredentialFrame(credential));

  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_SEND_CREDENTIAL,
        base::Bind(&NetLogSpdyCredentialCallback, credential.slot, &origin));
  }
  return OK;
}

scoped_ptr<SpdyBuffer> SpdySession::CreateDataBuffer(SpdyStreamId stream_id,
                                                     IOBuffer* data,
                                                     int len,
                                                     SpdyDataFlags flags) {
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  CHECK(it != active_streams_.end());
  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  if (len < 0) {
    NOTREACHED();
    return scoped_ptr<SpdyBuffer>();
  }

  int effective_len = std::min(len, kMaxSpdyFrameChunkSize);

  bool send_stalled_by_stream =
      (flow_control_state_ >= FLOW_CONTROL_STREAM) &&
      (stream->send_window_size() <= 0);
  bool send_stalled_by_session = IsSendStalled();

  // NOTE: There's an enum of the same name in histograms.xml.
  enum SpdyFrameFlowControlState {
    SEND_NOT_STALLED,
    SEND_STALLED_BY_STREAM,
    SEND_STALLED_BY_SESSION,
    SEND_STALLED_BY_STREAM_AND_SESSION,
  };

  SpdyFrameFlowControlState frame_flow_control_state = SEND_NOT_STALLED;
  if (send_stalled_by_stream) {
    if (send_stalled_by_session) {
      frame_flow_control_state = SEND_STALLED_BY_STREAM_AND_SESSION;
    } else {
      frame_flow_control_state = SEND_STALLED_BY_STREAM;
    }
  } else if (send_stalled_by_session) {
    frame_flow_control_state = SEND_STALLED_BY_SESSION;
  }

  if (flow_control_state_ == FLOW_CONTROL_STREAM) {
    UMA_HISTOGRAM_ENUMERATION(
        "Net.SpdyFrameStreamFlowControlState",
        frame_flow_control_state,
        SEND_STALLED_BY_STREAM + 1);
  } else if (flow_control_state_ == FLOW_CONTROL_STREAM_AND_SESSION) {
    UMA_HISTOGRAM_ENUMERATION(
        "Net.SpdyFrameStreamAndSessionFlowControlState",
        frame_flow_control_state,
        SEND_STALLED_BY_STREAM_AND_SESSION + 1);
  }

  // Obey send window size of the stream if stream flow control is
  // enabled.
  if (flow_control_state_ >= FLOW_CONTROL_STREAM) {
    if (send_stalled_by_stream) {
      stream->set_send_stalled_by_flow_control(true);
      // Even though we're currently stalled only by the stream, we
      // might end up being stalled by the session also.
      QueueSendStalledStream(*stream);
      net_log().AddEvent(
          NetLog::TYPE_SPDY_SESSION_STREAM_STALLED_BY_STREAM_SEND_WINDOW,
          NetLog::IntegerCallback("stream_id", stream_id));
      return scoped_ptr<SpdyBuffer>();
    }

    effective_len = std::min(effective_len, stream->send_window_size());
  }

  // Obey send window size of the session if session flow control is
  // enabled.
  if (flow_control_state_ == FLOW_CONTROL_STREAM_AND_SESSION) {
    if (send_stalled_by_session) {
      stream->set_send_stalled_by_flow_control(true);
      QueueSendStalledStream(*stream);
      net_log().AddEvent(
          NetLog::TYPE_SPDY_SESSION_STREAM_STALLED_BY_SESSION_SEND_WINDOW,
          NetLog::IntegerCallback("stream_id", stream_id));
      return scoped_ptr<SpdyBuffer>();
    }

    effective_len = std::min(effective_len, session_send_window_size_);
  }

  DCHECK_GE(effective_len, 0);

  // Clear FIN flag if only some of the data will be in the data
  // frame.
  if (effective_len < len)
    flags = static_cast<SpdyDataFlags>(flags & ~DATA_FLAG_FIN);

  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_SEND_DATA,
        base::Bind(&NetLogSpdyDataCallback, stream_id, effective_len,
                   (flags & DATA_FLAG_FIN) != 0));
  }

  // Send PrefacePing for DATA_FRAMEs with nonzero payload size.
  if (effective_len > 0)
    SendPrefacePingIfNoneInFlight();

  // TODO(mbelshe): reduce memory copies here.
  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> frame(
      buffered_spdy_framer_->CreateDataFrame(
          stream_id, data->data(),
          static_cast<uint32>(effective_len), flags));

  scoped_ptr<SpdyBuffer> data_buffer(new SpdyBuffer(frame.Pass()));

  if (flow_control_state_ == FLOW_CONTROL_STREAM_AND_SESSION) {
    DecreaseSendWindowSize(static_cast<int32>(effective_len));
    data_buffer->AddConsumeCallback(
        base::Bind(&SpdySession::OnWriteBufferConsumed,
                   weak_factory_.GetWeakPtr(),
                   static_cast<size_t>(effective_len)));
  }

  return data_buffer.Pass();
}

void SpdySession::CloseActiveStream(SpdyStreamId stream_id, int status) {
  DCHECK_NE(stream_id, 0u);

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    NOTREACHED();
    return;
  }

  CloseActiveStreamIterator(it, status);
}

void SpdySession::CloseCreatedStream(
    const base::WeakPtr<SpdyStream>& stream, int status) {
  DCHECK_EQ(stream->stream_id(), 0u);

  CreatedStreamSet::iterator it = created_streams_.find(stream.get());
  if (it == created_streams_.end()) {
    NOTREACHED();
    return;
  }

  CloseCreatedStreamIterator(it, status);
}

void SpdySession::ResetStream(SpdyStreamId stream_id,
                              SpdyRstStreamStatus status,
                              const std::string& description) {
  DCHECK_NE(stream_id, 0u);

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    NOTREACHED();
    return;
  }

  ResetStreamIterator(it, status, description);
}

bool SpdySession::IsStreamActive(SpdyStreamId stream_id) const {
  return ContainsKey(active_streams_, stream_id);
}

LoadState SpdySession::GetLoadState() const {
  // Just report that we're idle since the session could be doing
  // many things concurrently.
  return LOAD_STATE_IDLE;
}

void SpdySession::CloseActiveStreamIterator(ActiveStreamMap::iterator it,
                                            int status) {
  // TODO(mbelshe): We should send a RST_STREAM control frame here
  //                so that the server can cancel a large send.

  // For push streams, if they are being deleted normally, we leave
  // the stream in the unclaimed_pushed_streams_ list.  However, if
  // the stream is errored out, clean it up entirely.
  if (status != OK) {
    for (PushedStreamMap::iterator it2 = unclaimed_pushed_streams_.begin();
         it2 != unclaimed_pushed_streams_.end(); ++it2) {
      if (it2->second.stream_id == it->first) {
        unclaimed_pushed_streams_.erase(it2);
        break;
      }
    }
  }

  scoped_ptr<SpdyStream> owned_stream(it->second.stream);
  active_streams_.erase(it);

  DeleteStream(owned_stream.Pass(), status);
}

void SpdySession::CloseCreatedStreamIterator(CreatedStreamSet::iterator it,
                                             int status) {
  scoped_ptr<SpdyStream> owned_stream(*it);
  created_streams_.erase(it);
  DeleteStream(owned_stream.Pass(), status);
}

void SpdySession::ResetStreamIterator(ActiveStreamMap::iterator it,
                                      SpdyRstStreamStatus status,
                                      const std::string& description) {
  // Make sure CloseActiveStreamIterator() does not release the last
  // reference to |this|.
  scoped_refptr<SpdySession> self(this);

  SpdyStreamId stream_id = it->first;
  RequestPriority priority = it->second.stream->priority();
  // Removes any pending writes for the stream except for possibly an
  // in-flight one.
  CloseActiveStreamIterator(it, ERR_SPDY_PROTOCOL_ERROR);

  SendResetStreamFrame(stream_id, priority, status, description);
}

void SpdySession::SendResetStreamFrame(SpdyStreamId stream_id,
                                       RequestPriority priority,
                                       SpdyRstStreamStatus status,
                                       const std::string& description) {
  DCHECK_NE(stream_id, 0u);
  DCHECK(active_streams_.find(stream_id) == active_streams_.end());

  net_log().AddEvent(
      NetLog::TYPE_SPDY_SESSION_SEND_RST_STREAM,
      base::Bind(&NetLogSpdyRstCallback, stream_id, status, &description));

  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> rst_frame(
      buffered_spdy_framer_->CreateRstStream(stream_id, status));

  EnqueueSessionWrite(priority, RST_STREAM, rst_frame.Pass());
  RecordProtocolErrorHistogram(
      static_cast<SpdyProtocolErrorDetails>(status + STATUS_CODE_INVALID));
}

int SpdySession::DoReadLoop(ReadState expected_read_state, int result) {
  DCHECK_EQ(read_state_, expected_read_state);

  if (availability_state_ == STATE_CLOSED) {
    DCHECK_LT(error_on_close_, ERR_IO_PENDING);
    return error_on_close_;
  }

  // The SpdyFramer will use callbacks onto |this| as it parses frames.
  // When errors occur, those callbacks can lead to teardown of all references
  // to |this|, so maintain a reference to self during this call for safe
  // cleanup.
  scoped_refptr<SpdySession> self(this);

  int bytes_read_without_yielding = 0;

  // Loop until the session is closed, the read becomes blocked, or
  // the read limit is exceeded.
  while (true) {
    switch (read_state_) {
      case READ_STATE_DO_READ:
        DCHECK_EQ(result, OK);
        result = DoRead();
        break;
      case READ_STATE_DO_READ_COMPLETE:
        if (result > 0)
          bytes_read_without_yielding += result;
        result = DoReadComplete(result);
        break;
      default:
        NOTREACHED() << "read_state_: " << read_state_;
        break;
    }

    if (availability_state_ == STATE_CLOSED) {
      DCHECK_EQ(result, error_on_close_);
      DCHECK_LT(result, ERR_IO_PENDING);
      return result;
    }

    if (result == ERR_IO_PENDING)
      return ERR_IO_PENDING;

    if (bytes_read_without_yielding > kMaxReadBytesWithoutYielding) {
      read_state_ = READ_STATE_DO_READ;
      base::MessageLoop::current()->PostTask(
          FROM_HERE,
          base::Bind(base::IgnoreResult(&SpdySession::DoReadLoop),
                     weak_factory_.GetWeakPtr(), READ_STATE_DO_READ, OK));
      return ERR_IO_PENDING;
    }
  }
}

int SpdySession::DoRead() {
  DCHECK_NE(availability_state_, STATE_CLOSED);

  CHECK(connection_);
  CHECK(connection_->socket());
  read_state_ = READ_STATE_DO_READ_COMPLETE;
  return connection_->socket()->Read(
      read_buffer_.get(),
      kReadBufferSize,
      base::Bind(base::IgnoreResult(&SpdySession::DoReadLoop),
                 weak_factory_.GetWeakPtr(), READ_STATE_DO_READ_COMPLETE));
}

int SpdySession::DoReadComplete(int result) {
  DCHECK_NE(availability_state_, STATE_CLOSED);

  // Parse a frame.  For now this code requires that the frame fit into our
  // buffer (kReadBufferSize).
  // TODO(mbelshe): support arbitrarily large frames!

  if (result == 0) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySession.BytesRead.EOF",
                                total_bytes_received_, 1, 100000000, 50);
    CloseSessionOnError(ERR_CONNECTION_CLOSED, "Connection closed");
    DCHECK_EQ(availability_state_, STATE_CLOSED);
    DCHECK_EQ(error_on_close_, ERR_CONNECTION_CLOSED);
    return ERR_CONNECTION_CLOSED;
  }

  if (result < 0) {
    CloseSessionOnError(static_cast<Error>(result), "result is < 0.");
    DCHECK_EQ(availability_state_, STATE_CLOSED);
    DCHECK_EQ(error_on_close_, result);
    return result;
  }

  total_bytes_received_ += result;

  last_activity_time_ = base::TimeTicks::Now();

  DCHECK(buffered_spdy_framer_.get());
  char* data = read_buffer_->data();
  while (result > 0) {
    uint32 bytes_processed = buffered_spdy_framer_->ProcessInput(data, result);
    result -= bytes_processed;
    data += bytes_processed;

    if (availability_state_ == STATE_CLOSED) {
      DCHECK_LT(error_on_close_, ERR_IO_PENDING);
      return error_on_close_;
    }

    DCHECK_EQ(buffered_spdy_framer_->error_code(), SpdyFramer::SPDY_NO_ERROR);
  }

  read_state_ = READ_STATE_DO_READ;
  return OK;
}

int SpdySession::DoWriteLoop(WriteState expected_write_state, int result) {
  DCHECK_EQ(write_state_, expected_write_state);

  if (availability_state_ == STATE_CLOSED) {
    DCHECK_LT(error_on_close_, ERR_IO_PENDING);
    return error_on_close_;
  }

  // Releasing the in-flight write in DoWriteComplete() can have a
  // side-effect of dropping the last reference to |this|.  Hold a
  // reference through this function.
  scoped_refptr<SpdySession> self(this);

  // Loop until the session is closed or the write becomes blocked.
  while (true) {
    switch (write_state_) {
      case WRITE_STATE_DO_WRITE:
        DCHECK_EQ(result, OK);
        result = DoWrite();
        break;
      case WRITE_STATE_DO_WRITE_COMPLETE:
        result = DoWriteComplete(result);
        break;
      default:
        NOTREACHED() << "write_state_: " << write_state_;
        break;
    }

    if (availability_state_ == STATE_CLOSED) {
      DCHECK_EQ(result, error_on_close_);
      DCHECK_LT(result, ERR_IO_PENDING);
      return result;
    }

    if (result == ERR_IO_PENDING)
      return ERR_IO_PENDING;
  }
}

int SpdySession::DoWrite() {
  DCHECK_NE(availability_state_, STATE_CLOSED);

  DCHECK(buffered_spdy_framer_);
  if (in_flight_write_) {
    DCHECK_GT(in_flight_write_->GetRemainingSize(), 0u);
  } else {
    // Grab the next frame to send.
    SpdyFrameType frame_type = DATA;
    scoped_ptr<SpdyBufferProducer> producer;
    base::WeakPtr<SpdyStream> stream;
    if (!write_queue_.Dequeue(&frame_type, &producer, &stream)) {
      DCHECK_EQ(write_state_, WRITE_STATE_DO_WRITE);
      return ERR_IO_PENDING;
    }

    if (stream.get())
      DCHECK(!stream->IsClosed());

    // Activate the stream only when sending the SYN_STREAM frame to
    // guarantee monotonically-increasing stream IDs.
    if (frame_type == SYN_STREAM) {
      if (stream.get() && stream->stream_id() == 0) {
        scoped_ptr<SpdyStream> owned_stream =
            ActivateCreatedStream(stream.get());
        InsertActivatedStream(owned_stream.Pass());
      } else {
        NOTREACHED();
        return ERR_UNEXPECTED;
      }
    }

    in_flight_write_ = producer->ProduceBuffer();
    if (!in_flight_write_) {
      NOTREACHED();
      return ERR_UNEXPECTED;
    }
    in_flight_write_frame_type_ = frame_type;
    in_flight_write_frame_size_ = in_flight_write_->GetRemainingSize();
    DCHECK_GE(in_flight_write_frame_size_,
              buffered_spdy_framer_->GetFrameMinimumSize());
    in_flight_write_stream_ = stream;
  }

  write_state_ = WRITE_STATE_DO_WRITE_COMPLETE;

  // Explicitly store in a scoped_refptr<IOBuffer> to avoid problems
  // with Socket implementations that don't store their IOBuffer
  // argument in a scoped_refptr<IOBuffer> (see crbug.com/232345).
  scoped_refptr<IOBuffer> write_io_buffer =
      in_flight_write_->GetIOBufferForRemainingData();
  // We keep |in_flight_write_| alive until OnWriteComplete(), so it's
  // okay to pass in |write_io_buffer| since the socket won't use it
  // past OnWriteComplete().
  return connection_->socket()->Write(
      write_io_buffer.get(),
      in_flight_write_->GetRemainingSize(),
      base::Bind(base::IgnoreResult(&SpdySession::DoWriteLoop),
                 weak_factory_.GetWeakPtr(), WRITE_STATE_DO_WRITE_COMPLETE));
}

int SpdySession::DoWriteComplete(int result) {
  DCHECK_NE(availability_state_, STATE_CLOSED);

  DCHECK_GT(in_flight_write_->GetRemainingSize(), 0u);

  last_activity_time_ = base::TimeTicks::Now();

  if (result < 0) {
    DCHECK_NE(result, ERR_IO_PENDING);
    in_flight_write_.reset();
    in_flight_write_frame_type_ = DATA;
    in_flight_write_frame_size_ = 0;
    in_flight_write_stream_.reset();
    CloseSessionOnError(static_cast<Error>(result), "Write error");
    DCHECK_EQ(error_on_close_, result);
    return result;
  }

  // It should not be possible to have written more bytes than our
  // in_flight_write_.
  DCHECK_LE(static_cast<size_t>(result),
            in_flight_write_->GetRemainingSize());

  if (result > 0) {
    in_flight_write_->Consume(static_cast<size_t>(result));

    // We only notify the stream when we've fully written the pending frame.
    if (in_flight_write_->GetRemainingSize() == 0) {
      // It is possible that the stream was cancelled while we were
      // writing to the socket.
      if (in_flight_write_stream_.get()) {
        DCHECK_GT(in_flight_write_frame_size_, 0u);
        in_flight_write_stream_->OnFrameWriteComplete(
            in_flight_write_frame_type_,
            in_flight_write_frame_size_);
      }

      // Cleanup the write which just completed.
      in_flight_write_.reset();
      in_flight_write_frame_type_ = DATA;
      in_flight_write_frame_size_ = 0;
      in_flight_write_stream_.reset();
    }
  }

  // This has to go after calling OnFrameWriteComplete() so that if
  // that ends up calling EnqueueWrite(), it properly avoids posting
  // another DoWriteLoop task.
  write_state_ = WRITE_STATE_DO_WRITE;
  return OK;
}

void SpdySession::CloseAllStreamsAfter(SpdyStreamId last_good_stream_id,
                                       Error status) {
  for (int i = 0; i < NUM_PRIORITIES; ++i) {
    PendingStreamRequestQueue queue;
    queue.swap(pending_create_stream_queues_[i]);
    for (PendingStreamRequestQueue::const_iterator it = queue.begin();
         it != queue.end(); ++it) {
      CHECK(*it);
      (*it)->OnRequestCompleteFailure(ERR_ABORTED);
    }
  }

  // The loops below are carefully written to avoid problems with
  // streams closing other streams.

  while (true) {
    ActiveStreamMap::iterator it =
        active_streams_.lower_bound(last_good_stream_id + 1);
    if (it == active_streams_.end())
      break;
    streams_abandoned_count_++;
    LogAbandonedStream(it->second.stream, status);
    CloseActiveStreamIterator(it, status);
  }

  while (!created_streams_.empty()) {
    CreatedStreamSet::iterator it = created_streams_.begin();
    LogAbandonedStream(*it, status);
    CloseCreatedStreamIterator(it, status);
  }

  write_queue_.RemovePendingWritesForStreamsAfter(last_good_stream_id);
}

void SpdySession::CloseAllStreams(Error status) {
  base::StatsCounter abandoned_streams("spdy.abandoned_streams");
  base::StatsCounter abandoned_push_streams(
      "spdy.abandoned_push_streams");

  if (!unclaimed_pushed_streams_.empty()) {
    streams_abandoned_count_ += unclaimed_pushed_streams_.size();
    abandoned_push_streams.Add(unclaimed_pushed_streams_.size());
    unclaimed_pushed_streams_.clear();
  }

  CloseAllStreamsAfter(0, status);
  write_queue_.Clear();
}

void SpdySession::LogAbandonedStream(SpdyStream* stream, Error status) {
  DCHECK(stream);
  std::string description = base::StringPrintf(
      "ABANDONED (stream_id=%d): ", stream->stream_id()) + stream->path();
  stream->LogStreamError(status, description);
}

int SpdySession::GetNewStreamId() {
  int id = stream_hi_water_mark_;
  stream_hi_water_mark_ += 2;
  if (stream_hi_water_mark_ > 0x7fff)
    stream_hi_water_mark_ = 1;
  return id;
}

void SpdySession::CloseSessionOnError(Error err,
                                      const std::string& description) {
  // Closing all streams can have a side-effect of dropping the last reference
  // to |this|.  Hold a reference through this function.
  scoped_refptr<SpdySession> self(this);

  DCHECK_LT(err, ERR_IO_PENDING);
  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_CLOSE,
      base::Bind(&NetLogSpdySessionCloseCallback, err, &description));

  // Don't close twice.  This can occur because we can have both
  // a read and a write outstanding, and each can complete with
  // an error.
  if (availability_state_ != STATE_CLOSED) {
    UMA_HISTOGRAM_SPARSE_SLOWLY("Net.SpdySession.ClosedOnError", -err);
    UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySession.BytesRead.OtherErrors",
                                total_bytes_received_, 1, 100000000, 50);
    availability_state_ = STATE_CLOSED;
    error_on_close_ = err;
    // TODO(akalin): Move this after CloseAllStreams() once we're
    // owned by the pool.
    RemoveFromPool();
    CloseAllStreams(err);
  }
}

base::Value* SpdySession::GetInfoAsValue() const {
  base::DictionaryValue* dict = new base::DictionaryValue();

  dict->SetInteger("source_id", net_log_.source().id);

  dict->SetString("host_port_pair", host_port_pair().ToString());
  if (!pooled_aliases_.empty()) {
    base::ListValue* alias_list = new base::ListValue();
    for (std::set<SpdySessionKey>::const_iterator it =
             pooled_aliases_.begin();
         it != pooled_aliases_.end(); it++) {
      alias_list->Append(new base::StringValue(
          it->host_port_pair().ToString()));
    }
    dict->Set("aliases", alias_list);
  }
  dict->SetString("proxy", host_port_proxy_pair().second.ToURI());

  dict->SetInteger("active_streams", active_streams_.size());

  dict->SetInteger("unclaimed_pushed_streams",
      unclaimed_pushed_streams_.size());

  dict->SetBoolean("is_secure", is_secure_);

  dict->SetString("protocol_negotiated",
                  SSLClientSocket::NextProtoToString(
                      connection_->socket()->GetNegotiatedProtocol()));

  dict->SetInteger("error", error_on_close_);
  dict->SetInteger("max_concurrent_streams", max_concurrent_streams_);

  dict->SetInteger("streams_initiated_count", streams_initiated_count_);
  dict->SetInteger("streams_pushed_count", streams_pushed_count_);
  dict->SetInteger("streams_pushed_and_claimed_count",
      streams_pushed_and_claimed_count_);
  dict->SetInteger("streams_abandoned_count", streams_abandoned_count_);
  DCHECK(buffered_spdy_framer_.get());
  dict->SetInteger("frames_received", buffered_spdy_framer_->frames_received());

  dict->SetBoolean("sent_settings", sent_settings_);
  dict->SetBoolean("received_settings", received_settings_);

  dict->SetInteger("send_window_size", session_send_window_size_);
  dict->SetInteger("recv_window_size", session_recv_window_size_);
  dict->SetInteger("unacked_recv_window_bytes",
                   session_unacked_recv_window_bytes_);
  return dict;
}

bool SpdySession::IsReused() const {
  return buffered_spdy_framer_->frames_received() > 0;
}

bool SpdySession::GetLoadTimingInfo(SpdyStreamId stream_id,
                                    LoadTimingInfo* load_timing_info) const {
  return connection_->GetLoadTimingInfo(stream_id != kFirstStreamId,
                                        load_timing_info);
}

int SpdySession::GetPeerAddress(IPEndPoint* address) const {
  int rv = ERR_SOCKET_NOT_CONNECTED;
  if (connection_->socket()) {
    rv = connection_->socket()->GetPeerAddress(address);
  }

  UMA_HISTOGRAM_BOOLEAN("Net.SpdySessionSocketNotConnectedGetPeerAddress",
                        rv == ERR_SOCKET_NOT_CONNECTED);

  return rv;
}

int SpdySession::GetLocalAddress(IPEndPoint* address) const {
  int rv = ERR_SOCKET_NOT_CONNECTED;
  if (connection_->socket()) {
    rv = connection_->socket()->GetLocalAddress(address);
  }

  UMA_HISTOGRAM_BOOLEAN("Net.SpdySessionSocketNotConnectedGetLocalAddress",
                        rv == ERR_SOCKET_NOT_CONNECTED);

  return rv;
}

void SpdySession::EnqueueSessionWrite(RequestPriority priority,
                                      SpdyFrameType frame_type,
                                      scoped_ptr<SpdyFrame> frame) {
  DCHECK(frame_type == RST_STREAM ||
         frame_type == SETTINGS ||
         frame_type == WINDOW_UPDATE ||
         frame_type == PING);
  EnqueueWrite(
      priority, frame_type,
      scoped_ptr<SpdyBufferProducer>(
          new SimpleBufferProducer(
              scoped_ptr<SpdyBuffer>(new SpdyBuffer(frame.Pass())))),
      base::WeakPtr<SpdyStream>());
}

void SpdySession::EnqueueWrite(RequestPriority priority,
                               SpdyFrameType frame_type,
                               scoped_ptr<SpdyBufferProducer> producer,
                               const base::WeakPtr<SpdyStream>& stream) {
  if (availability_state_ == STATE_CLOSED)
    return;

  bool was_idle = write_queue_.IsEmpty();
  write_queue_.Enqueue(priority, frame_type, producer.Pass(), stream);
  // We only want to post a task when the queue was just empty *and*
  // we're not being called from a callback from DoWriteComplete().
  if (was_idle && write_state_ == WRITE_STATE_DO_WRITE) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE,
        base::Bind(base::IgnoreResult(&SpdySession::DoWriteLoop),
                   weak_factory_.GetWeakPtr(), WRITE_STATE_DO_WRITE, OK));
  }
}

void SpdySession::InsertCreatedStream(scoped_ptr<SpdyStream> stream) {
  DCHECK_EQ(stream->stream_id(), 0u);
  DCHECK(created_streams_.find(stream.get()) == created_streams_.end());
  created_streams_.insert(stream.release());
}

scoped_ptr<SpdyStream> SpdySession::ActivateCreatedStream(SpdyStream* stream) {
  DCHECK_EQ(stream->stream_id(), 0u);
  DCHECK(created_streams_.find(stream) != created_streams_.end());
  stream->set_stream_id(GetNewStreamId());
  scoped_ptr<SpdyStream> owned_stream(stream);
  created_streams_.erase(stream);
  return owned_stream.Pass();
}

void SpdySession::InsertActivatedStream(scoped_ptr<SpdyStream> stream) {
  SpdyStreamId stream_id = stream->stream_id();
  DCHECK_NE(stream_id, 0u);
  std::pair<ActiveStreamMap::iterator, bool> result =
      active_streams_.insert(
          std::make_pair(stream_id, ActiveStreamInfo(stream.get())));
  if (result.second) {
    ignore_result(stream.release());
  } else {
    NOTREACHED();
  }
}

void SpdySession::DeleteStream(scoped_ptr<SpdyStream> stream, int status) {
  if (in_flight_write_stream_.get() == stream.get()) {
    // If we're deleting the stream for the in-flight write, we still
    // need to let the write complete, so we clear
    // |in_flight_write_stream_| and let the write finish on its own
    // without notifying |in_flight_write_stream_|.
    in_flight_write_stream_.reset();
  }

  write_queue_.RemovePendingWritesForStream(stream->GetWeakPtr());

  stream->OnClose(status);

  ProcessPendingStreamRequests();

  // Deleting |stream| may release the last reference to |this|.
}

void SpdySession::RemoveFromPool() {
  if (spdy_session_pool_) {
    SpdySessionPool* pool = spdy_session_pool_;
    spdy_session_pool_ = NULL;
    scoped_refptr<SpdySession> self(this);
    pool->MakeSessionUnavailable(self);
    pool->RemoveUnavailableSession(self);
  }
}

base::WeakPtr<SpdyStream> SpdySession::GetActivePushStream(
    const std::string& path) {
  base::StatsCounter used_push_streams("spdy.claimed_push_streams");

  PushedStreamMap::iterator it = unclaimed_pushed_streams_.find(path);
  if (it == unclaimed_pushed_streams_.end())
    return base::WeakPtr<SpdyStream>();

  SpdyStreamId stream_id = it->second.stream_id;
  unclaimed_pushed_streams_.erase(it);

  ActiveStreamMap::iterator it2 = active_streams_.find(stream_id);
  if (it2 == active_streams_.end()) {
    NOTREACHED();
    return base::WeakPtr<SpdyStream>();
  }

  net_log_.AddEvent(NetLog::TYPE_SPDY_STREAM_ADOPTED_PUSH_STREAM);
  used_push_streams.Increment();
  return it2->second.stream->GetWeakPtr();
}

bool SpdySession::GetSSLInfo(SSLInfo* ssl_info,
                             bool* was_npn_negotiated,
                             NextProto* protocol_negotiated) {
  *was_npn_negotiated = connection_->socket()->WasNpnNegotiated();
  *protocol_negotiated = connection_->socket()->GetNegotiatedProtocol();
  return connection_->socket()->GetSSLInfo(ssl_info);
}

bool SpdySession::GetSSLCertRequestInfo(
    SSLCertRequestInfo* cert_request_info) {
  if (!is_secure_)
    return false;
  GetSSLClientSocket()->GetSSLCertRequestInfo(cert_request_info);
  return true;
}

ServerBoundCertService* SpdySession::GetServerBoundCertService() const {
  if (!is_secure_)
    return NULL;
  return GetSSLClientSocket()->GetServerBoundCertService();
}

void SpdySession::OnError(SpdyFramer::SpdyError error_code) {
  if (availability_state_ == STATE_CLOSED)
    return;

  RecordProtocolErrorHistogram(
      static_cast<SpdyProtocolErrorDetails>(error_code));
  std::string description = base::StringPrintf(
      "SPDY_ERROR error_code: %d.", error_code);
  CloseSessionOnError(ERR_SPDY_PROTOCOL_ERROR, description);
}

void SpdySession::OnStreamError(SpdyStreamId stream_id,
                                const std::string& description) {
  if (availability_state_ == STATE_CLOSED)
    return;

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // We still want to send a frame to reset the stream even if we
    // don't know anything about it.
    SendResetStreamFrame(
        stream_id, IDLE, RST_STREAM_PROTOCOL_ERROR, description);
    return;
  }

  ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, description);
}

void SpdySession::OnStreamFrameData(SpdyStreamId stream_id,
                                    const char* data,
                                    size_t len,
                                    bool fin) {
  if (availability_state_ == STATE_CLOSED)
    return;

  DCHECK_LT(len, 1u << 24);
  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_RECV_DATA,
        base::Bind(&NetLogSpdyDataCallback, stream_id, len, fin));
  }

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);

  // By the time data comes in, the stream may already be inactive.
  if (it == active_streams_.end())
    return;

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  if (it->second.waiting_for_syn_reply) {
    const std::string& error = "Data received before SYN_REPLY.";
    stream->LogStreamError(ERR_SPDY_PROTOCOL_ERROR, error);
    ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, error);
    return;
  }

  scoped_ptr<SpdyBuffer> buffer;
  if (data) {
    DCHECK_GT(len, 0u);
    buffer.reset(new SpdyBuffer(data, len));

    if (flow_control_state_ == FLOW_CONTROL_STREAM_AND_SESSION) {
      DecreaseRecvWindowSize(static_cast<int32>(len));
      buffer->AddConsumeCallback(
          base::Bind(&SpdySession::OnReadBufferConsumed,
                     weak_factory_.GetWeakPtr()));
    }
  } else {
    DCHECK_EQ(len, 0u);
  }
  stream->OnDataReceived(buffer.Pass());
}

void SpdySession::OnSettings(bool clear_persisted) {
  if (availability_state_ == STATE_CLOSED)
    return;

  if (clear_persisted)
    http_server_properties_->ClearSpdySettings(host_port_pair());

  if (net_log_.IsLoggingAllEvents()) {
    net_log_.AddEvent(
        NetLog::TYPE_SPDY_SESSION_RECV_SETTINGS,
        base::Bind(&NetLogSpdySettingsCallback, host_port_pair(),
                   clear_persisted));
  }
}

void SpdySession::OnSetting(SpdySettingsIds id,
                            uint8 flags,
                            uint32 value) {
  if (availability_state_ == STATE_CLOSED)
    return;

  HandleSetting(id, value);
  http_server_properties_->SetSpdySetting(
      host_port_pair(),
      id,
      static_cast<SpdySettingsFlags>(flags),
      value);
  received_settings_ = true;

  // Log the setting.
  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_RECV_SETTING,
      base::Bind(&NetLogSpdySettingCallback,
                 id, static_cast<SpdySettingsFlags>(flags), value));
}

void SpdySession::OnSendCompressedFrame(
    SpdyStreamId stream_id,
    SpdyFrameType type,
    size_t payload_len,
    size_t frame_len) {
  if (type != SYN_STREAM)
    return;

  DCHECK(buffered_spdy_framer_.get());
  size_t compressed_len =
      frame_len - buffered_spdy_framer_->GetSynStreamMinimumSize();

  if (payload_len) {
    // Make sure we avoid early decimal truncation.
    int compression_pct = 100 - (100 * compressed_len) / payload_len;
    UMA_HISTOGRAM_PERCENTAGE("Net.SpdySynStreamCompressionPercentage",
                             compression_pct);
  }
}

int SpdySession::OnInitialResponseHeadersReceived(
    const SpdyHeaderBlock& response_headers,
    base::Time response_time,
    base::TimeTicks recv_first_byte_time,
    SpdyStream* stream) {
  // Make sure the stream being closed does not release the last
  // reference to |this|.
  scoped_refptr<SpdySession> self(this);

  SpdyStreamId stream_id = stream->stream_id();
  // May invalidate |stream|.
  int rv = stream->OnInitialResponseHeadersReceived(
      response_headers, response_time, recv_first_byte_time);
  if (rv < 0) {
    DCHECK_NE(rv, ERR_IO_PENDING);
    DCHECK(active_streams_.find(stream_id) == active_streams_.end());
  }
  return rv;
}

void SpdySession::OnSynStream(SpdyStreamId stream_id,
                              SpdyStreamId associated_stream_id,
                              SpdyPriority priority,
                              uint8 credential_slot,
                              bool fin,
                              bool unidirectional,
                              const SpdyHeaderBlock& headers) {
  if (availability_state_ == STATE_CLOSED)
    return;

  // TODO(akalin): Reset the stream when |availability_state_| ==
  // STATE_GOING_AWAY.

  base::Time response_time = base::Time::Now();
  base::TimeTicks recv_first_byte_time = base::TimeTicks::Now();

  if (net_log_.IsLoggingAllEvents()) {
    net_log_.AddEvent(
        NetLog::TYPE_SPDY_SESSION_PUSHED_SYN_STREAM,
        base::Bind(&NetLogSpdySynCallback,
                   &headers, fin, unidirectional,
                   stream_id, associated_stream_id));
  }

  // Server-initiated streams should have even sequence numbers.
  if ((stream_id & 0x1) != 0) {
    LOG(WARNING) << "Received invalid OnSyn stream id " << stream_id;
    return;
  }

  if (IsStreamActive(stream_id)) {
    LOG(WARNING) << "Received OnSyn for active stream " << stream_id;
    return;
  }

  RequestPriority request_priority =
      ConvertSpdyPriorityToRequestPriority(priority, GetProtocolVersion());

  if (associated_stream_id == 0) {
    std::string description = base::StringPrintf(
        "Received invalid OnSyn associated stream id %d for stream %d",
        associated_stream_id, stream_id);
    SendResetStreamFrame(stream_id, request_priority,
                         RST_STREAM_REFUSED_STREAM, description);
    return;
  }

  streams_pushed_count_++;

  // TODO(mbelshe): DCHECK that this is a GET method?

  // Verify that the response had a URL for us.
  GURL gurl = GetUrlFromHeaderBlock(headers, GetProtocolVersion(), true);
  if (!gurl.is_valid()) {
    SendResetStreamFrame(stream_id, request_priority, RST_STREAM_PROTOCOL_ERROR,
                         "Pushed stream url was invalid: " + gurl.spec());
    return;
  }
  const std::string& url = gurl.spec();

  // Verify we have a valid stream association.
  ActiveStreamMap::iterator associated_it =
      active_streams_.find(associated_stream_id);
  if (associated_it == active_streams_.end()) {
    SendResetStreamFrame(
        stream_id, request_priority, RST_STREAM_INVALID_STREAM,
        base::StringPrintf(
            "Received OnSyn with inactive associated stream %d",
            associated_stream_id));
    return;
  }

  // Check that the SYN advertises the same origin as its associated stream.
  // Bypass this check if and only if this session is with a SPDY proxy that
  // is trusted explicitly via the --trusted-spdy-proxy switch.
  if (trusted_spdy_proxy_.Equals(host_port_pair())) {
    // Disallow pushing of HTTPS content.
    if (gurl.SchemeIs("https")) {
      SendResetStreamFrame(
          stream_id, request_priority, RST_STREAM_REFUSED_STREAM,
          base::StringPrintf(
              "Rejected push of Cross Origin HTTPS content %d",
              associated_stream_id));
    }
  } else {
    GURL associated_url(associated_it->second.stream->GetUrl());
    if (associated_url.GetOrigin() != gurl.GetOrigin()) {
      SendResetStreamFrame(
          stream_id, request_priority, RST_STREAM_REFUSED_STREAM,
          base::StringPrintf(
              "Rejected Cross Origin Push Stream %d",
              associated_stream_id));
      return;
    }
  }

  // There should not be an existing pushed stream with the same path.
  if (unclaimed_pushed_streams_.find(url) != unclaimed_pushed_streams_.end()) {
    SendResetStreamFrame(stream_id, request_priority, RST_STREAM_PROTOCOL_ERROR,
                         "Received duplicate pushed stream with url: " + url);
    return;
  }

  scoped_ptr<SpdyStream> stream(
      new SpdyStream(SPDY_PUSH_STREAM, this, gurl.PathForRequest(),
                     request_priority,
                     stream_initial_send_window_size_,
                     stream_initial_recv_window_size_,
                     net_log_));
  stream->set_stream_id(stream_id);

  DeleteExpiredPushedStreams();
  unclaimed_pushed_streams_[url] = PushedStreamInfo(stream_id, time_func_());

  InsertActivatedStream(stream.Pass());

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    NOTREACHED();
    return;
  }

  // Parse the headers.
  if (OnInitialResponseHeadersReceived(
          headers, response_time,
          recv_first_byte_time, it->second.stream) != OK)
    return;

  base::StatsCounter push_requests("spdy.pushed_streams");
  push_requests.Increment();
}

void SpdySession::DeleteExpiredPushedStreams() {
  if (unclaimed_pushed_streams_.empty())
    return;

  // Check that adequate time has elapsed since the last sweep.
  if (time_func_() < next_unclaimed_push_stream_sweep_time_)
    return;

  // Gather old streams to delete.
  base::TimeTicks minimum_freshness = time_func_() -
      base::TimeDelta::FromSeconds(kMinPushedStreamLifetimeSeconds);
  std::vector<SpdyStreamId> streams_to_close;
  for (PushedStreamMap::iterator it = unclaimed_pushed_streams_.begin();
       it != unclaimed_pushed_streams_.end(); ++it) {
    if (minimum_freshness > it->second.creation_time)
      streams_to_close.push_back(it->second.stream_id);
  }

  for (std::vector<SpdyStreamId>::const_iterator it = streams_to_close.begin();
       it != streams_to_close.end(); ++it) {
    ActiveStreamMap::iterator it2 = active_streams_.find(*it);
    if (it2 == active_streams_.end())
      continue;

    base::StatsCounter abandoned_push_streams(
        "spdy.abandoned_push_streams");
    base::StatsCounter abandoned_streams("spdy.abandoned_streams");
    abandoned_push_streams.Increment();
    abandoned_streams.Increment();
    streams_abandoned_count_++;
    // CloseActiveStreamIterator() will remove the stream from
    // |unclaimed_pushed_streams_|.
    CloseActiveStreamIterator(it2, ERR_INVALID_SPDY_STREAM);
  }

  next_unclaimed_push_stream_sweep_time_ = time_func_() +
      base::TimeDelta::FromSeconds(kMinPushedStreamLifetimeSeconds);
}

void SpdySession::OnSynReply(SpdyStreamId stream_id,
                             bool fin,
                             const SpdyHeaderBlock& headers) {
  if (availability_state_ == STATE_CLOSED)
    return;

  base::Time response_time = base::Time::Now();
  base::TimeTicks recv_first_byte_time = base::TimeTicks::Now();

  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_SYN_REPLY,
        base::Bind(&NetLogSpdySynCallback,
                   &headers, fin, false,  // not unidirectional
                   stream_id, 0));
  }

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // NOTE:  it may just be that the stream was cancelled.
    return;
  }

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  if (!it->second.waiting_for_syn_reply) {
    const std::string& error =
        "Received duplicate SYN_REPLY for stream.";
    stream->LogStreamError(ERR_SPDY_PROTOCOL_ERROR, error);
    ResetStreamIterator(it, RST_STREAM_STREAM_IN_USE, error);
    return;
  }
  it->second.waiting_for_syn_reply = false;

  ignore_result(OnInitialResponseHeadersReceived(
      headers, response_time, recv_first_byte_time, stream));
}

void SpdySession::OnHeaders(SpdyStreamId stream_id,
                            bool fin,
                            const SpdyHeaderBlock& headers) {
  if (availability_state_ == STATE_CLOSED)
    return;

  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_RECV_HEADERS,
        base::Bind(&NetLogSpdySynCallback,
                   &headers, fin, /*unidirectional=*/false,
                   stream_id, 0));
  }

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // NOTE:  it may just be that the stream was cancelled.
    LOG(WARNING) << "Received HEADERS for invalid stream " << stream_id;
    return;
  }

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  int rv = stream->OnAdditionalResponseHeadersReceived(headers);
  if (rv < 0) {
    DCHECK_NE(rv, ERR_IO_PENDING);
    DCHECK(active_streams_.find(stream_id) == active_streams_.end());
  }
}

void SpdySession::OnRstStream(SpdyStreamId stream_id,
                              SpdyRstStreamStatus status) {
  if (availability_state_ == STATE_CLOSED)
    return;

  std::string description;
  net_log().AddEvent(
      NetLog::TYPE_SPDY_SESSION_RST_STREAM,
      base::Bind(&NetLogSpdyRstCallback,
                 stream_id, status, &description));

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // NOTE:  it may just be that the stream was cancelled.
    LOG(WARNING) << "Received RST for invalid stream" << stream_id;
    return;
  }

  CHECK_EQ(it->second.stream->stream_id(), stream_id);

  if (status == 0) {
    it->second.stream->OnDataReceived(scoped_ptr<SpdyBuffer>());
  } else if (status == RST_STREAM_REFUSED_STREAM) {
    CloseActiveStreamIterator(it, ERR_SPDY_SERVER_REFUSED_STREAM);
  } else {
    RecordProtocolErrorHistogram(
        PROTOCOL_ERROR_RST_STREAM_FOR_NON_ACTIVE_STREAM);
    it->second.stream->LogStreamError(
        ERR_SPDY_PROTOCOL_ERROR,
        base::StringPrintf("SPDY stream closed with status: %d", status));
    // TODO(mbelshe): Map from Spdy-protocol errors to something sensical.
    //                For now, it doesn't matter much - it is a protocol error.
    CloseActiveStreamIterator(it, ERR_SPDY_PROTOCOL_ERROR);
  }
}

void SpdySession::OnGoAway(SpdyStreamId last_accepted_stream_id,
                           SpdyGoAwayStatus status) {
  if (availability_state_ == STATE_CLOSED)
    return;

  net_log_.AddEvent(NetLog::TYPE_SPDY_SESSION_GOAWAY,
      base::Bind(&NetLogSpdyGoAwayCallback,
                 last_accepted_stream_id,
                 active_streams_.size(),
                 unclaimed_pushed_streams_.size(),
                 status));
  // TODO(akalin): Move into a "going away" state in the pool instead.
  RemoveFromPool();
  CloseAllStreamsAfter(last_accepted_stream_id, ERR_ABORTED);
}

void SpdySession::OnPing(uint32 unique_id) {
  if (availability_state_ == STATE_CLOSED)
    return;

  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_PING,
      base::Bind(&NetLogSpdyPingCallback, unique_id, "received"));

  // Send response to a PING from server.
  if (unique_id % 2 == 0) {
    WritePingFrame(unique_id);
    return;
  }

  --pings_in_flight_;
  if (pings_in_flight_ < 0) {
    RecordProtocolErrorHistogram(PROTOCOL_ERROR_UNEXPECTED_PING);
    CloseSessionOnError(ERR_SPDY_PROTOCOL_ERROR, "pings_in_flight_ is < 0.");
    pings_in_flight_ = 0;
    return;
  }

  if (pings_in_flight_ > 0)
    return;

  // We will record RTT in histogram when there are no more client sent
  // pings_in_flight_.
  RecordPingRTTHistogram(base::TimeTicks::Now() - last_ping_sent_time_);
}

void SpdySession::OnWindowUpdate(SpdyStreamId stream_id,
                                 uint32 delta_window_size) {
  if (availability_state_ == STATE_CLOSED)
    return;

  DCHECK_LE(delta_window_size, static_cast<uint32>(kint32max));
  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_RECEIVED_WINDOW_UPDATE_FRAME,
      base::Bind(&NetLogSpdyWindowUpdateFrameCallback,
                 stream_id, delta_window_size));

  if (stream_id == kSessionFlowControlStreamId) {
    // WINDOW_UPDATE for the session.
    if (flow_control_state_ < FLOW_CONTROL_STREAM_AND_SESSION) {
      LOG(WARNING) << "Received WINDOW_UPDATE for session when "
                   << "session flow control is not turned on";
      // TODO(akalin): Record an error and close the session.
      return;
    }

    if (delta_window_size < 1u) {
      RecordProtocolErrorHistogram(PROTOCOL_ERROR_INVALID_WINDOW_UPDATE_SIZE);
      CloseSessionOnError(
          ERR_SPDY_PROTOCOL_ERROR,
          "Received WINDOW_UPDATE with an invalid delta_window_size " +
          base::UintToString(delta_window_size));
      return;
    }

    IncreaseSendWindowSize(static_cast<int32>(delta_window_size));
  } else {
    // WINDOW_UPDATE for a stream.
    if (flow_control_state_ < FLOW_CONTROL_STREAM) {
      // TODO(akalin): Record an error and close the session.
      LOG(WARNING) << "Received WINDOW_UPDATE for stream " << stream_id
                   << " when flow control is not turned on";
      return;
    }

    ActiveStreamMap::iterator it = active_streams_.find(stream_id);

    if (it == active_streams_.end()) {
      // NOTE:  it may just be that the stream was cancelled.
      LOG(WARNING) << "Received WINDOW_UPDATE for invalid stream " << stream_id;
      return;
    }

    SpdyStream* stream = it->second.stream;
    CHECK_EQ(stream->stream_id(), stream_id);

    if (delta_window_size < 1u) {
      ResetStreamIterator(it,
                          RST_STREAM_FLOW_CONTROL_ERROR,
                          base::StringPrintf(
                              "Received WINDOW_UPDATE with an invalid "
                              "delta_window_size %ud", delta_window_size));
      return;
    }

    CHECK_EQ(it->second.stream->stream_id(), stream_id);
    it->second.stream->IncreaseSendWindowSize(
        static_cast<int32>(delta_window_size));
  }
}

void SpdySession::OnPushPromise(SpdyStreamId stream_id,
                                SpdyStreamId promised_stream_id) {
  // TODO(akalin): Handle PUSH_PROMISE frames.
}

void SpdySession::SendStreamWindowUpdate(SpdyStreamId stream_id,
                                         uint32 delta_window_size) {
  CHECK_GE(flow_control_state_, FLOW_CONTROL_STREAM);
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  CHECK(it != active_streams_.end());
  CHECK_EQ(it->second.stream->stream_id(), stream_id);
  SendWindowUpdateFrame(
      stream_id, delta_window_size, it->second.stream->priority());
}

void SpdySession::SendInitialSettings() {
  // First, notify the server about the settings they should use when
  // communicating with us.
  if (GetProtocolVersion() >= 2 && enable_sending_initial_settings_) {
    SettingsMap settings_map;
    // Create a new settings frame notifying the sever of our
    // max_concurrent_streams_ and initial window size.
    settings_map[SETTINGS_MAX_CONCURRENT_STREAMS] =
        SettingsFlagsAndValue(SETTINGS_FLAG_NONE, kMaxConcurrentPushedStreams);
    if (GetProtocolVersion() > 2 &&
        stream_initial_recv_window_size_ != kSpdyStreamInitialWindowSize) {
      settings_map[SETTINGS_INITIAL_WINDOW_SIZE] =
          SettingsFlagsAndValue(SETTINGS_FLAG_NONE,
                                stream_initial_recv_window_size_);
    }
    SendSettings(settings_map);
  }

  // Next, notify the server about our initial recv window size.
  if (flow_control_state_ == FLOW_CONTROL_STREAM_AND_SESSION &&
      enable_sending_initial_settings_) {
    // Bump up the receive window size to the real initial value. This
    // has to go here since the WINDOW_UPDATE frame sent by
    // IncreaseRecvWindowSize() call uses |buffered_spdy_framer_|.
    DCHECK_GT(kDefaultInitialRecvWindowSize, session_recv_window_size_);
    // This condition implies that |kDefaultInitialRecvWindowSize| -
    // |session_recv_window_size_| doesn't overflow.
    DCHECK_GT(session_recv_window_size_, 0);
    IncreaseRecvWindowSize(
        kDefaultInitialRecvWindowSize - session_recv_window_size_);
  }

  // Finally, notify the server about the settings they have previously
  // told us to use when communicating with them.
  const SettingsMap& settings_map =
      http_server_properties_->GetSpdySettings(host_port_pair());
  if (settings_map.empty())
    return;

  const SpdySettingsIds id = SETTINGS_CURRENT_CWND;
  SettingsMap::const_iterator it = settings_map.find(id);
  uint32 value = 0;
  if (it != settings_map.end())
    value = it->second.second;
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwndSent", value, 1, 200, 100);

  const SettingsMap& settings_map_new =
      http_server_properties_->GetSpdySettings(host_port_pair());
  for (SettingsMap::const_iterator i = settings_map_new.begin(),
           end = settings_map_new.end(); i != end; ++i) {
    const SpdySettingsIds new_id = i->first;
    const uint32 new_val = i->second.second;
    HandleSetting(new_id, new_val);
  }

  SendSettings(settings_map_new);
}


void SpdySession::SendSettings(const SettingsMap& settings) {
  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_SEND_SETTINGS,
      base::Bind(&NetLogSpdySendSettingsCallback, &settings));

  // Create the SETTINGS frame and send it.
  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> settings_frame(
      buffered_spdy_framer_->CreateSettings(settings));
  sent_settings_ = true;
  EnqueueSessionWrite(HIGHEST, SETTINGS, settings_frame.Pass());
}

void SpdySession::HandleSetting(uint32 id, uint32 value) {
  switch (id) {
    case SETTINGS_MAX_CONCURRENT_STREAMS:
      max_concurrent_streams_ = std::min(static_cast<size_t>(value),
                                         kMaxConcurrentStreamLimit);
      ProcessPendingStreamRequests();
      break;
    case SETTINGS_INITIAL_WINDOW_SIZE: {
      if (flow_control_state_ < FLOW_CONTROL_STREAM) {
        net_log().AddEvent(
            NetLog::TYPE_SPDY_SESSION_INITIAL_WINDOW_SIZE_NO_FLOW_CONTROL);
        return;
      }

      if (value > static_cast<uint32>(kint32max)) {
        net_log().AddEvent(
            NetLog::TYPE_SPDY_SESSION_INITIAL_WINDOW_SIZE_OUT_OF_RANGE,
            NetLog::IntegerCallback("initial_window_size", value));
        return;
      }

      // SETTINGS_INITIAL_WINDOW_SIZE updates initial_send_window_size_ only.
      int32 delta_window_size =
          static_cast<int32>(value) - stream_initial_send_window_size_;
      stream_initial_send_window_size_ = static_cast<int32>(value);
      UpdateStreamsSendWindowSize(delta_window_size);
      net_log().AddEvent(
          NetLog::TYPE_SPDY_SESSION_UPDATE_STREAMS_SEND_WINDOW_SIZE,
          NetLog::IntegerCallback("delta_window_size", delta_window_size));
      break;
    }
  }
}

void SpdySession::UpdateStreamsSendWindowSize(int32 delta_window_size) {
  DCHECK_GE(flow_control_state_, FLOW_CONTROL_STREAM);
  for (ActiveStreamMap::iterator it = active_streams_.begin();
       it != active_streams_.end(); ++it) {
    it->second.stream->AdjustSendWindowSize(delta_window_size);
  }

  for (CreatedStreamSet::const_iterator it = created_streams_.begin();
       it != created_streams_.end(); it++) {
    (*it)->AdjustSendWindowSize(delta_window_size);
  }
}

void SpdySession::SendPrefacePingIfNoneInFlight() {
  if (pings_in_flight_ || !enable_ping_based_connection_checking_)
    return;

  base::TimeTicks now = base::TimeTicks::Now();
  // If there is no activity in the session, then send a preface-PING.
  if ((now - last_activity_time_) > connection_at_risk_of_loss_time_)
    SendPrefacePing();
}

void SpdySession::SendPrefacePing() {
  WritePingFrame(next_ping_id_);
}

void SpdySession::SendWindowUpdateFrame(SpdyStreamId stream_id,
                                        uint32 delta_window_size,
                                        RequestPriority priority) {
  CHECK_GE(flow_control_state_, FLOW_CONTROL_STREAM);
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  if (it != active_streams_.end()) {
    CHECK_EQ(it->second.stream->stream_id(), stream_id);
  } else {
    CHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);
    CHECK_EQ(stream_id, kSessionFlowControlStreamId);
  }

  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_SENT_WINDOW_UPDATE_FRAME,
      base::Bind(&NetLogSpdyWindowUpdateFrameCallback,
                 stream_id, delta_window_size));

  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> window_update_frame(
      buffered_spdy_framer_->CreateWindowUpdate(stream_id, delta_window_size));
  EnqueueSessionWrite(priority, WINDOW_UPDATE, window_update_frame.Pass());
}

void SpdySession::WritePingFrame(uint32 unique_id) {
  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> ping_frame(
      buffered_spdy_framer_->CreatePingFrame(unique_id));
  EnqueueSessionWrite(HIGHEST, PING, ping_frame.Pass());

  if (net_log().IsLoggingAllEvents()) {
    net_log().AddEvent(
        NetLog::TYPE_SPDY_SESSION_PING,
        base::Bind(&NetLogSpdyPingCallback, unique_id, "sent"));
  }
  if (unique_id % 2 != 0) {
    next_ping_id_ += 2;
    ++pings_in_flight_;
    PlanToCheckPingStatus();
    last_ping_sent_time_ = base::TimeTicks::Now();
  }
}

void SpdySession::PlanToCheckPingStatus() {
  if (check_ping_status_pending_)
    return;

  check_ping_status_pending_ = true;
  base::MessageLoop::current()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&SpdySession::CheckPingStatus, weak_factory_.GetWeakPtr(),
                 base::TimeTicks::Now()), hung_interval_);
}

void SpdySession::CheckPingStatus(base::TimeTicks last_check_time) {
  // Check if we got a response back for all PINGs we had sent.
  if (pings_in_flight_ == 0) {
    check_ping_status_pending_ = false;
    return;
  }

  DCHECK(check_ping_status_pending_);

  base::TimeTicks now = base::TimeTicks::Now();
  base::TimeDelta delay = hung_interval_ - (now - last_activity_time_);

  if (delay.InMilliseconds() < 0 || last_activity_time_ < last_check_time) {
    CloseSessionOnError(ERR_SPDY_PING_FAILED, "Failed ping.");
    // Track all failed PING messages in a separate bucket.
    const base::TimeDelta kFailedPing =
        base::TimeDelta::FromInternalValue(INT_MAX);
    RecordPingRTTHistogram(kFailedPing);
    return;
  }

  // Check the status of connection after a delay.
  base::MessageLoop::current()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&SpdySession::CheckPingStatus, weak_factory_.GetWeakPtr(),
                 now),
      delay);
}

void SpdySession::RecordPingRTTHistogram(base::TimeDelta duration) {
  UMA_HISTOGRAM_TIMES("Net.SpdyPing.RTT", duration);
}

void SpdySession::RecordProtocolErrorHistogram(
    SpdyProtocolErrorDetails details) {
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySessionErrorDetails2", details,
                            NUM_SPDY_PROTOCOL_ERROR_DETAILS);
  if (EndsWith(host_port_pair().host(), "google.com", false)) {
    UMA_HISTOGRAM_ENUMERATION("Net.SpdySessionErrorDetails_Google2", details,
                              NUM_SPDY_PROTOCOL_ERROR_DETAILS);
  }
}

void SpdySession::RecordHistograms() {
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsPerSession",
                              streams_initiated_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsPushedPerSession",
                              streams_pushed_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsPushedAndClaimedPerSession",
                              streams_pushed_and_claimed_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsAbandonedPerSession",
                              streams_abandoned_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySettingsSent",
                            sent_settings_ ? 1 : 0, 2);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySettingsReceived",
                            received_settings_ ? 1 : 0, 2);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamStallsPerSession",
                              stalled_streams_,
                              0, 300, 50);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySessionsWithStalls",
                            stalled_streams_ > 0 ? 1 : 0, 2);

  if (received_settings_) {
    // Enumerate the saved settings, and set histograms for it.
    const SettingsMap& settings_map =
        http_server_properties_->GetSpdySettings(host_port_pair());

    SettingsMap::const_iterator it;
    for (it = settings_map.begin(); it != settings_map.end(); ++it) {
      const SpdySettingsIds id = it->first;
      const uint32 val = it->second.second;
      switch (id) {
        case SETTINGS_CURRENT_CWND:
          // Record several different histograms to see if cwnd converges
          // for larger volumes of data being sent.
          UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd",
                                      val, 1, 200, 100);
          if (total_bytes_received_ > 10 * 1024) {
            UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd10K",
                                        val, 1, 200, 100);
            if (total_bytes_received_ > 25 * 1024) {
              UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd25K",
                                          val, 1, 200, 100);
              if (total_bytes_received_ > 50 * 1024) {
                UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd50K",
                                            val, 1, 200, 100);
                if (total_bytes_received_ > 100 * 1024) {
                  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd100K",
                                              val, 1, 200, 100);
                }
              }
            }
          }
          break;
        case SETTINGS_ROUND_TRIP_TIME:
          UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsRTT",
                                      val, 1, 1200, 100);
          break;
        case SETTINGS_DOWNLOAD_RETRANS_RATE:
          UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsRetransRate",
                                      val, 1, 100, 50);
          break;
        default:
          break;
      }
    }
  }
}

void SpdySession::CompleteStreamRequest(SpdyStreamRequest* pending_request) {
  CHECK(pending_request);

  PendingStreamRequestCompletionSet::iterator it =
      pending_stream_request_completions_.find(pending_request);

  // Abort if the request has already been cancelled.
  if (it == pending_stream_request_completions_.end())
    return;

  base::WeakPtr<SpdyStream> stream;
  int rv = CreateStream(*pending_request, &stream);
  pending_stream_request_completions_.erase(it);

  if (rv == OK) {
    DCHECK(stream.get());
    pending_request->OnRequestCompleteSuccess(&stream);
  } else {
    DCHECK(!stream.get());
    pending_request->OnRequestCompleteFailure(rv);
  }
}

SSLClientSocket* SpdySession::GetSSLClientSocket() const {
  if (!is_secure_)
    return NULL;
  SSLClientSocket* ssl_socket =
      reinterpret_cast<SSLClientSocket*>(connection_->socket());
  DCHECK(ssl_socket);
  return ssl_socket;
}

void SpdySession::OnWriteBufferConsumed(
    size_t frame_payload_size,
    size_t consume_size,
    SpdyBuffer::ConsumeSource consume_source) {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);
  if (consume_source == SpdyBuffer::DISCARD) {
    // If we're discarding a frame or part of it, increase the send
    // window by the number of discarded bytes. (Although if we're
    // discarding part of a frame, it's probably because of a write
    // error and we'll be tearing down the session soon.)
    size_t remaining_payload_bytes = std::min(consume_size, frame_payload_size);
    DCHECK_GT(remaining_payload_bytes, 0u);
    IncreaseSendWindowSize(static_cast<int32>(remaining_payload_bytes));
  }
  // For consumed bytes, the send window is increased when we receive
  // a WINDOW_UPDATE frame.
}

void SpdySession::IncreaseSendWindowSize(int32 delta_window_size) {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);
  DCHECK_GE(delta_window_size, 1);

  // Check for overflow.
  int32 max_delta_window_size = kint32max - session_send_window_size_;
  if (delta_window_size > max_delta_window_size) {
    RecordProtocolErrorHistogram(PROTOCOL_ERROR_INVALID_WINDOW_UPDATE_SIZE);
    CloseSessionOnError(
        ERR_SPDY_PROTOCOL_ERROR,
        "Received WINDOW_UPDATE [delta: " +
            base::IntToString(delta_window_size) +
            "] for session overflows session_send_window_size_ [current: " +
            base::IntToString(session_send_window_size_) + "]");
    return;
  }

  session_send_window_size_ += delta_window_size;

  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_UPDATE_SEND_WINDOW,
      base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                 delta_window_size, session_send_window_size_));

  DCHECK(!IsSendStalled());
  ResumeSendStalledStreams();
}

void SpdySession::DecreaseSendWindowSize(int32 delta_window_size) {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);

  // We only call this method when sending a frame. Therefore,
  // |delta_window_size| should be within the valid frame size range.
  DCHECK_GE(delta_window_size, 1);
  DCHECK_LE(delta_window_size, kMaxSpdyFrameChunkSize);

  // |send_window_size_| should have been at least |delta_window_size| for
  // this call to happen.
  DCHECK_GE(session_send_window_size_, delta_window_size);

  session_send_window_size_ -= delta_window_size;

  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_UPDATE_SEND_WINDOW,
      base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                 -delta_window_size, session_send_window_size_));
}

void SpdySession::OnReadBufferConsumed(
    size_t consume_size,
    SpdyBuffer::ConsumeSource consume_source) {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);
  DCHECK_GE(consume_size, 1u);
  DCHECK_LE(consume_size, static_cast<size_t>(kint32max));
  IncreaseRecvWindowSize(static_cast<int32>(consume_size));
}

void SpdySession::IncreaseRecvWindowSize(int32 delta_window_size) {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);
  DCHECK_GE(session_unacked_recv_window_bytes_, 0);
  DCHECK_GE(session_recv_window_size_, session_unacked_recv_window_bytes_);
  DCHECK_GE(delta_window_size, 1);
  // Check for overflow.
  DCHECK_LE(delta_window_size, kint32max - session_recv_window_size_);

  session_recv_window_size_ += delta_window_size;
  net_log_.AddEvent(
      NetLog::TYPE_SPDY_STREAM_UPDATE_RECV_WINDOW,
      base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                 delta_window_size, session_recv_window_size_));

  session_unacked_recv_window_bytes_ += delta_window_size;
  if (session_unacked_recv_window_bytes_ > kSpdySessionInitialWindowSize / 2) {
    SendWindowUpdateFrame(kSessionFlowControlStreamId,
                          session_unacked_recv_window_bytes_,
                          HIGHEST);
    session_unacked_recv_window_bytes_ = 0;
  }
}

void SpdySession::DecreaseRecvWindowSize(int32 delta_window_size) {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);
  DCHECK_GE(delta_window_size, 1);

  // Since we never decrease the initial receive window size,
  // |delta_window_size| should never cause |recv_window_size_| to go
  // negative. If we do, the receive window isn't being respected.
  if (delta_window_size > session_recv_window_size_) {
    RecordProtocolErrorHistogram(PROTOCOL_ERROR_RECEIVE_WINDOW_VIOLATION);
    CloseSessionOnError(
        ERR_SPDY_PROTOCOL_ERROR,
        "delta_window_size is " + base::IntToString(delta_window_size) +
            " in DecreaseRecvWindowSize, which is larger than the receive " +
            "window size of " + base::IntToString(session_recv_window_size_));
    return;
  }

  session_recv_window_size_ -= delta_window_size;
  net_log_.AddEvent(
      NetLog::TYPE_SPDY_SESSION_UPDATE_RECV_WINDOW,
      base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                 -delta_window_size, session_recv_window_size_));
}

void SpdySession::QueueSendStalledStream(const SpdyStream& stream) {
  DCHECK(stream.send_stalled_by_flow_control());
  stream_send_unstall_queue_[stream.priority()].push_back(stream.stream_id());
}

namespace {

// Helper function to return the total size of an array of objects
// with .size() member functions.
template <typename T, size_t N> size_t GetTotalSize(const T (&arr)[N]) {
  size_t total_size = 0;
  for (size_t i = 0; i < N; ++i) {
    total_size += arr[i].size();
  }
  return total_size;
}

}  // namespace

void SpdySession::ResumeSendStalledStreams() {
  DCHECK_EQ(flow_control_state_, FLOW_CONTROL_STREAM_AND_SESSION);

  // We don't have to worry about new streams being queued, since
  // doing so would cause IsSendStalled() to return true. But we do
  // have to worry about streams being closed, as well as ourselves
  // being closed.

  while (availability_state_ != STATE_CLOSED && !IsSendStalled()) {
    size_t old_size = 0;
    if (DCHECK_IS_ON())
      old_size = GetTotalSize(stream_send_unstall_queue_);

    SpdyStreamId stream_id = PopStreamToPossiblyResume();
    if (stream_id == 0)
      break;
    ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
    // The stream may actually still be send-stalled after this (due
    // to its own send window) but that's okay -- it'll then be
    // resumed once its send window increases.
    if (it != active_streams_.end())
      it->second.stream->PossiblyResumeIfSendStalled();

    // The size should decrease unless we got send-stalled again.
    if (!IsSendStalled())
      DCHECK_LT(GetTotalSize(stream_send_unstall_queue_), old_size);
  }
}

SpdyStreamId SpdySession::PopStreamToPossiblyResume() {
  for (int i = NUM_PRIORITIES - 1; i >= 0; --i) {
    std::deque<SpdyStreamId>* queue = &stream_send_unstall_queue_[i];
    if (!queue->empty()) {
      SpdyStreamId stream_id = queue->front();
      queue->pop_front();
      return stream_id;
    }
  }
  return 0;
}

}  // namespace net
