#!/usr/bin/env bash
# Tickrate tests of the q2trek forks.
#
#   ./run.sh [--negative] [--only REGEX] Q2PRO OPENFFA OPENTDM
#
# Q2PRO, OPENFFA, OPENTDM: checkouts of the forks (branch master). Their
# UPSTREAM_REF (default origin/master, origin being the upstream remote) is the
# upstream the peers and controls come from. Environment:
#   Q2_DATA  Quake 2 directory with baseq2/pak*.pak (engine stages need a map)
#   Q2_MAP   deathmatch map of that data (default q2dm1)
#   RATES    server rates (default "10 20 30 40 50 60")
#   OUT      builds and logs (default ${TMPDIR:-/tmp}/tickrate-tests)
# --negative runs the negative controls of controls.txt instead of the
# matrix: each mutated fork file must make its scenarios fail.
# --only REGEX runs the rated stages (NAME@RATE) and controls (neg-*) whose
# name matches; builds always run (debugging; not a green run).
#
# Every stage ends PASS, FAIL or NOT_RUN (a missing input). The exit code is 0
# only when every stage passed, 1 when one did not, 3 for a partial run
# (--only) whose stages all passed.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ONLY= NEGATIVE=0
while [[ ${1:-} == --* ]]; do
    case $1 in
        --negative) NEGATIVE=1; shift ;;
        --only) ONLY=${2:-}; shift 2 ;;
        *) break ;;
    esac
done
(($# == 3)) || { sed -n '3,/^set /{/^#/s/^# \{0,1\}//p}' "$0"; exit 2; }
Q2PRO=$(realpath -m "$1") OPENFFA=$(realpath -m "$2") OPENTDM=$(realpath -m "$3")
OUT=$(realpath -m "${OUT:-${TMPDIR:-/tmp}/tickrate-tests}")
ALL_RATES="10 20 30 40 50 60"
RATES=${RATES:-$ALL_RATES}
# the engine fixture of api3 makes its BSP directory under TMPDIR
export TMPDIR=$OUT/tmp
mkdir -p "$TMPDIR"
export Q2_MAP=${Q2_MAP:-q2dm1}
mkdir -p "$OUT/log"
SUMMARY=() SKIPPED=()

# stage NAME CMD...: rc 0 PASS, 77 NOT_RUN (first line of the log says why), else FAIL
stage() {
    local name=$1 log="$OUT/log/$1.log" rc
    shift
    if [[ -n $ONLY && ($name == *@* || $name == neg-*) && ! $name =~ $ONLY ]]; then SKIPPED+=("$name"); return; fi
    echo "=== $name"
    "$@" >"$log" 2>&1
    rc=$?
    case $rc in
        0) SUMMARY+=("PASS     $name") ;;
        77) SUMMARY+=("NOT_RUN  $name: $(head -1 "$log")") ;;
        *) grep -E '^ *FAIL|error' "$log" | head -8
           SUMMARY+=("FAIL     $name (log/$name.log)") ;;
    esac
}

# need VAR...: NOT_RUN unless every input is there
need() {
    local v
    for v; do
        case $v in
            Q2_DATA) compgen -G "${Q2_DATA:-/nonexistent}/baseq2/pak*.pak" >/dev/null ||
                { echo "missing input: Q2_DATA (no baseq2/pak*.pak in '${Q2_DATA:-}')"; return 77; } ;;
            *) [[ -d ${!v}/.git ]] || { echo "missing input: $v (no checkout at ${!v})"; return 77; } ;;
        esac
    done
}

# scenarios BIN ENV...: every scenario of `BIN list` passes (an empty list fails)
scenarios() {
    local bin=$1 s n=0 fail=0
    shift
    [[ -x $bin ]] || { echo "FAIL no harness $bin"; return 1; }
    for s in $(env "$@" "$bin" list); do
        n=$((n + 1))
        env "$@" timeout -k 5 120 "$bin" "$s" || { fail=$((fail + 1)); echo "FAIL $s"; }
    done
    echo "scenarios: $n, failed $fail"
    ((n && !fail))
}

# meson_build DIR X11 VARIABLE_FPS TARGET [SRC [MESON_OPTION...]]: plain
# native build of the q2pro fork (or of SRC)
meson_build() {
    local dir=$1 x11=$2 fps=$3 target=$4 want=${5:-$Q2PRO} src
    shift $(($# < 5 ? $# : 5))
    need Q2PRO || return
    [[ -d $want/src ]] || { echo "missing input: no q2pro source at $want"; return 77; }
    if [[ -f $dir/build.ninja ]]; then
        src=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["directories"]["source"])' \
            "$dir/meson-info/meson-info.json")
        [[ $src == "$want" ]] || { echo "FAIL $dir was configured for $src, not $want (use another OUT)"; return 1; }
    else
        meson setup "$dir" "$want" -Dwrap_mode=nofallback -Dsystem-wide=false -Dsdl2=disabled -Dx11=$x11 \
            -Dwayland=disabled -Dopenal=disabled -Dsoftware-sound=disabled -Dlibcurl=disabled -Dlibjpeg=disabled \
            -Dlibpng=disabled -Davcodec=disabled -Dgame-new-api=false -Dvariable-fps=$fps "$@" || return 1
    fi
    ninja -C "$dir" "$target" || return 1
    grep -q "define USE_FPS $([[ $fps == true ]] && echo 1 || echo 0)" "$dir/config.h" ||
        { echo "FAIL USE_FPS of $dir does not match variable-fps=$fps"; return 1; }
}

# upstream NAME DIR: UPSTREAM_REF of a checkout, extracted into OUT/upstream/NAME
upstream() {
    local name=$1 dir=$2 ref
    need "$name" || return
    ref=${UPSTREAM_REF:-origin/master}
    git -C "$dir" rev-parse -q --verify "$ref^{commit}" >/dev/null ||
        { echo "missing input: $name has no $ref"; return 77; }
    rm -rf "$OUT/upstream/$name"
    mkdir -p "$OUT/upstream/$name"
    git -C "$dir" archive "$ref" | tar -x -C "$OUT/upstream/$name" && echo "$name upstream: $ref $(git -C "$dir" rev-parse --short "$ref")"
}

engine_harness() {     # engine_harness scheduler|client ENGINE_BUILD OUT_DIR [--replace ..]
    local kind=$1 build=$2 dir=$3
    shift 3
    python3 "$HERE/engine/build.py" "$kind" --engine-build "$build" --out "$dir" --data "$Q2_DATA" "$@"
}

scheduler() {          # scheduler ENGINE_BUILD DIR RATE
    need Q2PRO Q2_DATA || return
    engine_harness scheduler "$1" "$2" &&
        scenarios "$2/driver" SCHED_GAME="$2/testgame.so" SCHED_BASEDIR="$2/base" SCHED_RATE="$3"
}

client() {             # client ENGINE_BUILD DIR RATE KIND UPDATERATE [--replace ..]
    local build=$1 dir=$2 rate=$3 kind=$4 update=$5
    shift 5
    need Q2PRO Q2_DATA || return
    engine_harness client "$build" "$dir" "$@" &&
        scenarios "$dir/driver" CL_GAME="$dir/testgame.so" CL_BASE="$dir/base" CL_RATE="$rate" CL_KIND="$kind" \
            CL_UPDATERATE="$update"
}

# upstream's copy of a q2pro file as a --replace argument
up() { local f; for f; do printf -- '--replace\n%s=%s\n' "$f" "$OUT/upstream/Q2PRO/$f"; done; }
client_kind() {        # client_kind KIND RATE FILE...
    local kind=$1 rate=$2 args
    shift 2
    need Q2PRO Q2_DATA || return
    [[ -d $OUT/upstream/Q2PRO/src ]] || { echo "missing input: upstream of Q2PRO (see stage upstream-q2pro)"; return 77; }
    mapfile -t args < <(up "$@")
    client "$OUT/bcl-fps" "$OUT/cl-$kind" "$rate" "$kind" 0 "${args[@]}"
}

capability() {
    need Q2PRO || return
    cc -I"$OUT/bded-fps" -I"$Q2PRO/inc" -I"$Q2PRO" -I"$Q2PRO/src" -iquote "$Q2PRO/src/server" -DHAVE_CONFIG_H \
        -D_GNU_SOURCE -DUSE_SERVER=1 -std=c11 -fms-extensions -fsigned-char -O2 -ffunction-sections \
        -fdata-sections "$HERE/engine/client/capability.c" -Wl,--gc-sections -o "$OUT/capability" && "$OUT/capability"
}

api3() {               # api3 MOD SRC_VAR DIR RATE [box|engine|sched] [--replace ..]
    local mod=$1 var=$2 dir=$3 rate=$4 how=$5 s rc=0 engine=()
    shift 5
    need "$var" || return
    [[ $how != box ]] && engine=(--engine)
    python3 "$HERE/mods/api3/build.py" --engine-build "$OUT/bded-fps" --out "$dir" --mod "$mod" \
        --mod-src "${!var}" "${engine[@]}" "$@" || return 1
    [[ $how != sched ]] && { scenarios "$dir/harness" HARNESS_RATE="$rate"; return; }
    for s in 30 10 batch; do scenarios "$dir/harness" HARNESS_RATE="$rate" HARNESS_SCHED=$s || rc=1; done
    return $rc
}

# fleet HOST_ENV PEER...: a real-time listen server of the fork client
# harness (fleet_host) on a free UDP port and peers "NAME DIR ENV" (fleet_peer)
# at once; all must pass
free_port() { python3 -c 'import socket; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'; }
peers() {              # peers PORT PEER...: start the peers, append their PIDs to PIDS
    local port=$1 p name dir penv
    shift
    for p; do
        read -r name dir penv <<<"$p"
        [[ -x $dir/driver ]] || { echo "missing input: $dir/driver (built by the client stages)"; return 77; }
        env CL_GAME="$dir/testgame.so" CL_BASE="$dir/base" $penv CL_NAME="$name" CL_QPORT=$((${#PIDS[@]} + 1)) \
            CL_CONNECT="127.0.0.1:$port" timeout -k 5 90 "$dir/driver" fleet_peer >"$LOGD/$name.log" 2>&1 &
        PIDS+=($!)
        sleep 1     # the server keeps one challenge per IP address
    done
}
collect() {            # wait for PIDS, print the logs
    local p rc=0
    for p in "${PIDS[@]}"; do wait "$p" || rc=1; done
    for p in "$LOGD"/*.log; do echo "--- $(basename "$p" .log)"; cat "$p"; done
    return $rc
}
fleet() {              # fleet RATE NEW LEGACY NOFPS: expected rates of the peers (protocol 34: 10)
    local r=$1 port host=$OUT/cl-fps
    need Q2PRO Q2_DATA || return
    [[ -x $host/driver ]] || { echo "missing input: $host/driver (built by the client stages)"; return 77; }
    port=$(free_port) || return 1
    LOGD=$OUT/fleet/$r PIDS=()
    mkdir -p "$LOGD"
    env CL_GAME="$host/testgame.so" CL_BASE="$host/base" CL_RATE="$r" CL_PEERS="h_new:$2:1,h_legacy:$3:0,h_nofps:$4:0,h_v34:10:0" \
        CL_SERVE="$port" timeout -k 5 120 "$host/driver" fleet_host >"$LOGD/host.log" 2>&1 &
    PIDS+=($!)
    sleep 2
    peers "$port" "h_new $OUT/cl-fps CL_RATE=$r" "h_legacy $OUT/cl-legacy CL_RATE=$r CL_KIND=legacy" \
        "h_nofps $OUT/cl CL_RATE=$r" "h_v34 $OUT/cl CL_RATE=$r CL_PROTOCOL=34" || { kill "${PIDS[@]}"; return 77; }
    collect
}
# an upstream dedicated server (UPSTREAM_REF, variable FPS) at RATE with the fork client
fleet_upstream() {
    local r=$1 port dir=$OUT/cl-fps host
    need Q2PRO Q2_DATA || return
    [[ -x $OUT/bup-ded/q2proded && -x $dir/driver ]] ||
        { echo "missing input: $OUT/bup-ded/q2proded or $dir/driver (built by earlier stages)"; return 77; }
    port=$(free_port) || return 1
    LOGD=$OUT/fleet/upstream-$r PIDS=()
    mkdir -p "$LOGD"
    timeout -k 5 60 "$OUT/bup-ded/q2proded" +set basedir "$dir/base" +set homedir "$dir/base" +set sys_console 0 \
        +set sys_forcegamelib "$dir/testgame.so" +set logfile 0 +set deathmatch 1 +set maxclients 4 \
        +set net_ip 127.0.0.1 +set net_port "$port" +set sv_fps "$r" +map "$Q2_MAP" >"$LOGD/host.log" 2>&1 &
    host=$!
    sleep 2
    peers "$port" "h_fork $dir CL_RATE=$r CL_KIND=oldsv CL_PEER_LEAVE=1" || { kill "$host"; return 77; }
    collect
    local rc=$?
    kill "$host" 2>/dev/null
    wait "$host"
    return $rc
}

# status_n RATE: the fork server publishes sv_fps in serverinfo and has `status n`
status_n() {
    local r=$1 port dir=$OUT/cl-fps host pw=tt$RANDOM$RANDOM rc
    need Q2PRO Q2_DATA || return
    [[ -x $OUT/bded-fps/q2proded && -f $dir/testgame.so ]] ||
        { echo "missing input: $OUT/bded-fps/q2proded or $dir/testgame.so (built by earlier stages)"; return 77; }
    port=$(free_port) || return 1
    timeout -k 5 30 "$OUT/bded-fps/q2proded" +set basedir "$dir/base" +set homedir "$dir/base" +set sys_console 0 \
        +set sys_forcegamelib "$dir/testgame.so" +set logfile 0 +set deathmatch 1 +set maxclients 4 +set rcon_password "$pw" \
        +set net_ip 127.0.0.1 +set net_port "$port" +set sv_fps "$r" +map "$Q2_MAP" &
    host=$!
    python3 "$HERE/engine/server/status_n.py" "$port" "$pw" "$r"
    rc=$?
    kill "$host" 2>/dev/null
    wait "$host"
    return $rc
}

# remote_lerp: host test of the client's remote player smoothing module
remote_lerp() {
    need Q2PRO || return
    cc -std=c99 -Wall -Wextra -Werror -O2 -I"$Q2PRO/src/client" "$HERE/engine/client/remote_lerp_test.c" \
        "$Q2PRO/src/client/remote_lerp.c" -lm -o "$OUT/remote_lerp_test" && "$OUT/remote_lerp_test"
}

# control NAME FILE HARNESS RATE SCENARIOS SED...: see controls.txt. PASS
# when a scenario fails an assertion (a crash or a timeout is not a catch)
control() {
    local name=$1 file=$2 how=$3 rate=$4 list=$5 var dir=$OUT/neg/$1 rel copy s out bin caught=0 e=() run=() args=()
    shift 5
    var=${file%%/*} rel=${file#*/}
    var=${var^^}
    need "$var" Q2_DATA || return
    copy=$dir/$(basename "$rel")
    mkdir -p "$dir"
    if [[ $1 == @master ]]; then
        [[ -f $OUT/upstream/$var/$rel ]] || { echo "missing input: upstream of $var (see stage upstream-${var,,})"; return 77; }
        cp "$OUT/upstream/$var/$rel" "$copy"
    else
        for s; do e+=(-e "$s"); done
        sed "${e[@]}" "${!var}/$rel" >"$copy" || return 1
    fi
    cmp -s "${!var}/$rel" "$copy" && { echo "FAIL mutation did not apply"; return 1; }
    diff -u "${!var}/$rel" "$copy" | sed "s|${!var}/||; s|$OUT/|OUT/|"
    case $how in
        sched)
            engine_harness scheduler "$OUT/bded-fps" "$dir/build" --replace "$rel=$copy" || { echo "FAIL mutated build"; return 1; }
            bin=$dir/build/driver run=(SCHED_GAME="$dir/build/testgame.so" SCHED_BASEDIR="$dir/build/base" SCHED_RATE="$rate") ;;
        client*)    # client[:KIND[:UPDATERATE[:BUILD]]], BUILD an engine build in OUT (default bcl-fps)
            IFS=: read -r _ kind update build <<<"$how"
            kind=${kind:-new}
            case $kind in
                legacy) mapfile -t args < <(up src/client/entities.c src/client/main.c src/client/parse.c src/client/tent.c) ;;
                oldsv) mapfile -t args < <(up src/server/user.c) ;;
            esac
            engine_harness client "$OUT/${build:-bcl-fps}" "$dir/build" "${args[@]}" --replace "$rel=$copy" ||
                { echo "FAIL mutated build"; return 1; }
            bin=$dir/build/driver run=(CL_GAME="$dir/build/testgame.so" CL_BASE="$dir/build/base" CL_RATE="$rate"
                CL_KIND="$kind" CL_UPDATERATE="${update:-0}") ;;
        api3:*)
            [[ ${how#api3:} == engine ]] && args=(--engine)
            python3 "$HERE/mods/api3/build.py" --engine-build "$OUT/bded-fps" --out "$dir/build" --mod "${var,,}" \
                --mod-src "${!var}" "${args[@]}" --replace "$rel=$copy" || { echo "FAIL mutated build"; return 1; }
            bin=$dir/build/harness run=(HARNESS_RATE="$rate") ;;
        *) echo "FAIL unknown harness $how"; return 1 ;;
    esac
    [[ $list == '*' ]] && list=$(env "${run[@]}" "$bin" list)
    for s in $list; do
        out=$(env "${run[@]}" timeout -k 5 120 "$bin" "$s")
        # an assertion (CHECK prints file:line), not an abort or a precondition
        grep -E '^  FAIL .+\.[ch]:[0-9]+: ' <<<"$out" && { caught=1; echo "negative control $name: caught by $s at $rate Hz"; break; }
    done
    ((caught)) || { echo "FAIL negative control $name: $list still pass at $rate Hz"; return 1; }
}

# on a fixed 10 Hz engine a requested rate cannot engage: both the API3 and
# the scheduler harness must refuse 60 Hz
neg_rate() {
    local dir=$OUT/neg/neg-rate rc=0 out
    need Q2PRO OPENFFA Q2_DATA || return
    python3 "$HERE/mods/api3/build.py" --engine-build "$OUT/bded" --out "$dir/api3" --mod openffa --mod-src "$OPENFFA" ||
        { echo "FAIL build"; return 1; }
    engine_harness scheduler "$OUT/bded" "$dir/sched" || { echo "FAIL build"; return 1; }
    out=$(HARNESS_RATE=60 timeout -k 5 60 "$dir/api3/harness" smoke)
    echo "$out"
    grep -q 'game error: module does not export GMF_VARIABLE_FPS' <<<"$out" || { echo "FAIL api3 did not refuse 60 Hz"; rc=1; }
    out=$(SCHED_GAME="$dir/sched/testgame.so" SCHED_BASEDIR="$dir/sched/base" SCHED_RATE=60 \
        timeout -k 5 60 "$dir/sched/driver" t01_early_call)
    echo "$out"
    grep -q 'precondition: server runs 10 Hz, requested 60 Hz' <<<"$out" || { echo "FAIL scheduler did not refuse 60 Hz"; rc=1; }
    return $rc
}

clean_state() { local d; for d in "$Q2PRO" "$OPENFFA" "$OPENTDM" "$HERE"; do echo "== $d"; git -C "$d" status --porcelain 2>&1; done; }
BEFORE=$(clean_state)

stage build-ded meson_build "$OUT/bded" disabled false q2proded
stage build-ded-fps meson_build "$OUT/bded-fps" disabled true q2proded
stage build-cl meson_build "$OUT/bcl" enabled false q2pro
stage build-cl-fps meson_build "$OUT/bcl-fps" enabled true q2pro
# the packetdup hack compiled: its build lists only t06_packetdup_loopback
stage build-cl-dup meson_build "$OUT/bcl-dup" enabled true q2pro "$Q2PRO" -Dpacketdup-hack=true
stage upstream-q2pro upstream Q2PRO "$Q2PRO"
if ((NEGATIVE)); then
    stage upstream-openffa upstream OPENFFA "$OPENFFA"
    stage upstream-opentdm upstream OPENTDM "$OPENTDM"
    stage neg-rate neg_rate
    while IFS= read -r line; do
        [[ $line == neg-* ]] || continue
        mapfile -t f < <(awk -F ' [|] ' '{ for (i = 1; i <= NF; i++) print $i }' <<<"$line")
        stage "${f[0]}" control "${f[@]}"
    done <"$HERE/controls.txt"
else
stage capability capability
stage remote-lerp remote_lerp
# the fixed (10 Hz only) engine
stage scheduler-fixed@10 scheduler "$OUT/bded" "$OUT/sched" 10
stage client-fixed@10 client "$OUT/bcl" "$OUT/cl" 10 new 0
for r in $RATES; do
    stage scheduler@$r scheduler "$OUT/bded-fps" "$OUT/sched-fps" "$r"
    stage client@$r client "$OUT/bcl-fps" "$OUT/cl-fps" "$r" new 0
    stage client-dup@$r client "$OUT/bcl-dup" "$OUT/cl-dup" "$r" new 0
    # a request below the server rate (60 -> 30, 50 and 40 -> 20, 30 and 20 -> 10)
    ((r > 10)) && stage client-u$((r / 20 * 10))@$r client "$OUT/bcl-fps" "$OUT/cl-fps" "$r" new $((r / 20 * 10))
    # an upstream client (upstream's client files) and an upstream server (upstream's user.c)
    stage client-legacy@$r client_kind legacy "$r" src/client/entities.c src/client/main.c src/client/parse.c \
        src/client/tent.c
    stage client-oldsv@$r client_kind oldsv "$r" src/server/user.c
    for m in openffa:OPENFFA opentdm:OPENTDM; do
        for how in box engine sched; do
            stage api3-${m%:*}-$how@$r api3 "${m%:*}" "${m#*:}" "$OUT/api3-${m%:*}-${how/sched/engine}" "$r" $how
        done
    done
done
# real time over UDP: rates written to a fork client, an upstream client, a
# client without variable FPS and one on protocol 34 by one fork server
# (README, expected rates)
for r in $RATES; do stage status-n@$r status_n "$r"; done
stage fleet@30 fleet 30 30 10 10
stage fleet@50 fleet 50 50 50 10
stage fleet@60 fleet 60 60 20 10
stage build-upstream-ded meson_build "$OUT/bup-ded" disabled true q2proded "$OUT/upstream/Q2PRO"
stage fleet-upstream@60 fleet_upstream 60
fi
[[ $(clean_state) == "$BEFORE" ]] || SUMMARY+=("FAIL     checkouts changed by the run: $(diff <(echo "$BEFORE") <(clean_state) | tr '\n' ' ')")

echo
echo "===== summary (q2pro $(git -C "$Q2PRO" rev-parse --short HEAD 2>/dev/null), openffa $(git -C "$OPENFFA" rev-parse --short HEAD 2>/dev/null), opentdm $(git -C "$OPENTDM" rev-parse --short HEAD 2>/dev/null); OUT=$OUT)"
printf '%s\n' "${SUMMARY[@]}"
printf '%s\n' "${SUMMARY[@]}" | grep -qv '^PASS' && exit 1
((NEGATIVE)) || [[ $RATES == "$ALL_RATES" ]] || { echo "partial run, not green: RATES='$RATES'"; exit 3; }
((${#SKIPPED[@]})) && { echo "partial run, not green: --only '$ONLY' left out ${#SKIPPED[@]} stage(s)"; exit 3; }
exit 0
