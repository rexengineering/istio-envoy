#include "server/lds_api.h"

#include <unordered_map>

#include "envoy/api/v2/lds.pb.validate.h"
#include "envoy/api/v2/listener/listener.pb.validate.h"
#include "envoy/stats/scope.h"

#include "common/common/cleanup.h"
#include "common/config/resources.h"
#include "common/config/subscription_factory.h"
#include "common/config/utility.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Server {

LdsApiImpl::LdsApiImpl(const envoy::api::v2::core::ConfigSource& lds_config,
                       Upstream::ClusterManager& cm, Event::Dispatcher& dispatcher,
                       Runtime::RandomGenerator& random, Init::Manager& init_manager,
                       const LocalInfo::LocalInfo& local_info, Stats::Scope& scope,
                       ListenerManager& lm, Api::Api& api)
    : listener_manager_(lm), scope_(scope.createScope("listener_manager.lds.")), cm_(cm),
      init_target_("LDS", [this]() { subscription_->start({}, *this); }) {
  subscription_ = Envoy::Config::SubscriptionFactory::subscriptionFromConfigSource(
      lds_config, local_info, dispatcher, cm, random, *scope_,
      "envoy.api.v2.ListenerDiscoveryService.FetchListeners",
      "envoy.api.v2.ListenerDiscoveryService.StreamListeners",
      Grpc::Common::typeUrl(envoy::api::v2::Listener().GetDescriptor()->full_name()), api);
  Config::Utility::checkLocalInfo("lds", local_info);
  init_manager.add(init_target_);
}

void LdsApiImpl::onConfigUpdate(const Protobuf::RepeatedPtrField<ProtobufWkt::Any>& resources,
                                const std::string& version_info) {
  cm_.adsMux().pause(Config::TypeUrl::get().RouteConfiguration);
  Cleanup rds_resume([this] { cm_.adsMux().resume(Config::TypeUrl::get().RouteConfiguration); });

  std::vector<envoy::api::v2::Listener> listeners;
  for (const auto& listener_blob : resources) {
    listeners.push_back(MessageUtil::anyConvert<envoy::api::v2::Listener>(listener_blob));
    MessageUtil::validate(listeners.back());
  }
  std::vector<std::string> exception_msgs;
  std::unordered_set<std::string> listener_names;
  for (const auto& listener : listeners) {
    if (!listener_names.insert(listener.name()).second) {
      throw EnvoyException(fmt::format("duplicate listener {} found", listener.name()));
    }
  }
  // We need to keep track of which listeners we might need to remove.
  std::unordered_map<std::string, std::reference_wrapper<Network::ListenerConfig>>
      listeners_to_remove;

  // We build the list of listeners to be removed and remove them before
  // adding new listeners. This allows adding a new listener with the same
  // address as a listener that is to be removed. Do not change the order.
  for (const auto& listener : listener_manager_.listeners()) {
    listeners_to_remove.emplace(listener.get().name(), listener);
  }
  for (const auto& listener : listeners) {
    listeners_to_remove.erase(listener.name());
  }
  for (const auto& listener : listeners_to_remove) {
    if (listener_manager_.removeListener(listener.first)) {
      ENVOY_LOG(info, "lds: remove listener '{}'", listener.first);
    }
  }

  for (const auto& listener : listeners) {
    const std::string& listener_name = listener.name();
    try {
      if (listener_manager_.addOrUpdateListener(listener, version_info, true)) {
        ENVOY_LOG(info, "lds: add/update listener '{}'", listener_name);
      } else {
        ENVOY_LOG(debug, "lds: add/update listener '{}' skipped", listener_name);
      }
    } catch (const EnvoyException& e) {
      exception_msgs.push_back(fmt::format("{}: {}", listener_name, e.what()));
    }
  }

  version_info_ = version_info;
  init_target_.ready();
  if (!exception_msgs.empty()) {
    throw EnvoyException(fmt::format("Error adding/updating listener(s) {}",
                                     StringUtil::join(exception_msgs, ", ")));
  }
}

void LdsApiImpl::onConfigUpdateFailed(const EnvoyException*) {
  // We need to allow server startup to continue, even if we have a bad
  // config.
  init_target_.ready();
}

} // namespace Server
} // namespace Envoy
