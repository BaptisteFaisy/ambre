/*
 * edr_mcap_schemas.cpp — JSON schemas of the seven MCAP channels (spec 6.4).
 *
 * Emitted as jsonschema string records in the MCAP summary, sorted keys.
 * Kept identical byte-for-byte to SCHEMAS in host/mcap/export_mcap.py.
 */

#include "edr_mcap_schemas.hpp"

namespace ambre {
namespace archive {

const char* schema_class_a_point()
{
    return "{\"properties\": {\"abs\": {\"type\": \"integer\"}, "
           "\"accel_lat_mm_s2\": {\"type\": \"integer\"}, "
           "\"accel_long_mm_s2\": {\"type\": \"integer\"}, "
           "\"airbag_lamp\": {\"type\": \"integer\"}, "
           "\"belt_driver\": {\"type\": \"integer\"}, "
           "\"brake\": {\"type\": \"integer\"}, "
           "\"engine_rpm\": {\"type\": \"integer\"}, "
           "\"esc\": {\"type\": \"integer\"}, "
           "\"rel_time_ms\": {\"description\": \"ms before T0\", \"type\": \"integer\"}, "
           "\"speed_kmh\": {\"type\": \"number\"}, "
           "\"steer_deg\": {\"type\": \"number\"}, "
           "\"throttle_pct\": {\"type\": \"integer\"}, "
           "\"yaw_rate_dps\": {\"type\": \"number\"}}, "
           "\"required\": [\"rel_time_ms\", \"speed_kmh\"], \"type\": \"object\"}";
}

const char* schema_delta_v()
{
    return "{\"properties\": {\"dv_lat_kmh\": {\"type\": \"number\"}, "
           "\"dv_lat_mm_s\": {\"type\": \"integer\"}, "
           "\"dv_long_kmh\": {\"type\": \"number\"}, "
           "\"dv_long_mm_s\": {\"type\": \"integer\"}, "
           "\"rel_time_ms\": {\"description\": \"ms after T0\", \"type\": \"integer\"}}, "
           "\"required\": [\"rel_time_ms\", \"dv_long_mm_s\", \"dv_lat_mm_s\"], "
           "\"type\": \"object\"}";
}

const char* schema_event_summary()
{
    return "{\"properties\": {\"cont_records\": {\"type\": \"integer\"}, "
           "\"event_id\": {\"type\": \"integer\"}, "
           "\"event_number\": {\"type\": \"integer\"}, "
           "\"file_complete\": {\"type\": \"integer\"}, "
           "\"ignition_cycle\": {\"type\": \"integer\"}, "
           "\"locked\": {\"type\": \"integer\"}, "
           "\"max_dv_lat_mm_s\": {\"type\": \"integer\"}, "
           "\"max_dv_long_mm_s\": {\"type\": \"integer\"}, "
           "\"restraint_deploy_ms\": {\"type\": \"integer\"}, "
           "\"seal_cause\": {\"type\": \"string\"}, "
           "\"time_since_first_ms\": {\"type\": \"integer\"}, "
           "\"trig_reason\": {\"type\": \"integer\"}, "
           "\"vru_deploy_ms\": {\"type\": \"integer\"}}, "
           "\"required\": [\"event_id\", \"file_complete\"], \"type\": \"object\"}";
}

const char* schema_accel()
{
    return "{\"properties\": {\"quality\": {\"type\": \"object\"}, "
           "\"x_mm_s2\": {\"type\": \"integer\"}, "
           "\"y_mm_s2\": {\"type\": \"integer\"}, "
           "\"z_mm_s2\": {\"type\": \"integer\"}}, \"type\": \"object\"}";
}

const char* schema_vehicle()
{
    return "{\"properties\": {\"brake\": {\"type\": \"integer\"}, "
           "\"engine_rpm\": {\"type\": \"integer\"}, "
           "\"quality\": {\"type\": \"object\"}, "
           "\"roll_rate_dps\": {\"type\": \"number\"}, "
           "\"steer_deg\": {\"type\": \"number\"}, "
           "\"throttle_pct\": {\"type\": \"number\"}, "
           "\"wheel_fl_mm_s\": {\"type\": \"integer\"}, "
           "\"wheel_fr_mm_s\": {\"type\": \"integer\"}, "
           "\"wheel_rl_mm_s\": {\"type\": \"integer\"}, "
           "\"wheel_rr_mm_s\": {\"type\": \"integer\"}, "
           "\"yaw_rate_dps\": {\"type\": \"number\"}}, \"type\": \"object\"}";
}

const char* schema_status()
{
    return "{\"properties\": {\"adas\": {\"type\": \"integer\"}, "
           "\"belts\": {\"type\": \"integer\"}, "
           "\"gear\": {\"type\": \"integer\"}, "
           "\"lamps\": {\"type\": \"integer\"}, "
           "\"quality\": {\"type\": \"object\"}}, \"type\": \"object\"}";
}

const char* schema_gap()
{
    return "{\"properties\": {\"duration_us\": {\"type\": \"integer\"}, "
           "\"end_ts_us\": {\"type\": \"integer\"}, "
           "\"reason\": {\"type\": \"integer\"}, "
           "\"start_ts_us\": {\"type\": \"integer\"}}, "
           "\"required\": [\"start_ts_us\", \"end_ts_us\"], \"type\": \"object\"}";
}

}  // namespace archive
}  // namespace ambre
