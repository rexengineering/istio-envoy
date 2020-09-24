#include "common/router/route_config_update_receiver_impl.h"

#include <string>

#include "envoy/config/route/v3/route.pb.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/protobuf/utility.h"
#include "common/router/config_impl.h"
#include "envoy/upstream/cluster_manager.h"

namespace Envoy {
namespace Router {

bool RouteConfigUpdateReceiverImpl::onRdsUpdate(
    const envoy::config::route::v3::RouteConfiguration& rc, const std::string& version_info) {
  const uint64_t new_hash = MessageUtil::hash(rc);
  if (new_hash == last_config_hash_) {
    return false;
  }
  route_config_proto_ = rc;
  last_config_hash_ = new_hash;
  const uint64_t new_vhds_config_hash = rc.has_vhds() ? MessageUtil::hash(rc.vhds()) : 0ul;
  vhds_configuration_changed_ = new_vhds_config_hash != last_vhds_config_hash_;
  last_vhds_config_hash_ = new_vhds_config_hash;
  initializeRdsVhosts(route_config_proto_);
  onUpdateCommon(route_config_proto_, version_info);

  // begin REX Code
  std::map<std::string, Envoy::VirtualServiceRoute>& next_cluster_map = cluster_manager_.nextClusterMap();

  const auto& vhs = rc.virtual_hosts();
  for (const auto virtual_host : vhs) {
    if (virtual_host.name() != "bavs-host.default.svc.cluster.local:9881") continue;

    for (const auto route : virtual_host.routes()) {
      const auto match = route.match();
      // get the header
      const auto header_match = match.headers();
      for (const auto header : header_match) {
        if (header.name() == "decisionpoint") {
          std::string decisionpoint = header.exact_match();
          std::string cluster = route.route().cluster();
          std::cout << "Set Cluster for decisionpoint" << decisionpoint << " to  " << cluster << std::endl;

          std::string path;
          if (route.route().has_regex_rewrite()) {
            path = route.route().regex_rewrite().substitution();
          } else {
            path = route.route().prefix_rewrite();
            if (path == "") {
              path = "/";
            }
          }
          std::cout << "Set Path for decisionpoint" << decisionpoint << " to  " << path << std::endl;

          std::string method = "POST";  // default to POST
          for (const auto header_setter : route.request_headers_to_add()) {
            if (header_setter.header().key() == "method" || header_setter.header().key() == "Method") {
              method = header_setter.header().value();
              std::cout << "Set Method for decisionpoint" << decisionpoint << " to  " << method << std::endl;
            }
          }

          VirtualServiceRoute vsr(cluster, method, path);
          next_cluster_map[header.exact_match()] = vsr;
        }
      }
    }

  }


  // end REX Code

  return true;
}

void RouteConfigUpdateReceiverImpl::onUpdateCommon(
    const envoy::config::route::v3::RouteConfiguration& rc, const std::string& version_info) {
  last_config_version_ = version_info;
  last_updated_ = time_source_.systemTime();
  rebuildRouteConfig(rds_virtual_hosts_, vhds_virtual_hosts_, route_config_proto_);
  config_info_.emplace(RouteConfigProvider::ConfigInfo{rc, last_config_version_});
}

bool RouteConfigUpdateReceiverImpl::onVhdsUpdate(
    const VirtualHostRefVector& added_vhosts, const std::set<std::string>& added_resource_ids,
    const Protobuf::RepeatedPtrField<std::string>& removed_resources,
    const std::string& version_info) {
  resource_ids_in_last_update_ = added_resource_ids;
  const bool removed = removeVhosts(vhds_virtual_hosts_, removed_resources);
  const bool updated = updateVhosts(vhds_virtual_hosts_, added_vhosts);
  onUpdateCommon(route_config_proto_, version_info);
  return removed || updated || !resource_ids_in_last_update_.empty();
}

void RouteConfigUpdateReceiverImpl::initializeRdsVhosts(
    const envoy::config::route::v3::RouteConfiguration& route_configuration) {
  rds_virtual_hosts_.clear();
  for (const auto& vhost : route_configuration.virtual_hosts()) {
    rds_virtual_hosts_.emplace(vhost.name(), vhost);
  }
}

bool RouteConfigUpdateReceiverImpl::removeVhosts(
    std::map<std::string, envoy::config::route::v3::VirtualHost>& vhosts,
    const Protobuf::RepeatedPtrField<std::string>& removed_vhost_names) {
  bool vhosts_removed = false;
  for (const auto& vhost_name : removed_vhost_names) {
    auto found = vhosts.find(vhost_name);
    if (found != vhosts.end()) {
      vhosts_removed = true;
      vhosts.erase(vhost_name);
    }
  }
  return vhosts_removed;
}

bool RouteConfigUpdateReceiverImpl::updateVhosts(
    std::map<std::string, envoy::config::route::v3::VirtualHost>& vhosts,
    const VirtualHostRefVector& added_vhosts) {
  bool vhosts_added = false;
  for (const auto& vhost : added_vhosts) {
    auto found = vhosts.find(vhost.get().name());
    if (found != vhosts.end()) {
      vhosts.erase(found);
    }
    vhosts.emplace(vhost.get().name(), vhost.get());
    vhosts_added = true;
  }
  return vhosts_added;
}

void RouteConfigUpdateReceiverImpl::rebuildRouteConfig(
    const std::map<std::string, envoy::config::route::v3::VirtualHost>& rds_vhosts,
    const std::map<std::string, envoy::config::route::v3::VirtualHost>& vhds_vhosts,
    envoy::config::route::v3::RouteConfiguration& route_config) {
  route_config.clear_virtual_hosts();
  for (const auto& vhost : rds_vhosts) {
    route_config.mutable_virtual_hosts()->Add()->CopyFrom(vhost.second);
  }
  for (const auto& vhost : vhds_vhosts) {
    route_config.mutable_virtual_hosts()->Add()->CopyFrom(vhost.second);
  }
}

} // namespace Router
} // namespace Envoy
