/*
 * edr_archive_run.cpp — MCAP assembly of one event, file digest, and the
 * top-level run entry points (standalone image mode and journal service mode).
 */

#include "edr_archive_internal.hpp"

#include "ambre_log.hpp"
#include "ambre_sha256.hpp"
#include "edr_mcap_map.hpp"
#include "mcap_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace ambre {
namespace archive {

namespace {

const char* kLogTag = "edr-archive";
const char* kProfileName = "ambre";
const char* kLibraryName = "ambre-target-mcap-cpp";

}  // namespace

std::string build_event_mcap(const EventData& event,
                             const std::string& source_sha256,
                             uint64_t source_bytes)
{
    uint64_t t0 = event.trig_ts_us;
    if (t0 == 0u) {
        for (const EventRecord& record : event.records) {
            ambre_emerg_t emerg{};
            if (record.rec_type == AMBRE_REC_EMERG &&
                record.payload.size() >= sizeof(emerg)) {
                std::memcpy(&emerg, record.payload.data(), sizeof(emerg));
                t0 = emerg.t0_ts_us;
                break;
            }
        }
    }
    mcap::Writer writer(kProfileName, kLibraryName);
    const TopicIds channels = register_channels(&writer);
    emit_precrash(event, t0, channels.at(kTopicPrecrash), &writer);
    emit_delta_v(event, t0, channels.at(kTopicDeltaV), &writer);
    emit_event_summary(event, t0, channels.at(kTopicEvent), &writer);
    emit_class_b(event, t0, channels, &writer);
    emit_gaps(event, t0, channels.at(kTopicGaps), &writer);
    attach_metadata(event, t0, source_sha256, source_bytes, &writer);
    return writer.encode();
}

bool digest_file(const std::string& path, std::string* hex_out,
                 uint64_t* size_out)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    sha256::Context context;
    uint64_t total = 0u;
    uint8_t buffer[65536];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0u) {
        context.update(buffer, got);
        total += static_cast<uint64_t>(got);
    }
    std::fclose(file);
    *hex_out = context.hex_digest();
    *size_out = total;
    return true;
}

namespace {

/* Load, build, write and verify one event of the image (cahier section 4.7). */
bool archive_one(const std::string& image_path, const std::string& out_dir,
                 const ambre_ev_desc_t& desc, const std::string& source_sha,
                 uint64_t source_bytes, std::vector<ArchiveEntry>* index,
                 std::string* error)
{
    EventData event;
    if (!load_event(image_path, desc, &event, error)) {
        return false;
    }
    return store_event(out_dir, event, source_sha, source_bytes, index, error);
}

/* GEL-06: no archive write at all while an event is still open. The journal
 * enforces the same rule on the IPC path by refusing EXPORT with a NAK. */
bool any_event_open(const std::vector<ambre_ev_desc_t>& events)
{
    for (const ambre_ev_desc_t& desc : events) {
        if (desc.state != (uint8_t)AMBRE_EV_SEALED) {
            return true;
        }
    }
    return false;
}

/* Archive every event of the table, oldest order first. */
int archive_all_from_image(const std::string& image_path,
                           const std::string& out_dir)
{
    std::vector<ambre_ev_desc_t> events;
    std::string error;
    if (!list_events(image_path, &events, &error)) {
        log_error(kLogTag, "%s", error.c_str());
        return 1;
    }
    if (events.empty()) {
        log_info(kLogTag, "no event in %s", image_path.c_str());
        return 0;
    }
    if (any_event_open(events)) {
        log_warn(kLogTag, "GEL-06: an event is still open, no archive written");
        return 0;
    }
    std::sort(events.begin(), events.end(),
              [](const ambre_ev_desc_t& a, const ambre_ev_desc_t& b) {
                  return a.order < b.order;
              });
    std::string source_sha;
    uint64_t source_bytes = 0u;
    if (!digest_file(image_path, &source_sha, &source_bytes)) {
        log_error(kLogTag, "cannot hash %s", image_path.c_str());
        return 1;
    }
    std::vector<ArchiveEntry> index;
    (void)index_read(out_dir, &index);
    int failures = 0;
    for (const ambre_ev_desc_t& desc : events) {
        if (!archive_one(image_path, out_dir, desc, source_sha, source_bytes,
                         &index, &error)) {
            log_error(kLogTag, "event %u: %s", desc.event_id, error.c_str());
            failures++;
        }
    }
    return (failures == 0) ? 0 : 1;
}

int archive_selected_from_image(const std::string& image_path,
                                const std::string& out_dir, uint32_t event_id)
{
    std::vector<ambre_ev_desc_t> events;
    std::string error;
    if (!list_events(image_path, &events, &error)) {
        log_error(kLogTag, "%s", error.c_str());
        return 1;
    }
    if (any_event_open(events)) {
        log_warn(kLogTag, "GEL-06: an event is still open, nothing exported");
        return 1;
    }
    for (const ambre_ev_desc_t& desc : events) {
        if (desc.event_id != event_id) {
            continue;
        }
        std::string source_sha;
        uint64_t source_bytes = 0u;
        if (!digest_file(image_path, &source_sha, &source_bytes)) {
            log_error(kLogTag, "cannot hash %s", image_path.c_str());
            return 1;
        }
        std::vector<ArchiveEntry> index;
        (void)index_read(out_dir, &index);
        if (!archive_one(image_path, out_dir, desc, source_sha, source_bytes,
                         &index, &error)) {
            log_error(kLogTag, "event %u: %s", event_id, error.c_str());
            return 1;
        }
        return 0;
    }
    log_error(kLogTag, "event %u is not in the allocation table", event_id);
    return 1;
}

int list_image_events(const std::string& image_path)
{
    std::vector<ambre_ev_desc_t> events;
    std::string error;
    if (!list_events(image_path, &events, &error)) {
        log_error(kLogTag, "%s", error.c_str());
        return 1;
    }
    if (events.empty()) {
        log_info(kLogTag, "no event in %s", image_path.c_str());
        return 0;
    }
    for (const ambre_ev_desc_t& desc : events) {
        log_info(kLogTag,
                 "event %u order=%u state=%s locked=%u complete=%u "
                 "trig_ts_us=%llu",
                 desc.event_id, desc.order,
                 desc.state == AMBRE_EV_SEALED ? "sealed" : "open",
                 desc.locked, desc.file_complete,
                 static_cast<unsigned long long>(desc.trig_ts_us));
    }
    return 0;
}

}  // namespace

int run_standalone(const std::string& image_path, const std::string& out_dir,
                   uint32_t event_id, bool list_only)
{
    if (list_only) {
        return list_image_events(image_path);
    }
    if (event_id != 0u) {
        return archive_selected_from_image(image_path, out_dir, event_id);
    }
    return archive_all_from_image(image_path, out_dir);
}

int run(const Options& options)
{
    if (options.out_dir.empty()) {
        log_error(kLogTag, "--out is required");
        return 2;
    }
    if (!options.image_path.empty()) {
        return run_standalone(options.image_path, options.out_dir,
                              options.event_id, false);
    }
    if (!options.journal_sock.empty()) {
        return run_service(options.journal_sock, options.out_dir, options.once);
    }
    log_error(kLogTag, "either --journal or --image is required");
    return 2;
}

}  // namespace archive
}  // namespace ambre
