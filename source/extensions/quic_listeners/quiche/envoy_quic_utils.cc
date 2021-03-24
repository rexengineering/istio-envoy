#include "extensions/quic_listeners/quiche/envoy_quic_utils.h"

#include "envoy/common/platform.h"
#include "envoy/config/core/v3/base.pb.h"

#include "common/network/socket_option_factory.h"
#include "common/network/utility.h"

namespace Envoy {
namespace Quic {

// TODO(danzh): this is called on each write. Consider to return an address instance on the stack if
// the heap allocation is too expensive.
Network::Address::InstanceConstSharedPtr
quicAddressToEnvoyAddressInstance(const quic::QuicSocketAddress& quic_address) {
  return quic_address.IsInitialized()
             ? Network::Address::addressFromSockAddr(quic_address.generic_address(),
                                                     quic_address.host().address_family() ==
                                                             quic::IpAddressFamily::IP_V4
                                                         ? sizeof(sockaddr_in)
                                                         : sizeof(sockaddr_in6),
                                                     false)
             : nullptr;
}

quic::QuicSocketAddress envoyIpAddressToQuicSocketAddress(const Network::Address::Ip* envoy_ip) {
  if (envoy_ip == nullptr) {
    // Return uninitialized socket addr
    return quic::QuicSocketAddress();
  }

  uint32_t port = envoy_ip->port();
  sockaddr_storage ss;

  if (envoy_ip->version() == Network::Address::IpVersion::v4) {
    // Create and return quic ipv4 address
    auto ipv4_addr = reinterpret_cast<sockaddr_in*>(&ss);
    memset(ipv4_addr, 0, sizeof(sockaddr_in));
    ipv4_addr->sin_family = AF_INET;
    ipv4_addr->sin_port = htons(port);
    ipv4_addr->sin_addr.s_addr = envoy_ip->ipv4()->address();
  } else {
    // Create and return quic ipv6 address
    auto ipv6_addr = reinterpret_cast<sockaddr_in6*>(&ss);
    memset(ipv6_addr, 0, sizeof(sockaddr_in6));
    ipv6_addr->sin6_family = AF_INET6;
    ipv6_addr->sin6_port = htons(port);
    ASSERT(sizeof(ipv6_addr->sin6_addr.s6_addr) == 16u);
    *reinterpret_cast<absl::uint128*>(ipv6_addr->sin6_addr.s6_addr) = envoy_ip->ipv6()->address();
  }
  return quic::QuicSocketAddress(ss);
}

spdy::SpdyHeaderBlock envoyHeadersToSpdyHeaderBlock(const Http::HeaderMap& headers) {
  spdy::SpdyHeaderBlock header_block;
  headers.iterate([&header_block](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
    // The key-value pairs are copied.
    header_block.AppendValueOrAddHeader(header.key().getStringView(),
                                        header.value().getStringView());
    return Http::HeaderMap::Iterate::Continue;
  });
  return header_block;
}

quic::QuicRstStreamErrorCode envoyResetReasonToQuicRstError(Http::StreamResetReason reason) {
  switch (reason) {
  case Http::StreamResetReason::LocalRefusedStreamReset:
    return quic::QUIC_REFUSED_STREAM;
  case Http::StreamResetReason::ConnectionFailure:
  case Http::StreamResetReason::ConnectionTermination:
    return quic::QUIC_STREAM_CONNECTION_ERROR;
  case Http::StreamResetReason::LocalReset:
    return quic::QUIC_STREAM_CANCELLED;
  default:
    return quic::QUIC_BAD_APPLICATION_PAYLOAD;
  }
}

Http::StreamResetReason quicRstErrorToEnvoyLocalResetReason(quic::QuicRstStreamErrorCode rst_err) {
  switch (rst_err) {
  case quic::QUIC_REFUSED_STREAM:
    return Http::StreamResetReason::LocalRefusedStreamReset;
  case quic::QUIC_STREAM_CONNECTION_ERROR:
    return Http::StreamResetReason::ConnectionFailure;
  default:
    return Http::StreamResetReason::LocalReset;
  }
}

Http::StreamResetReason quicRstErrorToEnvoyRemoteResetReason(quic::QuicRstStreamErrorCode rst_err) {
  switch (rst_err) {
  case quic::QUIC_REFUSED_STREAM:
    return Http::StreamResetReason::RemoteRefusedStreamReset;
  case quic::QUIC_STREAM_CONNECTION_ERROR:
    return Http::StreamResetReason::ConnectError;
  default:
    return Http::StreamResetReason::RemoteReset;
  }
}

Http::StreamResetReason quicErrorCodeToEnvoyResetReason(quic::QuicErrorCode error) {
  if (error == quic::QUIC_NO_ERROR) {
    return Http::StreamResetReason::ConnectionTermination;
  } else {
    return Http::StreamResetReason::ConnectionFailure;
  }
}

Http::GoAwayErrorCode quicErrorCodeToEnvoyErrorCode(quic::QuicErrorCode error) noexcept {
  switch (error) {
  case quic::QUIC_NO_ERROR:
    return Http::GoAwayErrorCode::NoError;
  default:
    return Http::GoAwayErrorCode::Other;
  }
}

Network::ConnectionSocketPtr
createConnectionSocket(Network::Address::InstanceConstSharedPtr& peer_addr,
                       Network::Address::InstanceConstSharedPtr& local_addr,
                       const Network::ConnectionSocket::OptionsSharedPtr& options) {
  if (local_addr == nullptr) {
    local_addr = Network::Utility::getLocalAddress(peer_addr->ip()->version());
  }
  auto connection_socket = std::make_unique<Network::ConnectionSocketImpl>(
      Network::Socket::Type::Datagram, local_addr, peer_addr);
  connection_socket->addOptions(Network::SocketOptionFactory::buildIpPacketInfoOptions());
  connection_socket->addOptions(Network::SocketOptionFactory::buildRxQueueOverFlowOptions());
  if (options != nullptr) {
    connection_socket->addOptions(options);
  }
  if (!Network::Socket::applyOptions(connection_socket->options(), *connection_socket,
                                     envoy::config::core::v3::SocketOption::STATE_PREBIND)) {
    connection_socket->close();
    ENVOY_LOG_MISC(error, "Fail to apply pre-bind options");
    return connection_socket;
  }
  connection_socket->bind(local_addr);
  ASSERT(local_addr->ip());
  local_addr = connection_socket->addressProvider().localAddress();
  if (!Network::Socket::applyOptions(connection_socket->options(), *connection_socket,
                                     envoy::config::core::v3::SocketOption::STATE_BOUND)) {
    ENVOY_LOG_MISC(error, "Fail to apply post-bind options");
    connection_socket->close();
  }
  return connection_socket;
}

bssl::UniquePtr<X509> parseDERCertificate(const std::string& der_bytes,
                                          std::string* error_details) {
  const uint8_t* data;
  const uint8_t* orig_data;
  orig_data = data = reinterpret_cast<const uint8_t*>(der_bytes.data());
  bssl::UniquePtr<X509> cert(d2i_X509(nullptr, &data, der_bytes.size()));
  if (!cert.get()) {
    *error_details = "d2i_X509: fail to parse DER";
    return nullptr;
  }
  if (data < orig_data || static_cast<size_t>(data - orig_data) != der_bytes.size()) {
    *error_details = "There is trailing garbage in DER.";
    return nullptr;
  }
  return cert;
}

int deduceSignatureAlgorithmFromPublicKey(const EVP_PKEY* public_key, std::string* error_details) {
  int sign_alg = 0;
  const int pkey_id = EVP_PKEY_id(public_key);
  switch (pkey_id) {
  case EVP_PKEY_EC: {
    // We only support P-256 ECDSA today.
    const EC_KEY* ecdsa_public_key = EVP_PKEY_get0_EC_KEY(public_key);
    // Since we checked the key type above, this should be valid.
    ASSERT(ecdsa_public_key != nullptr);
    const EC_GROUP* ecdsa_group = EC_KEY_get0_group(ecdsa_public_key);
    if (ecdsa_group == nullptr || EC_GROUP_get_curve_name(ecdsa_group) != NID_X9_62_prime256v1) {
      *error_details = "Invalid leaf cert, only P-256 ECDSA certificates are supported";
      break;
    }
    // QUICHE uses SHA-256 as hash function in cert signature.
    sign_alg = SSL_SIGN_ECDSA_SECP256R1_SHA256;
  } break;
  case EVP_PKEY_RSA: {
    // We require RSA certificates with 2048-bit or larger keys.
    const RSA* rsa_public_key = EVP_PKEY_get0_RSA(public_key);
    // Since we checked the key type above, this should be valid.
    ASSERT(rsa_public_key != nullptr);
    const unsigned rsa_key_length = RSA_size(rsa_public_key);
#ifdef BORINGSSL_FIPS
    if (rsa_key_length != 2048 / 8 && rsa_key_length != 3072 / 8 && rsa_key_length != 4096 / 8) {
      *error_details = "Invalid leaf cert, only RSA certificates with 2048-bit, 3072-bit or "
                       "4096-bit keys are supported in FIPS mode";
      break;
    }
#else
    if (rsa_key_length < 2048 / 8) {
      *error_details =
          "Invalid leaf cert, only RSA certificates with 2048-bit or larger keys are supported";
      break;
    }
#endif
    sign_alg = SSL_SIGN_RSA_PSS_RSAE_SHA256;
  } break;
  default:
    *error_details = "Invalid leaf cert, only RSA and ECDSA certificates are supported";
  }
  return sign_alg;
}

} // namespace Quic
} // namespace Envoy
