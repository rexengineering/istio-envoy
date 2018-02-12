#pragma once

#include "envoy/grpc/async_client_manager.h"
#include "envoy/singleton/manager.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/cluster_manager.h"

namespace Envoy {
namespace Grpc {

class AsyncClientFactoryImpl : public AsyncClientFactory {
public:
  AsyncClientFactoryImpl(Upstream::ClusterManager& cm, const std::string& cluster_name);

  AsyncClientPtr create() override;

private:
  Upstream::ClusterManager& cm_;
  const std::string cluster_name_;
};

class GoogleAsyncClientFactoryImpl : public AsyncClientFactory {
public:
  GoogleAsyncClientFactoryImpl(ThreadLocal::Instance& tls, ThreadLocal::Slot& google_tls_slot,
                               Stats::Scope& scope,
                               const envoy::api::v2::core::GrpcService::GoogleGrpc& config);

  AsyncClientPtr create() override;

private:
  ThreadLocal::Instance& tls_;
  ThreadLocal::Slot& google_tls_slot_;
  Stats::ScopePtr scope_;
  const envoy::api::v2::core::GrpcService::GoogleGrpc config_;
};

class AsyncClientManagerImpl : public AsyncClientManager {
public:
  AsyncClientManagerImpl(Upstream::ClusterManager& cm, ThreadLocal::Instance& tls);

  // Grpc::AsyncClientManager
  AsyncClientFactoryPtr factoryForGrpcService(const envoy::api::v2::core::GrpcService& grpc_service,
                                              Stats::Scope& scope) override;

private:
  Upstream::ClusterManager& cm_;
  ThreadLocal::Instance& tls_;
  ThreadLocal::SlotPtr google_tls_slot_;
};

} // namespace Grpc
} // namespace Envoy
