/*
 * edr_archive_internal.hpp — helpers shared by the archive translation units.
 *
 * Not a public interface: it lets the image loader, the IPC loader and the
 * retention code reuse the same record walk (house rule: one implementation
 * per logic). Everything here works on an ambre_disk_t, so the same walk runs
 * against a file image and against blocks fetched from edr-journal.
 */

#ifndef AMBRE_EDR_ARCHIVE_INTERNAL_HPP
#define AMBRE_EDR_ARCHIVE_INTERNAL_HPP

#include "edr_archive.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ambre {
namespace archive {

/* Reassemble every record of one event from `disk`, starting at the block of
 * its EMERG record. Returns false when the EMERG record itself is unusable
 * (torn journal), which is REP-05's "never restore CRC-invalid data". */
bool load_event_from_disk(const ambre_disk_t& disk, const ambre_ev_desc_t& desc,
                          uint64_t session_id, EventData* out);

/* True when the archive already holds a file for that event. */
bool already_archived(const std::vector<ArchiveEntry>& index, uint32_t event_id);

/* Build the MCAP of one event, write it durably, re-read it (REP-07), record it
 * in the retention index and prune the archive (STO-07). */
bool store_event(const std::string& out_dir, const EventData& event,
                 const std::string& source_sha256, uint64_t source_bytes,
                 std::vector<ArchiveEntry>* index, std::string* error);

/* Read-only ambre_disk_t over an in-memory buffer (blocks fetched from the
 * journal service). `vtable()` must be called after the buffer is filled. */
class MemoryDisk {
 public:
    explicit MemoryDisk(std::vector<uint8_t> image) : image_(std::move(image)) {}

    ambre_disk_t vtable();

 private:
    static int read_thunk(void* ctx, uint64_t offset, void* buffer, size_t length);
    static int write_thunk(void*, uint64_t, const void*, size_t);
    static int flush_thunk(void*);

    std::vector<uint8_t> image_;
};

}  // namespace archive
}  // namespace ambre

#endif /* AMBRE_EDR_ARCHIVE_INTERNAL_HPP */
