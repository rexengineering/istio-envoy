#include "extensions/filters/http/ext_proc/ext_proc.h"

#include "test/common/http/common.h"
#include "test/extensions/filters/http/ext_proc/mock_server.h"
#include "test/extensions/filters/http/ext_proc/utils.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/router/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {
namespace {

using envoy::service::ext_proc::v3alpha::ProcessingRequest;
using envoy::service::ext_proc::v3alpha::ProcessingResponse;

using Http::FilterDataStatus;
using Http::FilterHeadersStatus;
using Http::FilterTrailersStatus;
using Http::LowerCaseString;

using testing::Invoke;

using namespace std::chrono_literals;

class HttpFilterTest : public testing::Test {
protected:
  void initialize(std::string&& yaml) {
    client_ = std::make_unique<MockClient>();
    EXPECT_CALL(*client_, start(_, _)).WillOnce(Invoke(this, &HttpFilterTest::doStart));

    envoy::extensions::filters::http::ext_proc::v3alpha::ExternalProcessor proto_config{};
    if (!yaml.empty()) {
      TestUtility::loadFromYaml(yaml, proto_config);
    }
    config_.reset(new FilterConfig(proto_config, 200ms, stats_store_, ""));
    filter_ = std::make_unique<Filter>(config_, std::move(client_));
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
  }

  ExternalProcessorStreamPtr doStart(ExternalProcessorCallbacks& callbacks,
                                     const std::chrono::milliseconds& timeout) {
    stream_callbacks_ = &callbacks;
    stream_timeout_ = timeout;

    auto stream = std::make_unique<MockStream>();
    EXPECT_CALL(*stream, send(_, _)).WillRepeatedly(Invoke(this, &HttpFilterTest::doSend));
    EXPECT_CALL(*stream, close()).WillRepeatedly(Invoke(this, &HttpFilterTest::doSendClose));
    return stream;
  }

  void doSend(ProcessingRequest&& request, bool end_stream) {
    ASSERT_FALSE(stream_close_sent_);
    last_request_ = std::move(request);
    if (end_stream) {
      stream_close_sent_ = true;
    }
  }

  void doSendClose() {
    ASSERT_FALSE(stream_close_sent_);
    stream_close_sent_ = true;
  }

  std::unique_ptr<MockClient> client_;
  ExternalProcessorCallbacks* stream_callbacks_ = nullptr;
  ProcessingRequest last_request_;
  bool stream_close_sent_ = false;
  std::chrono::milliseconds stream_timeout_;
  NiceMock<Stats::MockIsolatedStatsStore> stats_store_;
  FilterConfigSharedPtr config_;
  std::unique_ptr<Filter> filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
  Http::TestRequestHeaderMapImpl request_headers_;
  Http::TestResponseHeaderMapImpl response_headers_;
  Http::TestRequestTrailerMapImpl request_trailers_;
  Http::TestResponseTrailerMapImpl response_trailers_;
  Buffer::OwnedImpl data_;
};

TEST_F(HttpFilterTest, SimplestPost) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  failure_mode_allow: true
  )EOF");

  EXPECT_TRUE(config_->failureModeAllow());

  // Create synthetic HTTP request
  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");
  request_headers_.addCopy(LowerCaseString("content-type"), "text/plain");
  request_headers_.addCopy(LowerCaseString("content-length"), 10);
  request_headers_.addCopy(LowerCaseString("x-some-other-header"), "yes");

  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  // Verify that call was received by mock gRPC server
  EXPECT_FALSE(last_request_.async_mode());
  EXPECT_FALSE(stream_close_sent_);
  ASSERT_TRUE(last_request_.has_request_headers());
  const auto request_headers = last_request_.request_headers();
  EXPECT_FALSE(request_headers.end_of_stream());

  Http::TestRequestHeaderMapImpl expected{{":path", "/"},
                                          {":method", "POST"},
                                          {":scheme", "http"},
                                          {"host", "host"},
                                          {"content-type", "text/plain"},
                                          {"content-length", "10"},
                                          {"x-some-other-header", "yes"}};
  EXPECT_TRUE(
      ExtProcTestUtility::headerProtosEqualIgnoreOrder(expected, request_headers.headers()));

  // Send back a response
  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  std::unique_ptr<ProcessingResponse> resp1 = std::make_unique<ProcessingResponse>();
  resp1->mutable_request_headers();
  stream_callbacks_->onReceiveMessage(std::move(resp1));

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();
  EXPECT_TRUE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_received_.value());
  EXPECT_EQ(1, config_->stats().streams_closed_.value());
}

TEST_F(HttpFilterTest, PostAndChangeHeaders) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  )EOF");

  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");
  request_headers_.addCopy(LowerCaseString("x-some-other-header"), "yes");
  request_headers_.addCopy(LowerCaseString("x-do-we-want-this"), "no");

  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_FALSE(last_request_.async_mode());
  EXPECT_FALSE(stream_close_sent_);
  ASSERT_TRUE(last_request_.has_request_headers());

  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  std::unique_ptr<ProcessingResponse> resp1 = std::make_unique<ProcessingResponse>();
  auto req_headers_response = resp1->mutable_request_headers();
  auto headers_mut = req_headers_response->mutable_response()->mutable_header_mutation();
  auto add1 = headers_mut->add_set_headers();
  add1->mutable_header()->set_key("x-new-header");
  add1->mutable_header()->set_value("new");
  add1->mutable_append()->set_value(false);
  auto add2 = headers_mut->add_set_headers();
  add2->mutable_header()->set_key("x-some-other-header");
  add2->mutable_header()->set_value("no");
  add2->mutable_append()->set_value(true);
  *headers_mut->add_remove_headers() = "x-do-we-want-this";
  stream_callbacks_->onReceiveMessage(std::move(resp1));

  // We should now have changed the original header a bit
  request_headers_.iterate([](const Http::HeaderEntry& e) -> Http::HeaderMap::Iterate {
    std::cerr << e.key().getStringView() << ": " << e.value().getStringView() << '\n';
    return Http::HeaderMap::Iterate::Continue;
  });
  auto get1 = request_headers_.get(LowerCaseString("x-new-header"));
  EXPECT_EQ(get1.size(), 1);
  EXPECT_EQ(get1[0]->key(), "x-new-header");
  EXPECT_EQ(get1[0]->value(), "new");
  auto get2 = request_headers_.get(LowerCaseString("x-some-other-header"));
  EXPECT_EQ(get2.size(), 2);
  EXPECT_EQ(get2[0]->key(), "x-some-other-header");
  EXPECT_EQ(get2[0]->value(), "yes");
  EXPECT_EQ(get2[1]->key(), "x-some-other-header");
  EXPECT_EQ(get2[1]->value(), "no");
  auto get3 = request_headers_.get(LowerCaseString("x-do-we-want-this"));
  EXPECT_TRUE(get3.empty());

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();
  EXPECT_TRUE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_received_.value());
  EXPECT_EQ(1, config_->stats().streams_closed_.value());
}

TEST_F(HttpFilterTest, PostAndRespondImmediately) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  )EOF");

  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");

  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  std::unique_ptr<ProcessingResponse> resp1 = std::make_unique<ProcessingResponse>();
  auto* immediate_response = resp1->mutable_immediate_response();
  immediate_response->mutable_status()->set_code(envoy::type::v3::StatusCode::BadRequest);
  immediate_response->set_body("Bad request");
  immediate_response->set_details("Got a bad request");
  stream_callbacks_->onReceiveMessage(std::move(resp1));

  // Immediate response processing not yet implemented -- all we can expect
  // at this point is that continueDecoding is called and that the
  // stream is not yet closed.
  EXPECT_FALSE(stream_close_sent_);

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();
  EXPECT_TRUE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_received_.value());
  EXPECT_EQ(1, config_->stats().streams_closed_.value());
}

TEST_F(HttpFilterTest, PostAndFail) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  )EOF");

  EXPECT_FALSE(config_->failureModeAllow());

  // Create synthetic HTTP request
  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");
  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_FALSE(stream_close_sent_);

  // Oh no! The remote server had a failure!
  EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::InternalServerError, _, _, _, _));
  stream_callbacks_->onGrpcError(Grpc::Status::Internal);

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();
  // The other side closed the stream
  EXPECT_FALSE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().streams_failed_.value());
}

TEST_F(HttpFilterTest, PostAndIgnoreFailure) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  failure_mode_allow: true
  )EOF");

  EXPECT_TRUE(config_->failureModeAllow());

  // Create synthetic HTTP request
  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");
  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_FALSE(stream_close_sent_);

  // Oh no! The remote server had a failure which we will ignore
  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  stream_callbacks_->onGrpcError(Grpc::Status::Internal);

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();
  // The other side closed the stream
  EXPECT_FALSE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().streams_closed_.value());
  EXPECT_EQ(1, config_->stats().failure_mode_allowed_.value());
}

TEST_F(HttpFilterTest, PostAndClose) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  )EOF");

  EXPECT_FALSE(config_->failureModeAllow());

  // Create synthetic HTTP request
  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");
  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_FALSE(last_request_.async_mode());
  EXPECT_FALSE(stream_close_sent_);
  ASSERT_TRUE(last_request_.has_request_headers());

  // Close the stream, which should tell the filter to keep on going
  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  stream_callbacks_->onGrpcClose();

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();

  // The other side closed the stream
  EXPECT_FALSE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().streams_closed_.value());
}

TEST_F(HttpFilterTest, OutOfOrder) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_proc_server"
  )EOF");

  HttpTestUtility::addDefaultHeaders(request_headers_, "POST");
  EXPECT_EQ(FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));

  EXPECT_FALSE(last_request_.async_mode());
  EXPECT_FALSE(stream_close_sent_);
  ASSERT_TRUE(last_request_.has_request_headers());

  // Return an out-of-order message. The server should close the stream
  // and continue as if nothing happened.
  EXPECT_CALL(decoder_callbacks_, continueDecoding());
  std::unique_ptr<ProcessingResponse> resp1 = std::make_unique<ProcessingResponse>();
  resp1->mutable_request_body();
  stream_callbacks_->onReceiveMessage(std::move(resp1));

  data_.add("foo");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();

  // We closed the stream
  EXPECT_TRUE(stream_close_sent_);

  EXPECT_EQ(1, config_->stats().streams_started_.value());
  EXPECT_EQ(1, config_->stats().stream_msgs_sent_.value());
  EXPECT_EQ(1, config_->stats().spurious_msgs_received_.value());
  EXPECT_EQ(1, config_->stats().streams_closed_.value());
}

} // namespace
} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy