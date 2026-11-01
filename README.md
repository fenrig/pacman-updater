# pacman-updater

Licensed under the MIT License; see [LICENSE](LICENSE).

`pacman-updater` is a small root-owned C worker for Arch Linux. A daily
systemd timer starts one worker; it uses an advisory `flock` for its lifetime,
waits until seven days after the last complete success, then runs:

```
pacman -Syu --noconfirm
yay -Syu --noconfirm
```

Only completion of both stages records a success. A failed stage ends the
invocation and the next timer activation retries the complete sequence.

An optional custom-PKGBUILD stage can be enabled with `custom_repo_url`. The
worker clones that repository as `aur_user`, inspects
`makepkg --printsrcinfo`, and builds recipes whose output package names match
installed packages, including split packages. It then installs the built
artifacts with root pacman. The stage runs only after pacman and yay succeed;
any build or install failure fails the complete attempt. The repository is
cloned into a fresh `/tmp` directory for each attempt and removed afterward.
Only matched recipe directories are copied into the dedicated
`~/.cache/pacman-updater/` workspace. PKGBUILDs are never executed as
root. Pin `custom_repo_ref` to a branch, tag, or commit as appropriate.

## Custom PKGBUILD repository layout

`custom_repo_url` must point to a Git repository containing one or more
standard Arch package recipe directories. Each package directory must contain
one file named exactly `PKGBUILD`:

```
custom-packages/
├── package-one/
│   ├── PKGBUILD
│   ├── package-one.install       # optional
│   └── patches/                  # optional supporting files
├── package-two/
│   └── PKGBUILD
└── split-package/
    ├── PKGBUILD                  # may define multiple pkgname entries
    └── files/
        └── example.service
```

Recipes may be nested below the repository root. The worker recursively finds
`PKGBUILD` files and runs `makepkg --printsrcinfo` in each recipe directory.
The recipe is selected when at least one package name emitted by its `pkgname`
entries is already installed on the machine. Split packages are therefore
supported. Recipes for packages that are not installed are skipped.

All files required by a recipe must be inside its own directory or available
from the URLs used by the `PKGBUILD`. Relative `source=()` entries are copied
together with the recipe. The recipe must be a normal noninteractive Arch
`PKGBUILD` that works with:

```
makepkg --syncdeps --noconfirm
```

The repository is freshly cloned into `/tmp` for each update attempt as the
configured `aur_user`. It is not used as a persistent checkout, and a failed
clone or checkout fails the complete update attempt. Selected recipe
directories are copied to:

```
~/.cache/pacman-updater/
```

where `~` is the configured `aur_user` home directory. Builds happen there as
that unprivileged user. Resulting packages are installed by root pacman after
the build succeeds.

Keep the repository limited to trusted PKGBUILDs and supporting files. A
PKGBUILD is executable build code, even when its upstream source is public;
do not put credentials, private keys, or secrets in the repository. Use
`custom_repo_ref` to pin a reviewed branch, tag, or commit. The updater does
not currently verify Git commit signatures or package signatures beyond the
normal behavior of Git, makepkg, and pacman.

## Build and install

Dependencies are a C11 compiler, CMake, libcurl, and libsystemd:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Run `install.sh` as root (optionally passing a build directory and config path),
then review its `.new` configuration and install the two units to
`/etc/systemd/system`. The installer reads `timer_calendar` as data with
`awk`, never by sourcing it, and writes that value into the timer unit.
Create the configured unprivileged `aur_user` with a real home directory and
install yay for that account. Configuration validation rejects a missing user,
root as the build user, or a missing/non-absolute home directory. For example:

```
useradd --create-home --shell /usr/bin/nologin aurbuild
```

Enable the timer only after review; this project does not enable services
automatically.

The service runs yay as the configured user, with that user’s `HOME`, groups,
and working directory. `tools/pacman-updater-pacman-wrapper` is a supplied
root-owned, fixed-path helper that permits only `pacman -U` of absolute regular
package files. Set yay’s `sudo_bin` to it and allow only that exact executable
for `aur_user` in sudoers (with `NOPASSWD` if unattended operation is wanted);
do not grant `NOPASSWD: ALL`. The worker supplies `/dev/null` as stdin and
`--noconfirm`, so a password or other interaction fails instead of waiting for
a hidden prompt. Verify yay’s effective `sudo`, `sudoflags`, and
`makepkgconf` settings before enabling the timer.

## Configuration and safety

Configuration is a strict `key=value` file, never shell-sourced. It is opened
without following a final symlink and must be a regular, root-owned file that
is not group/world writable; validation occurs before any settings are used.
Durations use
`s`, `m`, `h`, or `d`. Unknown keys and invalid values fail before an update.
The required connectivity probe uses TLS verification, bounded timeouts, no
redirects, and accepts only HTTP 204; a link being up, DNS succeeding, or a
captive portal is not enough. The primary endpoint is tried first, followed by
the optional `fallback_endpoint` if it fails. Package databases are not
refreshed for the probe. There is no reliable machine-local test for internet
access: route, link, and DNS checks can all succeed while HTTPS access is
blocked.

Battery rules are conservative: no battery passes; system-scope batteries are
considered while peripheral (`POWER_SUPPLY_SCOPE=Device`) batteries are
excluded; multiple batteries use the lowest known capacity and any unknown
status/capacity postpones unless external power is confirmed. Discharging
requires strictly over 90%; external power/charging requires at least 25%.

User idle uses logind’s system idle hint and monotonic idle timestamp. Desktop
session reporting therefore affects availability; unavailable idle data
postpones. If logind reports no login sessions, the worker requires the
machine to have been up for at least `no_user_uptime` (15 minutes by default)
before passing user-idle eligibility. This avoids updating immediately during
boot; unavailable uptime data postpones. CPU mode samples aggregate
`/proc/stat` counters over the configured
window, treats iowait as idle, rejects counter resets, and does not use load
average. Suspend/resume may make a window unusable and it is retried. `both`
requires both conditions. CPU mode may make the worker sleep for its averaging
window before the first decision.

The worker stops checking at the next local calendar midnight. It never
inhibits while waiting. Immediately before package operations it acquires a
systemd-logind block inhibitor for shutdown and sleep and holds it across both
stages. Forced shutdown and power loss cannot be prevented. Pacman’s own
database lock is respected and never removed.

State is root-owned under `state_dir` and committed with a same-directory
temporary file and rename. Corrupt state fails conservatively. It keeps
attempt, stage, result, and complete-success timestamps. `--dry-run` performs
configuration/state/due/eligibility checks and explains the first decision; it
never invokes a package manager.

When `parallelism` is zero, no build override is added. When positive, yay is
given `--mflags -jN`, its documented pass-through to makepkg; this does not
alter persistent makepkg configuration. Individual build systems or PKGBUILDs
may ignore general parallelism settings.

Use `journalctl -u pacman-updater.service`, `pacman-updater --dry-run`, and
`/var/lib/pacman-updater/state` for troubleshooting. A second activation exits
cleanly because the worker lock is held by the first process.

For a read-only machine diagnostic, run `pacman-updater-check` as root. It
loads the same configuration and state, prints due status and timestamps, then
evaluates battery, idle, CPU, and connectivity independently in the worker's
order. It never takes the worker lock, acquires an inhibitor, invokes pacman,
or changes state. A due but ineligible result exits with status 1. If a
required data source cannot be evaluated, it prints `ERROR` and exits with
status 2. A machine without a system battery is a valid battery pass, not an
error. CPU mode
uses a quick sample by default so the command returns promptly; use
`pacman-updater-check --wait-cpu` to wait for the configured CPU averaging
window and test the same measurement requirement used for eligibility.
