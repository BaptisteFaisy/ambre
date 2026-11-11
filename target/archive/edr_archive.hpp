/*
 * edr_archive.hpp — MCAP export engine of the Ambre archive process.
 *
 * Implements the export described by docs/FORMAT.md section 12 and the cahier
 * des charges sections 4.7 and 5.6 (EXP-01..EXP-07): seven channels with their
 * JSON schemas, T0 at 60 s inside the file and no absolute timestamp (VIE-02),
 * the four metadata records, an atomic write (temporary name, flush, rename,
 * directory flush) and a re-read of the file before the event is considered
 * archived (REP-07). Retention keeps at least 16 events (STO-07).
 *
 * The journal is never re-implemented here: records are reassembled by the C
 * library fmt/ (ambre_read_record, ambre_table_read_best), reached through
 * extern "C".
 */

#ifndef AMBRE_EDR_ARCHIVE_HPP
#define AMBRE_EDR_ARCHIVE_HPP

#include <cstdint>
#include <string>
#include <vector>

/* The frozen C headers use the C11 keyword; C++ spells it static_assert. */
#define _Static_assert static_assert
extern "C" {
#include "../../fmt/ambre_fmt.h"
#include "../../fmt/ambre_journal.h"
#include "../edr_ipc.h"
}
#undef _Static_assert

namespace ambre {
namespace archive {

/* Priority of edr-archive (docs/ARCHITECTURE.md section 1). */
const int kPriorityArchive = 8;
/* STO-07: at least sixteen events are kept in the archive. */
const size_t kMinRetainedEvents = 16u;
/* FORMAT.md section 12: T0 sits at 60 s inside the file. */
const uint64_t kT0OffsetNs = 60000000000ULL;

/* One journal record, reassembled and validated by fmt/. */
struct EventRecord {
    uint64_t seq = 0;
    uint8_t rec_type = 0;
    uint64_t first_ts_us = 0;
    std::string payload;
};

/* Everything the MCAP builder needs about one event. */
struct EventData {
    uint32_t event_id = 0;
    uint32_t order = 0;
    bool locked = false;
    bool file_complete = false;
    uint8_t trig_reason = 0;
    uint64_t trig_ts_us = 0;
    std::vector<EventRecord> records;      /* in sequence order */
};

/* One entry of the archive retention index. */
struct ArchiveEntry {
    uint32_t event_id = 0;
    uint32_t order = 0;
    bool locked = false;
    std::string file;
};

/* ---- journal image (standalone mode) ----------------------------------- */

/* Events of a raw journal image, in table order. */
bool list_events(const std::string& image_path,
                 std::vector<ambre_ev_desc_t>* out, std::string* error);

/* Load one event: its descriptor and every record of its sequence window. */
bool load_event(const std::string& image_path, const ambre_ev_desc_t& desc,
                EventData* out, std::string* error);

/* ---- MCAP -------------------------------------------------------------- */

/* Build the MCAP bytes of one event (docs/FORMAT.md section 12). */
std::string build_event_mcap(const EventData& event,
                             const std::string& source_sha256,
                             uint64_t source_bytes);

/* ---- durable file handling (cahier section 4.7, REP-07) ---------------- */

/* Temporary name, write, flush, rename, directory flush. */
bool write_atomic(const std::string& target_path, const std::string& content,
                  std::string* error);

/* Re-read an MCAP file: magic at both ends, footer, summary CRC, chunk CRCs. */
bool verify_mcap_file(const std::string& path, std::string* error);

/* SHA-256 of a file, plus its size. */
bool digest_file(const std::string& path, std::string* hex_out,
                 uint64_t* size_out);

/* ---- retention (STO-07, SEC-03) ---------------------------------------- */

std::string event_file_name(uint32_t event_id, uint32_t order);
std::string index_file_name();

bool index_read(const std::string& dir, std::vector<ArchiveEntry>* out);
bool index_write(const std::string& dir, const std::vector<ArchiveEntry>& entries,
                 std::string* error);

/* Recycle the oldest unlocked archives above `keep`; returns how many went.
 * A locked event is never removed. */
size_t apply_retention(const std::string& dir, size_t keep, std::string* error);

/* ---- journal service (IPC mode) --------------------------------------- */

/* Ask edr-journal for the raw blocks of one sealed event and rebuild it with
 * fmt/. False with `error` filled when the service refuses (GEL-06, SEC-01) or
 * the socket is unreachable. */
bool fetch_event(const std::string& sock_path, uint32_t event_id,
                 bool* locked_out, EventData* out, std::string* error);

/* Sealed events the journal knows about, through AMBRE_MSG_LIST. */
bool list_events_service(const std::string& sock_path,
                         std::vector<ambre_ipc_event_line_t>* out,
                         std::string* error);

/* ---- entry points ----------------------------------------------------- */

struct Options {
    std::string journal_sock;
    std::string out_dir;
    std::string image_path;
    uint32_t event_id = 0;         /* 0 = every event */
    bool once = false;
};

/* Standalone: archive from a raw journal image (tests, bench, recovery). */
int run_standalone(const std::string& image_path, const std::string& out_dir,
                   uint32_t event_id, bool list_only);

/* Journal service mode: wait for EVENT_READY, archive what is sealed. */
int run_service(const std::string& sock_path, const std::string& out_dir,
                bool once_mode);

/* Dispatch on the options; 0 ok, 1 functional failure, 2 usage error. */
int run(const Options& options);

}  // namespace archive
}  // namespace ambre

#endif /* AMBRE_EDR_ARCHIVE_HPP */
