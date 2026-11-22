# EOP v1.0.0 — Known limitations and release procedure

What v1.0.0 does not prove yet, or proves differently from the PRD's wording, with the reason and
what would close each point. Written for the course staff; nothing here is hidden elsewhere.

## 1. Open items

| # | Item | Status at release preparation | What closes it |
|---|---|---|---|
| 1 | **30 days of SLO data (US-407 AC1/AC2)** | The run (#67, `docs/slo-30-day-run.md`) had not started when v1.0 was prepared; the planned start (2026-11-08) passed. The period shown at the demo will be shorter than 30 days. | Start it on the 14-core machine as soon as possible and state the real period at the demo. |
| 2 | **MTTR < 200 ms read as service restoration (ADR-027 §4)** | Measured on minikube: 0 of 1,955 probe requests failed and service restoration took 93 ms when a replica was deleted; the PRD's literal *pod death → pod ready* (replica replacement) took 3.4 s and is shown next to it. | The course confirms the reading (asked in ADR-027). |
| 3 | **Firmware on hardware (US-401..US-403)** | Tested on an ESP32-D0WD-V3 on 2026-11-17 (PR #80, firmware README): publishing, a 31 s broker outage with every sample delivered, thread restarts and reboot. First sample 8.9 s after a cold start but 15 s on quick resets, because the test router rejects the first authentications of a MAC that just left. Not seen on the board: a real WiFi loss and the LED blinking. | A WiFi-loss test and an LED on GPIO2. |
| 4 | **Device TLS** | Mosquitto has a TLS listener for devices (8883, certificates generated in the cluster). The firmware's TLS client is PR #89 (#88, on top of #80): `tls.conf`, broker verified against the built-in CA and host name, tested on `native_sim`; the ESP32 TLS build fits (DRAM 70 %) but has not published over TLS on the board yet. Until #89 is merged, devices use 1883 on a trusted network. | Merge #89; publish over TLS on the ESP32. |
| 5 | **Clean-cluster readiness and smoke test (US-406 AC2)** | On the laptop (7 GB) the production overlay applied cleanly and the core came up by itself; the < 3 min measurement failed only on downloading images, and SigNoz/ClickHouse did not fit in memory. | `make k8s-ready-time` with images preloaded and `make smoke-test-prod` on the 14-core machine. |
| 6 | **Live demo rehearsal (US-407)** | `make demo-script` and `make sla-export` are written and their logic tested; not run against the full stack. | Two rehearsals on the 14-core machine (`docs/SLA-DEMO.md`). |
| 7 | **One edge-ingestor replica** | Messages are processed in order by one replica (ADR-028 §2); throughput is far above v1.0's device count. | MQTT shared subscriptions plus an in-flight marker, if many more devices are added. |
| 8 | **Overlay path and dev overlay (ADR-029)** | The overlay is `deploy/production` (Kustomize rejects an overlay inside its own base); the 7 GB `dev` overlay is not done — `make k8s-apply` stays the development path. | Update ADR-029's path; add the dev overlay if needed. |
| 9 | **Alert thresholds in two places** | `config/production.env` and the Grafana alert files hold the same numbers; Kustomize cannot inject into a file blob. A contract test keeps them equal. | — (guarded by `make production-config-test`). |
| 10 | **CI minutes (free plan)** | On 2026-11-18 the 2,000 monthly Actions minutes ran out and GitHub stopped starting jobs in #81–#87 (not a test failure); the C++ jobs had passed. The workflows now cancel superseded runs, build images only on the PR into `development`, and can move to a self-hosted runner with a repository variable. | Register the runner on the 14-core machine ([ci-self-hosted-runner.md](ci-self-hosted-runner.md)) or wait for the monthly reset, then re-run the jobs. |

## 2. Release procedure

v1.0 is two stacks of pull requests that meet in `development`:

```
development ─┬─ #77 ADRs ─ #78 US-401 ─ #79 US-402 ─ #80 US-403                    (firmware stack)
             └─ #59 … #65 (v0.3) ─┬─ #81 30-day run
                                  └─ #82 MTTR ─ #83 US-404 ─ #84 US-405 ─ #85 US-406 ─ #86 US-407 ─ #87 release
```

1. Merge the v0.3 stack (#59 → #65) and cut `release/0.3.0` + tag, as #65 describes.
2. Merge #81 (#67) into the v0.3 line: #85 carries its four commits and must come after it.
3. Merge #82 → #87 in order, each after the one below it, rebasing the next one on its new base.
4. Merge the firmware stack (#77 → #80) into `development`. Expected textual conflicts with the
   server stack: `mosquitto/mosquitto.conf` (identical), the `mosquitto` service and
   `eop-mosquitto-data` volume in `docker-compose.yml` (copied verbatim), `.gitignore`, `Makefile`
   (separate targets), `.github/workflows/` (separate workflow files), `CHANGELOG.md` (move the
   firmware and ADR entries into `[1.0.0]`). Update ADR-029's overlay path in the same step.
5. With everything in `development` and CI green, set the date of `[1.0.0]`, cut
   `release/1.0.0` from `development` and tag `v1.0.0`.

The repository is squash-only: a squash merge changes commit identities, so rebase each next PR
onto the updated base before merging it.
