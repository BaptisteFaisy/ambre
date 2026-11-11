/*
 * edr_mcap_schemas.hpp — JSON schemas of the seven MCAP channels.
 *
 * The channel set is imposed by docs/FORMAT.md section 12; the schema bodies
 * are the wire contract that Foxglove (EXP-03) and edr-verify (EXP-07) read.
 * They are kept byte-identical to the ones the host exporter emits
 * (SCHEMAS in host/mcap/export_mcap.py) so that both implementations of the
 * archive describe the data the same way.
 */

#ifndef AMBRE_EDR_MCAP_SCHEMAS_HPP
#define AMBRE_EDR_MCAP_SCHEMAS_HPP

namespace ambre {
namespace archive {

const char* schema_class_a_point();
const char* schema_delta_v();
const char* schema_event_summary();
const char* schema_accel();
const char* schema_vehicle();
const char* schema_status();
const char* schema_gap();

}  // namespace archive
}  // namespace ambre

#endif /* AMBRE_EDR_MCAP_SCHEMAS_HPP */
