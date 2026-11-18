# CI on a self-hosted runner

The repository is private on a free plan, so GitHub Actions has 2,000 minutes a month. When they
run out, GitHub does not start the jobs ("recent account payments have failed or your spending
limit needs to be increased"): it is not a test failure. Every job can instead run on our own
machine, which has no minute limit.

## How the workflows choose the runner

Every `runs-on` in `ci.yml` and `firmware.yml` reads the repository variable `EOP_CI_RUNNER`:

```yaml
runs-on: ${{ vars.EOP_CI_RUNNER || 'ubuntu-24.04' }}
```

- Variable unset: GitHub-hosted runners, as before.
- `EOP_CI_RUNNER=self-hosted`: every job goes to our runner. No commit is needed to switch back
  and forth, only the variable.

Two more settings keep the minutes down on GitHub-hosted runners:

- `concurrency` with `cancel-in-progress`: a new push to a PR cancels its run still going. A
  rebase of a stack pushes every branch, and only the last run of each matters.
- `docker-image-build` runs only on the PR whose base is `development`. A stacked PR gets its
  images built and measured (50 MB limit) when it reaches `development`; before that,
  `pre-pr-check` builds and measures them locally.

## Requirements of the machine

The jobs were written for GitHub's Ubuntu images:

- **Ubuntu 24.04.** On Ubuntu 26, `libomp-dev` no longer matches clang-18 and CMake cannot
  find OpenMP for the HPC engine.
- **Docker**, usable by the runner's user: `services: redis` in the C++ jobs and the image
  builds need it.
- **Passwordless `sudo`** for the runner's user: the jobs install clang-18 and the other tools
  with `apt` and link them into `/usr/local/bin`.

Because the jobs install packages and change `/usr/local/bin`, run the runner in a dedicated
Ubuntu 24.04 virtual machine (or an LXD system container with Docker enabled), not on the
machine's own system. On the 14-core machine, give it a CPU limit (for example 6 cores) so it
does not disturb the 30-day SLO run ([slo-30-day-run.md](slo-30-day-run.md)).

## Registering it

1. In the VM, as a user with passwordless `sudo` that belongs to the `docker` group, follow
   *Settings → Actions → Runners → New self-hosted runner* of the repository (Linux, x64). It
   downloads the runner, runs `./config.sh --url … --token …` with a one-time token, and keeps
   the default label `self-hosted`.
2. Install it as a service so it survives reboots: `sudo ./svc.sh install && sudo ./svc.sh start`.
3. *Settings → Secrets and variables → Actions → Variables → New repository variable*:
   `EOP_CI_RUNNER` = `self-hosted`.
4. Re-run the jobs that did not start: `gh run rerun <run-id> --failed`, or push to the PR.

One runner takes one job at a time, so a PR's jobs run one after the other. Registering a second
runner in the same VM (another directory, `./config.sh` again) runs two at once.

## Security

A self-hosted runner executes the code of every PR that triggers the workflows. That is safe
here because the repository is private and only its owner opens PRs. If the repository is made
public, remove the runner first: a fork's PR could then run arbitrary code on the machine.

To go back to GitHub-hosted runners, delete the `EOP_CI_RUNNER` variable.
