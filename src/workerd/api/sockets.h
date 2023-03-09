// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include "http.h"


namespace workerd::api {

struct SocketAddress {
  kj::String hostname;
  uint16_t port;
  JSG_STRUCT(hostname, port);
};

struct SocketOptions {
  bool useSecureTransport = false;
  bool allowHalfOpen = false;
  JSG_STRUCT(useSecureTransport, allowHalfOpen);
};

class Socket: public jsg::Object {
public:
  Socket(jsg::Lock& js, jsg::Ref<ReadableStream> readableParam, jsg::Ref<WritableStream> writable,
      kj::Own<jsg::PromiseResolverPair<void>> close, kj::Promise<void> connDisconnPromise,
      jsg::Optional<SocketOptions> options)
      : readable(kj::mv(readableParam)), writable(kj::mv(writable)),
        closeFulfiller(kj::mv(close)),
        closedPromise(kj::mv(closeFulfiller->promise)),
        // Listen for abrupt disconnects and resolve the `closed` promise when they occur.
        writeDisconnectedPromise(IoContext::current().awaitIo(kj::mv(connDisconnPromise))
            .then(js, [this](jsg::Lock& js) {
              closeFulfiller->resolver.resolve();
            })),
        options(kj::mv(options)) { };

  jsg::Ref<ReadableStream> getReadable() { return readable.addRef(); }
  jsg::Ref<WritableStream> getWritable() { return writable.addRef(); }
  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed() {
    return closedPromise;
  }

  jsg::Promise<void> close(jsg::Lock& js);
  // Closes the socket connection.

  void handleProxyStatus(
      jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status);
  // Sets up relevant callbacks to handle the case when the proxy rejects our connection.

  void handleReadableEof(jsg::Lock& js);
  // Sets up relevant callbacks to handle the case when the readable stream reaches EOF.

  JSG_RESOURCE_TYPE(Socket, CompatibilityFlags::Reader flags) {
    JSG_READONLY_PROTOTYPE_PROPERTY(readable, getReadable);
    JSG_READONLY_PROTOTYPE_PROPERTY(writable, getWritable);
    JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
    JSG_METHOD(close);
  }

private:
  jsg::Ref<ReadableStream> readable;
  jsg::Ref<WritableStream> writable;
  kj::Own<jsg::PromiseResolverPair<void>> closeFulfiller;
  // This fulfiller is used to resolve the `closedPromise` below.
  jsg::MemoizedIdentity<jsg::Promise<void>> closedPromise;
  jsg::Promise<void> writeDisconnectedPromise;
  jsg::Optional<SocketOptions> options;

  kj::Promise<kj::Own<kj::AsyncIoStream>> processConnection();
  jsg::Promise<void> maybeCloseWriteSide(jsg::Lock& js);

  void resolveFulfiller(jsg::Lock& js, kj::Maybe<kj::Exception> maybeErr) {
    KJ_IF_MAYBE(err, maybeErr) {
      closeFulfiller->resolver.reject(js, kj::cp(*err));
    } else {
      closeFulfiller->resolver.resolve();
    }
  };

  jsg::Promise<void> errorHandler(jsg::Lock& js, jsg::Value err) {
    auto jsException = err.getHandle(js.v8Isolate);
    resolveFulfiller(js, jsg::createTunneledException(js.v8Isolate, jsException));
    return js.resolvedPromise();
  };

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(readable, writable);
  }
};

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options);

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options,
    CompatibilityFlags::Reader featureFlags);

#define EW_SOCKETS_ISOLATE_TYPES     \
  api::Socket,                       \
  api::SocketOptions,                \
  api::SocketAddress

// The list of sockets.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE
}  // namespace workerd::api