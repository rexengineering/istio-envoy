#pragma once

#include "envoy/api/v2/core/health_check.pb.h"

#include "common/common/logger.h"
#include "common/grpc/codec.h"
#include "common/http/codec_client.h"
#include "common/upstream/health_checker_base_impl.h"

#include "extensions/filters/network/redis_proxy/conn_pool.h"

#include "src/proto/grpc/health/v1/health.pb.h"

namespace Envoy {
namespace Upstream {

/**
 * Factory for creating health checker implementations.
 */
class HealthCheckerFactory {
public:
  /**
   * Create a health checker.
   * @param hc_config supplies the health check proto.
   * @param cluster supplies the owning cluster.
   * @param runtime supplies the runtime loader.
   * @param random supplies the random generator.
   * @param dispatcher supplies the dispatcher.
   * @return a health checker.
   */
  static HealthCheckerSharedPtr create(const envoy::api::v2::core::HealthCheck& hc_config,
                                       Upstream::Cluster& cluster, Runtime::Loader& runtime,
                                       Runtime::RandomGenerator& random,
                                       Event::Dispatcher& dispatcher);
};

/**
 * HTTP health checker implementation. Connection keep alive is used where possible.
 */
class HttpHealthCheckerImpl : public HealthCheckerImplBase {
public:
  HttpHealthCheckerImpl(const Cluster& cluster, const envoy::api::v2::core::HealthCheck& config,
                        Event::Dispatcher& dispatcher, Runtime::Loader& runtime,
                        Runtime::RandomGenerator& random);

private:
  struct HttpActiveHealthCheckSession : public ActiveHealthCheckSession,
                                        public Http::StreamDecoder,
                                        public Http::StreamCallbacks {
    HttpActiveHealthCheckSession(HttpHealthCheckerImpl& parent, const HostSharedPtr& host);
    ~HttpActiveHealthCheckSession();

    void onResponseComplete();
    bool isHealthCheckSucceeded();

    // ActiveHealthCheckSession
    void onInterval() override;
    void onTimeout() override;

    // Http::StreamDecoder
    void decode100ContinueHeaders(Http::HeaderMapPtr&&) override {}
    void decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) override;
    void decodeData(Buffer::Instance&, bool end_stream) override {
      if (end_stream) {
        onResponseComplete();
      }
    }
    void decodeTrailers(Http::HeaderMapPtr&&) override { onResponseComplete(); }

    // Http::StreamCallbacks
    void onResetStream(Http::StreamResetReason reason) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    void onEvent(Network::ConnectionEvent event);

    class ConnectionCallbackImpl : public Network::ConnectionCallbacks {
    public:
      ConnectionCallbackImpl(HttpActiveHealthCheckSession& parent) : parent_(parent) {}
      // Network::ConnectionCallbacks
      void onEvent(Network::ConnectionEvent event) override { parent_.onEvent(event); }
      void onAboveWriteBufferHighWatermark() override {}
      void onBelowWriteBufferLowWatermark() override {}

    private:
      HttpActiveHealthCheckSession& parent_;
    };

    ConnectionCallbackImpl connection_callback_impl_{*this};
    HttpHealthCheckerImpl& parent_;
    Http::CodecClientPtr client_;
    Http::StreamEncoder* request_encoder_{};
    Http::HeaderMapPtr response_headers_;
    bool expect_reset_{};
  };

  typedef std::unique_ptr<HttpActiveHealthCheckSession> HttpActiveHealthCheckSessionPtr;

  virtual Http::CodecClient* createCodecClient(Upstream::Host::CreateConnectionData& data) PURE;

  // HealthCheckerImplBase
  ActiveHealthCheckSessionPtr makeSession(HostSharedPtr host) override {
    return std::make_unique<HttpActiveHealthCheckSession>(*this, host);
  }

  const std::string path_;
  const std::string host_value_;
  absl::optional<std::string> service_name_;
};

/**
 * Production implementation of the HTTP health checker that allocates a real codec client.
 */
class ProdHttpHealthCheckerImpl : public HttpHealthCheckerImpl {
public:
  using HttpHealthCheckerImpl::HttpHealthCheckerImpl;

  // HttpHealthCheckerImpl
  Http::CodecClient* createCodecClient(Upstream::Host::CreateConnectionData& data) override;
};

/**
 * Utility class for loading a binary health checking config and matching it against a buffer.
 * Split out for ease of testing. The type of matching performed is the following (this is the
 * MongoDB health check request and response):
 *
 * "send": [
    {"binary": "39000000"},
    {"binary": "EEEEEEEE"},
    {"binary": "00000000"},
    {"binary": "d4070000"},
    {"binary": "00000000"},
    {"binary": "746573742e"},
    {"binary": "24636d6400"},
    {"binary": "00000000"},
    {"binary": "FFFFFFFF"},

    {"binary": "13000000"},
    {"binary": "01"},
    {"binary": "70696e6700"},
    {"binary": "000000000000f03f"},
    {"binary": "00"}
   ],
   "receive": [
    {"binary": "EEEEEEEE"},
    {"binary": "01000000"},
    {"binary": "00000000"},
    {"binary": "0000000000000000"},
    {"binary": "00000000"},
    {"binary": "11000000"},
    {"binary": "01"},
    {"binary": "6f6b"},
    {"binary": "00000000000000f03f"},
    {"binary": "00"}
   ]
 *
 * During each health check cycle, all of the "send" bytes are sent to the target server. Each
 * binary block can be of arbitrary length and is just concatenated together when sent.
 *
 * On the receive side, "fuzzy" matching is performed such that each binary block must be found,
 * and in the order specified, but not necessarly contiguous. Thus, in the example above,
 * "FFFFFFFF" could be inserted in the response between "EEEEEEEE" and "01000000" and the check
 * would still pass.
 */
class TcpHealthCheckMatcher {
public:
  typedef std::list<std::vector<uint8_t>> MatchSegments;

  static MatchSegments loadProtoBytes(
      const Protobuf::RepeatedPtrField<envoy::api::v2::core::HealthCheck::Payload>& byte_array);
  static bool match(const MatchSegments& expected, const Buffer::Instance& buffer);
};

/**
 * TCP health checker implementation.
 */
class TcpHealthCheckerImpl : public HealthCheckerImplBase {
public:
  TcpHealthCheckerImpl(const Cluster& cluster, const envoy::api::v2::core::HealthCheck& config,
                       Event::Dispatcher& dispatcher, Runtime::Loader& runtime,
                       Runtime::RandomGenerator& random);

private:
  struct TcpActiveHealthCheckSession;

  struct TcpSessionCallbacks : public Network::ConnectionCallbacks,
                               public Network::ReadFilterBaseImpl {
    TcpSessionCallbacks(TcpActiveHealthCheckSession& parent) : parent_(parent) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override { parent_.onEvent(event); }
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    // Network::ReadFilter
    Network::FilterStatus onData(Buffer::Instance& data, bool) override {
      parent_.onData(data);
      return Network::FilterStatus::StopIteration;
    }

    TcpActiveHealthCheckSession& parent_;
  };

  struct TcpActiveHealthCheckSession : public ActiveHealthCheckSession {
    TcpActiveHealthCheckSession(TcpHealthCheckerImpl& parent, const HostSharedPtr& host)
        : ActiveHealthCheckSession(parent, host), parent_(parent) {}
    ~TcpActiveHealthCheckSession();

    void onData(Buffer::Instance& data);
    void onEvent(Network::ConnectionEvent event);

    // ActiveHealthCheckSession
    void onInterval() override;
    void onTimeout() override;

    TcpHealthCheckerImpl& parent_;
    Network::ClientConnectionPtr client_;
    std::shared_ptr<TcpSessionCallbacks> session_callbacks_;
  };

  typedef std::unique_ptr<TcpActiveHealthCheckSession> TcpActiveHealthCheckSessionPtr;

  // HealthCheckerImplBase
  ActiveHealthCheckSessionPtr makeSession(HostSharedPtr host) override {
    return std::make_unique<TcpActiveHealthCheckSession>(*this, host);
  }

  const TcpHealthCheckMatcher::MatchSegments send_bytes_;
  const TcpHealthCheckMatcher::MatchSegments receive_bytes_;
};

/**
 * Redis health checker implementation. Sends PING and expects PONG.
 * TODO(mattklein123): Redis health checking should be via a pluggable module and not in the
 * "core".
 */
class RedisHealthCheckerImpl : public HealthCheckerImplBase {
public:
  RedisHealthCheckerImpl(
      const Cluster& cluster, const envoy::api::v2::core::HealthCheck& config,
      Event::Dispatcher& dispatcher, Runtime::Loader& runtime, Runtime::RandomGenerator& random,
      Extensions::NetworkFilters::RedisProxy::ConnPool::ClientFactory& client_factory);

  static const Extensions::NetworkFilters::RedisProxy::RespValue& pingHealthCheckRequest() {
    static HealthCheckRequest* request = new HealthCheckRequest();
    return request->request_;
  }

  static const Extensions::NetworkFilters::RedisProxy::RespValue&
  existsHealthCheckRequest(const std::string& key) {
    static HealthCheckRequest* request = new HealthCheckRequest(key);
    return request->request_;
  }

private:
  struct RedisActiveHealthCheckSession
      : public ActiveHealthCheckSession,
        public Extensions::NetworkFilters::RedisProxy::ConnPool::Config,
        public Extensions::NetworkFilters::RedisProxy::ConnPool::PoolCallbacks,
        public Network::ConnectionCallbacks {
    RedisActiveHealthCheckSession(RedisHealthCheckerImpl& parent, const HostSharedPtr& host);
    ~RedisActiveHealthCheckSession();
    // ActiveHealthCheckSession
    void onInterval() override;
    void onTimeout() override;

    // Extensions::NetworkFilters::RedisProxy::ConnPool::Config
    bool disableOutlierEvents() const override { return true; }
    std::chrono::milliseconds opTimeout() const override {
      // Allow the main HC infra to control timeout.
      return parent_.timeout_ * 2;
    }

    // Extensions::NetworkFilters::RedisProxy::ConnPool::PoolCallbacks
    void onResponse(Extensions::NetworkFilters::RedisProxy::RespValuePtr&& value) override;
    void onFailure() override;

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    RedisHealthCheckerImpl& parent_;
    Extensions::NetworkFilters::RedisProxy::ConnPool::ClientPtr client_;
    Extensions::NetworkFilters::RedisProxy::ConnPool::PoolRequest* current_request_{};
  };

  enum class Type { Ping, Exists };

  struct HealthCheckRequest {
    HealthCheckRequest(const std::string& key);
    HealthCheckRequest();

    Extensions::NetworkFilters::RedisProxy::RespValue request_;
  };

  typedef std::unique_ptr<RedisActiveHealthCheckSession> RedisActiveHealthCheckSessionPtr;

  // HealthCheckerImplBase
  ActiveHealthCheckSessionPtr makeSession(HostSharedPtr host) override {
    return std::make_unique<RedisActiveHealthCheckSession>(*this, host);
  }

  Extensions::NetworkFilters::RedisProxy::ConnPool::ClientFactory& client_factory_;
  Type type_;
  const std::string key_;
};

/**
 * gRPC health checker implementation.
 */
class GrpcHealthCheckerImpl : public HealthCheckerImplBase {
public:
  GrpcHealthCheckerImpl(const Cluster& cluster, const envoy::api::v2::core::HealthCheck& config,
                        Event::Dispatcher& dispatcher, Runtime::Loader& runtime,
                        Runtime::RandomGenerator& random);

private:
  struct GrpcActiveHealthCheckSession : public ActiveHealthCheckSession,
                                        public Http::StreamDecoder,
                                        public Http::StreamCallbacks {
    GrpcActiveHealthCheckSession(GrpcHealthCheckerImpl& parent, const HostSharedPtr& host);
    ~GrpcActiveHealthCheckSession();

    void onRpcComplete(Grpc::Status::GrpcStatus grpc_status, const std::string& grpc_message,
                       bool end_stream);
    bool isHealthCheckSucceeded(Grpc::Status::GrpcStatus grpc_status) const;
    void resetState();
    void logHealthCheckStatus(Grpc::Status::GrpcStatus grpc_status,
                              const std::string& grpc_message);

    // ActiveHealthCheckSession
    void onInterval() override;
    void onTimeout() override;

    // Http::StreamDecoder
    void decode100ContinueHeaders(Http::HeaderMapPtr&&) override {}
    void decodeHeaders(Http::HeaderMapPtr&& headers, bool end_stream) override;
    void decodeData(Buffer::Instance&, bool end_stream) override;
    void decodeTrailers(Http::HeaderMapPtr&&) override;

    // Http::StreamCallbacks
    void onResetStream(Http::StreamResetReason reason) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    void onEvent(Network::ConnectionEvent event);
    void onGoAway();

    class ConnectionCallbackImpl : public Network::ConnectionCallbacks {
    public:
      ConnectionCallbackImpl(GrpcActiveHealthCheckSession& parent) : parent_(parent) {}
      // Network::ConnectionCallbacks
      void onEvent(Network::ConnectionEvent event) override { parent_.onEvent(event); }
      void onAboveWriteBufferHighWatermark() override {}
      void onBelowWriteBufferLowWatermark() override {}

    private:
      GrpcActiveHealthCheckSession& parent_;
    };

    class HttpConnectionCallbackImpl : public Http::ConnectionCallbacks {
    public:
      HttpConnectionCallbackImpl(GrpcActiveHealthCheckSession& parent) : parent_(parent) {}
      // Http::ConnectionCallbacks
      void onGoAway() override { parent_.onGoAway(); }

    private:
      GrpcActiveHealthCheckSession& parent_;
    };

    ConnectionCallbackImpl connection_callback_impl_{*this};
    HttpConnectionCallbackImpl http_connection_callback_impl_{*this};
    GrpcHealthCheckerImpl& parent_;
    Http::CodecClientPtr client_;
    Http::StreamEncoder* request_encoder_;
    Grpc::Decoder decoder_;
    std::unique_ptr<grpc::health::v1::HealthCheckResponse> health_check_response_;
    // If true, stream reset was initiated by us (GrpcActiveHealthCheckSession), not by HTTP stack,
    // e.g. remote reset. In this case healthcheck status has already been reported, only state
    // cleanup is required.
    bool expect_reset_ = false;
  };

  virtual Http::CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) PURE;

  // HealthCheckerImplBase
  ActiveHealthCheckSessionPtr makeSession(HostSharedPtr host) override {
    return std::make_unique<GrpcActiveHealthCheckSession>(*this, host);
  }

  const Protobuf::MethodDescriptor& service_method_;
  absl::optional<std::string> service_name_;
};

/**
 * Production implementation of the gRPC health checker that allocates a real codec client.
 */
class ProdGrpcHealthCheckerImpl : public GrpcHealthCheckerImpl {
public:
  using GrpcHealthCheckerImpl::GrpcHealthCheckerImpl;

  // GrpcHealthCheckerImpl
  Http::CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData& data) override;
};

} // namespace Upstream
} // namespace Envoy
