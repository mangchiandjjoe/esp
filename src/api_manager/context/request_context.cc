// Copyright (C) Endpoints Server Proxy Authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//
////////////////////////////////////////////////////////////////////////////////
//

#include "src/api_manager/context/request_context.h"

#include <uuid/uuid.h>

using ::google::api_manager::utils::Status;

namespace google {
namespace api_manager {
namespace context {

namespace {

// Cloud Trace Context Header
const char kCloudTraceContextHeader[] = "X-Cloud-Trace-Context";

// Log message prefix for a success method.
const char kSuccessMessage[] = "Method: ";
// Log message prefix for a failed method.
const char kFailedMessage[] = "Failed to call method: ";
// Log message prefix for an ignored method.
const char kIgnoredMessage[] =
    "Endpoints management skipped for an unrecognized HTTP call: ";
// Unknown HTTP verb.
const char kUnknownHttpVerb[] = "<Unknown HTTP Verb>";

// Service control does not currently support logging with an empty
// operation name so we use this value until fix is available.
const char kUnrecognizedOperation[] = "<Unknown Operation Name>";

// Maximum 36 byte string for UUID
const int kMaxUUIDBufSize = 40;

// Default api key names
const char kDefaultApiKeyName1[] = "key";
const char kDefaultApiKeyName2[] = "api_key";

// Default location
const char kDefaultLocation[] = "us-central1";

// Genereates a UUID string
std::string GenerateUUID() {
  char uuid_buf[kMaxUUIDBufSize];
  uuid_t uuid;
  uuid_generate(uuid);
  uuid_unparse(uuid, uuid_buf);
  return uuid_buf;
}

}  // namespace

using context::ServiceContext;

RequestContext::RequestContext(std::shared_ptr<ServiceContext> service_context,
                               std::unique_ptr<Request> request)
    : service_context_(service_context),
      request_(std::move(request)),
      is_api_key_valid_(true) {
  operation_id_ = GenerateUUID();
  const std::string &method = request_->GetRequestHTTPMethod();
  const std::string &path = request_->GetRequestPath();

  // In addition to matching the method, service_context_->GetMethodCallInfo()
  // will extract the variable bindings from the url. We need variable bindings
  // only when we need to do transcoding. If this turns out to be a performance
  // problem for non-transcoded calls, we have a couple of options:
  // 1) Do not extract variable bindings here, and do the method matching again
  //    with extracting variable bindings when transcoding is needed.
  // 2) Store all the pieces needed for extracting variable bindings (such as
  //    http template variables, url path parts) in MethodCallInfo and extract
  //    variables lazily when needed.
  method_call_ = service_context_->GetMethodCallInfo(
      method.c_str(), method.length(), path.c_str(), path.length());

  if (method_call_.method_info) {
    ExtractApiKey();
  }
  request_->FindHeader("referer", &http_referer_);

  // Enable trace if the triggering header is set.
  std::string trace_context_header;
  request_->FindHeader(kCloudTraceContextHeader, &trace_context_header);

  cloud_trace_.reset(cloud_trace::CreateCloudTrace(trace_context_header));
}

void RequestContext::ExtractApiKey() {
  bool api_key_defined = false;
  auto url_queries = method()->api_key_url_query_parameters();
  if (url_queries) {
    api_key_defined = true;
    for (const auto &url_query : *url_queries) {
      if (request_->FindQuery(url_query, &api_key_)) {
        return;
      }
    }
  }

  auto headers = method()->api_key_http_headers();
  if (headers) {
    api_key_defined = true;
    for (const auto &header : *headers) {
      if (request_->FindHeader(header, &api_key_)) {
        return;
      }
    }
  }

  if (!api_key_defined) {
    // If api_key is not specified for a method,
    // check "key" first, if not, check "api_key" in query parameter.
    if (!request_->FindQuery(kDefaultApiKeyName1, &api_key_)) {
      request_->FindQuery(kDefaultApiKeyName2, &api_key_);
    }
  }
}

void RequestContext::CompleteCheck(Status status) {
  // Makes sure set_check_continuation() is called.
  // Only making sure CompleteCheck() is NOT called twice.
  GOOGLE_CHECK(check_continuation_);

  auto temp_continuation = check_continuation_;
  check_continuation_ = nullptr;

  temp_continuation(status);
}

void RequestContext::FillOperationInfo(service_control::OperationInfo *info) {
  info->service_name = service_context_->service_name();
  if (method()) {
    info->operation_name = method()->name();
  } else {
    info->operation_name = kUnrecognizedOperation;
  }
  info->operation_id = operation_id_;
  info->api_key = api_key_;
  info->is_api_key_valid = is_api_key_valid_;
  info->producer_project_id = service_context()->project_id();
  info->referer = http_referer_;
}

void RequestContext::FillLocation(service_control::ReportRequestInfo *info) {
  if (service_context()->gce_metadata()->has_valid_data() &&
      !service_context()->gce_metadata()->zone().empty()) {
    info->location = service_context()->gce_metadata()->zone();
  } else {
    info->location = kDefaultLocation;
  }
}

void RequestContext::FillComputePlatform(
    service_control::ReportRequestInfo *info) {
  compute_platform::ComputePlatform cp;

  GceMetadata *metadata = service_context()->gce_metadata();
  if (metadata == nullptr || !metadata->has_valid_data()) {
    cp = compute_platform::UNKNOWN;
  } else {
    if (!metadata->gae_server_software().empty()) {
      cp = compute_platform::GAE;
    } else if (!metadata->kube_env().empty()) {
      cp = compute_platform::GKE;
    } else {
      cp = compute_platform::GCE;
    }
  }

  info->compute_platform = cp;
}

void RequestContext::FillLogMessage(service_control::ReportRequestInfo *info) {
  if (method()) {
    const std::string &method_name = method()->name();
    info->api_method = method_name;

    if (info->response_code >= 400) {
      info->log_message = std::string(kFailedMessage) + method_name;
    } else {
      info->log_message = std::string(kSuccessMessage) + method_name;
    }
  } else {
    std::string http_verb = info->method;
    if (http_verb.empty()) {
      http_verb = kUnknownHttpVerb;
    }
    info->log_message = std::string(kIgnoredMessage) + http_verb + " " +
                        request_->GetUnparsedRequestPath();
  }
}

void RequestContext::FillCheckRequestInfo(
    service_control::CheckRequestInfo *info) {
  FillOperationInfo(info);
  info->client_ip = request_->GetClientIP();
}

void RequestContext::FillReportRequestInfo(
    Response *response, service_control::ReportRequestInfo *info) {
  FillOperationInfo(info);
  FillLocation(info);
  FillComputePlatform(info);

  info->url = request_->GetUnparsedRequestPath();
  info->method = request_->GetRequestHTTPMethod();
  info->api_name = info->service_name;

  info->request_size = response->GetRequestSize();
  info->response_size = response->GetResponseSize();
  info->status = response->GetResponseStatus();
  info->response_code = info->status.HttpCode();
  info->protocol = request_->GetRequestProtocol();

  info->auth_issuer = auth_issuer_;
  info->auth_audience = auth_audience_;

  // Must be after response_code and method are assigned.
  FillLogMessage(info);

  response->GetLatencyInfo(&info->latency);
}

void RequestContext::StartBackendSpan() {
  backend_span_.reset(CreateSpan(cloud_trace_.get(), "Backend"));
}

}  // namespace context
}  // namespace api_manager
}  // namespace google
