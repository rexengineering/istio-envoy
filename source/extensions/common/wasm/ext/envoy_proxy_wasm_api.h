// NOLINT(namespace-envoy)
#pragma once

class EnvoyContextBase {
public:
  virtual ~EnvoyContextBase() = default;

  virtual void onResolveDns(uint32_t /* token */, uint32_t /* result_size */) {}
  virtual void onStat(uint32_t /* result_size */) {}
};

class EnvoyRootContext : public RootContext, public EnvoyContextBase {
public:
  EnvoyRootContext(uint32_t id, StringView root_id) : RootContext(id, root_id) {}
  ~EnvoyRootContext() override = default;
};

class EnvoyContext : public Context, public EnvoyContextBase {
public:
  EnvoyContext(uint32_t id, RootContext* root) : Context(id, root) {}
  ~EnvoyContext() override = default;
};

struct DnsResult {
  uint32_t ttl_seconds;
  std::string address;
};

struct CounterResult {
  uint64_t delta;
  std::string name;
  uint64_t value;
};

struct GaugeResult {
  uint64_t value;
  std::string name;
};

struct StatResult {
  std::vector<CounterResult> counters;
  std::vector<GaugeResult> gauges;
};

inline std::vector<DnsResult> parseDnsResults(StringView data) {
  if (data.size() < 4) {
    return {};
  }
  const uint32_t* pn = reinterpret_cast<const uint32_t*>(data.data());
  uint32_t n = *pn++;
  std::vector<DnsResult> results;
  results.resize(n);
  const char* pa = data.data() + (1 + n) * sizeof(uint32_t); // skip n + n TTLs
  for (uint32_t i = 0; i < n; i++) {
    auto& e = results[i];
    e.ttl_seconds = *pn++;
    auto alen = strlen(pa);
    e.address.assign(pa, alen);
    pa += alen + 1;
  }
  return results;
}

inline StatResult parseStatResults(StringView data) {
  StatResult results;
  uint32_t data_len = 0;
  while (data_len < data.length()) {
    const uint32_t* n = reinterpret_cast<const uint32_t*>(data.data() + data_len);
    uint32_t block_size = *n++;
    uint32_t block_type = *n++;
    uint32_t num_stats = *n++;
    if (block_type == 1) { // counter
      std::vector<CounterResult> counters(num_stats);
      uint32_t stat_index = data_len + 3 * sizeof(uint32_t);
      for (uint32_t i = 0; i < num_stats; i++) {
        const uint32_t* stat_name = reinterpret_cast<const uint32_t*>(data.data() + stat_index);
        uint32_t name_len = *stat_name;
        stat_index += sizeof(uint32_t);

        auto& e = counters[i];
        e.name.assign(data.data() + stat_index, name_len);
        stat_index += name_len + sizeof(uint64_t);

        const uint64_t* stat_vals = reinterpret_cast<const uint64_t*>(data.data() + stat_index);
        e.value = *stat_vals++;
        e.delta = *stat_vals++;

        stat_index += 2 * sizeof(uint64_t);
      }
      results.counters = counters;
    } else if (block_type == 2) { // gauge
      std::vector<GaugeResult> gauges(num_stats);
      uint32_t stat_index = data_len + 3 * sizeof(uint32_t);
      for (uint32_t i = 0; i < num_stats; i++) {
        const uint32_t* stat_name = reinterpret_cast<const uint32_t*>(data.data() + stat_index);
        uint32_t name_len = *stat_name;
        stat_index += sizeof(uint32_t);

        auto& e = gauges[i];
        e.name.assign(data.data() + stat_index, name_len);
        stat_index += name_len + sizeof(uint64_t);

        const uint64_t* stat_vals = reinterpret_cast<const uint64_t*>(data.data() + stat_index);
        e.value = *stat_vals++;

        stat_index += sizeof(uint64_t);
      }
      results.gauges = gauges;
    }
    data_len += block_size;
  }

  return results;
}
