# Working on the 14-core machine

The 14-core machine is used in person: no SSH. The repository is cloned there and everything runs
through `scripts/lab.sh`, which logs each command to `~/eop-lab/logs/` and packs the results into
one file to carry back. The lab machine's shell is fish, where `VAR=value command` does not work:
every command here that sets a variable is written `env VAR=value command`, which works in fish and
bash alike. The clone can be anywhere: run the commands below from its directory
(`cd "<clone>"`, quoted if the path has spaces); `lab.sh` finds the repository by itself and logs
which one it used. No commits are made on that machine: the evidence comes back to the
laptop, where it is committed with the signing key.

What it is for: the 30-day SLO run ([slo-30-day-run.md](slo-30-day-run.md)), the clean-cluster
readiness and smoke test (US-406), and the demo rehearsals ([SLA-DEMO.md](SLA-DEMO.md)).

## 1. Access to the repository (once)

The repository is private. Do not copy the GPG key or the personal `gh` login to a shared machine;
use a token that can only read this repository:

1. On github.com: *Settings → Developer settings → Personal access tokens → Fine-grained tokens →
   Generate new token*.
   - Repository access: *Only select repositories* → `sd-2026-af-developers-v0.3-v1.0`.
   - Permissions: *Contents: Read-only* (nothing else).
   - Expiration: the end of the course (it has to outlive the 30-day run).
2. On the machine:

   ```bash
   git config --global credential.helper 'cache --timeout=43200'   # remembered 12 h, in memory
   git clone https://github.com/AldanaPavetGarcia/sd-2026-af-developers-v0.3-v1.0.git eop
   cd eop && git checkout feature/76-release-v1.0                    # development once merged
   ```

   Username: `AldanaPavetGarcia`; password: the token.

   On the lab machine the clone is in `~/Descargas/Aldana Pavet/eop`, with the symlink `~/eop`
   pointing to it.

If the token leaks, revoking it on that page is enough: it cannot write to anything.

## The machine

As set up for the run (2026-11-18):

| | |
|---|---|
| OS | Rocky Linux 8.10, kernel 7.2 (ELRepo) |
| cgroups | **v2**. Kubernetes v1.37's kubelet does not start on cgroups v1, because the `cpuset` controller is missing there. Switched with `sudo grubby --update-kernel=ALL --args="systemd.unified_cgroup_hierarchy=1"` and a reboot. `stat -fc %T /sys/fs/cgroup` must print `cgroup2fs` |
| Tools | Docker CE 26.1.3, minikube v1.39.0, kubectl v1.37.2 |

## 2. First day

```bash
scripts/lab.sh check       # what is missing; it needs git, make, python3, docker, minikube, kubectl
scripts/lab.sh setup       # minikube profile "eop" (10 CPUs, 12 GB) and every image
scripts/lab.sh start       # the product plus the SLO traffic; writes the start date
scripts/lab.sh autostart   # after a reboot, minikube comes back by itself
scripts/lab.sh ready-time  # US-406 on a throwaway profile, deleted at the end
```

`check` prints the cores, memory and disk. If the machine has less memory, lower the profiles:
`env EOP_LAB_MEMORY=8g EOP_LAB_CLEAN_MEMORY=6g scripts/lab.sh setup` (the same variables for
`ready-time`). `ready-time` needs both profiles at once; it never touches the long-lived one.

The machine must not suspend or install updates that reboot it: every hour down is an hour with
no data, which the report has to declare.

## 3. Each visit

```bash
scripts/lab.sh status      # days collected, traffic flowing; exits 1 if it is not collecting
scripts/lab.sh update      # only when there is a new build to try: git pull + images + rollout
                           # (also restarts Prometheus and Grafana; the SLO data stays)
scripts/lab.sh evidence    # ~/eop-lab/evidence-<date>.tar.gz
```

Copy the `.tar.gz` to a USB drive or your Drive. On the laptop, its contents go to the PR or issue
they prove.

## 4. Demo rehearsals and the end of the run

```bash
scripts/lab.sh demo        # step by step with pauses; env DEMO_AUTO=1 scripts/lab.sh demo without them
scripts/lab.sh smoke       # US-406 AC8 on the long-lived stack
scripts/lab.sh sla-export  # SLA evidence of the period so far
make slo-run-export        # only at the end: the TSDB snapshot (large) in slo-run-export/
scripts/lab.sh evidence
```

**Never** run `minikube delete -p eop` or delete the `prometheus-data` claim: that is the SLO
data, and the run would start over.

## The current run

Started **2026-11-19 00:47 UTC** (2026-11-18 21:47 ART, `~/eop-lab/slo-start.txt`); 30 days end on
2026-12-19 00:47 UTC (2026-12-18 21:47 ART). As of 2026-11-27: 8 days 23 hours of uptime with no
reboot, Prometheus with 0 restarts, `prometheus-data` Bound, 215 hourly samples (8.96 days), about
6,500 eop-server requests an hour and none failed.

### What the first extraction found (2026-11-27)

The data was being collected correctly, but the tools that read it did not show it:

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | `status`, `sla-export` and `slo-run-export` gave `n/a` and an empty `slo-series.json` | The rules record one sample an hour; an instant query looks back only 5 minutes, and `query_range` with a 1 h step does the same at each step | Every reader uses a range selector: `last_over_time(eop:…[2h])`, and `eop:…[35d]` for the hourly series. `make vault-tec-dashboard-test` rejects a panel that reads a rule without one |
| 2 | `status` said "data since 1970-01-01 (0.0 days)" and "0 MiB" | Prometheus did not scrape itself, so `prometheus_tsdb_lowest_timestamp` did not exist | `prometheus` self-scrape job. `status` also prints the hourly samples recorded |
| 3 | `eop:gateway_success:ratio30d` empty | With no 5xx in the period the 5xx series never existed, so the division had nothing to divide | The 5xx and OK rules fall back to 0 × the total. `make prometheus-rules-test` (promtool) covers both cases |
| 4 | `lab.sh evidence` exited 1 with "file changed as we read it" | It packed `~/eop-lab` while writing its own log there | The current log is left out and tar's exit 1 is no longer a failure |
| 5 | The guide assumed `~/eop` | | Commands run from wherever the clone is |
| 6 | The first `lab.sh update` with these fixes (2026-11-28 00:38 UTC) applied the new Prometheus configuration and rules but did not restart Prometheus or Grafana | Bash had already read the old `lab.sh` before `git pull`, so the old list of Deployments to restart ran | `update` now runs the rest from the pulled script (`lab.sh rollout`). That time they were restarted by hand at 00:45:08 UTC |

None of this lost data: the raw metrics and the hourly samples are on the volume. On the next visit:

```bash
scripts/lab.sh update      # new rules, self-scrape, dashboard; Prometheus restarts, data stays
scripts/lab.sh status      # now shows the days and the 30-day figures
```

Applied on 2026-11-28 (Prometheus and Grafana restarted by hand, finding 6). Afterwards the status
showed data since 2026-11-19 00:46 UTC (9.0 days), 216 hourly samples, 34 MiB of TSDB blocks and
success ratio and error budget at 1.00000. The new gateway rule had its first evaluation pending.
The synthetic traffic showed 6 failed requests out of 958 in the 10 minutes after the restarts,
where it had always shown 0: see "Events of the period" in `docs/SLA-DEMO.md`.

The figures of the first 9 days come from the hourly series saved that day (`raw-eop_*.json`):
`make slo-partial-report RAW=<dir> OUT=<dir> START="2026-11-19 00:47 UTC"` writes `slo-series.json`,
`slo-report.json` and `slo-report.md` on the laptop, without the cluster.

## 5. Optional: CI on this machine

While the stack runs, the same machine can be the CI runner when GitHub's free minutes run out:
[ci-self-hosted-runner.md](ci-self-hosted-runner.md). It needs a one-time registration token
from the repository's settings, not the token of section 1.
