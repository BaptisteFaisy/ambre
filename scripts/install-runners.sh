#!/usr/bin/env bash
# install-runners.sh — (ré)installe les 5 runners self-hosted du dépôt sur r630.
#
# Idempotent : relancer ce script répare un runner dont le dossier a disparu.
#
#   ssh r630 'bash -s' < scripts/install-runners.sh
#   # ou, localement si les dossiers sont sur cette machine :
#   bash scripts/install-runners.sh
#
# Trois leçons apprises en les installant (2026-11-11), toutes visibles dans
# l'historique du dépôt :
#
#  1. Ne jamais copier une installation de runner avec `cp -a`. `bin` et
#     `externals` sont des liens symboliques ABSOLUS vers le dossier versionné
#     du runner source ; copiés tels quels, le nouveau runner exécute le
#     binaire d'un AUTRE dossier et config.sh répond « already configured »
#     alors que `.runner` est absent. D'où `cp -rL` (déréférencement).
#  2. Ne pas les installer dans ~/actions-runner-* : un nettoyage quotidien de
#     l'hôte parcourt ce motif (~/bin/purge-disque.sh) et d'autres routines de
#     la machine le balaient. Ils vivent ici dans ~/ci-runners/.
#  3. Pas de sudo sur cet hôte : les services sont des unités **utilisateur**
#     (`systemctl --user`), possibles parce que `linger=yes`.
#
# Le jeton d'enregistrement est lu sur stdin (jamais en argument, donc jamais
# visible dans la liste des processus) :
#   gh api -X POST repos/Tradoooo/ambre/actions/runners/registration-token \
#     --jq .token | ssh r630 'RUNNER_NAMES="02 03" bash -s' < scripts/install-runners.sh

set -euo pipefail

REPO="${REPO:-Tradoooo/ambre}"
LABELS="${LABELS:-self-hosted,Linux,X64,ambre-worker,ci-node}"
BASE="${BASE:-$HOME/ci-runners}"
PREFIX="${PREFIX:-ambre}"
NAMES="${RUNNER_NAMES:-01 02 03 04 05}"
# Installation donneuse : n'importe quel runner déjà présent fait l'affaire,
# elle n'est que la source du paquet (bin/, externals/, scripts).
DONOR="${DONOR:-$HOME/actions-runner-placard-01}"

read -r TOKEN || true
if [ -z "${TOKEN:-}" ]; then
    echo "aucun jeton d'enregistrement sur stdin — voir l'en-tête du script" >&2
    exit 2
fi

[ -d "$DONOR" ] || { echo "donneur introuvable : $DONOR" >&2; exit 2; }
mkdir -p "$BASE"

for i in $NAMES; do
    D="$BASE/$PREFIX-$i"
    name="r630-$PREFIX-$i"
    unit="$PREFIX-runner-$i.service"

    echo "== $name -> $D"
    systemctl --user stop "$unit" 2>/dev/null || true

    rm -rf "$D"
    mkdir -p "$D"
    # -L : copie le contenu réel, pas le lien symbolique du donneur.
    cp -rL "$DONOR"/bin "$DONOR"/externals "$D"/
    cp "$DONOR"/config.sh "$DONOR"/run.sh "$DONOR"/runsvc.sh "$DONOR"/env.sh \
       "$DONOR"/run-helper.sh.template "$DONOR"/run-helper.cmd.template \
       "$DONOR"/safe_sleep.sh "$D"/
    chmod +x "$D"/config.sh "$D"/run.sh "$D"/runsvc.sh

    (cd "$D" && ./config.sh --url "https://github.com/$REPO" --token "$TOKEN" \
        --name "$name" --labels "$LABELS" --work "_work" --unattended \
        > "/tmp/ambre-runner-$i-config.log" 2>&1) \
        || { echo "   config KO, voir /tmp/ambre-runner-$i-config.log" >&2;
             tail -3 "/tmp/ambre-runner-$i-config.log" >&2; exit 1; }

    cat > "$HOME/.config/systemd/user/$unit" <<EOF
[Unit]
Description=GitHub Actions Runner ($REPO $name)
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=$D
ExecStart=$D/runsvc.sh
Restart=always
RestartSec=5
KillMode=process

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user reset-failed "$unit" 2>/dev/null || true
    systemctl --user enable --now "$unit"
    echo "   service $unit: $(systemctl --user is-active "$unit")"
done

echo
echo "Vérifier côté GitHub (jusqu'à ~30 s) :"
echo "  gh api repos/$REPO/actions/runners --jq '.runners[]|\"\\(.name) \\(.status)\"'"
