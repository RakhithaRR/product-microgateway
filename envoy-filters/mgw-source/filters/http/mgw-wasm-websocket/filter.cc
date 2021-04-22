// NOLINT(namespace-envoy)
#include <algorithm>
#include <google/protobuf/stubs/status.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <sstream>

#include "handler.h"

#include "proxy_wasm_intrinsics.h"
#include "proxy_wasm_intrinsics_lite.pb.h"

#include "google/protobuf/util/json_util.h"

#include "config.pb.h"
#include "filter.h"

// Protobuf defined package and method name.
static constexpr char EnforcerServiceName[] = "envoy.extensions.filters.http.mgw_wasm_websocket.v3.WebSocketFrameService";
static constexpr char PublishFrameData[] = "PublishFrameData";

using google::protobuf::util::JsonParseOptions;
using google::protobuf::util::error::Code;
using google::protobuf::util::Status;

using envoy::extensions::filters::http::mgw_wasm_websocket::v3::WebSocketFrameRequest;
using envoy::extensions::filters::http::mgw_wasm_websocket::v3::WebSocketFrameResponse;
using envoy::extensions::filters::http::mgw_wasm_websocket::v3::Config;
using envoy::extensions::filters::http::mgw_wasm_websocket::v3::Metadata;

// mgw_WASM_websocket_root is the root_id for the filter. Root ID is unique ID for a set of filters/services in a VM which will 
// share a RootContext and Contexts if applicable. If two or more filters have a common base, then we can assign the same root_id
// and share the RootContext. Otherwise it is recommended to have separate RootContexts for separate filters. 'mgw_WASM_websocket_root'
// should be same as the value assigned in the xDS configuration.
static RegisterContextFactory register_MgwWebSocketContext(CONTEXT_FACTORY(MgwWebSocketContext),
                                                      ROOT_FACTORY(MgwWebSocketRootContext),
                                                      "mgw_WASM_websocket_root");
// Called when RootContext gets created
bool MgwWebSocketRootContext::onStart(size_t) {
  LOG_TRACE("onStart RootContext mgw_WASM_websocket");
  return true;
}

// Called once when the VM loads and once when each hook loads and whenever
// configuration changes. Returns false if the configuration is invalid.
bool MgwWebSocketRootContext::onConfigure(size_t config_size) {
  LOG_TRACE("onConfigure RootContext mgw_WASM_websocket");
  const WasmDataPtr configuration = getBufferBytes(WasmBufferType::PluginConfiguration, 0, config_size);
  JsonParseOptions json_options;
  const Status options_status = google::protobuf::util::JsonStringToMessage(
      configuration->toString(),
      &config_, json_options);
  if (options_status != Status::OK) {
    LOG_WARN("Cannot parse plugin configuration JSON string: " + configuration->toString());
    return false;
  }
  LOG_TRACE("Loading Config: " + config_.node_id());
  return true;
}

// Called when a new HTTP filter is created.
void MgwWebSocketContext::onCreate() { 
  LOG_TRACE(std::string("onCreate " + std::to_string(id())));
  MgwWebSocketRootContext *r = dynamic_cast<MgwWebSocketRootContext*>(root());
  // Read config provided by xDS and initialize member varibales.
  this->node_id_ = r->config_.node_id();
  this->failure_mode_deny_ = r->config_.failure_mode_deny();
  // Initialize throttle period to now or 0
  struct timeval now;
  // NULL value is provided since we want UTC. Otherwise we should provide a param for the relevant timezone.
  int rc = gettimeofday(&now, NULL);
  if(rc == 0){
    this->throttle_period_ = now.tv_sec;
  }else{
    LOG_WARN("Throttle period initialization failed. Default set to 0");
    this->throttle_period_ = 0;
  }
}

// Called when the initial HTTP upgrade request intercepted by the filter. gRPC bidirectional service 
// is initiated to the enforcer and ext_authz dynamic metadata are assigned as a member variable for 
// reference in the onRequestBody() and onResponseBody() callbacks when a data frame is intercepted.
FilterHeadersStatus MgwWebSocketContext::onRequestHeaders(uint32_t, bool) {
  LOG_TRACE(std::string("onRequestHeaders called mgw_WASM_websocket") + std::to_string(id()));

  // Create a new gRPC bidirectional stream.
  establishNewStream();

  // Extract ext_authz dynamic metadata and assign it to a member variable 
  auto buffer = getProperty<std::string>({"metadata", "filter_metadata", "envoy.filters.http.ext_authz"});
  if (buffer.has_value() && buffer.value()->size() != 0) {
    auto pairs = buffer.value()->pairs();
    for (auto &p : pairs) {
      (*this->metadata_->mutable_ext_authz_metadata())[std::string(p.first)] = std::string(p.second);
      LOG_TRACE(std::string(p.first) + std::string(" -> ") + std::string(p.second));
    }
  }
                     
  return FilterHeadersStatus::Continue;
}

FilterHeadersStatus MgwWebSocketContext::onResponseHeaders(uint32_t, bool) {
  LOG_TRACE(std::string("onResponseHeaders called mgw_WASM_websocket") + std::to_string(id()));
  return FilterHeadersStatus::Continue;
}

// Called when a web socket frame from downstream is intercepted by the filter.
// Publish frame data through the gRPC bidi stream opened in the onRequestHeaders method.
// Handles all the throttling logic.
FilterDataStatus MgwWebSocketContext::onRequestBody(size_t body_buffer_length,
                                               bool /* end_of_stream */) {
  auto body = getBufferBytes(WasmBufferType::HttpRequestBody, 0, body_buffer_length);
  LOG_TRACE(std::string("onRequestBody called mgw_WASM_websocket") + std::string(body->view()));
  auto data = body->view();
  
  if(isDataFrame(data)){
    // Get remoteIP of the upstream 
    std::string upstream_address;
    auto buffer = getValue({"upstream", "address"}, &upstream_address);
    
    // Create WebSocketFrameRequest with related fields
    WebSocketFrameRequest request;
    request.set_node_id(this->node_id_);
    request.set_frame_length(body_buffer_length);
    request.set_remote_ip(upstream_address);
    // Read ext_authz_metadata_ metdata saved as a member variable
    *request.mutable_metadata() = *this->metadata_;
    request.set_payload(std::string(body->view()));
    
    // Perform throttling logic.
    // If the throttle state is underlimit and if the gRPC stream is open, send WebSocketFrameRequest. 
    // If no gRPC stream, try to open a new stream and then send. 
    if(this->throttle_state_ == ThrottleState::UnderLimit){
      if(this->handler_state_ == HandlerState::OK){
        LOG_TRACE(std::string("gRPC bidi stream available. publishing frame data..."));
        auto ack = this->stream_handler_->send(request, false);
        if (ack != WasmResult::Ok) {
          LOG_TRACE(std::string("error sending frame data")+ toString(ack));
        }
        LOG_TRACE(std::string("frame data successfully sent:"+ toString(ack)));
      }else{
        establishNewStream();
        auto ack = this->stream_handler_->send(request, false);
        if (ack != WasmResult::Ok) {
          LOG_TRACE(std::string("error sending frame data")+ toString(ack));
        }
        LOG_TRACE(std::string("frame data successfully sent:"+ toString(ack)));
      }
      return FilterDataStatus::Continue;
    // If throttle state is FailureModeAllowed, then try to esatblish a new gRPC stream and 
    // pass the request to next filter. 
    }else if (this->throttle_state_ == ThrottleState::FailureModeAllowed){
      establishNewStream();
      return FilterDataStatus::Continue;
    // If throttle state is FailureModeBlocked, then try to establish a new gRPC stream and 
    // stop interation.
    }else if(this->throttle_state_ == ThrottleState::FailureModeBlocked){
      establishNewStream();
      return FilterDataStatus::StopIterationNoBuffer;
    // If throttle state is overlimit, then check the throttle period before making a decision. 
    // If the current time has passed the throttle period in UTC seconds, then continue to the 
    // next filter and change the status to UnderLimit. If the current time is in the throttle period
    // then stop iteration. If we can't get the current time due to some error , then the default is to 
    // stop iteration.
    }else{
      struct timeval now;
      int rc = gettimeofday(&now, NULL);
      if(rc == 0){
        if(this->throttle_period_ <= now.tv_sec){
          this->throttle_state_ = ThrottleState::UnderLimit;
          // publish to enforcer
          return FilterDataStatus::Continue;
        }else{
          return FilterDataStatus::StopIterationNoBuffer;
        }
      }else{
        return FilterDataStatus::StopIterationNoBuffer;
      }
    }
  }else{
    LOG_TRACE("proxying web socket control frame");
    return FilterDataStatus::Continue;
  }
}

// Called when a web socket frame from upstream is intercepted by the filter.
// Publish frame data through the gRPC bidi stream opened in the onRequestHeaders method.
// Handles all the throttling logic.
FilterDataStatus MgwWebSocketContext::onResponseBody(size_t body_buffer_length,
                                                bool /* end_of_stream */) {
  auto body = getBufferBytes(WasmBufferType::HttpResponseBody, 0, body_buffer_length);
  LOG_TRACE(std::string("onResponseBody called mgw_WASM_websocket") + std::string(body->view()));
  auto data = body->view();
  
  if(isDataFrame(data)){
    // Get remoteIP of the upstream 
    std::string upstream_address;
    auto buffer = getValue({"upstream", "address"}, &upstream_address);
    
    // Create WebSocketFrameRequest with related fields
    WebSocketFrameRequest request;
    request.set_node_id(this->node_id_);
    request.set_frame_length(body_buffer_length);
    request.set_remote_ip(upstream_address);
    // Read ext_authz_metadata_ metdata saved as a member variable
    *request.mutable_metadata() = *this->metadata_;
    request.set_payload(std::string(body->view()));

    // Perform throttling logic.
    // If the throttle state is underlimit and if the gRPC stream is open, send WebSocketFrameRequest. 
    // If no gRPC stream, try to open a new stream and then send. 
    if(this->throttle_state_ == ThrottleState::UnderLimit){
      if(this->handler_state_ == HandlerState::OK){
        LOG_TRACE(std::string("gRPC bidi stream available. publishing frame data..."));
        auto ack = this->stream_handler_->send(request, false);
        if (ack != WasmResult::Ok) {
          LOG_TRACE(std::string("error sending frame data")+ toString(ack));
        }
        LOG_TRACE(std::string("frame data successfully sent:"+ toString(ack)));
      }else{
        establishNewStream();
        auto ack = this->stream_handler_->send(request, false);
        if (ack != WasmResult::Ok) {
          LOG_TRACE(std::string("error sending frame data")+ toString(ack));
        }
        LOG_TRACE(std::string("frame data successfully sent:"+ toString(ack)));
      }
      return FilterDataStatus::Continue;
    // If throttle state is FailureModeAllowed, then try to esatblish a new gRPC stream and 
    // pass the request to next filter.
    }else if (this->throttle_state_ == ThrottleState::FailureModeAllowed){
      establishNewStream();
      return FilterDataStatus::Continue;
    // If throttle state is FailureModeBlocked, then try to establish a new gRPC stream and 
    // stop interation.
    }else if(this->throttle_state_ == ThrottleState::FailureModeBlocked){
      establishNewStream();
      return FilterDataStatus::StopIterationNoBuffer;
    // If throttle state is overlimit, then check the throttle period before making a decision. 
    // If the current time has passed the throttle period in UTC seconds, then continue to the 
    // next filter and change the status to UnderLimit. If the current time is in the throttle period
    // then stop iteration. If we can't get the current time due to some error , then the default is to 
    // stop iteration.
    }else{
      struct timeval now;
      int rc = gettimeofday(&now, NULL);
      if(rc == 0){
        if(this->throttle_period_ <= now.tv_sec){
          this->throttle_state_ = ThrottleState::UnderLimit;
          // publish to enforcer
          return FilterDataStatus::Continue;
        }else{
          return FilterDataStatus::StopIterationNoBuffer;
        }
      }else{
        return FilterDataStatus::StopIterationNoBuffer;
      }
    }
  }else{
    LOG_TRACE("proxying web socket control frame");
    return FilterDataStatus::Continue;
  }
}

void MgwWebSocketContext::onDone() { LOG_TRACE(std::string("onDone " + std::to_string(id()))); }

void MgwWebSocketContext::onLog() { LOG_TRACE(std::string("onLog " + std::to_string(id()))); }

void MgwWebSocketContext::onDelete() { 
  LOG_TRACE(std::string("onDelete " + std::to_string(id())));
 }

// Callback used by the handler to pass the throttle response received by the gRPC stream.
void MgwWebSocketContext::updateFilterState(ResponseStatus status){
  LOG_TRACE(std::string("updateFilterState") + std::to_string(static_cast<int>(status)));
  if(status == ResponseStatus::OK){
    this->throttle_state_ = ThrottleState::UnderLimit;
    LOG_TRACE("mgw_wasm_websocket filter state changed to UnderLimit");
  }else if(status == ResponseStatus::OverLimit){
    this->throttle_state_ = ThrottleState::OverLimit;
    LOG_TRACE("mgw_wasm_websocket filter state changed to OverLimit !!!");
  }else{
    LOG_TRACE("Enforcer throttle decision unknown");
  }
}

// Callback used by the handler to update the handler state reference in the filter.
void MgwWebSocketContext::updateHandlerState(HandlerState state){
    LOG_TRACE(std::string("updateHandlerState") + std::to_string(static_cast<int>(state)));
    this->handler_state_ = state;
    if(this->failure_mode_deny_){
      this->throttle_state_ = ThrottleState::FailureModeBlocked;
    }else{
      this->throttle_state_ = ThrottleState::FailureModeAllowed;
    }
}

// Check for data frames by reading the opcode and validate frame length of min 3.
bool MgwWebSocketContext::isDataFrame(const std::string_view data){
  int frame_opcode = data[0] & 0x0F;
  if(!(frame_opcode >= 8 && frame_opcode <= 15) && data.length() >= 3){
    return true;
  }else{
    return false;
  }
}

// Establish a new gRPC stream.
void MgwWebSocketContext::establishNewStream() {
  LOG_TRACE("establish new stream called");
  this->stream_handler_ = new MgwGrpcStreamHandler(this);
  GrpcService grpc_service;
  MgwWebSocketRootContext *r = dynamic_cast<MgwWebSocketRootContext*>(root());
  grpc_service.mutable_envoy_grpc()->set_cluster_name(r->config_.rate_limit_service());  
  std::string grpc_service_string;
  grpc_service.SerializeToString(&grpc_service_string);
  HeaderStringPairs initial_metadata;
  initial_metadata.push_back(std::pair("mgw_wasm_websocket", "initial"));
  auto handler_response = root()->grpcStreamHandler(grpc_service_string, EnforcerServiceName, PublishFrameData, initial_metadata, std::unique_ptr<GrpcStreamHandlerBase>(this->stream_handler_));
  if (handler_response != WasmResult::Ok) {
    LOG_TRACE("gRPC bidi stream initialization failed: " + toString(handler_response));
  }else{
    this->handler_state_ = HandlerState::OK;
    // TODO : check throttle_period and decide
    if(this->throttle_state_ == ThrottleState::FailureModeBlocked || this->throttle_state_ == ThrottleState::FailureModeAllowed){
      this->throttle_state_ = ThrottleState::UnderLimit;
    }
    LOG_TRACE(std::string("gRPC bidi stream created successfully"));     
  }
}

// Callback used by the handler to update throttle period.
void MgwWebSocketContext::updateThrottlePeriod(const int throttle_period){
  this->throttle_period_ = throttle_period;
  LOG_TRACE("Throttle period updated to"+ std::to_string(throttle_period));
}
