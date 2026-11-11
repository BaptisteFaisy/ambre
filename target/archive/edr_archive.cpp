/*
 * edr_archive.cpp — read access to a raw journal image, through fmt/.
 *
 * The format is NOT re-implemented here: ambre_table_read_best() picks the
 * valid allocation-table copy with the highest generation (REP-02) and
 * ambre_read_record() reassembles and validates every record (block magic,
 * version, both CRCs, chunk geometry, session and sequence continuity).
 * A record that fails any of those checks is dropped (REP-05).
 */

#include "edr_archive_internal.hpp"

#include <cstdio>
#include <cstring>

namespace ambre {
namespace archive {

namespace {

/* Read-only file image of the raw partition. */
class FileDisk {
 public:
    explicit FileDisk(const std::string& path)
        : file_(std::fopen(path.c_str(), "rb"))
    {
    }
    ~FileDisk()
    {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }
    FileDisk(const FileDisk&) = delete;
    FileDisk& operator=(const FileDisk&) = delete;

    bool ok() const { return file_ != nullptr; }

    ambre_disk_t vtable()
    {
        ambre_disk_t disk{};
        std::memset(&disk, 0, sizeof(disk));
        disk.ctx = this;
        disk.size = size();
        disk.sector_size = AMBRE_BLOCK_SIZE;
        disk.read = &FileDisk::read_thunk;
        disk.write = &FileDisk::write_thunk;
        disk.flush = &FileDisk::flush_thunk;
        disk.mark = nullptr;
        return disk;
    }

 private:
    uint64_t size() const
    {
        if (file_ == nullptr) {
            return 0u;
        }
        const long current = std::ftell(file_);
        (void)std::fseek(file_, 0, SEEK_END);
        const long end = std::ftell(file_);
        (void)std::fseek(file_, current, SEEK_SET);
        return static_cast<uint64_t>(end);
    }

    static int read_thunk(void* ctx, uint64_t offset, void* buffer, size_t length)
    {
        FileDisk* self = static_cast<FileDisk*>(ctx);
        if (self->file_ == nullptr ||
            std::fseek(self->file_, static_cast<long>(offset), SEEK_SET) != 0) {
            return -1;
        }
        return (std::fread(buffer, 1, length, self->file_) == length) ? 0 : -1;
    }
    static int write_thunk(void*, uint64_t, const void*, size_t) { return -1; }
    static int flush_thunk(void*) { return 0; }

    std::FILE* file_;
};

/* Read the allocation table, or explain why there is none. */
bool read_table(const ambre_disk_t& disk, ambre_alloc_table_t* table,
                std::string* error)
{
    int copy_used = -1;
    if (ambre_table_read_best(&disk, table, &copy_used) != AMBRE_OK) {
        *error = "neither allocation-table copy is valid";
        return false;
    }
    return true;
}

}  // namespace

bool load_event_from_disk(const ambre_disk_t& disk, const ambre_ev_desc_t& desc,
                          uint64_t session_id, EventData* out)
{
    uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    uint32_t block = desc.start_block;
    int64_t expect = static_cast<int64_t>(desc.seq_emerg);
    bool found_emerg = false;

    out->event_id = desc.event_id;
    out->order = desc.order;
    out->locked = (desc.locked != 0u);
    out->file_complete = (desc.file_complete != 0u);
    out->trig_reason = desc.trig_reason;
    out->trig_ts_us = desc.trig_ts_us;

    /* The event window is the sequence window of its descriptor: EMERG first,
     * then every CONT / DATA / GAP / SEAL record up to seq_last. A descriptor
     * whose seq_last is below seq_emerg (blocks fetched over IPC) is walked
     * until the chain stops on its own. */
    for (uint32_t step = 0u; step < 64u; step++) {
        ambre_block_t head{};
        uint32_t length = 0u;
        const int rc = ambre_read_record(&disk, desc.start_seg, block,
                                         session_id, expect, &head, payload,
                                         &length);
        if (rc != AMBRE_OK) {
            break;                        /* torn tail: keep what is valid */
        }
        EventRecord record;
        record.seq = head.hdr.seq;
        record.rec_type = head.hdr.rec_type;
        record.first_ts_us = head.hdr.first_ts_us;
        record.payload.assign(reinterpret_cast<const char*>(payload), length);
        if (record.rec_type == (uint8_t)AMBRE_REC_EMERG) {
            found_emerg = true;
        }
        out->records.push_back(std::move(record));
        if (desc.seq_last >= desc.seq_emerg && head.hdr.seq >= desc.seq_last) {
            break;
        }
        block += head.hdr.chunk_count;
        expect = static_cast<int64_t>(head.hdr.seq) + 1;
    }
    return found_emerg;
}

bool list_events(const std::string& image_path,
                 std::vector<ambre_ev_desc_t>* out, std::string* error)
{
    FileDisk file(image_path);
    if (!file.ok()) {
        *error = "cannot open journal image " + image_path;
        return false;
    }
    const ambre_disk_t disk = file.vtable();
    ambre_alloc_table_t table{};
    if (!read_table(disk, &table, error)) {
        *error = image_path + ": " + *error;
        return false;
    }
    out->clear();
    for (uint32_t i = 0u; i < AMBRE_MAX_EVENTS; i++) {
        if (table.events[i].state != (uint8_t)AMBRE_EV_EMPTY) {
            out->push_back(table.events[i]);
        }
    }
    return true;
}

bool load_event(const std::string& image_path, const ambre_ev_desc_t& desc,
                EventData* out, std::string* error)
{
    FileDisk file(image_path);
    if (!file.ok()) {
        *error = "cannot open journal image " + image_path;
        return false;
    }
    const ambre_disk_t disk = file.vtable();
    ambre_alloc_table_t table{};
    if (!read_table(disk, &table, error)) {
        *error = image_path + ": " + *error;
        return false;
    }
    if (!load_event_from_disk(disk, desc, table.session_id, out)) {
        *error = "event " + std::to_string(desc.event_id) +
                 " has no valid EMERG record";
        return false;
    }
    return true;
}

int MemoryDisk::read_thunk(void* ctx, uint64_t offset, void* buffer,
                           size_t length)
{
    MemoryDisk* self = static_cast<MemoryDisk*>(ctx);
    if (offset > self->image_.size() ||
        length > self->image_.size() - static_cast<size_t>(offset)) {
        return -1;
    }
    std::memcpy(buffer, self->image_.data() + offset, length);
    return 0;
}

int MemoryDisk::write_thunk(void*, uint64_t, const void*, size_t)
{
    return -1;
}

int MemoryDisk::flush_thunk(void*)
{
    return 0;
}

ambre_disk_t MemoryDisk::vtable()
{
    ambre_disk_t disk{};
    std::memset(&disk, 0, sizeof(disk));
    disk.ctx = this;
    disk.size = image_.size();
    disk.sector_size = AMBRE_BLOCK_SIZE;
    disk.read = &MemoryDisk::read_thunk;
    disk.write = &MemoryDisk::write_thunk;
    disk.flush = &MemoryDisk::flush_thunk;
    disk.mark = nullptr;
    return disk;
}

}  // namespace archive
}  // namespace ambre
