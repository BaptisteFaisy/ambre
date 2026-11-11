/*
 * edr_mcap_map.cpp — journal records to MCAP channels (implementation).
 *
 * Emits the seven imposed channels with their JSON schemas and the four
 * metadata records (docs/FORMAT.md §12, cahier §5.6 EXP-02, EXP-04).
 */

#include "edr_mcap_map.hpp"
#include "ambre_json.hpp"
#include "ambre_sha256.hpp"
#include "edr_mcap_schemas.hpp"

extern "C" {
#include "../../fmt/ambre_crc32c.h"
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace ambre {
namespace archive {

/* Channel names imposed by docs/FORMAT.md section 12. */
const char* const kTopicPrecrash = "/edr/a/precrash";
const char* const kTopicDeltaV = "/edr/a/delta_v";
const char* const kTopicEvent = "/edr/a/event";
const char* const kTopicAccel = "/edr/b/accel";
const char* const kTopicVehicle = "/edr/b/vehicle";
const char* const kTopicStatus = "/edr/b/status";
const char* const kTopicGaps = "/edr/meta/gaps";

namespace {

const uint64_t kNsPerUs = 1000ULL;

struct SignalInfo {
    uint16_t id;
    const char* topic;
    const char* field;
    uint32_t rate_hz;
    double scale;
};

const SignalInfo kSignals[] = {
    {1, kTopicAccel, "x_mm_s2", 1000, 1.0},
    {2, kTopicAccel, "y_mm_s2", 1000, 1.0},
    {3, kTopicAccel, "z_mm_s2", 1000, 1.0},
    {10, kTopicVehicle, "wheel_fl_mm_s", 100, 1.0},
    {11, kTopicVehicle, "wheel_fr_mm_s", 100, 1.0},
    {12, kTopicVehicle, "wheel_rl_mm_s", 100, 1.0},
    {13, kTopicVehicle, "wheel_rr_mm_s", 100, 1.0},
    {20, kTopicVehicle, "steer_deg", 100, 0.5},
    {21, kTopicVehicle, "throttle_pct", 100, 0.5},
    {22, kTopicVehicle, "brake", 100, 1.0},
    {23, kTopicVehicle, "yaw_rate_dps", 100, 0.1},
    {24, kTopicVehicle, "roll_rate_dps", 100, 0.1},
    {25, kTopicVehicle, "engine_rpm", 100, 1.0},
    {40, kTopicStatus, "adas", 10, 1.0},
    {41, kTopicStatus, "belts", 10, 1.0},
    {42, kTopicStatus, "lamps", 10, 1.0},
    {43, kTopicStatus, "gear", 10, 1.0},
};

const SignalInfo* signal_lookup(uint16_t signal_id)
{
    for (const SignalInfo& info : kSignals) {
        if (info.id == signal_id) {
            return &info;
        }
    }
    return nullptr;
}

uint64_t log_time_from_delta_us(int64_t delta_us)
{
    const long long value = static_cast<long long>(kT0OffsetNs) +
                            delta_us * static_cast<long long>(kNsPerUs);
    return (value > 0) ? static_cast<uint64_t>(value) : 0ULL;
}

uint64_t log_time_ns(uint64_t t0_us, uint64_t ts_us)
{
    return log_time_from_delta_us(static_cast<int64_t>(ts_us) -
                                  static_cast<int64_t>(t0_us));
}

const char* seal_cause_name(uint8_t cause)
{
    switch (cause) {
    case AMBRE_SEAL_NORMAL:    return "normal";
    case AMBRE_SEAL_POWERCUT:  return "powercut";
    case AMBRE_SEAL_VOLTAGE:   return "voltage";
    case AMBRE_SEAL_OVERWRITE: return "overwrite";
    default:                   return "none";
    }
}

template <typename T>
bool payload_as(const EventRecord& record, T* out)
{
    if (record.payload.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(out, record.payload.data(), sizeof(T));
    return true;
}

std::string class_a_payload(const ambre_a_point_t& p)
{
    json::Object obj;
    obj.integer("abs", p.abs_active);
    obj.integer("accel_lat_mm_s2", p.accel_lat_mm_s2);
    obj.integer("accel_long_mm_s2", p.accel_long_mm_s2);
    obj.integer("airbag_lamp", p.airbag_lamp);
    obj.integer("belt_driver", p.belt_driver);
    obj.integer("brake", p.brake_active);
    obj.integer("engine_rpm", p.engine_rpm);
    obj.integer("esc", p.esc_active);
    obj.integer("rel_time_ms", p.rel_time_ms);
    obj.number("speed_kmh", static_cast<double>(p.speed_kmh_x4) / 4.0);
    obj.number("steer_deg", static_cast<double>(p.steer_angle_deg_x2) / 2.0);
    obj.integer("throttle_pct", p.throttle_pct);
    obj.number("yaw_rate_dps", static_cast<double>(p.yaw_rate_deg_s_x10) / 10.0);
    return obj.text();
}

std::string delta_v_payload(const ambre_cont_t& c)
{
    json::Object obj;
    obj.number("dv_lat_kmh", static_cast<double>(c.dv_lat_mm_s) * 0.0036);
    obj.integer("dv_lat_mm_s", c.dv_lat_mm_s);
    obj.number("dv_long_kmh", static_cast<double>(c.dv_long_mm_s) * 0.0036);
    obj.integer("dv_long_mm_s", c.dv_long_mm_s);
    obj.integer("rel_time_ms", c.rel_time_ms);
    return obj.text();
}

std::string event_payload(const EventData& event, const ambre_emerg_t& emerg,
                          bool have_emerg, const ambre_seal_t& seal,
                          bool have_seal, size_t cont_count)
{
    json::Object obj;
    obj.integer("cont_records", static_cast<long long>(cont_count));
    obj.integer("event_id", event.event_id);
    obj.integer("event_number", have_emerg ? emerg.event_number : 1);
    obj.integer("file_complete", event.file_complete ? 1 : 0);
    obj.integer("ignition_cycle", have_emerg ? emerg.ignition_cycle : 0);
    obj.integer("locked", event.locked ? 1 : 0);
    obj.integer("max_dv_lat_mm_s", have_seal ? seal.max_dv_lat_mm_s : 0);
    obj.integer("max_dv_long_mm_s", have_seal ? seal.max_dv_long_mm_s : 0);
    obj.integer("restraint_deploy_ms", have_seal ? seal.restraint_deploy_ms : 0);
    obj.string("seal_cause", have_seal ? seal_cause_name(seal.seal_cause)
                                       : "none");
    obj.integer("time_since_first_ms", have_seal ? seal.time_since_first_ms : 0);
    obj.integer("trig_reason", have_emerg ? emerg.trig_reason
                                          : event.trig_reason);
    obj.integer("vru_deploy_ms", have_seal ? seal.vru_deploy_ms : 0);
    return obj.text();
}

std::string gap_payload(const ambre_gap_t& gap)
{
    json::Object obj;
    obj.integer("duration_us",
                static_cast<long long>(gap.end_ts_us - gap.start_ts_us));
    obj.integer("end_ts_us", static_cast<long long>(gap.end_ts_us));
    obj.integer("reason", gap.reason);
    obj.integer("start_ts_us", static_cast<long long>(gap.start_ts_us));
    return obj.text();
}

struct ClassBSlot {
    json::Object fields;
    json::Object quality;
};

using ClassBMap = std::map<std::pair<std::string, uint64_t>, ClassBSlot>;

void merge_sample(uint64_t first_ts_us, const ambre_sample_t& sample,
                  uint32_t* index, ClassBMap* slots)
{
    const SignalInfo* info = signal_lookup(sample.signal_id);
    if (info == nullptr) {
        return;
    }
    const uint64_t step_us = 1000000ULL / info->rate_hz;
    const uint64_t ts_us = first_ts_us + (*index) * step_us;
    (*index)++;
    ClassBSlot& slot = (*slots)[std::make_pair(std::string(info->topic), ts_us)];
    const double value = static_cast<double>(sample.value) * info->scale;
    if (info->scale == 1.0) {
        slot.fields.integer(info->field, sample.value);
    } else {
        slot.fields.number(info->field, value);
    }
    slot.quality.integer(info->field, sample.quality);
}

void collect_class_b(const EventData& event, ClassBMap* slots)
{
    for (const EventRecord& record : event.records) {
        if (record.rec_type != AMBRE_REC_DATA) {
            continue;
        }
        std::map<uint16_t, uint32_t> counters;
        const size_t count = record.payload.size() / sizeof(ambre_sample_t);
        for (size_t i = 0; i < count; i++) {
            ambre_sample_t sample{};
            std::memcpy(&sample, record.payload.data() + i * sizeof(sample),
                        sizeof(sample));
            merge_sample(record.first_ts_us, sample, &counters[sample.signal_id],
                         slots);
        }
    }
}

}  // namespace

TopicIds register_channels(mcap::Writer* writer)
{
    struct Entry {
        const char* topic;
        const char* schema;
        const char* (*body)();
    };
    const Entry entries[] = {
        {kTopicPrecrash, "ambre.ClassAPoint", schema_class_a_point},
        {kTopicDeltaV, "ambre.DeltaV", schema_delta_v},
        {kTopicEvent, "ambre.EventSummary", schema_event_summary},
        {kTopicAccel, "ambre.Accel", schema_accel},
        {kTopicVehicle, "ambre.Vehicle", schema_vehicle},
        {kTopicStatus, "ambre.Status", schema_status},
        {kTopicGaps, "ambre.Gap", schema_gap},
    };
    TopicIds ids;
    for (const Entry& entry : entries) {
        const uint16_t schema_id =
            writer->add_schema(entry.schema, "jsonschema", entry.body());
        ids[entry.topic] = writer->add_channel(entry.topic, "json", schema_id);
    }
    return ids;
}

void emit_precrash(const EventData& event, uint64_t t0_us, uint16_t channel_id,
                   mcap::Writer* writer)
{
    (void)t0_us;
    ambre_emerg_t emerg{};
    for (const EventRecord& record : event.records) {
        if (record.rec_type != AMBRE_REC_EMERG || !payload_as(record, &emerg)) {
            continue;
        }
        const uint32_t count = emerg.point_count > 21u ? 21u : emerg.point_count;
        /* Sort points oldest first (highest rel_time_ms first) to match Python. */
        std::vector<ambre_a_point_t> points;
        for (uint32_t i = 0; i < count; i++) {
            points.push_back(emerg.points[i]);
        }
        std::sort(points.begin(), points.end(),
                  [](const ambre_a_point_t& a, const ambre_a_point_t& b) {
                      return a.rel_time_ms > b.rel_time_ms;
                  });
        for (const ambre_a_point_t& pt : points) {
            const int64_t delta_us =
                -static_cast<int64_t>(pt.rel_time_ms) * 1000;
            const uint64_t log_ns = log_time_from_delta_us(delta_us);
            writer->add_message(channel_id, log_ns, class_a_payload(pt));
        }
        break;
    }
}

void emit_delta_v(const EventData& event, uint64_t t0_us, uint16_t channel_id,
                  mcap::Writer* writer)
{
    (void)t0_us;
    std::vector<ambre_cont_t> conts;
    for (const EventRecord& record : event.records) {
        ambre_cont_t cont{};
        if (record.rec_type == AMBRE_REC_CONT && payload_as(record, &cont)) {
            conts.push_back(cont);
        }
    }
    std::sort(conts.begin(), conts.end(),
              [](const ambre_cont_t& a, const ambre_cont_t& b) {
                  return a.rel_time_ms < b.rel_time_ms;
              });
    for (const ambre_cont_t& cont : conts) {
        const int64_t delta_us =
            static_cast<int64_t>(cont.rel_time_ms) * 1000;
        const uint64_t log_ns = log_time_from_delta_us(delta_us);
        writer->add_message(channel_id, log_ns, delta_v_payload(cont));
    }
}

void emit_event_summary(const EventData& event, uint64_t t0_us,
                        uint16_t channel_id, mcap::Writer* writer)
{
    (void)t0_us;
    /* Zero-initialised: payload_as() only fills them when the record is valid,
     * and event_payload() reads whichever of the two is present. GCC 14 on the
     * CI workers cannot prove that and warns (-Werror=maybe-uninitialized);
     * initialising is correct anyway. */
    ambre_emerg_t emerg{};
    ambre_seal_t seal{};
    bool have_emerg = false;
    bool have_seal = false;
    size_t cont_count = 0;
    for (const EventRecord& record : event.records) {
        if (record.rec_type == AMBRE_REC_EMERG && !have_emerg) {
            have_emerg = payload_as(record, &emerg);
        } else if (record.rec_type == AMBRE_REC_SEAL && !have_seal) {
            have_seal = payload_as(record, &seal);
        } else if (record.rec_type == AMBRE_REC_CONT) {
            cont_count++;
        }
    }
    const std::string body = event_payload(event, emerg, have_emerg, seal,
                                           have_seal, cont_count);
    writer->add_message(channel_id, kT0OffsetNs, body);
}

void emit_class_b(const EventData& event, uint64_t t0_us,
                  const TopicIds& channels, mcap::Writer* writer)
{
    ClassBMap slots;
    collect_class_b(event, &slots);
    for (auto& entry : slots) {
        const std::string& topic = entry.first.first;
        const uint64_t ts_us = entry.first.second;
        const auto it = channels.find(topic);
        if (it == channels.end()) {
            continue;
        }
        entry.second.fields.raw("quality", entry.second.quality.text());
        writer->add_message(it->second, log_time_ns(t0_us, ts_us),
                            entry.second.fields.text());
    }
}

void emit_gaps(const EventData& event, uint64_t t0_us, uint16_t channel_id,
               mcap::Writer* writer)
{
    for (const EventRecord& record : event.records) {
        ambre_gap_t gap{};
        if (record.rec_type == AMBRE_REC_GAP && payload_as(record, &gap)) {
            writer->add_message(channel_id, log_time_ns(t0_us, gap.start_ts_us),
                                gap_payload(gap));
        }
    }
}

/* EXP-04: the event summary, the vehicle identity, the build fingerprint and
 * the integrity chain travel as MCAP metadata records. */
mcap::StringMap event_metadata(const EventData& event, uint64_t t0_us,
                               const ambre_emerg_t& emerg, bool have_emerg,
                               const ambre_seal_t& seal, bool have_seal)
{
    mcap::StringMap map;
    map["event_id"] = std::to_string(event.event_id);
    map["event_number"] =
        std::to_string(have_emerg ? emerg.event_number : 1);
    map["file_complete"] = event.file_complete ? "1" : "0";
    map["ignition_cycle"] =
        std::to_string(have_emerg ? emerg.ignition_cycle : 0);
    map["locked"] = event.locked ? "1" : "0";
    map["seal_cause"] = have_seal ? seal_cause_name(seal.seal_cause) : "none";
    /* Monotonic T0 of the event (never an absolute epoch): it lets a verifier
     * map every class A rel_time_ms back onto the injected scenario. */
    map["t0_ts_us"] = std::to_string(t0_us);
    map["trig_reason"] =
        std::to_string(have_emerg ? emerg.trig_reason : event.trig_reason);
    return map;
}

mcap::StringMap integrity_metadata(const EventData& event,
                                   const std::string& source_sha256,
                                   uint64_t source_bytes, uint32_t payload_crc,
                                   uint64_t trig_ts_us)
{
    char crc_hex[16];
    std::snprintf(crc_hex, sizeof(crc_hex), "%08x", payload_crc);
    mcap::StringMap map;
    map["event_payload_crc32c"] = crc_hex;
    map["image_bytes"] = std::to_string(source_bytes);
    map["image_sha256"] = source_sha256;
    map["signature"] = "none";
    map["t0_offset_ns"] = std::to_string(kT0OffsetNs);
    map["trig_ts_us"] = std::to_string(trig_ts_us);
    (void)event;
    return map;
}

void attach_metadata(const EventData& event, uint64_t t0_us,
                     const std::string& source_sha256, uint64_t source_bytes,
                     mcap::Writer* writer)
{
    ambre_emerg_t emerg{};
    ambre_seal_t seal{};
    bool have_emerg = false;
    bool have_seal = false;
    uint32_t payload_crc = 0u;
    uint64_t trig_ts_us = 0u;
    for (const EventRecord& record : event.records) {
        if (record.rec_type == (uint8_t)AMBRE_REC_EMERG && !have_emerg) {
            have_emerg = payload_as(record, &emerg);
            if (have_emerg) {
                trig_ts_us = emerg.trig_ts_us;
            }
        } else if (record.rec_type == (uint8_t)AMBRE_REC_SEAL && !have_seal) {
            have_seal = payload_as(record, &seal);
        }
        payload_crc = ambre_crc32c(payload_crc, record.payload.data(),
                                   record.payload.size());
    }

    writer->add_metadata("edr.event",
                         event_metadata(event, t0_us, emerg, have_emerg, seal,
                                        have_seal));

    mcap::StringMap vehicle;
    vehicle["type"] = "ambre-edr";
    vehicle["variant"] = "socle";
    vehicle["version"] = "1";
    writer->add_metadata("edr.vehicle_type", vehicle);

    mcap::StringMap build;
    build["config_sha256"] = sha256::hex_of("ambre-format-v1", 15u);
    build["profile"] = "socle";
    build["version"] = "0.1.0";
    writer->add_metadata("edr.build", build);

    writer->add_metadata("edr.integrity",
                         integrity_metadata(event, source_sha256, source_bytes,
                                            payload_crc, trig_ts_us));
}

}  // namespace archive
}  // namespace ambre
