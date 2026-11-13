/* live.js — la page « Emulator » : l'invité QNX 8.0 en direct.
 *
 *   - /console : le journal série de l'invité (texte brut)
 *   - /stats   : l'état de la vitrine (invité connecté, img/s, âge du cadre)
 *   - /key     : injection d'une touche dans l'invité (sendkey du moniteur)
 *   - /type    : taper une ligne de commande, touche par touche
 *
 * La capture de l'écran VGA passe par <img src="…/stream"> : un flux
 * multipart, donc aucune JavaScript pour l'image elle-même.
 *
 * LES VRAIS CHIFFRES. L'invité publie lui-même ses mesures sur sa console
 * série, en JSON lisible par une machine (edrctl --json exécuté DANS le
 * guest) :
 *   AMBRE-STATUS {…}  l'état du journal (événements, octets, flushs, freezes)
 *   AMBRE-EVENT  {…}  un événement, un par ligne
 *   AMBRE-END         fin d'un lot
 * La page les relit et les affiche : ce sont les mesures de la cible, pas
 * celles du backend PC.
 *
 * DEUX BANCS, UN SEUL VISIBLE. Le premier est le serveur r630 ; s'il ne
 * répond pas (machine hors ligne, réseau coupé), la page bascule une fois sur
 * le banc du PC et le dit dans l'état. Elle ne bascule jamais en silence.
 */
(function () {
    "use strict";

    var BANCS = [
        { nom: "r630", api: "https://r630-debian.tail3a8bdf.ts.net:8443/ambre" },
        { nom: "PC", api: "https://baptiste-ms-7d18.tail3a8bdf.ts.net:8443/ambre" }
    ];
    var CONSOLE_TAIL = 24000;   /* caractères gardés : le journal grossit sans fin */
    var courant = 0;

    function el(id) { return document.getElementById(id); }
    function api() { return BANCS[courant].api; }

    /* Une requête bornée : si la vitrine ne répond pas, on veut le dire vite,
       pas laisser la page « en attente » pendant une minute. */
    function demande(url, type) {
        var ctrl = new AbortController();
        var minuterie = setTimeout(function () { ctrl.abort(); }, 4000);
        return fetch(url, { cache: "no-store", signal: ctrl.signal })
            .then(function (r) { return type === "json" ? r.json() : r.text(); })
            .finally(function () { clearTimeout(minuterie); });
    }

    function echec(texte) {
        var s = el("live-status-msg");
        if (s) { s.textContent = texte; }
    }

    function phase(etat, texte) {
        var p = el("live-phase");
        if (!p) { return; }
        p.dataset.state = etat;
        p.textContent = texte;
    }

    /* Bascule une seule fois vers le banc suivant ; rend false si aucun. */
    function basculer() {
        if (courant + 1 >= BANCS.length) { return false; }
        courant += 1;
        var img = el("live-screen-img");
        if (img) { img.src = api() + "/stream?bascule=" + Date.now(); }
        echec("bascule sur le banc « " + BANCS[courant].nom + " »");
        return true;
    }

    function majConsole() {
        demande(api() + "/console", "text")
            .then(function (t) {
                var c = el("live-console");
                if (c) {
                    if (t.length > CONSOLE_TAIL) { t = t.slice(-CONSOLE_TAIL); }
                    c.textContent = t;
                    c.scrollTop = c.scrollHeight;
                }
                majChiffres(t);
            })
            .catch(function () { echec("console inaccessible"); });
    }

    /* Les vraies mesures, lues dans ce que l'invité a écrit lui-même. */
    function majChiffres(texte) {
        var lignes = texte.split("\n");
        var status = null;
        var evenements = [];
        for (var i = 0; i < lignes.length; i += 1) {
            var l = lignes[i].trim();
            if (l.indexOf("AMBRE-STATUS ") === 0) {
                try { status = JSON.parse(l.slice(13)); } catch (e) { status = null; }
            } else if (l.indexOf("AMBRE-EVENT ") === 0) {
                try { evenements.push(JSON.parse(l.slice(12))); } catch (e) { /* ligne tronquée */ }
            }
        }
        var perf = /mean (\d+) ns/.exec(texte);
        var verdict = /verdict: (.+)/.exec(texte);
        majTableau(status, evenements, perf, verdict);
    }

    function champ(nom, valeur, note) {
        return '<div class="live-fig"><span class="live-fig-n">' + nom + "</span>"
            + '<span class="live-fig-v">' + valeur + "</span>"
            + (note ? '<span class="live-fig-s">' + note + "</span>" : "")
            + "</div>";
    }

    function majTableau(status, evenements, perf, verdict) {
        var hote = el("live-chiffres");
        if (!hote) { return; }
        if (!status) {
            hote.innerHTML = '<p class="live-note">Waiting for the guest to publish its '
                + "measurements (they arrive with each cycle).</p>";
            return;
        }
        var html = "";
        html += champ("backend", status.backend, "the target, not the PC backend");
        html += champ("health", status.health, "");
        html += champ("events", status.sealed_events + " sealed / "
            + status.acquired_events + " acquired", "the crash and its triggers");
        html += champ("bytes written", status.bytes_written, "on the raw partition");
        html += champ("flushes", status.flush_count, "durable writes");
        html += champ("worst freeze", status.worst_freeze_us + " µs",
            "last: " + status.last_freeze_us + " µs");
        if (evenements.length) {
            var e = evenements[0];
            html += champ("event 1", e.state + " · " + e.trig_reason,
                "locked=" + e.locked + " · file_complete=" + e.file_complete
                + " · age " + e.age_ms + " ms");
        }
        if (perf) {
            html += champ("send round trip (PERF-05)", Math.round(perf[1] / 1000) + " µs mean",
                verdict ? verdict[1].trim() : "");
        }
        hote.innerHTML = html;
    }

    function majStats() {
        demande(api() + "/stats", "json")
            .then(function (s) {
                var d = el("live-status-detail");
                if (d) {
                    d.textContent = "banc « " + BANCS[courant].nom + " » · invité "
                        + (s.invite_connecte ? "connecté" : "déconnecté")
                        + " · " + (s.producer_fps || 0).toFixed(1) + " img/s"
                        + " · cadre vieux de " + (s.frame_age_s === null ? "?" : s.frame_age_s) + " s";
                }
                if (s.invite_connecte) { phase("live", "en direct"); }
                else if (s.unite_active) { phase("boot", "démarrage de l'invité"); }
                else { phase("off", "invité arrêté (la garde le relance à la visite)"); }
            })
            .catch(function () {
                if (basculer()) { majStats(); return; }
                phase("off", "vitrine hors ligne");
            });
    }

    function touche(cle) {
        demande(api() + "/key?k=" + encodeURIComponent(cle), "text")
            .catch(function () { echec("touche non envoyée"); });
    }

    function commande(texte, retour) {
        var url = api() + "/type?t=" + encodeURIComponent(texte) + "&ret=" + (retour ? "1" : "0");
        demande(url, "text")
            .then(function (r) { echec(r); })
            .catch(function () { echec("commande non envoyée"); });
    }

    /* L'image peut échouer avant le premier /stats : même bascule, une fois. */
    function surveillerEcran() {
        var img = el("live-screen-img");
        if (!img) { return; }
        img.addEventListener("error", function () {
            if (basculer()) { return; }
            phase("off", "vitrine injoignable");
            var n = el("live-screen-note");
            if (n) {
                n.textContent = "The live vitrine is unreachable right now (r630 and the PC "
                    + "bench both down). The recorded proof of the same run is on the On QNX page.";
            }
        });
    }

    function brancher() {
        document.querySelectorAll("button[data-k]").forEach(function (b) {
            b.addEventListener("click", function () { touche(b.dataset.k); });
        });
        var f = el("live-cmd");
        if (f) {
            f.addEventListener("submit", function (ev) {
                ev.preventDefault();
                var v = f.querySelector("input").value;
                if (!v) { return; }
                commande(v, true);
                f.querySelector("input").value = "";
            });
        }
        var cc = el("live-ctrl-c");
        if (cc) { cc.addEventListener("click", function () { commande("\u0003", false); }); }
        surveillerEcran();
    }

    function tick() {
        majConsole();
        majStats();
    }

    document.addEventListener("DOMContentLoaded", function () {
        brancher();
        tick();
        setInterval(tick, 2000);
    });
}());
