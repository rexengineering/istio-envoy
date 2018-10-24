#include "extensions/filters/http/wasm/wasm_filter.h"

#include "envoy/http/codes.h"

#include "common/buffer/buffer_impl.h"
#include "common/common/assert.h"
#include "common/common/enum_to_int.h"
#include "common/http/message_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Wasm {

StreamHandleWrapper::StreamHandleWrapper(Session& session, Http::HeaderMap& headers,
                                         bool end_stream, Filter& filter,
                                         FilterCallbacks& callbacks)
    : session_(session), headers_(headers), end_stream_(end_stream), filter_(filter),
      callbacks_(callbacks), yield_callback_([this]() {
        if (state_ == State::Running) {
          throw WasmException("script performed an unexpected yield");
        }
      }) {}

Http::FilterHeadersStatus StreamHandleWrapper::start(int) {
#if 0
  // We are on the top of the stack.
  coroutine_.start(function_ref, 1, yield_callback_);
  Http::FilterHeadersStatus status =
      (state_ == State::WaitForBody || state_ == State::HttpCall || state_ == State::Responded)
          ? Http::FilterHeadersStatus::StopIteration
          : Http::FilterHeadersStatus::Continue;

  if (status == Http::FilterHeadersStatus::Continue) {
    headers_continued_ = true;
  }

  return status;
#else
  return Http::FilterHeadersStatus::Continue;
#endif
}

Http::FilterDataStatus StreamHandleWrapper::onData(Buffer::Instance&, bool) {
#if 0
  ASSERT(!end_stream_);
  end_stream_ = end_stream;
  saw_body_ = true;

  if (state_ == State::WaitForBodyChunk) {
    ENVOY_LOG(trace, "resuming for next body chunk");
    Common::Wasm::WasmRef<Common::Lua::BufferWrapper> wrapper(
        Filters::Common::Lua::BufferWrapper::create(coroutine_.luaState(), data), true);
    state_ = State::Running;
    coroutine_.resume(1, yield_callback_);
  } else if (state_ == State::WaitForBody && end_stream_) {
    ENVOY_LOG(debug, "resuming body due to end stream");
    callbacks_.addData(data);
    state_ = State::Running;
    coroutine_.resume(luaBody(coroutine_.luaState()), yield_callback_);
  } else if (state_ == State::WaitForTrailers && end_stream_) {
    ENVOY_LOG(debug, "resuming nil trailers due to end stream");
    state_ = State::Running;
    coroutine_.resume(0, yield_callback_);
  }

  if (state_ == State::HttpCall || state_ == State::WaitForBody) {
    ENVOY_LOG(trace, "buffering body");
    return Http::FilterDataStatus::StopIterationAndBuffer;
  } else if (state_ == State::Responded) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  } else {
    headers_continued_ = true;
    return Http::FilterDataStatus::Continue;
  }
#else
  return Http::FilterDataStatus::Continue;
#endif
}

Http::FilterTrailersStatus StreamHandleWrapper::onTrailers(Http::HeaderMap&) {
#if 0
  ASSERT(!end_stream_);
  end_stream_ = true;
  trailers_ = &trailers;

  if (state_ == State::WaitForBodyChunk) {
    ENVOY_LOG(debug, "resuming nil body chunk due to trailers");
    state_ = State::Running;
    coroutine_.resume(0, yield_callback_);
  } else if (state_ == State::WaitForBody) {
    ENVOY_LOG(debug, "resuming body due to trailers");
    state_ = State::Running;
    coroutine_.resume(luaBody(coroutine_.luaState()), yield_callback_);
  }

  if (state_ == State::WaitForTrailers) {
    // Mimic a call to trailers which will push the trailers onto the stack and then resume.
    state_ = State::Running;
    coroutine_.resume(luaTrailers(coroutine_.luaState()), yield_callback_);
  }

  Http::FilterTrailersStatus status = (state_ == State::HttpCall || state_ == State::Responded)
                                          ? Http::FilterTrailersStatus::StopIteration
                                          : Http::FilterTrailersStatus::Continue;

  if (status == Http::FilterTrailersStatus::Continue) {
    headers_continued_ = true;
  }

  return status;
#else
  return Http::FilterTrailersStatus::Continue;
#endif
}

int StreamHandleWrapper::wasmRespond(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  if (headers_continued_) {
    luaL_error(state, "respond() cannot be called if headers have been continued");
  }

  luaL_checktype(state, 2, LUA_TTABLE);
  size_t body_size;
  const char* raw_body = luaL_optlstring(state, 3, nullptr, &body_size);
  Http::HeaderMapPtr headers = buildHeadersFromTable(state, 2);

  uint64_t status;
  if (headers->Status() == nullptr ||
      !StringUtil::atoul(headers->Status()->value().c_str(), status) || status < 200 ||
      status >= 600) {
    luaL_error(state, ":status must be between 200-599");
  }

  Buffer::InstancePtr body;
  if (raw_body != nullptr) {
    body.reset(new Buffer::OwnedImpl(raw_body, body_size));
    headers->insertContentLength().value(body_size);
  }

  // Once we respond we treat that as the end of the script even if there is more code. Thus we
  // yield.
  callbacks_.respond(std::move(headers), body.get(), state);
  state_ = State::Responded;
  return lua_yield(state, 0);
#else
  return 0;
#endif
}

Http::HeaderMapPtr StreamHandleWrapper::buildHeadersFromTable(wasm_State*, int) {
  Http::HeaderMapPtr headers(new Http::HeaderMapImpl());
#if 0
  // Build a header map to make the request. We iterate through the provided table to do this and
  // check that we are getting strings.
  lua_pushnil(state);
  while (lua_next(state, table_index) != 0) {
    // Uses 'key' (at index -2) and 'value' (at index -1).
    const char* key = luaL_checkstring(state, -2);
    const char* value = luaL_checkstring(state, -1);
    headers->addCopy(Http::LowerCaseString(key), value);

    // Removes 'value'; keeps 'key' for next iteration. This is the input for lua_next() so that
    // it can push the next key/value pair onto the stack.
    lua_pop(state, 1);
  }
#endif
  return headers;
}

int StreamHandleWrapper::wasmHttpCall(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  const std::string cluster = luaL_checkstring(state, 2);
  luaL_checktype(state, 3, LUA_TTABLE);
  size_t body_size;
  const char* body = luaL_optlstring(state, 4, nullptr, &body_size);
  int timeout_ms = luaL_checkint(state, 5);
  if (timeout_ms < 0) {
    return luaL_error(state, "http call timeout must be >= 0");
  }

  if (filter_.clusterManager().get(cluster) == nullptr) {
    return luaL_error(state, "http call cluster invalid. Must be configured");
  }

  Http::MessagePtr message(new Http::RequestMessageImpl(buildHeadersFromTable(state, 3)));

  // Check that we were provided certain headers.
  if (message->headers().Path() == nullptr || message->headers().Method() == nullptr ||
      message->headers().Host() == nullptr) {
    return luaL_error(state, "http call headers must include ':path', ':method', and ':authority'");
  }

  if (body != nullptr) {
    message->body().reset(new Buffer::OwnedImpl(body, body_size));
    message->headers().insertContentLength().value(body_size);
  }

  absl::optional<std::chrono::milliseconds> timeout;
  if (timeout_ms > 0) {
    timeout = std::chrono::milliseconds(timeout_ms);
  }

  http_request_ = filter_.clusterManager().httpAsyncClientForCluster(cluster).send(
      std::move(message), *this, timeout);
  if (http_request_) {
    state_ = State::HttpCall;
    return lua_yield(state, 0);
  } else {
    // Immediate failure case. The return arguments are already on the stack.
    ASSERT(lua_gettop(state) >= 2);
    return 2;
  }
#else
  return 0;
#endif
}

void StreamHandleWrapper::onSuccess(Http::MessagePtr&&) {
#if 0
  ASSERT(state_ == State::HttpCall || state_ == State::Running);
  ENVOY_LOG(debug, "async HTTP response complete");
  http_request_ = nullptr;

  // We need to build a table with the headers as return param 1. The body will be return param 2.
  lua_newtable(coroutine_.luaState());
  response->headers().iterate(
      [](const Http::HeaderEntry& header, void* context) -> Http::HeaderMap::Iterate {
        lua_State* state = static_cast<lua_State*>(context);
        lua_pushstring(state, header.key().c_str());
        lua_pushstring(state, header.value().c_str());
        lua_settable(state, -3);
        return Http::HeaderMap::Iterate::Continue;
      },
      coroutine_.luaState());

  // TODO(mattklein123): Avoid double copy here.
  if (response->body() != nullptr) {
    lua_pushstring(coroutine_.luaState(), response->bodyAsString().c_str());
  } else {
    lua_pushnil(coroutine_.luaState());
  }

  // In the immediate failure case, we are just going to immediately return to the script. We
  // have already pushed the return arguments onto the stack.
  if (state_ == State::HttpCall) {
    state_ = State::Running;
    markLive();

    try {
      coroutine_.resume(2, yield_callback_);
      markDead();
    } catch (const Filters::Common::Lua::LuaException& e) {
      filter_.scriptError(e);
    }

    if (state_ == State::Running) {
      headers_continued_ = true;
      callbacks_.continueIteration();
    }
  }
#endif
}

void StreamHandleWrapper::onFailure(Http::AsyncClient::FailureReason) {
#if 0
  ASSERT(state_ == State::HttpCall || state_ == State::Running);
  ENVOY_LOG(debug, "async HTTP failure");

  // Just fake a basic 503 response.
  Http::MessagePtr response_message(new Http::ResponseMessageImpl(Http::HeaderMapPtr{
      new Http::HeaderMapImpl{{Http::Headers::get().Status,
                               std::to_string(enumToInt(Http::Code::ServiceUnavailable))}}}));
  response_message->body().reset(new Buffer::OwnedImpl("upstream failure"));
  onSuccess(std::move(response_message));
#endif
}

int StreamHandleWrapper::wasmHeaders(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  if (headers_wrapper_.get() != nullptr) {
    headers_wrapper_.pushStack();
  } else {
    headers_wrapper_.reset(HeaderMapWrapper::create(state, headers_,
                                                    [this] {
                                                      // If we are about to do a modifiable header
                                                      // operation, blow away the route cache. We
                                                      // could be a little more intelligent about
                                                      // when we do this so the performance would be
                                                      // higher, but this is simple and will get the
                                                      // job done for now. This is a NOP on the
                                                      // encoder path.
                                                      if (!headers_continued_) {
                                                        callbacks_.onHeadersModified();
                                                      }

                                                      return !headers_continued_;
                                                    }),
                           true);
  }
#endif
  return 1;
}

int StreamHandleWrapper::wasmBody(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  if (end_stream_) {
    if (!buffered_body_ && saw_body_) {
      return luaL_error(state, "cannot call body() after body has been streamed");
    } else if (callbacks_.bufferedBody() == nullptr) {
      ENVOY_LOG(debug, "end stream. no body");
      return 0;
    } else {
      if (body_wrapper_.get() != nullptr) {
        body_wrapper_.pushStack();
      } else {
        body_wrapper_.reset(
            Filters::Common::Lua::BufferWrapper::create(state, *callbacks_.bufferedBody()), true);
      }
      return 1;
    }
  } else if (saw_body_) {
    return luaL_error(state, "cannot call body() after body streaming has started");
  } else {
    ENVOY_LOG(debug, "yielding for full body");
    state_ = State::WaitForBody;
    buffered_body_ = true;
    return lua_yield(state, 0);
  }
#else
  return 0;
#endif
}

int StreamHandleWrapper::wasmBodyChunks(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  if (saw_body_) {
    luaL_error(state, "cannot call bodyChunks after body processing has begun");
  }

  // We are currently at the top of the stack. Push a closure that has us as the upvalue.
  lua_pushcclosure(state, static_luaBodyIterator, 1);
#endif
  return 1;
}

int StreamHandleWrapper::wasmBodyIterator(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  if (end_stream_) {
    ENVOY_LOG(debug, "body complete. no more body chunks");
    return 0;
  } else {
    ENVOY_LOG(debug, "yielding for next body chunk");
    state_ = State::WaitForBodyChunk;
    return lua_yield(state, 0);
  }
#else
  return 0;
#endif
}

int StreamHandleWrapper::wasmTrailers(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);

  if (end_stream_ && trailers_ == nullptr) {
    ENVOY_LOG(debug, "end stream. no trailers");
    return 0;
  } else if (trailers_ != nullptr) {
    if (trailers_wrapper_.get() != nullptr) {
      trailers_wrapper_.pushStack();
    } else {
      trailers_wrapper_.reset(HeaderMapWrapper::create(state, *trailers_, []() { return true; }),
                              true);
    }
    return 1;
  } else {
    ENVOY_LOG(debug, "yielding for trailers");
    state_ = State::WaitForTrailers;
    return lua_yield(state, 0);
  }
#else
  return 0;
#endif
}

int StreamHandleWrapper::wasmMetadata(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);
  if (metadata_wrapper_.get() != nullptr) {
    metadata_wrapper_.pushStack();
  } else {
    metadata_wrapper_.reset(
        Filters::Common::Lua::MetadataMapWrapper::create(state, callbacks_.metadata()), true);
  }
#endif
  return 1;
}

int StreamHandleWrapper::wasmStreamInfo(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);
  if (stream_info_wrapper_.get() != nullptr) {
    stream_info_wrapper_.pushStack();
  } else {
    stream_info_wrapper_.reset(StreamInfoWrapper::create(state, callbacks_.streamInfo()), true);
  }
#endif
  return 1;
}

int StreamHandleWrapper::wasmConnection(wasm_State*) {
#if 0
  ASSERT(state_ == State::Running);
  if (connection_wrapper_.get() != nullptr) {
    connection_wrapper_.pushStack();
  } else {
    connection_wrapper_.reset(
        Filters::Common::Lua::ConnectionWrapper::create(state, callbacks_.connection()), true);
  }
#endif
  return 1;
}

int StreamHandleWrapper::wasmLogTrace(wasm_State*) {
#if 0
  const char* message = luaL_checkstring(state, 2);
  filter_.scriptLog(spdlog::level::trace, message);
#endif
  return 0;
}

int StreamHandleWrapper::wasmLogDebug(wasm_State*) {
#if 0
  const char* message = luaL_checkstring(state, 2);
  filter_.scriptLog(spdlog::level::debug, message);
#endif
  return 0;
}

int StreamHandleWrapper::wasmLogInfo(wasm_State*) {
#if 0
  const char* message = luaL_checkstring(state, 2);
  filter_.scriptLog(spdlog::level::info, message);
#endif
  return 0;
}

int StreamHandleWrapper::wasmLogWarn(wasm_State*) {
#if 0
  const char* message = luaL_checkstring(state, 2);
  filter_.scriptLog(spdlog::level::warn, message);
#endif
  return 0;
}

int StreamHandleWrapper::wasmLogErr(wasm_State*) {
#if 0
  const char* message = luaL_checkstring(state, 2);
  filter_.scriptLog(spdlog::level::err, message);
#endif
  return 0;
}

int StreamHandleWrapper::wasmLogCritical(wasm_State*) {
#if 0
  const char* message = luaL_checkstring(state, 2);
  filter_.scriptLog(spdlog::level::critical, message);
#endif
  return 0;
}

FilterConfig::FilterConfig(const std::string& wasm_code, ThreadLocal::SlotAllocator& tls,
                           Upstream::ClusterManager& cluster_manager)
    : cluster_manager_(cluster_manager), wasm_state_(wasm_code, tls) {
#if 0
  wasm_state_.registerType<Filters::Common::Lua::BufferWrapper>();
  wasm_state_.registerType<Filters::Common::Lua::MetadataMapWrapper>();
  wasm_state_.registerType<Filters::Common::Lua::MetadataMapIterator>();
  wasm_state_.registerType<Filters::Common::Lua::ConnectionWrapper>();
  wasm_state_.registerType<Filters::Common::Lua::SslConnectionWrapper>();
  wasm_state_.registerType<HeaderMapWrapper>();
  wasm_state_.registerType<HeaderMapIterator>();
  wasm_state_.registerType<StreamInfoWrapper>();
  wasm_state_.registerType<DynamicMetadataMapWrapper>();
  wasm_state_.registerType<DynamicMetadataMapIterator>();
  wasm_state_.registerType<StreamHandleWrapper>();

  request_function_slot_ = wasm_state_.registerGlobal("envoy_on_request");
  if (wasm_state_.getGlobalRef(request_function_slot_) == LUA_REFNIL) {
    ENVOY_LOG(info, "envoy_on_request() function not found. Lua filter will not hook requests.");
  }

  response_function_slot_ = wasm_state_.registerGlobal("envoy_on_response");
  if (wasm_state_.getGlobalRef(response_function_slot_) == LUA_REFNIL) {
    ENVOY_LOG(info, "envoy_on_response() function not found. Lua filter will not hook responses.");
  }
#endif
}

void Filter::onDestroy() {
#if 0
  destroyed_ = true;
  if (request_stream_wrapper_.get()) {
    request_stream_wrapper_.get()->onReset();
  }
  if (response_stream_wrapper_.get()) {
    response_stream_wrapper_.get()->onReset();
  }
#endif
}

Http::FilterHeadersStatus Filter::doHeaders(StreamHandleRef&, SessionPtr&, FilterCallbacks&, int,
                                            Http::HeaderMap&, bool) {
#if 0
  if (function_ref == LUA_REFNIL) {
    return Http::FilterHeadersStatus::Continue;
  }

  coroutine = config_->createCoroutine();
  handle.reset(StreamHandleWrapper::create(coroutine->luaState(), *coroutine, headers, end_stream,
                                           *this, callbacks),
               true);

  Http::FilterHeadersStatus status = Http::FilterHeadersStatus::Continue;
  try {
    status = handle.get()->start(function_ref);
    handle.markDead();
  } catch (const Filters::Common::Lua::LuaException& e) {
    scriptError(e);
  }

  return status;
#else
  return Http::FilterHeadersStatus::Continue;
#endif
}

Http::FilterDataStatus Filter::doData(StreamHandleRef&, Buffer::Instance&, bool) {
#if 0
  Http::FilterDataStatus status = Http::FilterDataStatus::Continue;
  if (handle.get() != nullptr) {
    try {
      handle.markLive();
      status = handle.get()->onData(data, end_stream);
      handle.markDead();
    } catch (const Filters::Common::Lua::LuaException& e) {
      scriptError(e);
    }
  }

  return status;
#else
  return Http::FilterDataStatus::Continue;
#endif
}

Http::FilterTrailersStatus Filter::doTrailers(StreamHandleRef&, Http::HeaderMap&) {
#if 0
  Http::FilterTrailersStatus status = Http::FilterTrailersStatus::Continue;
  if (handle.get() != nullptr) {
    try {
      handle.markLive();
      status = handle.get()->onTrailers(trailers);
      handle.markDead();
    } catch (const Filters::Common::Lua::LuaException& e) {
      scriptError(e);
    }
  }

  return status;
#else
  return Http::FilterTrailersStatus::Continue;
#endif
}

void Filter::scriptError(const WasmException& e) {
  scriptLog(spdlog::level::err, e.what());
  request_stream_wrapper_.reset();
  response_stream_wrapper_.reset();
}

void Filter::scriptLog(spdlog::level::level_enum level, const char* message) {
  switch (level) {
  case spdlog::level::trace:
    ENVOY_LOG(trace, "script log: {}", message);
    return;
  case spdlog::level::debug:
    ENVOY_LOG(debug, "script log: {}", message);
    return;
  case spdlog::level::info:
    ENVOY_LOG(info, "script log: {}", message);
    return;
  case spdlog::level::warn:
    ENVOY_LOG(warn, "script log: {}", message);
    return;
  case spdlog::level::err:
    ENVOY_LOG(error, "script log: {}", message);
    return;
  case spdlog::level::critical:
    ENVOY_LOG(critical, "script log: {}", message);
    return;
  case spdlog::level::off:
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }
}

void Filter::DecoderCallbacks::respond(Http::HeaderMapPtr&&, Buffer::Instance*, wasm_State*) {
#if 0
  callbacks_->encodeHeaders(std::move(headers), body == nullptr);
  if (body && !parent_.destroyed_) {
    callbacks_->encodeData(*body, true);
  }
#endif
}

void Filter::EncoderCallbacks::respond(Http::HeaderMapPtr&&, Buffer::Instance*, wasm_State*) {
#if 0
  // TODO(mattklein123): Support response in response path if nothing has been continued
  // yet.
  luaL_error(state, "respond not currently supported in the response path");
#endif
}

} // namespace Wasm
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
