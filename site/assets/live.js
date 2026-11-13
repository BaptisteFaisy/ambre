/* live.js — la page « Emulator » : l'invité QNX 8.0 en direct.
 *
 * Trois requêtes, pas plus :
 *   - /console : le journal série de l'invité (texte brut)
 *   - /stats   : l'état de la vitrine (invité connected, producer fps…)
 *   - /key     : injection d'une touche dans l'invité (sendkey du moniteur)
 *   - /type    : taper une ligne de commande, touche par touche
 *
 * La capture de l'écran VGA passe par <img src="…/stream"> : un flux
 * multipart, donc aucune JavaScript pour l'image elle-même.
 */
(function () {
    "use strict";

    var API = "https://r630-debian.tail3a8bdf.ts.net:8443/ambre";
    var CONSOLE_TAIL = 24000;   /* caractères gardés : le journal grossit sans fin */

    function el(id) { return document.getElementById(id); }

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

    function majConsole() {
        demande(API + "/console", "text")
            .then(function (t) {
                var c = el("live-console");
                if (!c) { return; }
                if (t.length > CONSOLE_TAIL) { t = t.slice(-CONSOLE_TAIL); }
                c.textContent = t;
                c.scrollTop = c.scrollHeight;
            })
            .catch(function () { echec("console inaccessible"); });
    }

    function majStats() {
        demande(API + "/stats", "json")
            .then(function (s) {
                var d = el("live-status-detail");
                if (d) {
                    d.textContent = "invité " + (s.invite_connecte ? "connecté" : "déconnecté")
                        + " · " + (s.producer_fps || 0).toFixed(1) + " img/s"
                        + " · cadre vieux de " + (s.frame_age_s === null ? "?" : s.frame_age_s) + " s";
                }
                if (s.invite_connecte) { phase("live", "en direct"); }
                else if (s.unite_active) { phase("boot", "démarrage de l'invité"); }
                else { phase("off", "invité arrêté (la garde le relance à la visite)"); }
            })
            .catch(function () { phase("off", "vitrine hors ligne"); });
    }

    function touche(cle) {
        demande(API + "/key?k=" + encodeURIComponent(cle), "text")
            .catch(function () { echec("touche non envoyée"); });
    }

    function commande(texte, retour) {
        var url = API + "/type?t=" + encodeURIComponent(texte) + "&ret=" + (retour ? "1" : "0");
        demande(url, "text")
            .then(function (r) { echec(r); })
            .catch(function () { echec("commande non envoyée"); });
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
        var img = el("live-screen-img");
        if (img) {
            /* Vitrine injoignable (serveur éteint, réseau coupé) : on le dit au
               lieu de laisser une image cassée muette. */
            img.addEventListener("error", function () {
                phase("off", "vitrine injoignable");
                var n = el("live-screen-note");
                if (n) {
                    n.textContent = "The live vitrine (r630) is unreachable right now. "
                        + "The recorded proof of the same run is on the On QNX page.";
                }
            });
        }
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
