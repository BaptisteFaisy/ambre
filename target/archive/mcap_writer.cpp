/*
 * mcap_writer.cpp — minimal MCAP writer (implementation).
 *
 * The record codec lives here and only here: schemas, channels, messages,
 * chunks, indexes, statistics and the footer are assembled in three passes
 * (data section, summary section, summary offsets), then concatenated between
 * the two magic markers.
 */

#include "mcap_writer.hpp"

extern "C" {
#include "../../fmt/ambre_crc32c.h"
}

#include <utility>

namespace ambre {
namespace mcap {

const char kMagic[8] = {'\x89', 'M', 'C', 'A', 'P', '0', '\r', '\n'};

namespace {

const size_t kMagicLen = sizeof(kMagic);

void put_u8(std::string& out, uint8_t value)
{
    out.push_back(static_cast<char>(value));
}

void put_u16(std::string& out, uint16_t value)
{
    put_u8(out, static_cast<uint8_t>(value & 0xFFu));
    put_u8(out, static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(std::string& out, uint32_t value)
{
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        put_u8(out, static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

void put_u64(std::string& out, uint64_t value)
{
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
        put_u8(out, static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

void put_string(std::string& out, const std::string& text)
{
    put_u32(out, static_cast<uint32_t>(text.size()));
    out.append(text);
}

void put_string_map(std::string& out, const StringMap& map)
{
    std::string body;
    for (const auto& entry : map) {
        put_string(body, entry.first);
        put_string(body, entry.second);
    }
    put_u32(out, static_cast<uint32_t>(body.size()));
    out.append(body);
}

void put_u16_map(std::string& out, const std::map<uint16_t, uint64_t>& map)
{
    std::string body;
    for (const auto& entry : map) {
        put_u16(body, entry.first);
        put_u64(body, entry.second);
    }
    put_u32(out, static_cast<uint32_t>(body.size()));
    out.append(body);
}

void put_record(std::string& out, uint8_t opcode, const std::string& body)
{
    put_u8(out, opcode);
    put_u64(out, static_cast<uint64_t>(body.size()));
    out.append(body);
}

uint32_t crc32c(const std::string& data)
{
    return ambre_crc32c(0u, data.data(), data.size());
}

/* Message record body: channel, sequence, log and publish time, payload. */
std::string message_body(uint16_t channel_id, uint32_t sequence,
                         uint64_t log_time_ns, const std::string& data)
{
    std::string body;
    put_u16(body, channel_id);
    put_u32(body, sequence);
    put_u64(body, log_time_ns);
    put_u64(body, log_time_ns);
    put_string(body, data);
    return body;
}

}  // namespace

Writer::Writer(std::string profile, std::string library, size_t chunk_size)
    : profile_(std::move(profile)), library_(std::move(library)),
      chunk_size_(chunk_size == 0u ? 1u : chunk_size)
{
}

uint16_t Writer::add_schema(const std::string& name, const std::string& encoding,
                            const std::string& data)
{
    uint16_t id = static_cast<uint16_t>(schemas_.size() + 1u);
    std::string body;
    put_u16(body, id);
    put_string(body, name);
    put_string(body, encoding);
    put_string(body, data);
    schemas_.emplace_back(id, body);
    return id;
}

uint16_t Writer::add_channel(const std::string& topic,
                             const std::string& message_encoding,
                             uint16_t schema_id, const StringMap& metadata)
{
    uint16_t id = static_cast<uint16_t>(channels_.size() + 1u);
    std::string body;
    put_u16(body, id);
    put_u16(body, schema_id);
    put_string(body, topic);
    put_string(body, message_encoding);
    put_string_map(body, metadata);
    channels_.emplace_back(id, body);
    sequences_[id] = 0u;
    counts_[id] = 0u;
    return id;
}

void Writer::add_metadata(const std::string& name, const StringMap& metadata)
{
    std::string body;
    put_string(body, name);
    put_string_map(body, metadata);
    metadata_.emplace_back(name, body);
}

void Writer::add_message(uint16_t channel_id, uint64_t log_time_ns,
                         const std::string& data)
{
    std::string record;
    put_record(record, OP_MESSAGE,
               message_body(channel_id, sequences_[channel_id]++, log_time_ns,
                            data));
    const uint64_t offset = static_cast<uint64_t>(open_.records.size());
    open_.index[channel_id].push_back(IndexEntry{log_time_ns, offset});
    open_.records.append(record);
    if (!open_.timed) {
        open_.start_time = log_time_ns;
        open_.end_time = log_time_ns;
        open_.timed = true;
    } else if (log_time_ns < open_.start_time) {
        open_.start_time = log_time_ns;
    } else if (log_time_ns > open_.end_time) {
        open_.end_time = log_time_ns;
    }
    if (!timed_) {
        first_time_ = log_time_ns;
        last_time_ = log_time_ns;
        timed_ = true;
    } else if (log_time_ns < first_time_) {
        first_time_ = log_time_ns;
    } else if (log_time_ns > last_time_) {
        last_time_ = log_time_ns;
    }
    counts_[channel_id]++;
    message_count_++;
    if (open_.records.size() >= chunk_size_) {
        close_chunk();
    }
}

void Writer::close_chunk()
{
    if (open_.records.empty()) {
        return;
    }
    ChunkPlan plan;
    plan.records = std::move(open_.records);
    plan.start_time = open_.start_time;
    plan.end_time = open_.end_time;
    plan.index = std::move(open_.index);
    chunks_.push_back(std::move(plan));
    chunk_count_++;
    open_ = Chunk();
}

std::string Writer::chunk_body(const ChunkPlan& plan)
{
    std::string body;
    put_u64(body, plan.start_time);
    put_u64(body, plan.end_time);
    put_u64(body, static_cast<uint64_t>(plan.records.size()));
    put_u32(body, crc32c(plan.records));
    put_string(body, "");                       /* no compression */
    put_string(body, plan.records);
    return body;
}

std::string Writer::chunk_index_body(const ChunkPlan& plan,
                                     const std::map<uint16_t, uint64_t>& offsets,
                                     uint64_t index_length)
{
    std::string body;
    put_u64(body, plan.start_time);
    put_u64(body, plan.end_time);
    put_u64(body, plan.file_offset);
    put_u64(body, plan.length);
    put_u16_map(body, offsets);
    put_u64(body, index_length);
    put_string(body, "");                       /* no compression */
    put_u64(body, static_cast<uint64_t>(plan.records.size()));
    put_u64(body, static_cast<uint64_t>(plan.records.size()));
    return body;
}

std::string Writer::statistics_body() const
{
    std::string body;
    put_u64(body, message_count_);
    put_u16(body, static_cast<uint16_t>(schemas_.size()));
    put_u32(body, static_cast<uint32_t>(channels_.size()));
    put_u32(body, 0u);                          /* attachments */
    put_u32(body, static_cast<uint32_t>(metadata_.size()));
    put_u32(body, chunk_count_);
    put_u64(body, first_time_);
    put_u64(body, last_time_);
    put_u16_map(body, counts_);
    return body;
}

/* Data section: magic, header, schemas, channels, metadata, chunks. The
 * absolute offsets of the metadata and of each chunk are reported so that the
 * summary section can index them. */
void Writer::append_data_section(std::string* file,
                                 std::vector<uint64_t>* meta_offsets,
                                 std::vector<uint64_t>* meta_lengths)
{
    file->assign(kMagic, kMagicLen);
    std::string header;
    put_string(header, profile_);
    put_string(header, library_);
    put_record(*file, OP_HEADER, header);
    for (const auto& schema : schemas_) {
        put_record(*file, OP_SCHEMA, schema.second);
    }
    for (const auto& channel : channels_) {
        put_record(*file, OP_CHANNEL, channel.second);
    }
    for (const auto& entry : metadata_) {
        meta_offsets->push_back(file->size());
        meta_lengths->push_back(entry.second.size() + 9u);
        put_record(*file, OP_METADATA, entry.second);
    }
    for (auto& chunk : chunks_) {
        chunk.file_offset = file->size();
        put_record(*file, OP_CHUNK, chunk_body(chunk));
        chunk.length = file->size() - chunk.file_offset;
    }
}

/* Message indexes of every chunk, then the chunk index records that point at
 * them. `index_records` receives the message index records. */
std::string Writer::build_chunk_indexes(uint64_t summary_start,
                                        size_t summary_len,
                                        std::string* index_records) const
{
    std::string chunk_indexes;
    for (const auto& chunk : chunks_) {
        std::map<uint16_t, uint64_t> offsets;
        const size_t before = index_records->size();
        for (const auto& entry : chunk.index) {
            std::string body;
            put_u16(body, entry.first);
            for (const auto& item : entry.second) {
                put_u64(body, item.log_time);
                put_u64(body, item.offset);
            }
            offsets[entry.first] = summary_start + summary_len +
                                   index_records->size();
            put_record(*index_records, OP_MESSAGE_INDEX, body);
        }
        std::string body = chunk_index_body(
            chunk, offsets, index_records->size() - before);
        put_record(chunk_indexes, OP_CHUNK_INDEX, body);
    }
    return chunk_indexes;
}

std::string Writer::build_summary(uint64_t summary_start,
                                  const std::vector<uint64_t>& meta_offsets,
                                  const std::vector<uint64_t>& meta_lengths,
                                  std::vector<Group>* groups) const
{
    std::string summary;
    auto add_group = [&groups, &summary, summary_start](uint8_t opcode,
                                                        const std::string& body) {
        if (body.empty()) {
            return;
        }
        groups->push_back(Group{opcode, summary_start + summary.size(),
                                static_cast<uint64_t>(body.size())});
        summary.append(body);
    };

    std::string block;
    for (const auto& schema : schemas_) {
        put_record(block, OP_SCHEMA, schema.second);
    }
    add_group(OP_SCHEMA, block);
    block.clear();
    for (const auto& channel : channels_) {
        put_record(block, OP_CHANNEL, channel.second);
    }
    add_group(OP_CHANNEL, block);
    block.clear();
    put_record(block, OP_STATISTICS, statistics_body());
    add_group(OP_STATISTICS, block);

    std::string index_records;
    const std::string chunk_indexes =
        build_chunk_indexes(summary_start, summary.size(), &index_records);
    summary.append(index_records);
    add_group(OP_CHUNK_INDEX, chunk_indexes);

    block.clear();
    for (size_t i = 0; i < metadata_.size(); i++) {
        std::string body;
        put_u64(body, meta_offsets[i]);
        put_u64(body, meta_lengths[i]);
        put_string(body, metadata_[i].first);
        put_record(block, OP_METADATA_INDEX, body);
    }
    add_group(OP_METADATA_INDEX, block);
    return summary;
}

std::string Writer::encode()
{
    close_chunk();

    std::vector<uint64_t> meta_offsets;
    std::vector<uint64_t> meta_lengths;
    std::string file;
    append_data_section(&file, &meta_offsets, &meta_lengths);

    /* DataEnd: its CRC covers the data section and its own record header. */
    std::string covered = file.substr(kMagicLen);
    put_u8(covered, OP_DATA_END);
    put_u64(covered, 4u);
    std::string data_end_body;
    put_u32(data_end_body, crc32c(covered));
    put_record(file, OP_DATA_END, data_end_body);

    const uint64_t summary_start = file.size();
    std::vector<Group> groups;
    const std::string summary = build_summary(summary_start, meta_offsets,
                                              meta_lengths, &groups);
    std::string offsets;
    for (const auto& group : groups) {
        std::string body;
        put_u8(body, group.opcode);
        put_u64(body, group.start);
        put_u64(body, group.length);
        put_record(offsets, OP_SUMMARY_OFFSET, body);
    }

    std::string footer_body;
    put_u64(footer_body, summary_start);
    put_u64(footer_body, summary_start + summary.size());
    put_u32(footer_body, crc32c(summary));
    file.append(summary);
    file.append(offsets);
    put_record(file, OP_FOOTER, footer_body);
    file.append(kMagic, kMagicLen);
    return file;
}

}  // namespace mcap
}  // namespace ambre
