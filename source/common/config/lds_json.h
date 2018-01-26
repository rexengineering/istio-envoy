#pragma once

#include "envoy/api/v2/listener/listener.pb.h"
#include "envoy/json/json_object.h"

namespace Envoy {
namespace Config {

class LdsJson {
public:
  /**
   * Translate a v1 JSON Listener to v2 envoy::api::v2::listener::Listener.
   * @param json_listener source v1 JSON Listener object.
   * @param listener destination v2 envoy::api::v2::listener::Listener.
   */
  static void translateListener(const Json::Object& json_listener,
                                envoy::api::v2::listener::Listener& listener);
};

} // namespace Config
} // namespace Envoy
