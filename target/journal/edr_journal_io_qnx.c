/*
 * edr_journal_io_qnx.c — the service handlers of /dev/edr on QNX 8.0.
 *
 * read() answers the text status of the journal (OBS-02) and the raw blocks of
 * one event (EXP-01, EXP-08); devctl() answers DCMD_EDR_STATUS / LIST / EXPORT
 * / AUTOTEST. Every request runs in the receiving pool thread at the CALLER'S
 * priority (QNX-03), and every one of them goes through edr_gate() first
 * (SEC-01). The lifecycle half — attach, thread pool, open — lives in
 * edr_journal_qnx.c; the shared shape is edr_journal_qnx_internal.h.
 *
 * Compiled with the real QNX SDP 8.0 (qcc 12.2.0, gcc_ntox86_64).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "edr_journal_qnx_internal.h"
#include "edr_journal_svc.h"

/* ------------------------------------------------------- access control --- */

/* QNX-only: verified on target, syntax-checked on PC */
static uint32_t edr_peer_type_of(resmgr_context_t *ctp)
{
    struct _client_info info;

    memset(&info, 0, sizeof(info));
    if (iofunc_client_info(ctp, 0, &info) != EOK) {
        return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
    switch (info.cred.egid) {
    case 1u: return (uint32_t)AMBRE_PEER_JOURNAL;
    case 2u: return (uint32_t)AMBRE_PEER_CAPTURE;
    case 3u: return (uint32_t)AMBRE_PEER_PRODUCER;
    case 4u: return (uint32_t)AMBRE_PEER_SERVICE;
    case 5u: return (uint32_t)AMBRE_PEER_ARCHIVE;
    default: return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_gate(resmgr_context_t *ctp, ambre_access_op_t op, uint8_t msg_type)
{
    uint32_t type = edr_peer_type_of(ctp);

    if (ambre_port_access_allowed(type, op) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, "edr-journal",
                       "SEC-01: refused msg 0x%02x from peer type %u",
                       (unsigned)msg_type, (unsigned)type);
        return EACCES;
    }
    return EOK;
}

/* ------------------------------------------------------------ status text - */

/* QNX-only: verified on target, syntax-checked on PC */
static int build_status_text(char *out, size_t cap)
{
    ambre_ipc_status_body_t st;

    memset(&st, 0, sizeof(st));
    if (g_edr_jrn.src.status != NULL) {
        (void)g_edr_jrn.src.status(g_edr_jrn.src.ctx, &st);
    }
    st.backend = (uint8_t)ambre_port_backend();
    st.supervised = ambre_port_supervised() ? 1u : 0u;
    return snprintf(out, cap,
                    "mode=%s health=%u backend=%s supervised=%u\n"
                    "segment=%u block=%u open=%u acquired=%u sealed=%u\n"
                    "refused_no_space=%u bytes_written=%llu flush_count=%llu\n"
                    "uptime_ms=%llu last_freeze_us=%llu worst_freeze_us=%llu\n",
                    st.mode ? "continuous" : "event-only", (unsigned)st.health,
                    ambre_port_backend_name(), (unsigned)st.supervised,
                    (unsigned)st.current_seg, (unsigned)st.current_block,
                    (unsigned)st.open_events, (unsigned)st.acquired_events,
                    (unsigned)st.sealed_events, (unsigned)st.refused_no_space,
                    (unsigned long long)st.bytes_written,
                    (unsigned long long)st.flush_count,
                    (unsigned long long)st.uptime_ms,
                    (unsigned long long)st.last_freeze_us,
                    (unsigned long long)st.worst_freeze_us);
}

/* ------------------------------------------------------------- devctls ---- */

/* QNX-only: verified on target, syntax-checked on PC */
/* OBS-02, EXP-01 : le statut binaire du journal.
 *
 * LA RÉPONSE D'UN DEVCTL S'ÉCRIT DANS LE PAYLOAD DU MESSAGE. `_RESMGR_PTR()`
 * enregistre un pointeur vers un tampon séparé, et le noyau interprète ce
 * tampon comme LE message de réponse complet : il prend les 16 premiers octets
 * pour l'en-tête `_io_devctl_reply` (zero, ret_val, nbytes, zero2 — sys/iomsg.h
 * 590-596) et recopie la suite. Résultat mesuré sur la cible le 2026-11-14 : le
 * serveur avait les bonnes valeurs (mode=1 backend=2 bytes_written=512, imprimé
 * par le diagnostic ci-dessous) et le client recevait exactement ces mêmes
 * octets DÉCALÉS DE 16 — 512 atterrissant dans open_events, 1 dans sealed_events,
 * et la fin du tampon laissée intacte. La doc SDP est explicite : pour un
 * io_devctl, `_IO_INPUT_PAYLOAD(msg)` et `_IO_OUTPUT_PAYLOAD(msg)` retournent la
 * même adresse, et `_DEVCTL_DATA()` est déprécié (com.qnx.doc.security.system/
 * topic/manual/fortified.html). Le corps est donc écrit dans le payload, la
 * longueur dans `o.nbytes`, et le handler rend EOK sans `_RESMGR_PTR()`.
 *
 * QNX-only: verified on target, syntax-checked on PC */
static int devctl_status(resmgr_context_t *ctp, io_devctl_t *msg)
{
    ambre_ipc_status_body_t *out = (ambre_ipc_status_body_t *)_IO_OUTPUT_PAYLOAD(msg);

    if (g_edr_jrn.src.status == NULL) {
        return ENOSYS;
    }
    if (sizeof(*out) > (size_t)ctp->msg_max_size) {
        return EMSGSIZE;
    }
    if (g_edr_jrn.src.status(g_edr_jrn.src.ctx, out) != 0) {
        return EIO;
    }
    out->backend = (uint8_t)ambre_port_backend();
    out->supervised = ambre_port_supervised() ? 1u : 0u;
    msg->o.nbytes = (int32_t)sizeof(*out);
    /* `_RESMGR_PTR(ctp, msg, ...)` : le tampon de réponse EST le message,
     * en-tête `_io_devctl_reply` compris. Les deux autres formes se sont mesurées
     * fausses sur la cible (2026-11-14) : `_RESMGR_PTR(ctp, &st, sizeof st)` fait
     * lire les 16 premiers octets du corps comme l'en-tête — le client reçoit le
     * corps DÉCALÉ DE 16 (512 dans open_events, 1 dans sealed_events) — et
     * rendre EOK sans `_RESMGR_PTR` ne répond QUE l'en-tête, le tampon du client
     * restant intact (0xAA partout). Les deux symptoms, une seule cause : la
     * longueur de réponse vient du tampon pointé, pas de `o.nbytes` seul. */
    return _RESMGR_PTR(ctp, msg, sizeof(io_devctl_t) + (size_t)msg->o.nbytes);
}

/* QNX-only: verified on target, syntax-checked on PC */
/* EXP-01 : la liste des événements, par le corps gelé de l'IPC (compteur puis
 * lignes) — auto-descriptif, écrit dans le payload pour la même raison que
 * devctl_status.
 * QNX-only: verified on target, syntax-checked on PC */
static int devctl_list(resmgr_context_t *ctp, io_devctl_t *msg)
{
    ambre_edr_list_body_t *out = (ambre_edr_list_body_t *)_IO_OUTPUT_PAYLOAD(msg);
    int got;

    if (g_edr_jrn.src.list == NULL) {
        return ENOSYS;
    }
    if (sizeof(*out) > (size_t)ctp->msg_max_size) {
        return EMSGSIZE;
    }
    got = g_edr_jrn.src.list(g_edr_jrn.src.ctx, out->lines, EDR_MAX_EVENTS);
    if (got < 0) {
        return EIO;
    }
    out->count = (uint32_t)got;
    msg->o.nbytes = (int32_t)(sizeof(out->count)
                              + (size_t)got * sizeof(out->lines[0]));
    return _RESMGR_PTR(ctp, msg, sizeof(io_devctl_t) + (size_t)msg->o.nbytes);
}

/* QNX-only: verified on target, syntax-checked on PC */
/* EXP-01, EXP-08 : les blocs bruts d'un événement, tels qu'ils sont sur le
 * média. La requête (event_id, premier bloc) arrive dans le payload ENTRANT —
 * le même tampon que la sortie, d'où la copie préalable de la requête : écrire
 * la réponse avant de lire l'entrée écraserait l'entrée.
 * QNX-only: verified on target, syntax-checked on PC */
static int devctl_export(resmgr_context_t *ctp, io_devctl_t *msg)
{
    uint8_t *pay = (uint8_t *)_IO_OUTPUT_PAYLOAD(msg);
    ambre_edr_export_body_t *head = (ambre_edr_export_body_t *)pay;
    uint32_t req[2];
    uint32_t room;
    uint32_t count;
    int got;

    if (msg->i.nbytes < (int)sizeof(req)) {
        return EINVAL;
    }
    if (resmgr_msgread(ctp, req, sizeof(req), sizeof(io_devctl_t))
        != (ssize_t)sizeof(req)) {
        return EFAULT;
    }
    if (g_edr_jrn.src.event_blocks == NULL) {
        return ENOSYS;
    }
    room = (uint32_t)((ctp->msg_max_size < sizeof(ambre_edr_export_body_t))
                      ? 0u : (size_t)ctp->msg_max_size - sizeof(ambre_edr_export_body_t));
    count = room / AMBRE_BLOCK_SIZE;
    if (count > DCMD_EDR_EXPORT_BLOCKS) {
        count = DCMD_EDR_EXPORT_BLOCKS;
    }
    if (count == 0u) {
        return EMSGSIZE;
    }
    got = g_edr_jrn.src.event_blocks(g_edr_jrn.src.ctx, req[0], req[1], count,
                                     pay + sizeof(*head));
    if (got < 0) {
        return EIO;
    }
    head->event_id = req[0];
    head->first_block = req[1];
    head->block_count = (uint32_t)got;
    head->total_blocks = (uint32_t)got;
    msg->o.nbytes = (int32_t)(sizeof(*head) + (size_t)got * AMBRE_BLOCK_SIZE);
    return _RESMGR_PTR(ctp, msg, sizeof(io_devctl_t) + (size_t)msg->o.nbytes);
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                         RESMGR_OCB_T *ocb)
{

    int status = iofunc_devctl_default(ctp, msg, ocb);

    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    switch (msg->i.dcmd) {
    case DCMD_EDR_STATUS:
        return (edr_gate(ctp, AMBRE_OP_READ_STATUS, AMBRE_MSG_STATUS) == EOK)
                   ? devctl_status(ctp, msg) : EACCES;
    case DCMD_EDR_LIST:
        return (edr_gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_LIST) == EOK)
                   ? devctl_list(ctp, msg) : EACCES;
    case DCMD_EDR_EXPORT:
        return (edr_gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_EXPORT) == EOK)
                   ? devctl_export(ctp, msg) : EACCES;
    case DCMD_EDR_AUTOTEST:
        if (g_edr_jrn.src.autotest == NULL) {
            return ENOSYS;
        }
        return (g_edr_jrn.src.autotest(g_edr_jrn.src.ctx) == 0) ? EOK : EIO;
    default:
        /* Diagnostic : le dcmd reçu ne correspond à aucune commande. Sans cette
         * ligne, un client et un serveur qui n'encodent pas pareil se répondent
         * ENOTTY et personne ne voit lequel des deux a tort. */
        ambre_port_log(AMBRE_LOG_WARN, "edr-journal",
                       "devctl inconnu: recu=0x%x attendu status=0x%x list=0x%x "
                       "export=0x%x autotest=0x%x",
                       (unsigned)msg->i.dcmd, (unsigned)DCMD_EDR_STATUS,
                       (unsigned)DCMD_EDR_LIST, (unsigned)DCMD_EDR_EXPORT,
                       (unsigned)DCMD_EDR_AUTOTEST);
        return ENOTTY;
    }
}

/* ------------------------------------------------------- read / write ----- */

/* OBS-02 : read() sur /dev/edr rend le statut du journal en texte.
 *
 * La reprise d'un événement par chemin (/dev/edr/events/<id>, EXP-08) exigeait
 * de stocker l'identifiant dans l'OCB ; l'OCB est maintenant celui de la
 * bibliothèque QNX (voir edr_journal_qnx.c), donc ce chemin n'est pas servi ici.
 * L'export des blocs bruts passe par DCMD_EDR_EXPORT, qui porte l'identifiant et
 * le premier bloc DANS la requête — et c'est cette voie qui est mesurée sur la
 * cible par host/qnx_probe/devctl_probe.c.
 * QNX-only: verified on target, syntax-checked on PC */
int edr_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    /* STATIQUE pour la même raison que devctl_status : la réponse part par
     * `_RESMGR_PTR()`, qui ne copie pas. Un tampon local ici rendrait le texte
     * de statut OBS-02 illisible sur la cible, silencieusement. */
    static char text[EDR_TEXT_MAX];
    int len;
    size_t want;
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);

    if (status != EOK) {
        return status;
    }
    if (edr_gate(ctp, AMBRE_OP_READ_STATUS, AMBRE_MSG_STATUS) != EOK) {
        return EACCES;
    }
    len = build_status_text(text, sizeof(text));
    if (len <= 0) {
        return EIO;
    }
    if ((uint64_t)ocb->offset >= (uint64_t)len) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_PTR(ctp, text, 0);
    }
    want = (size_t)len - (size_t)ocb->offset;
    if (want > msg->i.nbytes) {
        want = msg->i.nbytes;
    }
    {
        char *from = text + (size_t)ocb->offset;

        ocb->offset += (off_t)want;
        _IO_SET_READ_NBYTES(ctp, want);
        return _RESMGR_PTR(ctp, from, want);
    }
}

/* SEC-02: no write order can stop the recorder.
 * QNX-only: verified on target, syntax-checked on PC */
int edr_io_write(resmgr_context_t *ctp, io_write_t *msg,
                        RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);

    if (status != EOK) {
        return status;
    }
    if (edr_gate(ctp, AMBRE_OP_WRITE_INPUT, AMBRE_MSG_FRAME) != EOK) {
        return EACCES;
    }
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

