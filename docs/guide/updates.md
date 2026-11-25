# Updating

The panel updates itself. You do not need a computer or the browser installer
again.

**Settings → Firmware Update → Check for Updates**

Pick **Stable** or **Nightly** in the channel dropdown. Stable is the default and
the right choice unless you want to help test.

The device knows its own screen size and downloads the matching build.

## Automatic updates

Automatic updates are on by default and only follow the **Stable** channel.

- The panel checks for a new release once a day. The first check runs 10 minutes
  after it starts. It only checks while nobody is touching the panel.
- When a newer release is found, the install starts after 5 minutes without a
  touch. Music playing does not delay it.
- The panel restarts once and installs the update during the next start.
- Each version is tried automatically once. If it fails, the panel keeps running
  and you can install it by hand from **Settings → Firmware Update**.
- The switch is at **Settings → Firmware Update**, under the version card.

### Forks and your own builds

Updates come from the repository named by `GITHUB_REPO`. By default that is the
upstream project, `OpenSurface/SonosESP`. A build that points at the upstream
project shows a notice when a newer release exists and lets you install it by
hand, but it does not install upstream releases automatically.

To use automatic updates, pick one of these when you build:

- Publish releases from your own repository and build with
  `-DGITHUB_REPO=\"owner/repo\"` in the `build_flags` of your environment.
- Or accept automatic installs from the upstream project with
  `-DAUTOUPDATE_ALLOW_UPSTREAM=1`.

On a build where automatic installs are not allowed, the Update screen says so
under the switch.

::: warning Downloads are not certificate checked
The panel fetches update information and firmware over HTTPS but does not
validate the server certificate (`setInsecure`). Anyone who can intercept traffic
to github.com on your network could serve a firmware image. Turn the switch off
on a network you do not trust.
:::

## If an update stops partway

It resumes. If the transfer stalls, the panel reconnects and carries on from the
byte it reached, rather than starting the whole image again. Seeing
`Resuming from 47%…` means the recovery is working — let it run.

If it still fails, see
[Troubleshooting](/TROUBLESHOOTING#updates-fail-or-stop-partway).

::: warning Do not unplug during an update
If a flash is interrupted the board keeps the previous firmware and will boot
into it, but wait for the panel to restart on its own.
:::
