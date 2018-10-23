/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tools/trace_to_text/trace_to_summary.h"

#include <inttypes.h>

#include <stdio.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <istream>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <utility>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/traced/sys_stats_counters.h"
#include "tools/trace_to_text/ftrace_inode_handler.h"
#include "tools/trace_to_text/process_formatter.h"
#include "tools/trace_to_text/proto_full_utils.h"
#include "tools/trace_to_text/utils.h"

#include "perfetto/trace/ftrace/ftrace_event.pb.h"
#include "perfetto/trace/ftrace/ftrace_stats.pb.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_to_text {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FileDescriptor;
using google::protobuf::Message;
using google::protobuf::TextFormat;
using google::protobuf::compiler::DiskSourceTree;
using google::protobuf::compiler::Importer;
using google::protobuf::io::OstreamOutputStream;

using protos::FtraceEvent;
using protos::FtraceEventBundle;
using protos::InodeFileMap;
using protos::PrintFtraceEvent;
using protos::ProcessTree;
using protos::Trace;
using protos::TracePacket;
using protos::FtraceStats;
using protos::FtraceStats_Phase_START_OF_TRACE;
using protos::FtraceStats_Phase_END_OF_TRACE;
using protos::SysStats;
using Entry = protos::InodeFileMap::Entry;
using Process = protos::ProcessTree::Process;

void PrintFtraceTrack(std::ostream* output,
                      const uint64_t& start,
                      const uint64_t& end,
                      const std::multiset<uint64_t>& ftrace_timestamps) {
  constexpr char kFtraceTrackName[] = "ftrace ";
  size_t width = GetTerminalWidth();
  size_t bucket_count = width - strlen(kFtraceTrackName);
  size_t bucket_size = static_cast<size_t>(end - start) / bucket_count;
  size_t max = 0;
  std::vector<size_t> buckets(bucket_count);
  for (size_t i = 0; i < bucket_count; i++) {
    auto low = ftrace_timestamps.lower_bound(i * bucket_size + start);
    auto high = ftrace_timestamps.upper_bound((i + 1) * bucket_size + start);
    buckets[i] = static_cast<size_t>(std::distance(low, high));
    max = std::max(max, buckets[i]);
  }

  std::vector<std::string> out =
      std::vector<std::string>({" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇"});
  *output << "-------------------- " << kFtraceTrackName
          << "--------------------\n";
  char line[2048];
  for (size_t i = 0; i < bucket_count; i++) {
    sprintf(
        line, "%s",
        out[std::min(buckets[i] / (max / out.size()), out.size() - 1)].c_str());
    *output << std::string(line);
  }
  *output << "\n\n";
}

void PrintFtraceStats(std::ostream* output,
                      uint64_t overwrite_count,
                      std::map<FtraceEvent::EventCase, uint64_t> event_counts,
                      const FtraceStats& before_stats,
                      const FtraceStats& after_stats,
                      bool compact_output) {
  if (!compact_output)
    *output << "--------------------Ftrace Stats-------------------\n";

  char line[2048];
  if (compact_output) {
    sprintf(line, "ftrace_overwrite_count,%" PRIu64 "\n", overwrite_count);
  } else {
    sprintf(line, "Events overwritten: %" PRIu64 "\n", overwrite_count);
  }
  *output << std::string(line);

  DiskSourceTree dst;
  dst.MapPath("perfetto", "protos/perfetto");
  perfetto::trace_to_text::MultiFileErrorCollectorImpl mfe;
  Importer importer(&dst, &mfe);
  const FileDescriptor* parsed_file =
      importer.Import("perfetto/trace/ftrace/ftrace_event.proto");

  DynamicMessageFactory dmf;
  const Descriptor* ftrace_descriptor = parsed_file->message_type(0);
  for (const auto& event_to_count : event_counts) {
    const std::string& event_name =
        ftrace_descriptor->FindFieldByNumber(event_to_count.first)->name();
    uint64_t count = event_to_count.second;
    if (compact_output) {
      sprintf(line, "%s,%" PRIu64 "\n", event_name.c_str(), count);
    } else {
      sprintf(line, "%s count: %" PRIu64 "\n", event_name.c_str(), count);
    }
    *output << std::string(line);
  }

  uint64_t before_total_overrun = 0;
  uint64_t after_total_overrun = 0;
  for (const auto& cpu_stats : before_stats.cpu_stats()) {
    before_total_overrun += cpu_stats.overrun();
  }
  for (const auto& cpu_stats : after_stats.cpu_stats()) {
    after_total_overrun += cpu_stats.overrun();
  }

  if (compact_output) {
    sprintf(line, "total_overrun,%" PRIu64 "\n",
            after_total_overrun - before_total_overrun);
  } else {
    sprintf(line, "total_overrun: %" PRIu64 " (= %" PRIu64 " - %" PRIu64 ")\n",
            after_total_overrun - before_total_overrun, after_total_overrun,
            before_total_overrun);
  }
  *output << std::string(line);

  if (!compact_output)
    *output << "\n";
}

void PrintInodeStats(std::ostream* output,
                     const std::set<uint64_t>& ftrace_inodes,
                     const uint64_t& ftrace_inode_count,
                     const std::set<uint64_t>& resolved_map_inodes,
                     const std::set<uint64_t>& resolved_scan_inodes,
                     bool compact_output) {
  if (!compact_output)
    *output << "--------------------Inode Stats-------------------\n";

  char line[2048];
  if (compact_output) {
    sprintf(line, "events_inodes,%" PRIu64 "\n", ftrace_inode_count);
  } else {
    sprintf(line, "Events with inodes: %" PRIu64 "\n", ftrace_inode_count);
  }
  *output << std::string(line);

  if (compact_output) {
    sprintf(line, "events_unique_inodes,%zu\n", ftrace_inodes.size());
  } else {
    sprintf(line, "Unique inodes from events: %zu\n", ftrace_inodes.size());
  }
  *output << std::string(line);

  if (compact_output) {
    sprintf(line, "resolved_inodes_static,%zu\n", resolved_map_inodes.size());
  } else {
    sprintf(line, "Resolved inodes from static map: %zu\n",
            resolved_map_inodes.size());
  }
  *output << std::string(line);

  if (compact_output) {
    sprintf(line, "resolved_inodes_scan_cache,%zu\n",
            resolved_scan_inodes.size());
  } else {
    sprintf(line, "Resolved inodes from scan and cache: %zu\n",
            resolved_scan_inodes.size());
  }
  *output << std::string(line);

  std::set<uint64_t> resolved_inodes;
  set_union(resolved_map_inodes.begin(), resolved_map_inodes.end(),
            resolved_scan_inodes.begin(), resolved_scan_inodes.end(),
            std::inserter(resolved_inodes, resolved_inodes.begin()));

  if (compact_output) {
    sprintf(line, "total_resolved_inodes,%zu\n", resolved_inodes.size());
  } else {
    sprintf(line, "Total resolved inodes: %zu\n", resolved_inodes.size());
  }
  *output << std::string(line);

  std::set<uint64_t> intersect;
  set_intersection(resolved_inodes.begin(), resolved_inodes.end(),
                   ftrace_inodes.begin(), ftrace_inodes.end(),
                   std::inserter(intersect, intersect.begin()));

  size_t unresolved_inodes = ftrace_inodes.size() - intersect.size();
  if (compact_output) {
    sprintf(line, "unresolved_inodes,%zu\n", unresolved_inodes);
  } else {
    sprintf(line, "Unresolved inodes: %zu\n", unresolved_inodes);
  }
  *output << std::string(line);

  size_t unexpected_inodes = resolved_inodes.size() - intersect.size();
  if (compact_output) {
    sprintf(line, "unexpected_inodes_fs,%zu\n", unexpected_inodes);
  } else {
    sprintf(line, "Unexpected inodes from filesystem: %zu\n",
            unexpected_inodes);
  }
  *output << std::string(line);

  if (!compact_output)
    *output << "\n";
}

void PrintProcessStats(std::ostream* output,
                       const std::set<pid_t>& tids_in_tree,
                       const std::set<pid_t>& tids_in_events,
                       bool compact_output) {
  if (!compact_output)
    *output << "----------------Process Tree Stats----------------\n";

  char tid[2048];
  if (compact_output) {
    sprintf(tid, "unique_thread_process,%zu\n", tids_in_tree.size());
  } else {
    sprintf(tid, "Unique thread ids in process tree: %zu\n",
            tids_in_tree.size());
  }
  *output << std::string(tid);

  char tid_event[2048];
  if (compact_output) {
    sprintf(tid_event, "unique_thread_ftrace,%zu\n", tids_in_events.size());
  } else {
    sprintf(tid_event, "Unique thread ids in ftrace events: %zu\n",
            tids_in_events.size());
  }
  *output << std::string(tid_event);

  std::set<pid_t> intersect;
  set_intersection(tids_in_tree.begin(), tids_in_tree.end(),
                   tids_in_events.begin(), tids_in_events.end(),
                   std::inserter(intersect, intersect.begin()));

  char matching[2048];
  size_t thread_id_process_info =
      (intersect.size() * 100) / tids_in_events.size();
  if (compact_output) {
    sprintf(matching,
            "tids_with_pinfo,%zu\ntids,%zu\ntids_with_pinfo_percentage,%zu\n",
            intersect.size(), tids_in_events.size(), thread_id_process_info);
  } else {
    sprintf(matching, "Thread ids with process info: %zu/%zu -> %zu %%\n",
            intersect.size(), tids_in_events.size(), thread_id_process_info);
  }
  *output << std::string(matching);

  if (!compact_output)
    *output << "\n";
}

void PrintTraceStats(std::ostream* output,
                     const protos::TraceStats& stats,
                     bool compact_output) {
  if (compact_output)
    return;
  *output << "--------------------Trace Stats-------------------\n";
  size_t buf_num = 0;
  for (const auto& buf : stats.buffer_stats()) {
    *output << "Buffer " << buf_num++ << "\n"
            << "  bytes_written: " << buf.bytes_written() << "\n"
            << "  chunks_written: " << buf.chunks_written() << "\n"
            << "  chunks_overwritten: " << buf.chunks_overwritten() << "\n"
            << "  write_wrap_count: " << buf.write_wrap_count() << "\n"
            << "  patches_succeeded: " << buf.patches_succeeded() << "\n"
            << "  patches_failed: " << buf.patches_failed() << "\n"
            << "  readaheads_succeeded: " << buf.readaheads_succeeded() << "\n"
            << "  readaheads_failed: " << buf.readaheads_failed() << "\n"
            << "  abi_violations: " << buf.abi_violations() << "\n";
  }
  *output << "producers_connected: " << stats.producers_connected() << "\n"
          << "producers_seen: " << stats.producers_seen() << "\n"
          << "data_sources_reg: " << stats.data_sources_registered() << "\n"
          << "data_sources_seen: " << stats.data_sources_seen() << "\n"
          << "tracing_sessions: " << stats.tracing_sessions() << "\n"
          << "total_buffers: " << stats.total_buffers() << "\n";
}

}  // namespace

int TraceToSummary(std::istream* input,
                   std::ostream* output,
                   bool compact_output) {
  uint64_t ftrace_start = std::numeric_limits<uint64_t>::max();
  uint64_t ftrace_end = 0;
  uint64_t boottime_start = std::numeric_limits<uint64_t>::max();
  uint64_t boottime_end = 0;
  uint64_t ftrace_overwrites = 0;
  std::map<FtraceEvent::EventCase, uint64_t> ftrace_event_counts;
  std::multiset<uint64_t> ftrace_timestamps;
  std::set<pid_t> tids_in_tree;
  std::set<pid_t> tids_in_events;
  std::set<uint64_t> ftrace_inodes;
  uint64_t ftrace_inode_count = 0;
  std::set<uint64_t> resolved_map_inodes;
  std::set<uint64_t> resolved_scan_inodes;
  protos::TraceStats last_stats;

  FtraceStats before_stats;
  FtraceStats after_stats;

  ForEachPacketInTrace(
      input,
      [&ftrace_start, &ftrace_end, &ftrace_overwrites, &ftrace_event_counts,
       &before_stats, &after_stats, &ftrace_timestamps, &tids_in_tree,
       &tids_in_events, &ftrace_inodes, &ftrace_inode_count,
       &resolved_map_inodes, &resolved_scan_inodes, &last_stats,
       &boottime_start, &boottime_end](const protos::TracePacket& packet) {
        if (packet.has_process_tree()) {
          const ProcessTree& tree = packet.process_tree();
          for (Process process : tree.processes()) {
            tids_in_tree.insert(process.pid());
            for (ProcessTree::Thread thread : process.threads_deprecated())
              tids_in_tree.insert(thread.tid());
          }
          for (ProcessTree::Thread thread : tree.threads())
            tids_in_tree.insert(thread.tid());
        }

        if (packet.has_inode_file_map()) {
          const InodeFileMap& inode_file_map = packet.inode_file_map();
          const auto& mount_points = inode_file_map.mount_points();
          bool from_scan = std::find(mount_points.begin(), mount_points.end(),
                                     "/data") != mount_points.end();
          for (const auto& entry : inode_file_map.entries())
            if (from_scan)
              resolved_scan_inodes.insert(entry.inode_number());
            else
              resolved_map_inodes.insert(entry.inode_number());
        }

        if (packet.has_trace_stats())
          last_stats = packet.trace_stats();

        if (packet.has_ftrace_stats()) {
          const auto& ftrace_stats = packet.ftrace_stats();
          if (ftrace_stats.phase() == FtraceStats_Phase_START_OF_TRACE) {
            before_stats = ftrace_stats;
            // TODO(hjd): Check not yet set.
          } else if (ftrace_stats.phase() == FtraceStats_Phase_END_OF_TRACE) {
            after_stats = ftrace_stats;
            // TODO(hjd): Check not yet set.
          } else {
            // TODO(hjd): Error here.
          }
        }

        if (packet.has_clock_snapshot()) {
          for (const auto& clock : packet.clock_snapshot().clocks()) {
            if (clock.type() == protos::ClockSnapshot_Clock_Type_MONOTONIC) {
              boottime_start =
                  std::min<uint64_t>(boottime_start, clock.timestamp());
              boottime_end =
                  std::max<uint64_t>(boottime_end, clock.timestamp());
            }
          }
        }

        if (!packet.has_ftrace_events())
          return;

        const FtraceEventBundle& bundle = packet.ftrace_events();
        ftrace_overwrites += bundle.overwrite_count();

        uint64_t inode_number = 0;
        for (const FtraceEvent& event : bundle.event()) {
          ftrace_event_counts[event.event_case()] += 1;

          if (ParseInode(event, &inode_number)) {
            ftrace_inodes.insert(inode_number);
            ftrace_inode_count++;
          }
          if (event.pid()) {
            tids_in_events.insert(static_cast<int>(event.pid()));
          }
          if (event.timestamp()) {
            ftrace_start = std::min<uint64_t>(ftrace_start, event.timestamp());
            ftrace_end = std::max<uint64_t>(ftrace_end, event.timestamp());
            ftrace_timestamps.insert(event.timestamp());
          }
        }
      });

  fprintf(stderr, "\n");

  char line[2048];
  uint64_t ftrace_duration = (ftrace_end - ftrace_start) / (1000 * 1000);
  if (compact_output) {
    sprintf(line, "ftrace duration,%" PRIu64 "\n", ftrace_duration);
  } else {
    sprintf(line, "Ftrace duration: %" PRIu64 "ms\n", ftrace_duration);
  }
  *output << std::string(line);

  uint64_t boottime_duration = (boottime_end - boottime_start) / (1000 * 1000);
  if (compact_output) {
    sprintf(line, "boottime duration,%" PRIu64 "\n", boottime_duration);
  } else {
    sprintf(line, "Boottime duration: %" PRIu64 "ms\n", boottime_duration);
  }
  *output << std::string(line);

  if (!compact_output)
    PrintFtraceTrack(output, ftrace_start, ftrace_end, ftrace_timestamps);
  PrintFtraceStats(output, ftrace_overwrites, ftrace_event_counts, before_stats,
                   after_stats, compact_output);
  PrintProcessStats(output, tids_in_tree, tids_in_events, compact_output);
  PrintInodeStats(output, ftrace_inodes, ftrace_inode_count,
                  resolved_map_inodes, resolved_scan_inodes, compact_output);
  PrintTraceStats(output, last_stats, compact_output);

  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
