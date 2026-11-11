/*
 * test_archive_cpp.cpp — unit and integration tests for the C++17 MCAP
 * archive (target/archive).
 *
 * Cross-checks the C++ implementation against the independent Python host
 * reader and verifier (host/mcap/ambre_mcap_writer.py, host/mcap/export_mcap.py,
 * host/verify/edr_verify.py, tests/mcap_check.py) without duplicating any MCAP
 * validation logic in C++.
 *
 * Exit code = number of failures (0 = green).
 */

#include "ambre_sha256.hpp"
#include "edr_archive.hpp"
#include "mcap_writer.hpp"

#define _Static_assert static_assert
extern "C" {
#include "ambre_fmt.h"
#include "ambre_journal.h"
#include "ambre_port.h"
#include "edr_ipc.h"
#include "edr_ipc_ext.h"
#include "journal/edr_journal_svc.h"
}
#undef _Static_assert

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check_true(bool ok, const char* expr, int line)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        std::fprintf(stderr, "FAIL test_archive_cpp.cpp:%d: %s\n", line, expr);
    }
}

#define CHECK(cond) check_true((cond), #cond, __LINE__)

std::string make_tmpdir()
{
    char tpl[] = "/tmp/ambre-arch-XXXXXX";
    char* dir = ::mkdtemp(tpl);
    if (dir == nullptr) {
        return "/tmp/ambre-arch-fallback";
    }
    return std::string(dir);
}

void remove_tree(const std::string& path)
{
    const std::string cmd = "rm -rf '" + path + "'";
    const int rc = std::system(cmd.c_str());
    check_true(rc == 0, "cleanup of the temporary tree", __LINE__);
}

int run_cmd(const std::string& cmd)
{
    const int rc = std::system(cmd.c_str());
    return (rc == -1) ? -1 : WEXITSTATUS(rc);
}

/* ----------------------------------------------------- synthetic journal -- */

const uint64_t kT0Us = 25000000ULL;
const uint32_t kTestSegments = 2u;

ambre_a_point_t make_point(uint64_t t_us)
{
    ambre_a_point_t p;
    std::memset(&p, 0, sizeof(p));
    p.speed_kmh_x4 = static_cast<uint16_t>(200u + (t_us / 100000u) % 40u);
    p.throttle_pct = 40u;
    p.brake_active = 0u;
    p.engine_rpm = 1500u;
    p.steer_angle_deg_x2 = 10;
    p.accel_long_mm_s2 = 1200;
    p.accel_lat_mm_s2 = -300;
    p.abs_active = 0u;
    p.esc_active = 1u;
    p.belt_driver = 1u;
    p.airbag_lamp = 0u;
    p.yaw_rate_deg_s_x10 = 15;
    p.rel_time_ms = static_cast<uint16_t>((kT0Us - t_us) / 1000u);
    return p;
}

void fill_emerg_event(ambre_emerg_t* em, uint32_t event_id, uint8_t locked)
{
    std::memset(em, 0, sizeof(*em));
    em->event_id = event_id;
    em->trig_reason = AMBRE_TRIG_DV_LONG;
    em->locked = locked;
    em->event_number = 1u;
    em->ignition_cycle = 2u;
    em->t0_ts_us = kT0Us;
    em->trig_ts_us = kT0Us;
    em->crash_order = event_id;
    em->point_count = 11u;
    for (uint32_t i = 0u; i < 11u; i++) {
        const uint64_t rel_ms = static_cast<uint64_t>(5000u - i * 500u);
        em->points[i] = make_point(kT0Us - rel_ms * 1000ULL);
    }
}

/* Class B samples: 1 kHz accelerations, one 100 Hz wheel speed, one 10 Hz
 * status signal -- the three rates of the whitelist (FORMAT.md section 5). */
void append_class_b(ambre_journal_t* j)
{
    ambre_sample_t samples[20];
    const uint16_t sigs[5] = {1u, 2u, 3u, 10u, 40u};
    for (uint32_t i = 0u; i < 20u; i++) {
        std::memset(&samples[i], 0, sizeof(samples[i]));
        samples[i].signal_id = sigs[i % 5u];
        samples[i].value = 1000 + static_cast<int32_t>(i);
    }
    CHECK(ambre_journal_append_data(j, kT0Us - 200000ULL, samples, 20u) ==
          AMBRE_OK);
}

/* Three CONT records (delta-v during the crash) and one capture gap. */
void append_cont_and_gap(ambre_journal_t* j)
{
    for (uint16_t step = 0u; step < 3u; step++) {
        ambre_cont_t cont;
        std::memset(&cont, 0, sizeof(cont));
        cont.event_id = 1u;
        cont.sample_count = 1u;
        cont.rel_time_ms = static_cast<uint16_t>(step * 10u);
        cont.dv_long_mm_s = 500 * static_cast<int32_t>(step + 1u);
        cont.dv_lat_mm_s = -100;
        cont.points[0] = make_point(kT0Us);
        CHECK(ambre_journal_continue(j, &cont) == AMBRE_OK);
    }
    const ambre_gap_t gap{kT0Us + 5000ULL, kT0Us + 8000ULL, 1u};
    CHECK(ambre_journal_append_gap(j, &gap) == AMBRE_OK);
}

void seal_event(ambre_journal_t* j)
{
    ambre_seal_t seal;
    std::memset(&seal, 0, sizeof(seal));
    seal.event_id = 1u;
    seal.file_complete = 1u;
    seal.seal_cause = AMBRE_SEAL_NORMAL;
    seal.locked = 1u;
    seal.event_number = 1u;
    seal.time_since_first_ms = 21u;
    seal.max_dv_long_mm_s = 1500;
    seal.max_dv_long_at_ms = 20u;
    seal.max_dv_lat_mm_s = -100;
    seal.max_dv_lat_at_ms = 0u;
    CHECK(ambre_journal_seal(j, &seal) == AMBRE_OK);
}

/* One event written through the C fmt/ API only; `seal_it` false leaves the
 * event OPEN, which is what GEL-06 forbids archiving. */
bool write_synthetic_journal(const std::string& image_path, bool seal_it = true)
{
    ambre_port_disk_t* pd = nullptr;
    const uint64_t size =
        AMBRE_FIRST_DATA_OFFSET +
        static_cast<uint64_t>(kTestSegments) * AMBRE_SEGMENT_SIZE;
    if (ambre_port_disk_open(image_path.c_str(), size, &pd) != AMBRE_PORT_OK) {
        return false;
    }
    ambre_disk_t disk = *ambre_port_disk_vtable(pd);
    ambre_journal_cfg_t cfg;
    ambre_journal_cfg_default(&cfg);
    cfg.session_id = 0x1234u;
    cfg.segment_limit = kTestSegments;

    ambre_journal_t j;
    bool ok = (ambre_journal_format(&disk, &cfg) == AMBRE_OK) &&
              (ambre_journal_open(&disk, &cfg, false, &j) == AMBRE_OK);

    ambre_emerg_t em;
    fill_emerg_event(&em, 1u, 1u);
    ok = ok && (ambre_journal_trigger(&j, &em) == AMBRE_OK);
    if (ok) {
        append_class_b(&j);
        append_cont_and_gap(&j);
        if (seal_it) {
            seal_event(&j);
        }
    }
    ambre_port_disk_close(pd);
    return ok;
}

bool write_matching_scenario(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "# ambre scenario v1\nname=cpp-test\nseed=1\n"
                    "rate_hz=1000\nduration_ms=30000\ncrash_at_ms=25000\n");
    for (uint64_t t_ms = 0u; t_ms <= 30000u; t_ms++) {
        const uint64_t t_us = t_ms * 1000ULL;
        const uint32_t spd = 200u + static_cast<uint32_t>((t_us / 100000u) % 40u);
        std::fprintf(f, "%llu 1200 -300 %u 40 0 1500 10 0\n",
                     static_cast<unsigned long long>(t_us), spd);
    }
    std::fclose(f);
    return true;
}

/* ------------------------------------------------------------------ tests -- */

void test_sha256_vectors()
{
    CHECK(ambre::sha256::hex_of("", 0u) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ambre::sha256::hex_of("abc", 3u) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ambre::sha256::hex_of("ambre-format-v1", 15u) ==
          "765a86f281d8fe37333f2857b191dd4aaf503f44e706b966e666af9d059d8548");
}

void test_standalone_export_and_python_crosscheck(const std::string& tmp)
{
    const std::string img = tmp + "/journal.img";
    const std::string scen = tmp + "/scenario.txt";
    const std::string out_dir = tmp + "/out";
    const std::string py_mcap = tmp + "/py.mcap";
    CHECK(::mkdir(out_dir.c_str(), 0755) == 0);
    CHECK(write_synthetic_journal(img));
    CHECK(write_matching_scenario(scen));

    CHECK(ambre::archive::run_standalone(img, out_dir, 0u, false) == 0);

    const std::string cpp_mcap =
        out_dir + "/" + ambre::archive::event_file_name(1u, 1u);
    std::string err;
    CHECK(ambre::archive::verify_mcap_file(cpp_mcap, &err));

    /* Cross-check with the independent Python host exporter and edr_verify. */
    CHECK(run_cmd("python3 host/mcap/export_mcap.py --image '" + img +
                  "' --out '" + py_mcap + "' --event-id 1 >/dev/null") == 0);
    CHECK(run_cmd("python3 host/verify/edr_verify.py --mcap '" + cpp_mcap +
                  "' --scenario '" + scen + "' >/dev/null") == 0);
    CHECK(run_cmd("python3 tests/mcap_check.py --mcap '" + cpp_mcap +
                  "' --scenario '" + scen + "' --compare-py '" + py_mcap +
                  "' --expect-points 11") == 0);
}

/* GEL-06: while an event is open, the archive writes nothing at all. */
void test_gel06_no_write_while_event_open(const std::string& tmp)
{
    const std::string img = tmp + "/open.img";
    const std::string out_dir = tmp + "/out_open";
    CHECK(::mkdir(out_dir.c_str(), 0755) == 0);
    CHECK(write_synthetic_journal(img, false));
    CHECK(ambre::archive::run_standalone(img, out_dir, 0u, false) == 0);

    std::vector<ambre::archive::ArchiveEntry> index;
    CHECK(!ambre::archive::index_read(out_dir, &index));
    CHECK(::access((out_dir + "/" +
                    ambre::archive::event_file_name(1u, 1u)).c_str(), F_OK) != 0);
    /* An explicit request is refused too (functional failure, not a crash). */
    CHECK(ambre::archive::run_standalone(img, out_dir, 1u, false) == 1);
}

void test_multichunk_and_corruption_rejected(const std::string& tmp)
{
    ambre::mcap::Writer writer("ambre", "ambre-test", 128u);
    const uint16_t s1 = writer.add_schema("ambre.Test", "jsonschema",
                                          "{\"type\": \"object\"}");
    const uint16_t c1 = writer.add_channel("/edr/a/event", "json", s1);
    for (int i = 0; i < 25; i++) {
        writer.add_message(c1, ambre::archive::kT0OffsetNs +
                                   static_cast<uint64_t>(i) * 1000000ULL,
                           "{\"i\": " + std::to_string(i) + "}");
    }
    const std::string bytes = writer.encode();
    CHECK(writer.chunk_count() > 1u);

    const std::string good = tmp + "/multi.mcap";
    std::string err;
    CHECK(ambre::archive::write_atomic(good, bytes, &err));
    CHECK(ambre::archive::verify_mcap_file(good, &err));

    /* Flip one byte inside the chunk area -> verify_mcap_file must fail. */
    std::string broken = bytes;
    broken[broken.size() / 2u] ^= 0x01;
    const std::string bad = tmp + "/bad.mcap";
    CHECK(ambre::archive::write_atomic(bad, broken, &err));
    CHECK(!ambre::archive::verify_mcap_file(bad, &err));
}

void test_retention_keeps_16_and_locked(const std::string& tmp)
{
    const std::string dir = tmp + "/retention";
    CHECK(::mkdir(dir.c_str(), 0755) == 0);

    std::vector<ambre::archive::ArchiveEntry> entries;
    std::string err;
    for (uint32_t i = 1u; i <= 20u; i++) {
        ambre::archive::ArchiveEntry entry;
        entry.event_id = i;
        entry.order = i;
        entry.locked = (i == 1u || i == 2u);    /* two oldest are locked */
        entry.file = ambre::archive::event_file_name(i, i);
        CHECK(ambre::archive::write_atomic(dir + "/" + entry.file, "x", &err));
        entries.push_back(entry);
    }
    CHECK(ambre::archive::index_write(dir, entries, &err));

    const size_t removed = ambre::archive::apply_retention(
        dir, ambre::archive::kMinRetainedEvents, &err);
    CHECK(removed == 4u);

    std::vector<ambre::archive::ArchiveEntry> kept;
    CHECK(ambre::archive::index_read(dir, &kept));
    CHECK(kept.size() == 16u);
    CHECK(::access((dir + "/" + ambre::archive::event_file_name(1u, 1u)).c_str(),
                   F_OK) == 0);
    CHECK(::access((dir + "/" + ambre::archive::event_file_name(2u, 2u)).c_str(),
                   F_OK) == 0);
    CHECK(::access((dir + "/" + ambre::archive::event_file_name(3u, 3u)).c_str(),
                   F_OK) != 0);
}

/* ------------------------------------------------- IPC service thread --- */

struct ServiceArg {
    ambre_edr_service_t* svc;
    ambre_port_chan_t* chan;
    volatile bool stop;
};

void* service_thread(void* raw)
{
    ServiceArg* arg = static_cast<ServiceArg*>(raw);
    uint8_t msg[AMBRE_IPC_FRAME_MAX];
    uint8_t reply[AMBRE_EDR_SVC_REPLY_MAX];
    while (!arg->stop) {
        uint32_t peer = 0u;
        const int got = ambre_port_chan_recv(arg->chan, msg, sizeof(msg),
                                             20000u, &peer);
        if (got <= 0) {
            continue;
        }
        size_t reply_len = 0u;
        (void)ambre_edr_service_handle(arg->svc, AMBRE_PEER_ARCHIVE, msg,
                                       static_cast<size_t>(got), reply,
                                       sizeof(reply), &reply_len);
        if (reply_len > 0u) {
            (void)ambre_port_chan_reply(arg->chan, peer, reply, reply_len);
        }
    }
    return nullptr;
}

/* A sealed event is exported by the service; the blocks travel over the wire. */

/* Open a journal service on a fresh partition (2 segments). */
bool open_test_service(const std::string& disk_path, ambre_edr_service_t* svc)
{
    ambre_edr_service_cfg_t scfg;
    std::memset(&scfg, 0, sizeof(scfg));
    scfg.disk_spec = disk_path.c_str();
    scfg.disk_size = AMBRE_FIRST_DATA_OFFSET + 2ULL * AMBRE_SEGMENT_SIZE;
    ambre_journal_cfg_default(&scfg.cfg);
    scfg.cfg.segment_limit = 2u;
    return ambre_edr_service_open(svc, &scfg) == AMBRE_PORT_OK;
}

/* Send one capture order (TRIGGER / SEAL) straight to the service. */
bool service_order(ambre_edr_service_t* svc, ambre_ipc_msg_t type,
                   const void* body, uint32_t body_len)
{
    uint8_t req[AMBRE_IPC_FRAME_MAX];
    uint8_t rep[AMBRE_EDR_SVC_REPLY_MAX];
    size_t rlen = 0u;
    const int n = ambre_ipc_build(req, sizeof(req), type, body, body_len);
    return n > 0 && ambre_edr_service_handle(svc, AMBRE_PEER_CAPTURE, req,
                                             static_cast<size_t>(n), rep,
                                             sizeof(rep), &rlen) == AMBRE_PORT_OK;
}

bool service_seal_event(ambre_edr_service_t* svc, uint32_t event_id)
{
    ambre_seal_t seal;
    std::memset(&seal, 0, sizeof(seal));
    seal.event_id = event_id;
    seal.file_complete = 1u;
    seal.seal_cause = AMBRE_SEAL_NORMAL;
    return service_order(svc, AMBRE_MSG_SEAL, &seal, sizeof(seal));
}

void test_ipc_service_and_gel06(const std::string& tmp)
{
    const std::string sock = tmp + "/svc.sock";
    const std::string out_dir = tmp + "/ipc_out";
    CHECK(::mkdir(out_dir.c_str(), 0755) == 0);

    ambre_edr_service_t svc;
    CHECK(open_test_service(tmp + "/svc.img", &svc));

    /* 1. An event left OPEN: GEL-06 must make the service refuse the export. */
    ambre_emerg_t em;
    fill_emerg_event(&em, 1u, 0u);
    CHECK(service_order(&svc, AMBRE_MSG_TRIGGER, &em, sizeof(em)));

    ambre_port_chan_t* chan = nullptr;
    CHECK(ambre_port_chan_open(sock.c_str(), &chan) == AMBRE_PORT_OK);
    ServiceArg arg{&svc, chan, false};
    pthread_t th;
    CHECK(::pthread_create(&th, nullptr, service_thread, &arg) == 0);

    ambre::archive::EventData data;
    bool locked = false;
    std::string err;
    CHECK(!ambre::archive::fetch_event(sock, 1u, &locked, &data, &err));

    /* 2. Once sealed, run_service --once archives it over the frozen protocol. */
    CHECK(service_seal_event(&svc, 1u));
    CHECK(ambre::archive::run_service(sock, out_dir, true) == 0);
    arg.stop = true;
    ::pthread_join(th, nullptr);
    ambre_port_chan_close(chan);
    ambre_edr_service_close(&svc);

    const std::string mcap =
        out_dir + "/" + ambre::archive::event_file_name(1u, 1u);
    CHECK(run_cmd("python3 tests/mcap_check.py --mcap '" + mcap +
                  "' --expect-points 11") == 0);
}

}  // namespace

int main()
{
    const std::string tmp = make_tmpdir();
    test_sha256_vectors();
    test_standalone_export_and_python_crosscheck(tmp);
    test_gel06_no_write_while_event_open(tmp);
    test_multichunk_and_corruption_rejected(tmp);
    test_retention_keeps_16_and_locked(tmp);
    test_ipc_service_and_gel06(tmp);
    remove_tree(tmp);
    std::printf("%s: %d checks, %d failures\n",
                g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return g_failures;
}
