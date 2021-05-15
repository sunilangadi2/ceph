// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "crimson/admin/osd_admin.h"
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <seastar/core/do_with.hh>
#include <seastar/core/future.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/scollectd_api.hh>

#include "common/config.h"
#include "crimson/admin/admin_socket.h"
#include "crimson/common/log.h"
#include "crimson/osd/exceptions.h"
#include "crimson/osd/osd.h"

using crimson::osd::OSD;
using namespace crimson::common;

namespace crimson::admin {

using crimson::common::local_conf;

template <class Hook, class... Args>
std::unique_ptr<AdminSocketHook> make_asok_hook(Args&&... args)
{
  return std::make_unique<Hook>(std::forward<Args>(args)...);
}

/**
 * An OSD admin hook: OSD status
 */
class OsdStatusHook : public AdminSocketHook {
public:
  explicit OsdStatusHook(const crimson::osd::OSD& osd) :
    AdminSocketHook{"status", "", "OSD status"},
    osd(osd)
  {}
  seastar::future<tell_result_t> call(const cmdmap_t&,
				      std::string_view format,
				      ceph::bufferlist&& input) const final
  {
    unique_ptr<Formatter> f{Formatter::create(format, "json-pretty", "json-pretty")};
    f->open_object_section("status");
    osd.dump_status(f.get());
    f->close_section();
    return seastar::make_ready_future<tell_result_t>(std::move(f));
  }
private:
  const crimson::osd::OSD& osd;
};
template std::unique_ptr<AdminSocketHook>
make_asok_hook<OsdStatusHook>(const crimson::osd::OSD& osd);

/**
 * An OSD admin hook: send beacon
 */
class SendBeaconHook : public AdminSocketHook {
public:
  explicit SendBeaconHook(crimson::osd::OSD& osd) :
    AdminSocketHook{"send_beacon",
		    "",
		    "send OSD beacon to mon immediately"},
    osd(osd)
  {}
  seastar::future<tell_result_t> call(const cmdmap_t&,
				      std::string_view format,
				      ceph::bufferlist&& input) const final
  {
    return osd.send_beacon().then([] {
      return seastar::make_ready_future<tell_result_t>();
    });
  }
private:
  crimson::osd::OSD& osd;
};
template std::unique_ptr<AdminSocketHook>
make_asok_hook<SendBeaconHook>(crimson::osd::OSD& osd);

/**
 * send the latest pg stats to mgr
 */
class FlushPgStatsHook : public AdminSocketHook {
public:
  explicit FlushPgStatsHook(crimson::osd::OSD& osd) :
    AdminSocketHook("flush_pg_stats",
		    "",
		    "flush pg stats"),
    osd{osd}
  {}
  seastar::future<tell_result_t> call(const cmdmap_t&,
				      std::string_view format,
				      ceph::bufferlist&& input) const final
  {
    uint64_t seq = osd.send_pg_stats();
    unique_ptr<Formatter> f{Formatter::create(format, "json-pretty", "json-pretty")};
    f->dump_unsigned("stat_seq", seq);
    return seastar::make_ready_future<tell_result_t>(std::move(f));
  }

private:
  crimson::osd::OSD& osd;
};
template std::unique_ptr<AdminSocketHook> make_asok_hook<FlushPgStatsHook>(crimson::osd::OSD& osd);

/// dump the history of PGs' peering state
class DumpPGStateHistory final: public AdminSocketHook {
public:
  explicit DumpPGStateHistory(const crimson::osd::OSD &osd) :
    AdminSocketHook{"dump_pgstate_history",
                    "",
                    "dump history of PGs' peering state"},
    osd{osd}
  {}
  seastar::future<tell_result_t> call(const cmdmap_t&,
                                      std::string_view format,
                                      ceph::bufferlist&& input) const final
  {
    std::unique_ptr<Formatter> f{Formatter::create(format,
                                                   "json-pretty",
                                                   "json-pretty")};
    f->open_object_section("pgstate_history");
    osd.dump_pg_state_history(f.get());
    f->close_section();
    return seastar::make_ready_future<tell_result_t>(std::move(f));
  }
private:
  const crimson::osd::OSD& osd;
};
template std::unique_ptr<AdminSocketHook> make_asok_hook<DumpPGStateHistory>(const crimson::osd::OSD& osd);

//dump the contents of perfcounters in osd and store
class DumpPerfCountersHook final: public AdminSocketHook {
public:
  explicit DumpPerfCountersHook(const crimson::osd::OSD &osd) :
    AdminSocketHook{"perfcounters_dump",
                    "name=logger,type=CephString,req=false "
                    "name=counter,type=CephString,req=false",
                    "dump perfcounters in osd and store"},
    osd{osd}
  {}
  seastar::future<tell_result_t> call(const cmdmap_t& cmdmap,
                                      std::string_view format,
                                      ceph::bufferlist&& input) const final
  {
    std::unique_ptr<Formatter> f{Formatter::create(format,
                                                   "json-pretty",
                                                   "json-pretty")};
    std::string logger;
    std::string counter;
    cmd_getval(cmdmap, "logger", logger);
    cmd_getval(cmdmap, "counter", counter);

    crimson::common::local_perf_coll().dump_formatted(f.get(), false, logger, counter);
    return seastar::make_ready_future<tell_result_t>(std::move(f));
  }
private:
  const crimson::osd::OSD& osd;
};
template std::unique_ptr<AdminSocketHook> make_asok_hook<DumpPerfCountersHook>(const crimson::osd::OSD& osd);



/**
 * A CephContext admin hook: calling assert (if allowed by
 * 'debug_asok_assert_abort')
 */
class AssertAlwaysHook : public AdminSocketHook {
public:
  AssertAlwaysHook()  :
    AdminSocketHook{"assert",
		    "",
		    "asserts"}
  {}
  seastar::future<tell_result_t> call(const cmdmap_t&,
				      std::string_view format,
				      ceph::bufferlist&& input) const final
  {
    if (local_conf().get_val<bool>("debug_asok_assert_abort")) {
      ceph_assert_always(0);
      return seastar::make_ready_future<tell_result_t>();
    } else {
      return seastar::make_ready_future<tell_result_t>(
        tell_result_t{-EPERM, "configuration set to disallow asok assert"});
    }
  }
};
template std::unique_ptr<AdminSocketHook> make_asok_hook<AssertAlwaysHook>();

/**
* A Seastar admin hook: fetching the values of configured metrics
*/
class SeastarMetricsHook : public AdminSocketHook {
public:
 SeastarMetricsHook()  :
   AdminSocketHook("perf dump_seastar",
      "",
      "dump current configured seastar metrics and their values")
 {}
 seastar::future<tell_result_t> call(const cmdmap_t& cmdmap,
             std::string_view format,
             ceph::bufferlist&& input) const final
 {
   std::unique_ptr<Formatter> f{Formatter::create(format, "json-pretty", "json-pretty")};
   f->open_object_section("perf_dump_seastar");
   for (const auto& [full_name, metric_family]: seastar::scollectd::get_value_map()) {
     for (const auto& [labels, metric] : metric_family) {
       if (metric && metric->is_enabled()) {
	 dump_metric_value(f.get(), full_name, *metric);
       }
     }
   }
   f->close_section();
   return seastar::make_ready_future<tell_result_t>(std::move(f));
 }
private:
  using registered_metric = seastar::metrics::impl::registered_metric;
  using data_type = seastar::metrics::impl::data_type;

  static void dump_metric_value(Formatter* f,
                                string_view full_name,
                                const registered_metric& metric)
  {
    switch (auto v = metric(); v.type()) {
    case data_type::GAUGE:
      f->dump_float(full_name, v.d());
      break;
    case data_type::COUNTER:
      [[fallthrough]];
    case data_type::DERIVE:
      f->dump_unsigned(full_name, v.ui());
      break;
    case data_type::HISTOGRAM: {
      f->open_object_section(full_name);
      auto&& h = v.get_histogram();
      f->dump_float("sum", h.sample_sum);
      f->dump_unsigned("count", h.sample_count);
      f->open_array_section("buckets");
      for (auto i : h.buckets) {
        f->open_object_section("bucket");
        f->dump_float("le", i.upper_bound);
        f->dump_unsigned("count", i.count);
        f->close_section(); // "bucket"
      }
      {
        f->open_object_section("bucket");
        f->dump_string("le", "+Inf");
        f->dump_unsigned("count", h.sample_count);
        f->close_section();
      }
      f->close_section(); // "buckets"
      f->close_section(); // full_name
    }
      break;
    default:
      std::abort();
      break;
    }
  }
};
template std::unique_ptr<AdminSocketHook> make_asok_hook<SeastarMetricsHook>();

} // namespace crimson::admin
