# Releases

XRecomp's own releases, one page of notes each. Not to be confused with [CHANGELOG.md](../../CHANGELOG.md), which is
upstream xboxrecomp's history (its `v0.x` tags are upstream's, not ours): our tags are `xrecomp-v<version>`.

| Release | Notes |
|---|---|
| `xrecomp-v0.7.0` | [xrecomp-v0.7.0.md](xrecomp-v0.7.0.md) |
| `xrecomp-v0.6.0` | [xrecomp-v0.6.0.md](xrecomp-v0.6.0.md) |
| `xrecomp-v0.5.1` | [xrecomp-v0.5.1.md](xrecomp-v0.5.1.md) |
| `xrecomp-v0.5.0` | [xrecomp-v0.5.0.md](xrecomp-v0.5.0.md) |
| `xrecomp-v0.4.0` | [xrecomp-v0.4.0.md](xrecomp-v0.4.0.md) |
| `xrecomp-v0.3.0` | [xrecomp-v0.3.0.md](xrecomp-v0.3.0.md) |
| `xrecomp-v0.2.0` | [xrecomp-v0.2.0.md](xrecomp-v0.2.0.md) |
| `xrecomp-v0.1.0` | [xrecomp-v0.1.0.md](xrecomp-v0.1.0.md) |

Downloads: the [GitHub releases](https://github.com/MisterAlex95/Xrecomp/releases). Besides the tagged releases,
`xrecomp-latest` is a rolling prerelease rebuilt from every push to `main`: newer, less tested, debuggable.

## What a release contains

- `xrecomp-v<version>.apk`: the XRecomp Android app with the on-device Prepare. arm64-v8a, Android 11+.
  Release-signed (the same key as `xrecomp-latest`, so either updates the other in place) and **not debuggable**.
- `xrecomp-v<version>.apk.sha256`.
- `xrecomp-v<version>-macos-arm64.zip`: the Mac app (Apple silicon, macOS 14+) inside the toolkit folder it drives.
  Ad-hoc signed, not notarized: run `xattr -dr com.apple.quarantine <the XRecomp folder>` once before opening it,
  otherwise macOS blocks it or runs it from a translocated copy that cannot see its toolkit (Setup then asks for it).
- `xrecomp-v<version>-linux.tar.gz`: the toolkit for Linux (README « Linux »).
- A `.sha256` next to each.
- **Frame generation** (LSFG, XBA-187) in every one of them: the APK has the Vulkan bridge in `libmain.so` and the
  shader translator in `libxba_import.so` (Settings › Graphics › Frame generation, « Choose Lossless.dll »); the
  desktop folders carry its pinned sources (`third_party/lsfg-sources`), which a game build compiles in by default.
  Lossless.dll itself is never shipped: the player picks their own copy, from their Lossless Scaling install.
  Licences: NOTICE « Frame generation (LSFG) », `LICENSES/MIT-lsfg-vk.txt`, `LICENSES/Zlib-DXVK.txt`.
- No game, no game data, no generated C: you prepare your own games on your device or computer.

`xrecomp-latest` carries the same files, named `xrecomp-<commit>…`. The desktop files are built on GitHub's runners
by `.github/workflows/desktop.yml` (each one is this repository's tree at the commit, `tools/ci/package_toolkit.sh`,
plus `XRecomp.app` for the Mac) and attached once Gitea has published the release of that commit. No Windows build
yet: that route is upstream's and untested here.

Every XRecomp APK we publish (releases and `xrecomp-latest`) is signed with the same certificate, SHA-256:

```
BC:F2:04:46:E6:FE:38:6F:3C:FA:A1:B8:75:B2:5E:99:7A:77:0A:E6:1A:B2:48:68:DD:4C:9C:E3:18:F8:39:FD
```

Check a download with `apksigner verify --print-certs xrecomp-v<version>.apk` (Android SDK build-tools): its
`Signer #1 certificate SHA-256 digest` must be the same, lower case without colons. Any other signer is not ours.

The Mac app and the toolkit can also be built from the source at the tag (README « Get it »).

## Making a release (maintainers)

1. Pick a commit of `main` whose CI run is green (its on-device toolchain is then cached as `toolchain-<key>`).
2. Write `docs/releases/xrecomp-v<version>.md` (what changed, what works, known problems) and add it to the table
   above; merge that into `main` and let CI go green again.
3. Tag and push: `git tag -a xrecomp-v<version> -m "XRecomp <version>" && git push origin xrecomp-v<version>`.
4. `.gitea/workflows/release.yml` builds the APK (release key, not debuggable, version name
   `<version>+<commit>`) and publishes the release on Gitea and on GitHub with the notes. It stops without
   publishing when the notes file, the release key or `GH_RELEASE_TOKEN` is missing
   ([toolchain.md « Signing »](../android/toolchain.md#signing) lists the secrets).
5. GitHub's `Desktop` workflow (`.github/workflows/desktop.yml`) runs on the tag when the mirror pushes it, waits
   for that release, then attaches the macOS zip and the Linux archive and appends a « Desktop » section to the
   notes. A re-run of Gitea's release job recreates the release without them: re-run the Desktop run of the tag.
6. Check the GitHub release page: the APK, the two desktop files, their checksums, the notes ending with
   `signed with: release key`.

Versions follow [semantic versioning](https://semver.org) loosely while we are below 1.0: the minor number goes up
with a release that adds something, the patch number with a release that only fixes.
