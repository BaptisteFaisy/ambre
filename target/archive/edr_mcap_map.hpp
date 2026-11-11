/*
 * edr_mcap_map.hpp — declarations for mapping journal records to MCAP.
 *
 * Implements the seven channels and the four metadata records of FORMAT.md §12.
 */

#ifndef AMBRE_EDR_MCAP_MAP_HPP
#define AMBRE_EDR_MCAP_MAP_HPP

#include "edr_archive.hpp"
#include "mcap_writer.hpp"

#include <map>
#include <string>

namespace ambre {
namespace archive {

/* Channel names imposed by docs/FORMAT.md section 12. */
extern const char* const kTopicPrecrash;
extern const char* const kTopicDeltaV;
extern const char* const kTopicEvent;
extern const char* const kTopicAccel;
extern const char* const kTopicVehicle;
extern const char* const kTopicStatus;
extern const char* const kTopicGaps;

using TopicIds = std::map<std::string, uint16_t>;

TopicIds register_channels(mcap::Writer* writer);

void emit_precrash(const EventData& event, uint64_t t0_us, uint16_t channel_id,
                   mcap::Writer* writer);

void emit_delta_v(const EventData& event, uint64_t t0_us, uint16_t channel_id,
                  mcap::Writer* writer);

void emit_event_summary(const EventData& event, uint64_t t0_us,
                        uint16_t channel_id, mcap::Writer* writer);

void emit_class_b(const EventData& event, uint64_t t0_us,
                  const TopicIds& channels, mcap::Writer* writer);

void emit_gaps(const EventData& event, uint64_t t0_us, uint16_t channel_id,
               mcap::Writer* writer);

void attach_metadata(const EventData& event, uint64_t t0_us,
                     const std::string& source_sha256, uint64_t source_bytes,
                     mcap::Writer* writer);

}  // namespace archive
}  // namespace ambre

#endif /* AMBRE_EDR_MCAP_MAP_HPP */
