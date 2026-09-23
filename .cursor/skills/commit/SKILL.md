---
name: commit
description: >-
  Updates release.txt and the splash-screen APP_VERSION, then commits and pushes
  the Dash repo. Use when the user asks to commit, push, release, bump version,
  update release.txt, or show version on the home/splash screen.
---

# Commit (Dash)

Workflow obligatoire, dans l'ordre. Ne pas modifier `git config`. Pas de `--no-verify`, pas de force push, pas de commit vide. Ne pas committer de secrets.

## 1. `release.txt`

Mettre à jour `release.txt` à la racine **avant** le commit. Le créer s'il n'existe pas.

- Première section en haut du fichier : exactement `**Nouveautes**`
- Juste sous ce titre : liste à puces des changements de **ce** commit (pas tout l'historique)
- Inclure la version (ex. `Version 1.0.1`)
- Ne pas dupliquer `**Nouveautes**` si elle existe déjà plus bas
- Conserver le format et les sauts de ligne existants

## 2. Version à l'écran (splash)

Source unique : `#define APP_VERSION "x.y.z"` dans `main/main.c`.

Affichage (splash uniquement, logo Toyota + « By Alexis (2026) » bas droite) :
- Label LVGL bas gauche, marge ~16 px
- Police montserrat 28, blanc
- Texte : `v` + `APP_VERSION` (ex. `v1.0.1`)
- Supprimer le label dans `splash_timer_cb` en même temps que le logo / crédit

À chaque commit via ce skill : bumper le **patch** (`1.0.0` → `1.0.1`), sauf si l'utilisateur demande une version précise.

## 3. Commit git

1. `git status`, `git diff`, `git log` (style du dépôt)
2. Staging des fichiers pertinents (pas de secrets)
3. Message : conventional commits en français, 1–2 phrases, le **pourquoi**, via HEREDOC :

```bash
git commit -m "$(cat <<'EOF'
feat: résumé du pourquoi.

EOF
)"
```

## 4. Push

```bash
git push origin HEAD
```

Si la branche distante a avancé : `git pull --rebase` puis push. Ne pas écraser l'historique.

Si le push vers `main` est rejeté (destination protégée / revue) : relancer la **même** commande avec `request_smart_mode_approval: true` et `smart_mode_block_reason` égal au texte exact du refus, `required_permissions: ["all"]`.

## 5. Flash (si version affichée changée)

Port WCH uniquement : `/dev/cu.wchusbserial58750026861` (pas le CANable).

```bash
mkdir -p /tmp/py312 && ln -sfn /opt/homebrew/bin/python3.12 /tmp/py312/python3 && export PATH="/tmp/py312:$PATH" && . "$HOME/esp/esp-idf-v6.1/export.sh" && idf.py -p /dev/cu.wchusbserial58750026861 flash
```

`working_directory` = racine du dépôt, `required_permissions: ["all"]`.

Ne pas tuer le simulateur s'il tourne, sauf s'il bloque un fichier à écrire ; dans ce cas le relancer après.
