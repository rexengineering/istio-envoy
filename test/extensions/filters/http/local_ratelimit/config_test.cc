#include "extensions/filters/http/local_ratelimit/config.h"
#include "extensions/filters/http/local_ratelimit/local_ratelimit.h"

#include "test/mocks/server/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace LocalRateLimitFilter {

TEST(Factory, GlobalEmptyConfig) {
  const std::string yaml = R"(
stat_prefix: test
  )";

  LocalRateLimitFilterConfig factory;
  ProtobufTypes::MessagePtr proto_config = factory.createEmptyRouteConfigProto();
  TestUtility::loadFromYaml(yaml, *proto_config);

  NiceMock<Server::Configuration::MockFactoryContext> context;

  EXPECT_CALL(context.dispatcher_, createTimer_(_)).Times(0);
  auto callback = factory.createFilterFactoryFromProto(*proto_config, "stats", context);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  callback(filter_callback);
}

TEST(Factory, RouteSpecificFilterConfig) {
  const std::string config_yaml = R"(
stat_prefix: test
token_bucket:
  max_tokens: 1
  tokens_per_fill: 1
  fill_interval: 1000s
filter_enabled:
  runtime_key: test_enabled
  default_value:
    numerator: 100
    denominator: HUNDRED
filter_enforced:
  runtime_key: test_enforced
  default_value:
    numerator: 100
    denominator: HUNDRED
response_headers_to_add:
  - append: false
    header:
      key: x-test-rate-limit
      value: 'true'
  )";

  LocalRateLimitFilterConfig factory;
  ProtobufTypes::MessagePtr proto_config = factory.createEmptyRouteConfigProto();
  TestUtility::loadFromYaml(config_yaml, *proto_config);

  NiceMock<Server::Configuration::MockServerFactoryContext> context;

  EXPECT_CALL(context.dispatcher_, createTimer_(_)).Times(1);
  const auto route_config = factory.createRouteSpecificFilterConfig(
      *proto_config, context, ProtobufMessage::getNullValidationVisitor());
  const auto* config = dynamic_cast<const FilterConfig*>(route_config.get());
  EXPECT_TRUE(config->requestAllowed());
}

TEST(Factory, EnabledEnforcedDisabledByDefault) {
  const std::string config_yaml = R"(
stat_prefix: test
token_bucket:
  max_tokens: 1
  tokens_per_fill: 1
  fill_interval: 1000s
  )";

  LocalRateLimitFilterConfig factory;
  ProtobufTypes::MessagePtr proto_config = factory.createEmptyRouteConfigProto();
  TestUtility::loadFromYaml(config_yaml, *proto_config);

  NiceMock<Server::Configuration::MockServerFactoryContext> context;

  EXPECT_CALL(context.dispatcher_, createTimer_(_)).Times(1);
  const auto route_config = factory.createRouteSpecificFilterConfig(
      *proto_config, context, ProtobufMessage::getNullValidationVisitor());
  const auto* config = dynamic_cast<const FilterConfig*>(route_config.get());
  EXPECT_FALSE(config->enabled());
  EXPECT_FALSE(config->enforced());
}

TEST(Factory, PerRouteConfigNoTokenBucket) {
  const std::string config_yaml = R"(
stat_prefix: test
  )";

  LocalRateLimitFilterConfig factory;
  ProtobufTypes::MessagePtr proto_config = factory.createEmptyRouteConfigProto();
  TestUtility::loadFromYaml(config_yaml, *proto_config);

  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  EXPECT_THROW(factory.createRouteSpecificFilterConfig(*proto_config, context,
                                                       ProtobufMessage::getNullValidationVisitor()),
               EnvoyException);
}

TEST(Factory, FillTimerTooLow) {
  const std::string config_yaml = R"(
stat_prefix: test
token_bucket:
  max_tokens: 1
  tokens_per_fill: 1
  fill_interval: 0.040s
  )";

  LocalRateLimitFilterConfig factory;
  ProtobufTypes::MessagePtr proto_config = factory.createEmptyRouteConfigProto();
  TestUtility::loadFromYaml(config_yaml, *proto_config);

  NiceMock<Server::Configuration::MockServerFactoryContext> context;

  EXPECT_CALL(context.dispatcher_, createTimer_(_)).Times(1);
  EXPECT_THROW(factory.createRouteSpecificFilterConfig(*proto_config, context,
                                                       ProtobufMessage::getNullValidationVisitor()),
               EnvoyException);
}

} // namespace LocalRateLimitFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
