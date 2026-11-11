/*
 * mcap_writer.hpp — minimal MCAP writer, C++17, standard library only.
 *
 * Implements the subset of https://mcap.dev/spec that the Ambre export needs
 * (docs/FORMAT.md section 12): header, schemas, channels, messages, chunks with
 * CRC, DataEnd, a summary section (schema, channel, statistics, message index,
 * chunk index, metadata index, summary offsets) and the footer.
 *
 * This is the target-side implementation: it runs on QNX 8.0, where the MCAP
 * library is not available (feasibility essay E2, fallback "write a minimal
 * MCAP generator"). The host side keeps its own Python writer
 * (host/mcap/ambre_mcap_writer.py); the two share no code and are cross-checked
 * by the test suite.
 *
 * Framing (spec): a record is opcode (uint8) + length (uint64) + body; strings
 * and byte arrays are prefixed with their uint32 length; maps are prefixed with
 * the uint32 size of their body; everything is little-endian.
 *
 * CRC convention, identical to the host reader that validates this project:
 *   - Chunk.uncompressed_crc covers the uncompressed records field;
 *   - DataEnd.data_section_crc covers the bytes between the leading magic and
 *     the DataEnd record header (its opcode and length included);
 *   - Footer.summary_crc covers the summary records, from the first one up to
 *     (excluding) the first SummaryOffset record.
 * CRC-32C (Castagnoli) is used: the routine of fmt/ambre_crc32c.c is called
 * through its C interface instead of being re-implemented here.
 *
 * The file is assembled in memory and handed over as a byte string: the caller
 * owns the durability policy (temporary name, fsync, rename, directory fsync,
 * section 4.7 of the cahier des charges).
 */

#ifndef AMBRE_MCAP_WRITER_HPP
#define AMBRE_MCAP_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ambre {
namespace mcap {

/* Map used by the channel and the metadata records. */
using StringMap = std::map<std::string, std::string>;

/* Record opcodes (spec, record types). */
enum Opcode : uint8_t {
    OP_HEADER         = 0x01,
    OP_FOOTER         = 0x02,
    OP_SCHEMA         = 0x03,
    OP_CHANNEL        = 0x04,
    OP_MESSAGE        = 0x05,
    OP_CHUNK          = 0x06,
    OP_MESSAGE_INDEX  = 0x07,
    OP_CHUNK_INDEX    = 0x08,
    OP_STATISTICS     = 0x09,
    OP_METADATA       = 0x0C,
    OP_METADATA_INDEX = 0x0D,
    OP_SUMMARY_OFFSET = 0x0E,
    OP_DATA_END       = 0x0F
};

/* The eight bytes every MCAP file starts and ends with. */
extern const char kMagic[8];

class Writer {
 public:
    explicit Writer(std::string profile, std::string library,
                    size_t chunk_size = 512u * 1024u);

    uint16_t add_schema(const std::string& name, const std::string& encoding,
                        const std::string& data);
    uint16_t add_channel(const std::string& topic,
                         const std::string& message_encoding, uint16_t schema_id,
                         const StringMap& metadata = StringMap());
    void add_metadata(const std::string& name, const StringMap& metadata);
    /* Append one message to the open chunk (publish_time = log_time). */
    void add_message(uint16_t channel_id, uint64_t log_time_ns,
                     const std::string& data);

    /* Serialise the whole file: data section, summary section, footer. */
    std::string encode();

    uint64_t message_count() const { return message_count_; }
    uint32_t chunk_count() const { return chunk_count_; }

 private:
    struct IndexEntry {
        uint64_t log_time;
        uint64_t offset;              /* inside the chunk records field */
    };
    struct Chunk {
        std::string records;
        uint64_t start_time = 0;
        uint64_t end_time = 0;
        bool timed = false;
        std::map<uint16_t, std::vector<IndexEntry>> index;
    };
    struct ChunkPlan {
        std::string records;
        uint64_t start_time = 0;
        uint64_t end_time = 0;
        std::map<uint16_t, std::vector<IndexEntry>> index;
        uint64_t file_offset = 0;     /* absolute offset of the Chunk record */
        uint64_t length = 0;          /* Chunk record size, header included  */
    };
    struct Group {
        uint8_t opcode;
        uint64_t start;
        uint64_t length;
    };

    void close_chunk();
    void append_data_section(std::string* file,
                             std::vector<uint64_t>* meta_offsets,
                             std::vector<uint64_t>* meta_lengths);
    std::string statistics_body() const;
    std::string build_summary(uint64_t summary_start,
                              const std::vector<uint64_t>& meta_offsets,
                              const std::vector<uint64_t>& meta_lengths,
                              std::vector<Group>* groups) const;
    std::string build_chunk_indexes(uint64_t summary_start, size_t summary_len,
                                    std::string* index_records) const;
    static std::string chunk_body(const ChunkPlan& plan);
    static std::string chunk_index_body(const ChunkPlan& plan,
                                        const std::map<uint16_t, uint64_t>& offsets,
                                        uint64_t index_length);

    std::string profile_;
    std::string library_;
    size_t chunk_size_;
    std::vector<std::pair<uint16_t, std::string>> schemas_;
    std::vector<std::pair<uint16_t, std::string>> channels_;
    std::vector<std::pair<std::string, std::string>> metadata_;
    std::map<uint16_t, uint32_t> sequences_;
    std::map<uint16_t, uint64_t> counts_;
    std::vector<ChunkPlan> chunks_;
    Chunk open_;
    uint64_t message_count_ = 0;
    uint32_t chunk_count_ = 0;
    uint64_t first_time_ = 0;
    uint64_t last_time_ = 0;
    bool timed_ = false;
};

}  // namespace mcap
}  // namespace ambre

#endif /* AMBRE_MCAP_WRITER_HPP */
