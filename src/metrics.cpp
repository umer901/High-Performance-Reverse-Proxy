#include "hprp/metrics.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace hprp {
namespace {

std::uint64_t read_proc_self_statm_rss_bytes() {
  std::ifstream in("/proc/self/statm");
  std::uint64_t size_pages = 0;
  std::uint64_t resident_pages = 0;
  in >> size_pages >> resident_pages;
  return resident_pages * 4096;
}

double read_proc_self_cpu_seconds() {
  std::ifstream in("/proc/self/stat");
  std::string token;
  for (int i = 0; i < 13 && in >> token; ++i) {
  }
  long utime_ticks = 0;
  long stime_ticks = 0;
  in >> utime_ticks >> stime_ticks;
  constexpr double ticks_per_second = 100.0;
  return static_cast<double>(utime_ticks + stime_ticks) / ticks_per_second;
}

void append_counter(std::ostringstream &out, const char *name, const std::vector<Metrics::ErrorCounter> &items,
                    const char *label_name) {
  for (const auto &item : items) {
    out << name << "{" << label_name << "=\"" << item.label << "\"} " << item.value << "\n";
  }
}

} // namespace

Metrics::Metrics(std::vector<std::string> backend_names) : backend_names_(std::move(backend_names)) {
  requests_by_backend_status_.resize(backend_names_.size());
  backend_active_.assign(backend_names_.size(), 0);
  backend_health_.assign(backend_names_.size(), 1);
  queue_depth_.assign(backend_names_.size(), 0);
}

void Metrics::inc_request(std::size_t backend, int status_class) {
  std::lock_guard lock(mutex_);
  if (backend < requests_by_backend_status_.size() && status_class >= 1 && status_class <= 5) {
    ++requests_by_backend_status_[backend][static_cast<std::size_t>(status_class - 1)];
  }
}

void Metrics::observe_latency(std::chrono::nanoseconds latency) {
  const auto seconds = std::chrono::duration<double>(latency).count();
  std::lock_guard lock(mutex_);
  bool bucketed = false;
  for (std::size_t i = 0; i < kLatencyBuckets.size(); ++i) {
    if (seconds <= kLatencyBuckets[i]) {
      ++latency_buckets_[i];
      bucketed = true;
      break;
    }
  }
  if (!bucketed) {
    ++latency_buckets_.back();
  }
  latency_sum_seconds_ += seconds;
  ++latency_count_;
}

void Metrics::inc_error(const std::string &type) {
  std::lock_guard lock(mutex_);
  for (auto &item : errors_) {
    if (item.label == type) {
      ++item.value;
      return;
    }
  }
  errors_.push_back({type, 1});
}

void Metrics::inc_timeout(const std::string &phase) {
  std::lock_guard lock(mutex_);
  for (auto &item : timeouts_) {
    if (item.label == phase) {
      ++item.value;
      return;
    }
  }
  timeouts_.push_back({phase, 1});
}

void Metrics::set_active_connections(std::int64_t value) {
  std::lock_guard lock(mutex_);
  active_connections_ = value;
}

void Metrics::set_backend_active(std::size_t backend, std::int64_t value) {
  std::lock_guard lock(mutex_);
  if (backend < backend_active_.size()) {
    backend_active_[backend] = value;
  }
}

void Metrics::set_backend_health(std::size_t backend, bool healthy) {
  std::lock_guard lock(mutex_);
  if (backend < backend_health_.size()) {
    backend_health_[backend] = healthy ? 1 : 0;
  }
}

void Metrics::set_queue_depth(std::size_t backend, std::int64_t value) {
  std::lock_guard lock(mutex_);
  if (backend < queue_depth_.size()) {
    queue_depth_[backend] = value;
  }
}

std::string Metrics::render_prometheus() const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "# HELP hprp_requests_total Total proxied requests by backend and status class.\n";
  out << "# TYPE hprp_requests_total counter\n";
  for (std::size_t i = 0; i < backend_names_.size(); ++i) {
    for (std::size_t cls = 0; cls < requests_by_backend_status_[i].size(); ++cls) {
      out << "hprp_requests_total{backend=\"" << backend_names_[i] << "\",status_class=\"" << (cls + 1)
          << "xx\"} " << requests_by_backend_status_[i][cls] << "\n";
    }
  }

  out << "# HELP hprp_request_duration_seconds Request latency histogram.\n";
  out << "# TYPE hprp_request_duration_seconds histogram\n";
  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < kLatencyBuckets.size(); ++i) {
    cumulative += latency_buckets_[i];
    out << "hprp_request_duration_seconds_bucket{le=\"" << kLatencyBuckets[i] << "\"} " << cumulative << "\n";
  }
  cumulative += latency_buckets_.back();
  out << "hprp_request_duration_seconds_bucket{le=\"+Inf\"} " << cumulative << "\n";
  out << "hprp_request_duration_seconds_sum " << latency_sum_seconds_ << "\n";
  out << "hprp_request_duration_seconds_count " << latency_count_ << "\n";

  out << "# HELP hprp_active_connections Active client connections.\n";
  out << "# TYPE hprp_active_connections gauge\n";
  out << "hprp_active_connections " << active_connections_ << "\n";

  for (std::size_t i = 0; i < backend_names_.size(); ++i) {
    out << "hprp_backend_active_connections{backend=\"" << backend_names_[i] << "\"} " << backend_active_[i] << "\n";
    out << "hprp_backend_healthy{backend=\"" << backend_names_[i] << "\"} " << backend_health_[i] << "\n";
    out << "hprp_queue_depth{backend=\"" << backend_names_[i] << "\"} " << queue_depth_[i] << "\n";
  }

  append_counter(out, "hprp_errors_total", errors_, "type");
  append_counter(out, "hprp_timeouts_total", timeouts_, "phase");

  out << "process_cpu_seconds_total " << read_proc_self_cpu_seconds() << "\n";
  out << "process_resident_memory_bytes " << read_proc_self_statm_rss_bytes() << "\n";
  return out.str();
}

} // namespace hprp
