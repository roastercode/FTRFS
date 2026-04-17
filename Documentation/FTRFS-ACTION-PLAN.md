# RAF — Plan général consolidé FTRFS + Yocto
# Mis à jour : 2026-04-17

## Priorité 0 — Concordance documentaire et branches (IMMÉDIAT)

### roastercode/FTRFS (branche main)

| # | Tâche | État |
|---|-------|------|
| 0.1 | README.md — corriger chiffres benchmark : 0.25s latency, 0.34s 3-node, 4.37s 9-job (pas les anciens 0.052s/job) | 🔧 |
| 0.2 | Documentation/testing.md — résultats actuels, procédure BusyBox-compatible, cluster script | 🔧 |
| 0.3 | Documentation/roadmap.md — bitmap on-disk RS FEC marqué FAIT, pas planifié | 🔧 |
| 0.4 | Documentation/design.md — layout v2 avec block 5 bitmap RS FEC | 🔧 |

### roastercode/yocto-hardened

| # | Tâche | État |
|---|-------|------|
| 0.5 | Synchroniser branche main avec arm64-ftrfs ou documenter clairement la relation | 🔧 |
| 0.6 | Documenter branches yocto-hpc, ext4-dm-verity-selinux, squashfs-selinux-permissive | 🔧 |
| 0.7 | README quick start — corriger HPC_ADMIN_PUBKEY_FILE vers ${THISDIR} | 🔧 |

---

## Priorité 1 — Ce qui bloque kernel.org v4

| # | Tâche | Attendu par | État |
|---|-------|------------|------|
| 1.1 | xfstests Yocto recipe — generic/001,002,010,098,257 | Tous reviewers | PENDING |
| 1.2 | Répondre Eric Biggers — confirmation lib/reed_solomon exact match validé byte-for-byte | kernel.org | PENDING |
| 1.3 | Rebaser patch series v4 en série propre et atomique | Falcato, Wilcox | PENDING |
| 1.4 | Indirect block support — single indirect (~2 MiB) | Falcato (NACK use case) | PENDING |

---

## Priorité 2 — Consolidation FTRFS dans Yocto (avant meta-oe)

| # | Tâche | Scope | État |
|---|-------|-------|------|
| 2.1 | yocto-check-layer PASS complet sur arm64-ftrfs | Yocto upstream | PENDING |
| 2.2 | LIC_FILES_CHKSUM + HOMEPAGE sur toutes les recettes HPC | Yocto upstream | PENDING |
| 2.3 | Supprimer ou upstreamer patches GCC15/QEMU | Yocto upstream | PENDING |
| 2.4 | Bootstrap script reproductible (clone + build + benchmark one-shot) | Yocto upstream | PENDING |
| 2.5 | Soumission meta-openembedded | OE community | PENDING — après check-layer |

---

## Priorité 3 — Consolidation FTRFS kernel (après v4 accepté)

| # | Tâche | État |
|---|-------|------|
| 3.1 | RS FEC writeback sur data blocks (pas seulement bitmap/inode) | PENDING |
| 3.2 | kthread scrubber RT avec sysfs interface | PENDING |
| 3.3 | Shannon entropy dans RS journal | PENDING |
| 3.4 | SB_RDONLY enforcement avant writes | PENDING |

---

## Priorité 4 — FTRFS v2 (après v1 mergé dans kernel)

| # | Tâche | État |
|---|-------|------|
| 4.1 | Symlinks | PENDING |
| 4.2 | xattr + SELinux enforcing | PENDING |
| 4.3 | Compression read-only mode | PENDING |
| 4.4 | Post-quantique metadata auth (SLH-DSA/SPHINCS+) | PENDING long terme |

---

## Règle absolue

**Avant tout push sur GitHub :**
1. Vérifier concordance documentaire entre FTRFS et yocto-hardened
2. Lancer `bin/hpc-benchmark.sh` et valider les résultats
3. Vérifier que les chiffres dans les README correspondent aux derniers résultats réels
