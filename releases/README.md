# FTRFS Releases

This directory contains Sigstore bundles for tagged FTRFS releases.

Source tarballs themselves are not tracked here because they are
reproducible bit-for-bit from the corresponding git tag using
`git archive`. The bundles are tracked because each signature is
unique and not reproducible.

---

## Layout

```
releases/
├── README.md                                       (this file)
├── ftrfs-v0.1.0-baseline.tar.gz.sigstore.json      (bundle)
├── ftrfs-v0.1.0-baseline.tar.gz                    (regenerated, gitignored)
└── ...
```

---

## Reproducing a release tarball

For any tagged release `<TAG>`, the canonical tarball can be
regenerated locally:

```sh
git archive --format=tar.gz --prefix=ftrfs-<TAG>/ -o releases/ftrfs-<TAG>.tar.gz <TAG>
```

The byte-for-byte reproducibility of `git archive` output for a
given tag is guaranteed by the underlying tree object hash. Two
checkouts of the same tag will produce the same tarball with the
same SHA-256 digest.

Example for `v0.1.0-baseline`:

```sh
git archive --format=tar.gz --prefix=ftrfs-v0.1.0-baseline/ \
    -o releases/ftrfs-v0.1.0-baseline.tar.gz v0.1.0-baseline
sha256sum releases/ftrfs-v0.1.0-baseline.tar.gz
```

Expected SHA-256 for `v0.1.0-baseline`:
```
c362db2bc0005a77ebb6c5c7aca85489aa0cec6e94b64a3d88e60604d80c7540
```

If your locally produced hash differs from the expected hash above,
the tarball was either regenerated from a modified working tree, a
different tag, or with a different `--prefix`. Verify that you are
on the exact tag with `git describe v0.1.0-baseline` and that no
local modifications are present with `git status`.

---

## Verifying a Sigstore bundle

FTRFS releases are signed using Sigstore (`cosign sign-blob` with
keyless OIDC signing, certificate from Fulcio, signing event recorded
in the Rekor transparency log). To verify a release:

1. Install `cosign` (Gentoo: `app-containers/cosign`, version 3.0.3
   or later).

2. Regenerate the tarball as described above.

3. Run `cosign verify-blob` with the expected identity and issuer
   for the release in question.

The expected identity differs per release. See the table below.

### Identity table

| Tag                    | Signer identity                          | OIDC issuer                            |
|------------------------|------------------------------------------|----------------------------------------|
| `v0.1.0-baseline`      | `aurelien.desbrieres@gmail.com`          | `https://github.com/login/oauth`       |

The signer identity for `v0.1.0-baseline` corresponds to the maintainer's
GitHub OAuth primary email at the time of signing. Subsequent releases
are expected to use a workflow-bound identity from a GitHub Actions
release pipeline (path of the form
`https://github.com/roastercode/FTRFS/.github/workflows/<file>.yml@refs/tags/<tag>`),
once that pipeline is in place. The identity table above will be
updated accordingly.

### Verification command

For `v0.1.0-baseline`:

```sh
cosign verify-blob releases/ftrfs-v0.1.0-baseline.tar.gz \
    --bundle releases/ftrfs-v0.1.0-baseline.tar.gz.sigstore.json \
    --certificate-identity 'aurelien.desbrieres@gmail.com' \
    --certificate-oidc-issuer 'https://github.com/login/oauth'
```

A successful verification prints `Verified OK` and exits with status
zero. Any other output indicates a verification failure that must be
investigated before trusting the artifact.

`cosign verify-blob` checks all of the following in one operation:

- The bundle's signature is mathematically valid against the tarball
  contents.
- The bundle's certificate was issued by the Sigstore Fulcio CA.
- The certificate's Subject Alternative Name matches the expected
  identity.
- The certificate's `1.3.6.1.4.1.57264.1.1` extension (OIDC issuer)
  matches the expected issuer.
- The signing event is present in the Rekor transparency log.
- The signed timestamp falls within the certificate's validity
  window.

Verification can be performed offline against the bundle alone; no
network access to `rekor.sigstore.dev` or `fulcio.sigstore.dev` is
required at verification time, because the bundle contains the
inclusion proof and the timestamping authority signature.

---

## Why Sigstore

The choice of Sigstore for release signing is driven by the
auditability and certification requirements identified in
`Documentation/threat-model.md`:

- The Rekor transparency log provides a tamper-evident record of
  every release signing event, accessible to any auditor.
- The signing certificates are short-lived (10 minutes), so a
  long-term key compromise does not compromise prior signatures.
- The verification toolchain (`cosign`) is open source, packaged
  by upstream Linux distributions, and reproducible from source.
- The combined signature, certificate, timestamp, and Rekor inclusion
  proof are bundled in a single self-contained file, simplifying
  archival and offline verification.

These properties align with DO-178C, ECSS-E-ST-40C, and IEC 61508
expectations for software supply chain integrity in safety- or
mission-critical contexts.

GPG signing of git commits (via the maintainer's key
`319A8EAA89C7538AA9550E8BC35EE212519E4857`) provides per-commit
attribution and is the second layer of provenance. Sigstore is the
release-artifact layer. The two are complementary, not redundant.

---

## Note on the `v0.1.0-baseline` signature

This is the first Sigstore-signed release in the FTRFS history. The
signing was performed interactively from the maintainer's
workstation, using `cosign sign-blob` with the default browser-based
OIDC flow. The OIDC provider returned the maintainer's GitHub
primary email at the time of signing, which is the address recorded
in the Fulcio certificate and immutably in Rekor.

Subsequent signing operations are intended to use a non-interactive
flow (CLI device authorization or GitHub Actions workflow identity)
so that:

- No browser is required on the signing host.
- The signer identity recorded in Rekor is bound to a workflow path
  rather than a personal email, providing better auditability and
  decoupling from the maintainer's personal accounts.

This transition does not invalidate the `v0.1.0-baseline` signature.
That signature remains verifiable using the identity in the table
above.
