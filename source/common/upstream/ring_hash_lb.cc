#include "common/upstream/ring_hash_lb.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "common/common/assert.h"
#include "common/upstream/load_balancer_impl.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

RingHashLoadBalancer::RingHashLoadBalancer(
    const PrioritySet& priority_set, ClusterStats& stats, Stats::Scope& scope,
    Runtime::Loader& runtime, Runtime::RandomGenerator& random,
    const absl::optional<envoy::api::v2::Cluster::RingHashLbConfig>& config,
    const envoy::api::v2::Cluster::CommonLbConfig& common_config)
    : ThreadAwareLoadBalancerBase(priority_set, stats, runtime, random, common_config),
      config_(config), scope_(scope.createScope("ring_hash_lb.")), stats_(generateStats(*scope_)) {}

RingHashLoadBalancerStats RingHashLoadBalancer::generateStats(Stats::Scope& scope) {
  return {ALL_RING_HASH_LOAD_BALANCER_STATS(POOL_GAUGE(scope))};
}

HostConstSharedPtr RingHashLoadBalancer::Ring::chooseHost(uint64_t h) const {
  if (ring_.empty()) {
    return nullptr;
  }

  // Ported from https://github.com/RJ/ketama/blob/master/libketama/ketama.c (ketama_get_server)
  // I've generally kept the variable names to make the code easier to compare.
  // NOTE: The algorithm depends on using signed integers for lowp, midp, and highp. Do not
  //       change them!
  int64_t lowp = 0;
  int64_t highp = ring_.size();
  while (true) {
    int64_t midp = (lowp + highp) / 2;

    if (midp == static_cast<int64_t>(ring_.size())) {
      return ring_[0].host_;
    }

    uint64_t midval = ring_[midp].hash_;
    uint64_t midval1 = midp == 0 ? 0 : ring_[midp - 1].hash_;

    if (h <= midval && h > midval1) {
      return ring_[midp].host_;
    }

    if (midval < h) {
      lowp = midp + 1;
    } else {
      highp = midp - 1;
    }

    if (lowp > highp) {
      return ring_[0].host_;
    }
  }
}

// TODO(mergeconflict): Determine whether the locality weights we get here are already adjusted
//                      for partial availability (as in HostSetImpl::effectiveLocalityWeight), and
//                      promote this into ThreadAwareLoadBalancerBase or HashingLoadBalancer so
//                      Maglev LB can use it (see #5982).
namespace {

typedef std::vector<std::pair<HostConstSharedPtr, double>> NormalizedHostWeightVector;

void normalizeHostWeights(const HostVector& hosts, double normalized_locality_weight,
                          NormalizedHostWeightVector& normalized_weights,
                          double& min_normalized_weight) {
  uint32_t sum = 0;
  for (const auto& host : hosts) {
    sum += host->weight();
  }

  for (const auto& host : hosts) {
    const double weight = host->weight() * normalized_locality_weight / sum;
    normalized_weights.push_back({host, weight});
    min_normalized_weight = std::min(min_normalized_weight, weight);
  }
}

void normalizeLocalityWeights(const HostsPerLocality& hosts_per_locality,
                              const LocalityWeights& locality_weights,
                              NormalizedHostWeightVector& normalized_weights,
                              double& min_normalized_weight) {
  ASSERT(locality_weights.size() == hosts_per_locality.get().size());

  uint32_t sum = 0;
  for (const auto weight : locality_weights) {
    sum += weight;
  }

  // Locality weights (unlike host weights) may be 0. If _all_ locality weights were 0, bail out.
  if (sum == 0) {
    return;
  }

  // Compute normalized weights for all hosts in each locality. If a locality was assigned zero
  // weight, all hosts in that locality will be skipped.
  for (LocalityWeights::size_type i = 0; i < locality_weights.size(); ++i) {
    if (locality_weights[i] != 0) {
      const HostVector& hosts = hosts_per_locality.get()[i];
      const double normalized_locality_weight = static_cast<double>(locality_weights[i]) / sum;
      normalizeHostWeights(hosts, normalized_locality_weight, normalized_weights,
                           min_normalized_weight);
    }
  }
}

void normalizeWeights(const HostSet& host_set, bool in_panic,
                      NormalizedHostWeightVector& normalized_weights,
                      double& min_normalized_weight) {
  if (host_set.localityWeights() == nullptr || host_set.localityWeights()->empty()) {
    // If we're not dealing with locality weights, just normalize weights for the flat set of hosts.
    const auto& hosts = in_panic ? host_set.hosts() : host_set.healthyHosts();
    normalizeHostWeights(hosts, 1.0, normalized_weights, min_normalized_weight);
  } else {
    // Otherwise, normalize weights across all localities.
    const auto& hosts_per_locality =
        in_panic ? host_set.hostsPerLocality() : host_set.healthyHostsPerLocality();
    normalizeLocalityWeights(hosts_per_locality, *(host_set.localityWeights()), normalized_weights,
                             min_normalized_weight);
  }
}

} // namespace

using HashFunction = envoy::api::v2::Cluster_RingHashLbConfig_HashFunction;
RingHashLoadBalancer::Ring::Ring(
    const HostSet& host_set, bool in_panic,
    const absl::optional<envoy::api::v2::Cluster::RingHashLbConfig>& config,
    RingHashLoadBalancerStats& stats)
    : stats_(stats) {
  ENVOY_LOG(trace, "ring hash: building ring");

  const uint64_t min_ring_size =
      config
          ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.value(), minimum_ring_size, DefaultMinRingSize)
          : DefaultMinRingSize;
  const uint64_t max_ring_size =
      config
          ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.value(), maximum_ring_size, DefaultMaxRingSize)
          : DefaultMaxRingSize;

  // Sanity-check ring size bounds.
  if (min_ring_size > max_ring_size) {
    throw EnvoyException(fmt::format("ring hash: minimum_ring_size ({}) > maximum_ring_size ({})",
                                     min_ring_size, max_ring_size));
  }

  // Normalize weights, such that the sum of all weights = 1.
  NormalizedHostWeightVector normalized_weights;
  double min_normalized_weight = 1.0;
  normalizeWeights(host_set, in_panic, normalized_weights, min_normalized_weight);

  // We can't do anything sensible with no hosts.
  if (normalized_weights.empty()) {
    return;
  }

  // Scale up the number of hashes per host such that the least-weighted host gets a whole number
  // of hashes on the ring. Other hosts might not end up with whole numbers, and that's fine (the
  // ring-building algorithm below can handle this). This preserves the original implementation's
  // behavior: when weights aren't provided, all hosts should get an equal number of hashes. In the
  // case where this number exceeds the max_ring_size, it's scaled back down to fit.
  const double scale =
      std::min(std::ceil(min_normalized_weight * min_ring_size) / min_normalized_weight,
               static_cast<double>(max_ring_size));

  // Reserve memory for the entire ring up front.
  const uint64_t ring_size = std::ceil(scale);
  ring_.reserve(ring_size);

  const bool use_std_hash =
      config ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.value().deprecated_v1(), use_std_hash, false)
             : false;

  const HashFunction hash_function =
      config ? config.value().hash_function()
             : HashFunction::Cluster_RingHashLbConfig_HashFunction_XX_HASH;

  // Populate the hash ring by walking through the (host, weight) entries in the normalized_weights
  // map, and generating (scale * weight) hashes for each host. Since these aren't necessarily whole
  // numbers, we maintain running sums -- current_hashes and target_hashes -- which allows us to
  // populate the ring in a mostly stable way.
  //
  // For example, suppose we have 4 hosts, each with a normalized weight of 0.25, and a scale of
  // 6.0 (because the max_ring_size is 6). That means we want to generate 1.5 hashes per host.
  // We start the outer loop with current_hashes = 0 and target_hashes = 0.
  //   - For the first host, we set target_hashes = 1.5. After one run of the inner loop,
  //     current_hashes = 1. After another run, current_hashes = 2, so the inner loop ends.
  //   - For the second host, target_hashes becomes 3.0, and current_hashes is 2 from before.
  //     After only one run of the inner loop, current_hashes = 3, so the inner loop ends.
  //   - Likewise, the third host gets two hashes, and the fourth host gets one hash.
  //
  // For stats reporting, keep track of the minimum and maximum actual number of hashes per host.
  // Users should hopefully pay attention to these numbers and alert if min_hashes_per_host is too
  // low, since that implies an inaccurate request distribution.
  char hash_key_buffer[196];
  double current_hashes = 0.0;
  double target_hashes = 0.0;
  uint64_t min_hashes_per_host = ring_size;
  uint64_t max_hashes_per_host = 0;
  for (const auto& entry : normalized_weights) {
    const auto& host = entry.first;
    const std::string& address_string = host->address()->asString();
    uint64_t offset_start = address_string.size();

    // Currently, we support both IP and UDS addresses. The UDS max path length is ~108 on all Unix
    // platforms that I know of. Given that, we can use a 196 char buffer which is plenty of room
    // for UDS, '_', and up to 21 characters for the node ID. To be on the super safe side, there
    // is a RELEASE_ASSERT here that checks this, in case someone in the future adds some type of
    // new address that is larger, or runs on a platform where UDS is larger. I don't think it's
    // worth the defensive coding to deal with the heap allocation case (e.g. via
    // absl::InlinedVector) at the current time.
    RELEASE_ASSERT(
        address_string.size() + 1 + StringUtil::MIN_ITOA_OUT_LEN <= sizeof(hash_key_buffer), "");
    memcpy(hash_key_buffer, address_string.c_str(), offset_start);
    hash_key_buffer[offset_start++] = '_';

    // As noted above: maintain current_hashes and target_hashes as running sums across the entire
    // host set. `i` is needed only to construct the hash key, and tally min/max hashes per host.
    target_hashes += scale * entry.second;
    uint64_t i = 0;
    while (current_hashes < target_hashes) {
      const uint64_t total_hash_key_len =
          offset_start +
          StringUtil::itoa(hash_key_buffer + offset_start, StringUtil::MIN_ITOA_OUT_LEN, i);
      absl::string_view hash_key(hash_key_buffer, total_hash_key_len);

      // Sadly std::hash provides no mechanism for hashing arbitrary bytes so we must copy here.
      // xxHash is done without copies.
      const uint64_t hash =
          use_std_hash
              ? std::hash<std::string>()(std::string(hash_key))
              : (hash_function == HashFunction::Cluster_RingHashLbConfig_HashFunction_MURMUR_HASH_2)
                    ? MurmurHash::murmurHash2_64(hash_key, MurmurHash::STD_HASH_SEED)
                    : HashUtil::xxHash64(hash_key);

      ENVOY_LOG(trace, "ring hash: hash_key={} hash={}", hash_key.data(), hash);
      ring_.push_back({hash, host});
      ++i;
      ++current_hashes;
    }
    min_hashes_per_host = std::min(i, min_hashes_per_host);
    max_hashes_per_host = std::max(i, max_hashes_per_host);
  }

  std::sort(ring_.begin(), ring_.end(), [](const RingEntry& lhs, const RingEntry& rhs) -> bool {
    return lhs.hash_ < rhs.hash_;
  });
  if (ENVOY_LOG_CHECK_LEVEL(trace)) {
    for (const auto& entry : ring_) {
      ENVOY_LOG(trace, "ring hash: host={} hash={}", entry.host_->address()->asString(),
                entry.hash_);
    }
  }

  stats_.size_.set(ring_size);
  stats_.min_hashes_per_host_.set(min_hashes_per_host);
  stats_.max_hashes_per_host_.set(max_hashes_per_host);
}

} // namespace Upstream
} // namespace Envoy
