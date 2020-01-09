#include "extensions/common/wasm/null/null_plugin.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Wasm {
namespace Null {
namespace Plugin {
namespace CommonWasmTestCpp {
ThreadSafeSingleton<NullPluginRegistry> null_plugin_registry_;
} // namespace CommonWasmTestCpp

/**
 * Config registration for a Wasm filter plugin. @see NamedHttpFilterConfigFactory.
 */
class PluginFactory : public NullVmPluginFactory {
public:
  PluginFactory() {}

  const std::string name() const override {
    // FIXME: work around issue with coverage doubly registering this factory.
    return "CommonWasmTestCpp" + std::string(suffix_++, '_');
  }
  std::unique_ptr<NullVmPlugin> create() const override {
    return std::make_unique<NullPlugin>(
        &Envoy::Extensions::Common::Wasm::Null::Plugin::CommonWasmTestCpp::null_plugin_registry_
             .get());
  }

private:
  static int suffix_;
};
int PluginFactory::suffix_ = 0;

/**
 * Static registration for the null Wasm filter. @see RegisterFactory.
 */
static Registry::RegisterFactory<PluginFactory, NullVmPluginFactory> register_;
} // namespace Plugin
} // namespace Null
} // namespace Wasm
} // namespace Common
} // namespace Extensions
} // namespace Envoy
