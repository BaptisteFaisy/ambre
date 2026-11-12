/*
 * edr_archive_ipc.cpp — client side of the journal service, for edr-archive.
 *
 * The archive never opens the raw partition for writing and never re-implements
 * the journal format: it asks edr-journal for the raw blocks of a sealed event
 * (AMBRE_MSG_EXPORT, paginated) and reassembles them with fmt/.
 *
 * TRANSPORT. Every request goes through the portability layer
 * (target/port/ambre_port.h): ambre_port_conn_request() is one MsgSend() on
 * QNX 8.0 — the reply is part of the transaction and the journal serves it at
 * the caller's priority (QNX-03) — and one send plus one bounded receive on an
 * AF_UNIX SOCK_SEQPACKET connection on the PC, which preserves message
 * boundaries exactly like a QNX message. This file used to carry its own
 * socket client because the frozen port API had no request/reply; that API
 * exists now, so the sockets are gone and the same code builds for the target.
 *
 * Peer identity (SEC-01): the archive announces AMBRE_PEER_ARCHIVE in the
 * header's `reserved` byte, which is what makes edr-journal send it
 * AMBRE_MSG_EVENT_READY and what lets the journal refuse an intruder.
 */

#define _POSIX_C_SOURCE 200809L

#include "edr_archive_internal.hpp"

#include "ambre_log.hpp"

extern "C" {
#include "../ipc/edr_ipc_ext.h"
#include "../journal/edr_journal_svc.h"
#include "../port/ambre_port.h"
}

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>


namespace ambre {
namespace archive {

namespace {

const char* kLogTag = "edr-archive";
const int kTimeoutMs = 2000;
const size_t kPathMax = 104u;

/* One connection to edr-journal, through the portability layer. */
class ServiceLink {
 public:
    ServiceLink() = default;
    ~ServiceLink() { close(); }
    ServiceLink(const ServiceLink&) = delete;
    ServiceLink& operator=(const ServiceLink&) = delete;

    bool open(const std::string& path, std::string* error)
    {
        if (path.size() >= kPathMax) {
            *error = "service path too long: " + path;
            return false;
        }
        if (ambre_port_conn_open(path.c_str(), &conn_) != AMBRE_PORT_OK) {
            *error = "cannot connect to " + path;
            conn_ = nullptr;
            return false;
        }
        return true;
    }

    void close()
    {
        if (conn_ != nullptr) {
            ambre_port_conn_close(conn_);
            conn_ = nullptr;
        }
    }

    bool connected() const { return conn_ != nullptr; }

    /* One round trip; the answer must carry the expected type. */
    int request(ambre_ipc_msg_t type, const void* body, uint32_t body_len,
                uint8_t expected, uint8_t* reply, size_t capacity,
                std::string* error)
    {
        uint8_t buffer[AMBRE_IPC_FRAME_MAX];
        const int total = ambre_ipc_build(buffer, sizeof(buffer), type, body,
                                          body_len);
        if (total <= 0) {
            *error = "request does not fit the frame ceiling";
            return -1;
        }
        /* The peer type travels in the spare header byte (PC harness); on QNX
         * the kernel-provided credentials win (SEC-01). */
        buffer[7] = static_cast<uint8_t>(AMBRE_PEER_ARCHIVE);
        const int got = ambre_port_conn_request(
            conn_, buffer, static_cast<size_t>(total), reply, capacity,
            static_cast<uint64_t>(kTimeoutMs) * 1000u, nullptr);
        if (got < 0) {
            *error = transport_error(got);
            return -1;
        }
        /* The transport returns a status (0 = EOK), never a byte count: the
         * reply is self-describing, so read its length from the header. */
        const size_t len = ambre_ipc_frame_len(reply);
        if (len == 0u) {
            *error = "malformed reply from edr-journal";
            return -1;
        }
        return check_reply(reply, static_cast<int>(len), expected, error);
    }

 private:
    static const char* transport_error(int rc)
    {
        if (rc == AMBRE_PORT_ERR_TIMEOUT) return "timeout waiting for edr-journal";
        if (rc == 0) return "connection closed by edr-journal";
        return "transport error talking to edr-journal";
    }

    int check_reply(uint8_t* reply, int got, uint8_t expected,
                    std::string* error)
    {
        if (ambre_ipc_frame_check(reply, static_cast<size_t>(got)) != AMBRE_OK) {
            *error = "malformed reply from edr-journal";
            return -1;
        }
        const uint8_t reply_type = reply[5];
        if (reply_type == static_cast<uint8_t>(AMBRE_MSG_NAK)) {
            ambre_edr_nak_body_t nak{};
            std::memcpy(&nak, reply + AMBRE_IPC_HDR_SIZE, sizeof(nak));
            *error = "refused by edr-journal (code " + std::to_string(nak.code) + ")";
            return -1;
        }
        if (reply_type != expected) {
            *error = "unexpected reply type " + std::to_string(reply_type);
            return -1;
        }
        return got;
    }

    ambre_port_conn_t* conn_ = nullptr;
};

/* Copy a packed struct out of a reply body (never a pointer cast). */
template <typename T>
T body_as(const uint8_t* reply)
{
    T value;
    std::memcpy(&value, reply + AMBRE_IPC_HDR_SIZE, sizeof(value));
    return value;
}

/* Rebuild the event from the raw blocks the journal sent. The blocks are laid
 * out as segment 0 of a memory image, so the fmt/ walk can address them with
 * the same block offsets it uses on a real partition. */
bool load_from_blocks(const std::vector<uint8_t>& blocks, EventData* out)
{
    if (blocks.size() < AMBRE_BLOCK_SIZE) {
        return false;
    }
    std::vector<uint8_t> image(AMBRE_FIRST_DATA_OFFSET + blocks.size(), 0u);
    std::memcpy(image.data() + AMBRE_FIRST_DATA_OFFSET, blocks.data(),
                blocks.size());

    /* The first block carries the session and the first sequence number. */
    ambre_block_t head{};
    ambre_block_unpack(image.data() + AMBRE_FIRST_DATA_OFFSET, &head);
    if (ambre_block_validate(&head) != AMBRE_OK) {
        return false;
    }

    ambre_ev_desc_t desc{};
    std::memset(&desc, 0, sizeof(desc));
    desc.event_id = out->event_id;
    desc.order = out->order;
    desc.locked = out->locked ? 1u : 0u;
    desc.file_complete = out->file_complete ? 1u : 0u;
    desc.trig_reason = out->trig_reason;
    desc.trig_ts_us = out->trig_ts_us;
    desc.start_seg = 0u;
    desc.start_block = 0u;
    desc.seq_emerg = head.hdr.seq;
    desc.seq_last = 0u;              /* 0: walk until the chain stops */

    MemoryDisk disk(std::move(image));
    const ambre_disk_t vt = disk.vtable();
    return load_event_from_disk(vt, desc, head.hdr.session_id, out);
}

}  // namespace

bool list_events_service(const std::string& sock_path,
                         std::vector<ambre_ipc_event_line_t>* out,
                         std::string* error)
{
    ServiceLink link;
    if (!link.open(sock_path, error)) {
        return false;
    }
    uint8_t reply[AMBRE_EDR_SVC_REPLY_MAX];
    const int got = link.request(AMBRE_MSG_LIST, nullptr, 0u,
                                 static_cast<uint8_t>(AMBRE_MSG_LIST_REPLY),
                                 reply, sizeof(reply), error);
    if (got < 0) {
        return false;
    }
    const size_t body_len =
        static_cast<size_t>(got) - static_cast<size_t>(AMBRE_IPC_HDR_SIZE);
    if (body_len < sizeof(uint32_t)) {
        *error = "short LIST reply";
        return false;
    }
    ambre_edr_list_body_t list{};
    std::memset(&list, 0, sizeof(list));
    const size_t copy = (body_len < sizeof(list)) ? body_len : sizeof(list);
    std::memcpy(&list, reply + AMBRE_IPC_HDR_SIZE, copy);
    const uint32_t count =
        (list.count > AMBRE_MAX_EVENTS) ? AMBRE_MAX_EVENTS : list.count;
    out->clear();
    for (uint32_t i = 0u; i < count; i++) {
        out->push_back(list.lines[i]);
    }
    return true;
}

static bool fetch_blocks(ServiceLink* link, uint32_t event_id,
                          std::vector<uint8_t>* blocks, std::string* error)
{
    ambre_edr_export_req_t req{};
    req.event_id = event_id;
    req.first_block = 0u;
    uint8_t reply[AMBRE_EDR_SVC_REPLY_MAX];
    uint32_t total_blocks = 1u;
    while (req.first_block < total_blocks) {
        const int got = link->request(AMBRE_MSG_EXPORT, &req, sizeof(req),
                                      AMBRE_EDR_REPLY_EXPORT, reply,
                                      sizeof(reply), error);
        if (got < 0) {
            return false;
        }
        const ambre_edr_export_body_t head =
            body_as<ambre_edr_export_body_t>(reply);
        total_blocks = head.total_blocks;
        if (head.block_count == 0u) {
            break;
        }
        const uint8_t* raw = reply + AMBRE_IPC_HDR_SIZE + sizeof(head);
        blocks->insert(blocks->end(), raw,
                       raw + static_cast<size_t>(head.block_count) *
                                 AMBRE_BLOCK_SIZE);
        req.first_block = head.first_block + head.block_count;
    }
    return true;
}

static bool fill_descriptor(const std::vector<ambre_ipc_event_line_t>& lines,
                            uint32_t event_id, EventData* out,
                            bool* locked_out, std::string* error)
{
    for (const ambre_ipc_event_line_t& line : lines) {
        if (line.event_id == event_id) {
            out->event_id = line.event_id;
            out->order = line.event_id;
            out->locked = (line.locked != 0u);
            out->file_complete = (line.file_complete != 0u);
            out->trig_reason = line.trig_reason;
            out->trig_ts_us = line.trig_ts_us;
            if (locked_out != nullptr) {
                *locked_out = out->locked;
            }
            return true;
        }
    }
    *error = "event " + std::to_string(event_id) + " is not in the journal";
    return false;
}

bool fetch_event(const std::string& sock_path, uint32_t event_id,
                 bool* locked_out, EventData* out, std::string* error)
{
    ServiceLink link;
    if (!link.open(sock_path, error)) {
        return false;
    }
    std::vector<ambre_ipc_event_line_t> lines;
    if (!list_events_service(sock_path, &lines, error)) {
        return false;
    }
    if (!fill_descriptor(lines, event_id, out, locked_out, error)) {
        return false;
    }
    std::vector<uint8_t> blocks;
    if (!fetch_blocks(&link, event_id, &blocks, error)) {
        return false;
    }
    if (!load_from_blocks(blocks, out)) {
        *error = "event " + std::to_string(event_id) +
                 " could not be reassembled from its raw blocks";
        return false;
    }
    return true;
}

/* Service mode: ask the journal what it has sealed, archive it, repeat unless
 * --once was given. GEL-06 is enforced by the journal itself, which answers a
 * NAK to EXPORT while an event is open; the archive then simply waits. */
int run_service(const std::string& sock_path, const std::string& out_dir,
                bool once_mode)
{
    std::string error;
    std::vector<ArchiveEntry> index;
    (void)index_read(out_dir, &index);

    for (int pass = 0; once_mode || pass < 1; pass++) {
        std::vector<ambre_ipc_event_line_t> lines;
        if (!list_events_service(sock_path, &lines, &error)) {
            log_error(kLogTag, "%s", error.c_str());
            return 1;
        }
        for (const ambre_ipc_event_line_t& line : lines) {
            if (line.state != (uint8_t)AMBRE_EV_SEALED) {
                continue;                    /* GEL-06: an event is open */
            }
            if (already_archived(index, line.event_id)) {
                continue;
            }
            EventData event;
            bool locked = false;
            if (!fetch_event(sock_path, line.event_id, &locked, &event,
                             &error)) {
                log_warn(kLogTag, "event %u: %s", line.event_id,
                         error.c_str());
                continue;
            }
            if (!store_event(out_dir, event, "", 0u, &index, &error)) {
                log_error(kLogTag, "event %u: %s", line.event_id,
                         error.c_str());
                return 1;
            }
            log_info(kLogTag, "archived event %u from edr-journal",
                     line.event_id);
        }
        if (once_mode) {
            break;
        }
        (void)ambre_port_sleep_us(200000u);
    }
    return 0;
}

}  // namespace archive
}  // namespace ambre
