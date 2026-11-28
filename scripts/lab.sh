#!/usr/bin/env bash
set -euo pipefail

# Everything v1.0 still needs from the 14-core machine (docs/LAB-14-CORE.md), one short command at
# a time, on a machine used locally (no SSH): the 30-day SLO run, the clean-cluster readiness and
# smoke test, the demo rehearsal and the evidence to carry back.
#
# Two minikube profiles keep them apart:
#   eop        long-lived: the product (deploy/production) plus the SLO traffic. Never deleted.
#   eop-clean  throwaway: created, measured and deleted by `ready-time`.
#
# Output of every command is also written to $EOP_LAB_DIR/logs/, which `evidence` packs.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAB_DIR="${EOP_LAB_DIR:-$HOME/eop-lab}"
PROFILE="${EOP_LAB_PROFILE:-eop}"
CLEAN_PROFILE="${EOP_LAB_CLEAN_PROFILE:-eop-clean}"
CPUS="${EOP_LAB_CPUS:-10}"
MEMORY="${EOP_LAB_MEMORY:-12g}"
CLEAN_CPUS="${EOP_LAB_CLEAN_CPUS:-4}"
CLEAN_MEMORY="${EOP_LAB_CLEAN_MEMORY:-8g}"
DISK="${EOP_LAB_DISK:-60g}"
LOCAL_IMAGES=(eop-server hpc-engine api-gateway external-stub mttr-watcher edge-ingestor edge-sim slo-traffic)
LOG_FILE=""

log() { printf '[lab] %s\n' "$*" >&2; }
fail() { printf '[lab] FAIL: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage: scripts/lab.sh <command>

  check       what is installed and what is missing (run it first)
  setup       start the long-lived minikube profile "eop" and build the images
  start       deploy the product and start the 30-day SLO traffic (note the start date)
  status      is the SLO run collecting? (once a day)
  autostart   bring the run back by itself after a reboot (user crontab, no sudo)
  update      git pull, rebuild the images and roll them out; the SLO data stays
  rollout     the same without the git pull (update runs it from the pulled script)
  ready-time  US-406: clean throwaway cluster, < 3 min to Ready, smoke test, then delete it
  smoke       US-406 AC8 smoke test against the long-lived stack
  demo        US-407 demo rehearsal (interactive; DEMO_AUTO=1 for no pauses)
  sla-export  US-407 SLA evidence of the period so far
  evidence    pack logs, status and exports into one .tar.gz to carry back
EOF
}

# Every command's output also goes to a log file.
start_log() {
    mkdir -p "${LAB_DIR}/logs"
    LOG_FILE="${LAB_DIR}/logs/$(date +%Y%m%d-%H%M%S)-$1.log"
    exec > >(tee -a "${LOG_FILE}") 2>&1
    log "$(date -Is) $1 on $(git -C "${REPO}" rev-parse --abbrev-ref HEAD)@$(git -C "${REPO}" rev-parse --short HEAD) in ${REPO}; log: ${LOG_FILE}"
}

use_profile() {
    minikube -p "$1" status >/dev/null 2>&1 || fail "minikube profile $1 is not running (scripts/lab.sh setup)"
    kubectl config use-context "$1" >/dev/null
}

cmd_check() {
    local missing=0
    for tool in git make python3 docker minikube kubectl curl tar; do
        if command -v "${tool}" >/dev/null; then
            log "ok      ${tool}"
        else
            log "MISSING ${tool}"
            missing=1
        fi
    done
    if command -v docker >/dev/null && ! docker info >/dev/null 2>&1; then
        log "MISSING docker access: add the user to the docker group (sudo usermod -aG docker \$USER, then log in again)"
        missing=1
    fi
    log "cores: $(nproc); memory: $(free -g | awk '/^Mem:/ {print $2 " GB total, " $7 " GB available"}')"
    log "disk free in ${HOME}: $(df -h "${HOME}" | awk 'NR==2 {print $4}')"
    log "profile ${PROFILE}: ${CPUS} CPUs, ${MEMORY}; ${CLEAN_PROFILE} (ready-time only): ${CLEAN_CPUS} CPUs, ${CLEAN_MEMORY}"
    log "both at once need about $(( ${MEMORY%g} + ${CLEAN_MEMORY%g} )) GB; lower EOP_LAB_MEMORY / EOP_LAB_CLEAN_MEMORY if needed"
    if ((missing)); then
        log "install the missing tools (docs/LAB-14-CORE.md, section 1) and run check again"
        return 1
    fi
    log "ready for: scripts/lab.sh setup"
}

# Built once in the host's Docker, then loaded into whichever profile needs them.
build_images() {
    log "Building the EOP images in the host's Docker"
    docker build -t eop-server:local "${REPO}"
    docker build -t hpc-engine:local -f "${REPO}/hpc-engine/Dockerfile" "${REPO}"
    make -C "${REPO}" gateway-image
}

third_party_images() {
    (kubectl kustomize "${REPO}/deploy/production"; cat "${REPO}"/k8s/slo-run/*.yaml) |
        sed -nE 's/^ *(- )?image: *//p' | grep -v ':local$' | sort -u
}

# Third-party images are pulled once on the host and loaded too: the readiness measurement assumes
# images already present, and a slow network must not decide it.
load_images() {
    local profile=$1 image
    for image in $(third_party_images); do
        docker image inspect "${image}" >/dev/null 2>&1 || docker pull -q "${image}"
    done
    for image in "${LOCAL_IMAGES[@]/%/:local}" $(third_party_images); do
        log "loading ${image} into ${profile}"
        minikube -p "${profile}" image load "${image}"
    done
}

start_profile() {
    local profile=$1 cpus=$2 memory=$3
    if ! minikube -p "${profile}" status >/dev/null 2>&1; then
        minikube start -p "${profile}" --cpus "${cpus}" --memory "${memory}" --disk-size "${DISK}"
    fi
    minikube -p "${profile}" addons enable ingress >/dev/null
    kubectl config use-context "${profile}" >/dev/null
}

cmd_setup() {
    start_profile "${PROFILE}" "${CPUS}" "${MEMORY}"
    build_images
    load_images "${PROFILE}"
    log "ready for: scripts/lab.sh start"
}

cmd_start() {
    use_profile "${PROFILE}"
    make -C "${REPO}" k8s-prod-apply
    kubectl apply -f "${REPO}/k8s/slo-run/"
    kubectl rollout status deployment/slo-traffic --timeout=120s
    mkdir -p "${LAB_DIR}"
    [[ -f "${LAB_DIR}/slo-start.txt" ]] || date -u '+%Y-%m-%d %H:%M UTC' >"${LAB_DIR}/slo-start.txt"
    log "SLO run started: $(cat "${LAB_DIR}/slo-start.txt") (also in ${LAB_DIR}/slo-start.txt)"
    log "check it in an hour and then daily: scripts/lab.sh status"
}

cmd_status() {
    use_profile "${PROFILE}"
    log "SLO run started: $(cat "${LAB_DIR}/slo-start.txt" 2>/dev/null || echo 'not started (scripts/lab.sh start)')"
    make -C "${REPO}" slo-run-status
}

cmd_autostart() {
    local line="@reboot sleep 60 && $(command -v minikube) start -p ${PROFILE} >>${LAB_DIR}/logs/reboot.log 2>&1"
    mkdir -p "${LAB_DIR}/logs"
    if crontab -l 2>/dev/null | grep -qF "minikube start -p ${PROFILE}"; then
        log "already installed: $(crontab -l | grep -F "minikube start -p ${PROFILE}")"
        return
    fi
    (crontab -l 2>/dev/null; echo "${line}") | crontab -
    log "installed in the user crontab: ${line}"
    log "Docker itself must start at boot (systemctl is-enabled docker: $(systemctl is-enabled docker 2>/dev/null || echo unknown))"
}

cmd_update() {
    use_profile "${PROFILE}"
    local before
    before="$(git -C "${REPO}" rev-parse HEAD)"
    git -C "${REPO}" pull --ff-only
    # Bash has already read this file: what follows must come from the pulled version.
    if [[ "$(git -C "${REPO}" rev-parse HEAD)" != "${before}" ]]; then
        log "pulled $(git -C "${REPO}" rev-parse --short HEAD): continuing with its scripts/lab.sh"
        exec "${REPO}/scripts/lab.sh" rollout
    fi
    cmd_rollout
}

cmd_rollout() {
    use_profile "${PROFILE}"
    build_images
    load_images "${PROFILE}"
    make -C "${REPO}" k8s-prod-apply
    kubectl apply -f "${REPO}/k8s/slo-run/"
    # The Deployments that run EOP's own images, plus Prometheus and Grafana, which read their
    # configuration and rules only at start. Prometheus keeps its data on the prometheus-data volume
    # and loses only the seconds it is down (Recreate).
    local deployments="eop-server api-gateway-stable api-gateway-canary ai-service corporate-service mttr-watcher edge-ingestor edge-sim slo-traffic prometheus grafana"
    for name in ${deployments}; do
        kubectl rollout restart "deployment/${name}"
    done
    for name in ${deployments}; do
        kubectl rollout status "deployment/${name}" --timeout=180s
    done
    make -C "${REPO}" slo-run-status
}

cmd_ready_time() {
    [[ "${CLEAN_PROFILE}" != "${PROFILE}" ]] || fail "EOP_LAB_CLEAN_PROFILE must not be the long-lived profile"
    local previous
    previous="$(kubectl config current-context 2>/dev/null || true)"
    # The long-lived profile is never touched: everything below runs on the throwaway one.
    trap '[[ -n "${previous}" ]] && kubectl config use-context "${previous}" >/dev/null 2>&1 || true' EXIT
    minikube -p "${CLEAN_PROFILE}" delete >/dev/null 2>&1 || true
    start_profile "${CLEAN_PROFILE}" "${CLEAN_CPUS}" "${CLEAN_MEMORY}"
    docker image inspect eop-server:local >/dev/null 2>&1 || build_images
    load_images "${CLEAN_PROFILE}"
    local result=0
    make -C "${REPO}" k8s-ready-time || result=1
    make -C "${REPO}" smoke-test-prod || result=1
    kubectl get pods -o wide
    minikube -p "${CLEAN_PROFILE}" delete
    ((result == 0)) || fail "ready-time or smoke-test-prod failed (log above)"
    log "ready-time and smoke-test-prod passed on a clean cluster"
}

cmd_smoke() {
    use_profile "${PROFILE}"
    make -C "${REPO}" smoke-test-prod
}

cmd_demo() {
    use_profile "${PROFILE}"
    make -C "${REPO}" demo-script
}

cmd_sla_export() {
    use_profile "${PROFILE}"
    make -C "${REPO}" sla-export OUT="${LAB_DIR}/sla-export-$(date +%Y%m%d-%H%M)"
}

cmd_evidence() {
    local out
    out="${LAB_DIR}/evidence-$(date +%Y%m%d-%H%M).tar.gz"
    mkdir -p "${LAB_DIR}"
    {
        echo "commit: $(git -C "${REPO}" rev-parse HEAD) ($(git -C "${REPO}" rev-parse --abbrev-ref HEAD))"
        echo "machine: $(nproc) cores, $(free -g | awk '/^Mem:/ {print $2}') GB"
        echo "minikube: $(minikube version --short 2>/dev/null); kubectl: $(kubectl version --client 2>/dev/null | head -1)"
        echo "slo start: $(cat "${LAB_DIR}/slo-start.txt" 2>/dev/null || echo not started)"
    } >"${LAB_DIR}/machine.txt"
    if minikube -p "${PROFILE}" status >/dev/null 2>&1; then
        kubectl config use-context "${PROFILE}" >/dev/null
        make -C "${REPO}" slo-run-status >"${LAB_DIR}/slo-run-status.txt" 2>&1 || true
        kubectl get pods,pvc -o wide >"${LAB_DIR}/pods.txt" 2>&1 || true
    fi
    # The TSDB snapshot (make slo-run-export) is large: it goes only at the end of the run.
    # This command's own log is still being written, so it stays out, and a file that changes while
    # tar reads it (exit 1) is not a failure; only exit 2 is.
    local status=0
    tar -czf "${out}" -C "${LAB_DIR}" --exclude='evidence-*.tar.gz' \
        --exclude="logs/$(basename "${LOG_FILE}")" --warning=no-file-changed . || status=$?
    ((status <= 1)) || fail "tar failed (exit ${status}); ${out} is incomplete"
    log "evidence: ${out} ($(du -h "${out}" | cut -f1)); copy it to a USB drive or your Drive"
}

main() {
    local command="${1:-}"
    case "${command}" in
        check | setup | start | status | autostart | update | rollout | ready-time | smoke | demo | sla-export | evidence)
            start_log "${command}"
            "cmd_${command//-/_}"
            ;;
        -h | --help | help | "") usage ;;
        *)
            usage
            exit 2
            ;;
    esac
}

main "$@"
