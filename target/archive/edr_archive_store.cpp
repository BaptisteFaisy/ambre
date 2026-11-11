/*
 * edr_archive_store.cpp — durable archive storage: atomic write, re-read
 * verification, retention index and pruning.
 *
 * Sequence imposed by the cahier des charges section 4.7 (and REP-07):
 *   write under a temporary name -> flush -> rename -> flush the directory.
 * The journal segment is only released after the written file has been read
 * back successfully, which is what verify_mcap_file() establishes: the magic
 * at both ends, the footer, the summary CRC and every chunk CRC must hold.
 */

#define _POSIX_C_SOURCE 200809L

#include "edr_archive_internal.hpp"

#include "ambre_log.hpp"
#include "edr_mcap_map.hpp"
#include "mcap_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "../../fmt/ambre_crc32c.h"
}

namespace ambre {
namespace archive {

namespace {

const char* kLogTag = "edr-archive";
const char* kIndexName = "archive.index";

uint32_t read_u32(const std::string& data, size_t offset)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1]))
            << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2]))
            << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3]))
            << 24);
}

uint64_t read_u64(const std::string& data, size_t offset)
{
    uint64_t value = 0u;
    for (unsigned i = 0; i < 8u; i++) {
        value |= static_cast<uint64_t>(
                     static_cast<unsigned char>(data[offset + i]))
                 << (8u * i);
    }
    return value;
}

bool read_file(const std::string& path, std::string* out)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    out->clear();
    char buffer[65536];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0u) {
        out->append(buffer, got);
    }
    std::fclose(file);
    return true;
}

/* Flush a directory so that the rename itself is durable (section 4.7). */
bool flush_directory(const std::string& dir)
{
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return false;
    }
    const bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

std::string directory_of(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string(".")
                                        : path.substr(0u, slash);
}

/* Footer of an MCAP file: its offsets and the CRC of the summary section. */
bool check_footer(const std::string& data, uint64_t* summary_start,
                  std::string* error)
{
    const size_t magic_len = 8u;
    if (data.size() < 2u * magic_len + 29u ||
        std::memcmp(data.data(), mcap::kMagic, magic_len) != 0 ||
        std::memcmp(data.data() + data.size() - magic_len, mcap::kMagic,
                    magic_len) != 0) {
        *error = "missing MCAP magic at the start or the end of the file";
        return false;
    }
    const size_t footer_offset = data.size() - magic_len - 29u;
    if (static_cast<unsigned char>(data[footer_offset]) != mcap::OP_FOOTER) {
        *error = "last record before the trailing magic is not a Footer";
        return false;
    }
    *summary_start = read_u64(data, footer_offset + 9u);
    const uint64_t offset_start = read_u64(data, footer_offset + 17u);
    const uint32_t summary_crc = read_u32(data, footer_offset + 25u);
    if (*summary_start == 0u || offset_start > data.size() ||
        *summary_start > offset_start) {
        *error = "footer points outside the file (no summary section?)";
        return false;
    }
    const std::string summary = data.substr(
        static_cast<size_t>(*summary_start),
        static_cast<size_t>(offset_start - *summary_start));
    if (ambre_crc32c(0u, summary.data(), summary.size()) != summary_crc) {
        *error = "summary CRC mismatch";
        return false;
    }
    return true;
}

/* One Chunk record: its declared size and its CRC over the records field. */
bool check_chunk(const std::string& data, size_t body, size_t data_end,
                 std::string* error)
{
    const uint32_t stored_crc = read_u32(data, body + 24u);
    const uint64_t declared = read_u64(data, body + 16u);
    const size_t compression_len = read_u32(data, body + 28u);
    const size_t length_at = body + 28u + 4u + compression_len;
    if (length_at + 4u > data_end) {
        *error = "chunk header truncated";
        return false;
    }
    const size_t records_len = read_u32(data, length_at);
    const size_t records_at = length_at + 4u;
    if (records_at + records_len > data_end ||
        static_cast<uint64_t>(records_len) != declared) {
        *error = "chunk records field has an inconsistent size";
        return false;
    }
    if (ambre_crc32c(0u, data.data() + records_at, records_len) != stored_crc) {
        *error = "chunk CRC mismatch";
        return false;
    }
    return true;
}

/* Walk the data section: every record must fit, every chunk must be intact. */
bool check_data_section(const std::string& data, size_t data_end,
                        std::string* error)
{
    size_t pos = 8u;
    uint32_t chunks = 0u;
    while (pos + 9u <= data_end) {
        const uint8_t opcode = static_cast<unsigned char>(data[pos]);
        const uint64_t length = read_u64(data, pos + 1u);
        const size_t body = pos + 9u;
        if (body + length > data_end) {
            *error = "record overruns the data section";
            return false;
        }
        if (opcode == mcap::OP_CHUNK) {
            if (!check_chunk(data, body, data_end, error)) {
                return false;
            }
            chunks++;
        }
        pos = body + static_cast<size_t>(length);
    }
    if (chunks == 0u) {
        *error = "no chunk in the archive";
        return false;
    }
    return true;
}

bool check_mcap_bytes(const std::string& data, std::string* error)
{
    uint64_t summary_start = 0u;
    if (!check_footer(data, &summary_start, error)) {
        return false;
    }
    return check_data_section(data, static_cast<size_t>(summary_start), error);
}

}  // namespace

bool write_atomic(const std::string& target_path, const std::string& content,
                  std::string* error)
{
    const std::string dir = directory_of(target_path);
    const std::string temporary = target_path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) {
        *error = "cannot create " + temporary;
        return false;
    }
    const size_t written =
        std::fwrite(content.data(), 1, content.size(), file);
    const bool flushed = (std::fflush(file) == 0);
    const int fd = ::fileno(file);
    const bool synced = (fd >= 0) && (::fsync(fd) == 0);
    std::fclose(file);
    if (written != content.size() || !flushed || !synced) {
        *error = "cannot write " + temporary;
        (void)::unlink(temporary.c_str());
        return false;
    }
    if (::rename(temporary.c_str(), target_path.c_str()) != 0) {
        *error = "cannot rename " + temporary + " to " + target_path;
        (void)::unlink(temporary.c_str());
        return false;
    }
    if (!flush_directory(dir)) {
        *error = "cannot flush directory " + dir;
        return false;
    }
    return true;
}

bool verify_mcap_file(const std::string& path, std::string* error)
{
    std::string data;
    if (!read_file(path, &data)) {
        *error = "cannot read back " + path;
        return false;
    }
    if (!check_mcap_bytes(data, error)) {
        *error = path + ": " + *error;
        return false;
    }
    return true;
}

std::string event_file_name(uint32_t event_id, uint32_t order)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "event-%06u-%06u.mcap",
                  static_cast<unsigned>(order), static_cast<unsigned>(event_id));
    return std::string(buffer);
}

std::string index_file_name()
{
    return std::string(kIndexName);
}

bool index_read(const std::string& dir, std::vector<ArchiveEntry>* out)
{
    std::string data;
    if (!read_file(dir + "/" + kIndexName, &data)) {
        return false;
    }
    out->clear();
    size_t pos = 0u;
    while (pos + 20u <= data.size()) {
        ArchiveEntry entry;
        entry.order = read_u32(data, pos);
        entry.event_id = read_u32(data, pos + 4u);
        entry.locked = (static_cast<unsigned char>(data[pos + 8u]) != 0u);
        const size_t name_len = static_cast<unsigned char>(data[pos + 9u]);
        if (pos + 10u + name_len > data.size()) {
            return false;
        }
        entry.file = data.substr(pos + 10u, name_len);
        out->push_back(entry);
        pos += 10u + name_len;
    }
    return true;
}

bool index_write(const std::string& dir, const std::vector<ArchiveEntry>& entries,
                 std::string* error)
{
    std::string data;
    for (const ArchiveEntry& entry : entries) {
        char header[10];
        std::memset(header, 0, sizeof(header));
        for (unsigned i = 0; i < 4u; i++) {
            header[i] = static_cast<char>((entry.order >> (8u * i)) & 0xFFu);
            header[4u + i] = static_cast<char>((entry.event_id >> (8u * i)) & 0xFFu);
        }
        header[8] = entry.locked ? '\1' : '\0';
        header[9] = static_cast<char>(entry.file.size() & 0xFFu);
        data.append(header, sizeof(header));
        data.append(entry.file);
    }
    return write_atomic(dir + "/" + kIndexName, data, error);
}

/* Rebuild the index from the directory when the index file is missing. */
bool scan_archive_dir(const std::string& dir, std::vector<ArchiveEntry>* out)
{
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
        return false;
    }
    struct dirent* item;
    while ((item = ::readdir(handle)) != nullptr) {
        const std::string name(item->d_name);
        if (name.size() > 5u &&
            name.compare(name.size() - 5u, 5u, ".mcap") == 0) {
            ArchiveEntry entry;
            entry.file = name;
            entry.order = static_cast<uint32_t>(out->size());
            out->push_back(entry);
        }
    }
    ::closedir(handle);
    std::sort(out->begin(), out->end(),
              [](const ArchiveEntry& a, const ArchiveEntry& b) {
                  return a.file < b.file;
              });
    return true;
}

size_t apply_retention(const std::string& dir, size_t keep, std::string* error)
{
    std::vector<ArchiveEntry> entries;
    if (!index_read(dir, &entries) && !scan_archive_dir(dir, &entries)) {
        *error = "cannot list " + dir;
        return 0u;
    }
    if (entries.size() <= keep) {
        return 0u;
    }
    std::sort(entries.begin(), entries.end(),
              [](const ArchiveEntry& a, const ArchiveEntry& b) {
                  return a.order < b.order;
              });
    size_t removed = 0u;
    std::vector<ArchiveEntry> kept;
    for (const ArchiveEntry& entry : entries) {
        const bool surplus = (entries.size() - removed) > keep;
        if (surplus && !entry.locked &&
            ::unlink((dir + "/" + entry.file).c_str()) == 0) {
            removed++;
            continue;
        }
        kept.push_back(entry);
    }
    if (removed > 0u && !index_write(dir, kept, error)) {
        return removed;
    }
    return removed;
}

bool already_archived(const std::vector<ArchiveEntry>& index, uint32_t event_id)
{
    for (const ArchiveEntry& entry : index) {
        if (entry.event_id == event_id) {
            return true;
        }
    }
    return false;
}

bool store_event(const std::string& out_dir, const EventData& event,
                 const std::string& source_sha256, uint64_t source_bytes,
                 std::vector<ArchiveEntry>* index, std::string* error)
{
    const std::string bytes =
        build_event_mcap(event, source_sha256, source_bytes);
    const std::string name = event_file_name(event.event_id, event.order);
    const std::string target = out_dir + "/" + name;

    if (!write_atomic(target, bytes, error)) {
        return false;
    }
    /* REP-07: the event counts as archived only once the file reads back. */
    if (!verify_mcap_file(target, error)) {
        (void)::unlink(target.c_str());
        return false;
    }

    ArchiveEntry entry;
    entry.event_id = event.event_id;
    entry.order = event.order;
    entry.locked = event.locked;
    entry.file = name;
    for (ArchiveEntry& existing : *index) {
        if (existing.event_id == entry.event_id) {
            existing = entry;
            entry.event_id = 0u;              /* already replaced */
            break;
        }
    }
    if (entry.event_id != 0u) {
        index->push_back(entry);
    }
    if (!index_write(out_dir, *index, error)) {
        return false;
    }

    (void)apply_retention(out_dir, kMinRetainedEvents, error);
    (void)index_read(out_dir, index);
    log_info(kLogTag, "archived event %u -> %s (%u bytes)", event.event_id,
             target.c_str(), static_cast<unsigned>(bytes.size()));
    return true;
}

}  // namespace archive
}  // namespace ambre
