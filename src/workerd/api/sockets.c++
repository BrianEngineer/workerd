// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"
#include "system-streams.h"
#include <workerd/io/worker-interface.h>
#include "url-standard.h"


namespace workerd::api {

namespace {

bool isValidHost(kj::StringPtr host) {
  // This function performs some basic length and characters checks, it does not guarantee that
  // the specified host is a valid domain. It should only be used to reject malicious
  // hosts.
  if (host.size() > 255 || host.size() == 0) {
    // RFC1035 states that maximum domain name length is 255 octets.
    //
    // IP addresses are always shorter, so we take the max domain length instead.
    return false;
  }

  for (auto i : kj::indices(host)) {
    switch (host[i]) {
      case '-':
      case '.':
      case '_':
      case '[': case ']': case ':': // For IPv6.
        break;
      default:
        if ((host[i] >= 'a' && host[i] <= 'z') ||
            (host[i] >= 'A' && host[i] <= 'Z') ||
            (host[i] >= '0' && host[i] <= '9')) {
          break;
        }
        return false;
    }
  }
  return true;
}

SecureTransportKind parseSecureTransport(SocketOptions* opts) {
  auto value = KJ_UNWRAP_OR_RETURN(opts->secureTransport, SecureTransportKind::OFF).begin();
  if (value == "off"_kj) {
    return SecureTransportKind::OFF;
  } else if (value == "starttls"_kj) {
    return SecureTransportKind::STARTTLS;
  } else if (value == "on"_kj) {
    return SecureTransportKind::ON;
  } else {
    JSG_FAIL_REQUIRE(TypeError,
        kj::str("Unsupported value in secureTransport socket option: ", value));
  }
}

bool getAllowHalfOpen(jsg::Optional<SocketOptions>& opts) {
  KJ_IF_MAYBE(o, opts) {
    return o->allowHalfOpen;
  }

  // The allowHalfOpen flag is false by default.
  return false;
}

jsg::Ref<Socket> setupSocket(
    jsg::Lock& js, kj::Own<kj::AsyncIoStream> connection,
    jsg::Optional<SocketOptions> options, kj::Own<kj::TlsStarterCallback> tlsStarter,
    bool isSecureSocket, kj::String domain) {
  auto& ioContext = IoContext::current();
  auto connDisconnPromise = connection->whenWriteDisconnected();

  auto refcountedConnection = kj::refcountedWrapper(kj::mv(connection));
  // Initialise the readable/writable streams with the readable/writable sides of an AsyncIoStream.
  auto sysStreams = newSystemMultiStream(refcountedConnection->addWrappedRef(), ioContext);
  auto readable = jsg::alloc<ReadableStream>(ioContext, kj::mv(sysStreams.readable));
  auto allowHalfOpen = getAllowHalfOpen(options);
  kj::Maybe<jsg::Promise<void>> eofPromise;
  if (!allowHalfOpen) {
    eofPromise = readable->onEof(js);
  }
  auto writable = jsg::alloc<WritableStream>(ioContext, kj::mv(sysStreams.writable));

  auto closeFulfiller = jsg::newPromiseAndResolver<void>(ioContext.getCurrentLock().getIsolate());
  closeFulfiller.promise.markAsHandled();

  auto result = jsg::alloc<Socket>(
      js,
      kj::mv(refcountedConnection),
      kj::mv(readable),
      kj::mv(writable),
      kj::mv(closeFulfiller),
      kj::mv(connDisconnPromise),
      kj::mv(options),
      kj::mv(tlsStarter),
      isSecureSocket,
      kj::mv(domain));
  KJ_IF_MAYBE(p, eofPromise) {
    result->handleReadableEof(js, kj::mv(*p));
  }
  return result;
}

} // namespace

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options) {

  // Extract the domain/ip we are connecting to from the address.
  kj::String domain;
  KJ_SWITCH_ONEOF(address) {
    KJ_CASE_ONEOF(str, kj::String) {
      // We need just the hostname part of the address, i.e. we want to strip out the port.
      // We do this using the standard URL parser since it will handle IPv6 for us as well.
      auto record = JSG_REQUIRE_NONNULL(url::URL::parse(jsg::usv(kj::str("https://", str))),
          TypeError, "Specified address could not be parsed.");
      auto& host = JSG_REQUIRE_NONNULL(record.host, TypeError,
          "Specified address is missing hostname.");
      domain = host.toStr();
    }
    KJ_CASE_ONEOF(record, SocketAddress) {
      domain = kj::heapString(record.hostname);
    }
  }

  // Convert the address to a string that we can pass to kj.
  auto addressStr = kj::str("");
  KJ_SWITCH_ONEOF(address) {
    KJ_CASE_ONEOF(str, kj::String) {
      addressStr = kj::mv(str);
    }
    KJ_CASE_ONEOF(record, SocketAddress) {
      addressStr = kj::str(record.hostname, ":", record.port);
    }
  }

  JSG_REQUIRE(isValidHost(addressStr), TypeError,
      "Specified address is empty string, contains unsupported characters or is too long.");

  auto& ioContext = IoContext::current();

  JSG_REQUIRE(!ioContext.isFiddle(), TypeError, "Socket API not supported in web preview mode.");

  auto jsRequest = Request::constructor(js, kj::str(addressStr), nullptr);
  kj::Own<WorkerInterface> client = fetcher->getClient(
      ioContext, jsRequest->serializeCfBlobJson(js), "connect"_kj);

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = asHttpClient(kj::mv(client));
  kj::HttpConnectSettings httpConnectSettings = { .useTls = false };
  KJ_IF_MAYBE(opts, options) {
    httpConnectSettings.useTls =
        parseSecureTransport(opts) == SecureTransportKind::ON;
  }
  kj::Own<kj::TlsStarterCallback> tlsStarter = kj::heap<kj::TlsStarterCallback>();
  httpConnectSettings.tlsStarter = tlsStarter;
  auto request = httpClient->connect(addressStr, *headers, httpConnectSettings);
  request.connection = request.connection.attach(kj::mv(httpClient));

  auto result = setupSocket(
      js, kj::mv(request.connection), kj::mv(options), kj::mv(tlsStarter),
      httpConnectSettings.useTls, kj::mv(domain));
  // `handleProxyStatus` needs an initialised refcount to use `JSG_THIS`, hence it cannot be
  // called in Socket's constructor. Also it's only necessary when creating a Socket as a result of
  // a `connect`.
  result->handleProxyStatus(js, kj::mv(request.status));
  return result;
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, AnySocketAddress address,
    jsg::Optional<SocketOptions> options,
    CompatibilityFlags::Reader featureFlags) {
  jsg::Ref<Fetcher> actualFetcher = nullptr;
  KJ_IF_MAYBE(f, fetcher) {
    actualFetcher = kj::mv(*f);
  } else {
    actualFetcher = jsg::alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }
  return connectImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(address), kj::mv(options));
}

jsg::Promise<void> Socket::close(jsg::Lock& js) {
  // Forcibly close the readable/writable streams.
  auto cancelPromise = readable->getController().cancel(js, nullptr);
  auto abortPromise = writable->getController().abort(js, nullptr);
  // The below is effectively `Promise.all(cancelPromise, abortPromise)`
  return cancelPromise.then(js, [abortPromise = kj::mv(abortPromise), this](jsg::Lock& js) mutable {
    return abortPromise.then(js, [this](jsg::Lock& js) {
      resolveFulfiller(js, nullptr);
      return js.resolvedPromise();
    });
  }, [this](jsg::Lock& js, jsg::Value err) { return errorHandler(js, kj::mv(err)); });
}

jsg::Ref<Socket> Socket::startTls(jsg::Lock& js, jsg::Optional<TlsOptions> tlsOptions) {
  JSG_REQUIRE(!isSecureSocket, TypeError, "Cannot startTls on a TLS socket.");
  // TODO: Track closed state of socket properly and assert that it hasn't been closed here.
  JSG_REQUIRE(domain != nullptr, TypeError, "startTls can only be called once.");
  auto invalidOptKindMsg =
      "The `secureTransport` socket option must be set to 'starttls' for startTls to be used.";
  KJ_IF_MAYBE(opts, options) {
    JSG_REQUIRE(parseSecureTransport(opts) == SecureTransportKind::STARTTLS,
        TypeError, invalidOptKindMsg);
  } else {
    JSG_FAIL_REQUIRE(TypeError, invalidOptKindMsg);
  }

  // The current socket's writable buffers need to be flushed. The socket's WritableStream is backed
  // by an AsyncIoStream which doesn't implement any buffering, so we don't need to worry about
  // flushing. But the JS WritableStream holds a queue so some data may still be buffered. This
  // means we need to flush the WritableStream.
  //
  // Detach the AsyncIoStream from the Writable/Readable streams and make them unusable.
  auto& context = IoContext::current();
  auto secureStreamPromise = context.awaitJs(writable->flush(js).then(js,
      [this, domain = kj::heapString(domain), tlsOptions = kj::mv(tlsOptions),
      tlsStarter = kj::mv(tlsStarter)](jsg::Lock& js) mutable {
    writable->removeSink(js);
    readable = readable->detach(js, true);
    closeFulfiller.resolver.resolve();

    auto acceptedHostname = domain.asPtr();
    KJ_IF_MAYBE(s, tlsOptions) {
      KJ_IF_MAYBE(expectedHost, s->expectedServerHostname) {
        acceptedHostname = *expectedHost;
      }
    }

    // All non-secure sockets should have a tlsStarter.
    auto secureStream = KJ_ASSERT_NONNULL(*tlsStarter)(acceptedHostname).then(
      [stream = connectionStream->addWrappedRef()]() mutable -> kj::Own<kj::AsyncIoStream> {
        return kj::mv(stream);
      });
    return kj::newPromisedStream(kj::mv(secureStream));
  }));

  // The existing tlsStarter gets consumed and we won't need it again. Pass in an empty tlsStarter
  // to `setupSocket`.
  auto newTlsStarter = kj::heap<kj::TlsStarterCallback>();
  return setupSocket(js, kj::newPromisedStream(kj::mv(secureStreamPromise)), kj::mv(options),
      kj::mv(newTlsStarter), true, kj::mv(domain));
}

void Socket::handleProxyStatus(
    jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status) {
  auto& context = IoContext::current();
  auto result = context.awaitIo(js, kj::mv(status),
      [this, self = JSG_THIS](jsg::Lock& js, kj::HttpClient::ConnectRequest::Status&& status) -> void {
    if (status.statusCode < 200 || status.statusCode >= 300) {
      // If the status indicates an unsucessful connection we need to reject the `closeFulfiller`
      // with an exception. This will reject the socket's `closed` promise.
      closureInProgress = false;
      if (status.statusCode == 403) {
        KJ_IF_MAYBE(errorBody, status.errorBody) {
          // The proxy denied our request with a helpful error message. So read it here and return
          // to the user.
          auto& context = IoContext::current();
          auto maybeSize = status.headers->get(
              kj::HttpHeaderId::CONTENT_LENGTH).orDefault("").tryParseAs<int64_t>();
          KJ_IF_MAYBE(size, maybeSize) {
            KJ_DBG("Maybe size ", *size);
            closureInProgress = true;
            auto buffer = kj::heapString(*size);
            auto promise = context.awaitIo(js, (*errorBody)->tryRead(buffer.begin(), 1, *size),
                [this, self = JSG_THIS, buffer=kj::mv(buffer)](jsg::Lock& js, size_t amount) {
              KJ_DBG("Rejecting! ", amount, buffer);
              auto exc = kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
                  kj::str(JSG_EXCEPTION(Error), ": ", buffer));
              resolveFulfiller(js, exc);
              readable->getController().cancel(js, nullptr).markAsHandled();
              writable->getController().abort(js, nullptr).markAsHandled();
            });
            promise.markAsHandled();
          }
        }
      }
      if (!closureInProgress) {
        auto exc = kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
            kj::str(JSG_EXCEPTION(Error) ": proxy request failed"));
        resolveFulfiller(js, exc);
        readable->getController().cancel(js, nullptr).markAsHandled();
        writable->getController().abort(js, nullptr).markAsHandled();
      }
    }
  });
  result.markAsHandled();
}

void Socket::handleReadableEof(jsg::Lock& js, jsg::Promise<void> onEof) {
  KJ_ASSERT(!getAllowHalfOpen(options));
  // Listen for EOF on the ReadableStream.
  onEof.then(js,
      JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js) {
    return ref->maybeCloseWriteSide(js);
  })).markAsHandled();
}

jsg::Promise<void> Socket::maybeCloseWriteSide(jsg::Lock& js) {
  // When `allowHalfOpen` is set to true then we do not automatically close the write side on EOF.
  // This code shouldn't even run since we don't set up a callback which calls it unless
  // `allowHalfOpen` is false.
  KJ_ASSERT(!getAllowHalfOpen(options));

  // Do not call `close` on a controller that has already been closed or is in the process
  // of closing.
  if (writable->getController().isClosedOrClosing()) {
    return js.resolvedPromise();
  }

  // We want to close the socket, but only after its WritableStream has been flushed. We do this
  // below by calling `close` on the WritableStream which ensures that any data pending on it
  // is flushed. Then once the `close` either completes or fails we can be sure that any data has
  // been flushed.
  return writable->getController().close(js).catch_(js,
      JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js, jsg::Value&& exc) {
    ref->closeFulfiller.resolver.reject(js, exc.getHandle(js.v8Isolate));
  })).then(js, JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js) {
    ref->closeFulfiller.resolver.resolve();
  }));
}

}  // namespace workerd::api
