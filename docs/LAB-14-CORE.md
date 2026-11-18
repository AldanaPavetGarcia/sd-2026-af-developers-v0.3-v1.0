# Working on the 14-core machine

The 14-core machine is used in person: no SSH. The repository is cloned there and everything runs
through `scripts/lab.sh`, which logs each command to `~/eop-lab/logs/` and packs the results into
one file to carry back. No commits are made on that machine: the evidence comes back to the
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

If the token leaks, revoking it on that page is enough: it cannot write to anything.

## 2. First day

```bash
scripts/lab.sh check       # what is missing; it needs git, make, python3, docker, minikube, kubectl
scripts/lab.sh setup       # minikube profile "eop" (10 CPUs, 12 GB) and every image
scripts/lab.sh start       # the product plus the SLO traffic; writes the start date
scripts/lab.sh autostart   # after a reboot, minikube comes back by itself
scripts/lab.sh ready-time  # US-406 on a throwaway profile, deleted at the end
```

`check` prints the cores, memory and disk. If the machine has less memory, lower the profiles:
`EOP_LAB_MEMORY=8g EOP_LAB_CLEAN_MEMORY=6g scripts/lab.sh setup` (the same variables for
`ready-time`). `ready-time` needs both profiles at once; it never touches the long-lived one.

The machine must not suspend or install updates that reboot it: every hour down is an hour with
no data, which the report has to declare.

## 3. Each visit

```bash
scripts/lab.sh status      # days collected, traffic flowing; exits 1 if it is not collecting
scripts/lab.sh update      # only when there is a new build to try: git pull + images + rollout
scripts/lab.sh evidence    # ~/eop-lab/evidence-<date>.tar.gz
```

Copy the `.tar.gz` to a USB drive or your Drive. On the laptop, its contents go to the PR or issue
they prove.

## 4. Demo rehearsals and the end of the run

```bash
scripts/lab.sh demo        # step by step with pauses; DEMO_AUTO=1 without them
scripts/lab.sh smoke       # US-406 AC8 on the long-lived stack
scripts/lab.sh sla-export  # SLA evidence of the period so far
make slo-run-export        # only at the end: the TSDB snapshot (large) in slo-run-export/
scripts/lab.sh evidence
```

**Never** run `minikube delete -p eop` or delete the `prometheus-data` claim: that is the SLO
data, and the run would start over.

## 5. Optional: CI on this machine

While the stack runs, the same machine can be the CI runner when GitHub's free minutes run out:
[ci-self-hosted-runner.md](ci-self-hosted-runner.md). It needs a one-time registration token
from the repository's settings, not the token of section 1.
