#!/usr/bin/env bash
# Runs mxl-writer-nmos against a real nmos-cpp registry and checks what its
# MXL Senders promise, reading the flows through mxl-writer-status:
#
#   - the Node registers one Sender per flow, each naming the flow written and
#     the domain written into, with the flow's format and group hint, no
#     interface bindings, no manifest and a /transportfile that answers 404;
#   - both flows are written from the start;
#   - deactivating a Sender stops its flow and nothing else, and activating it
#     starts the flow again;
#   - another flow or another domain is refused, a no-op activation is not;
#   - a restarted Node keeps its resource ids;
#   - a writer that dies takes the Node with it, so the pod restarts;
#   - the ball crosses the frame's middle and the ticks start on whole TAI
#     seconds, with NMOS on, after a restart, after a Sender was stopped and
#     started, and with NMOS off.
#
# Then it runs the AMWA NMOS Testing Tool suites IS-04-01, IS-05-01, IS-05-02
# and BCP-007-03-01. Exits non-zero if any check or suite fails. Needs docker,
# curl and jq.
#
#   test/nmos/run.sh                  # builds the image, runs every check and suite
#   WRITER_IMAGE=... test/nmos/run.sh # uses an image already built
#   test/nmos/run.sh BCP-007-03-01    # the checks and one suite
set -euo pipefail

cd "$(dirname "$0")"
REPO_ROOT=$(cd ../.. && pwd)
export WRITER_IMAGE="${WRITER_IMAGE:-dmf-mf-mxl-writer:nmos-dev}"
export PROBE_IMAGE="${PROBE_IMAGE:-dmf-mf-mxl-writer:nmos-build}"
NODE=172.30.97.20
PORT=8080
QUERY=http://172.30.97.10:8010/x-nmos/query/v1.3
CONN=http://$NODE:$PORT/x-nmos/connection/v1.2/single/senders
STATUS=http://172.30.97.21:9102/status

if [ "${WRITER_IMAGE}" = "dmf-mf-mxl-writer:nmos-dev" ]; then
  docker build -t "${WRITER_IMAGE}" "${REPO_ROOT}"
fi
if [ "${PROBE_IMAGE}" = "dmf-mf-mxl-writer:nmos-build" ]; then
  docker build --target build -t "${PROBE_IMAGE}" "${REPO_ROOT}"
fi

# The runtime root the platform would have materialised: the primary domain,
# and a second one at domains/ID, which is the one the writer writes into.
PRIMARY_ID=6f1c2a3b-4d5e-4f60-8a71-b2c3d4e5f607
export SECOND_ID=fec11c1b-9fab-4275-ab2c-ef676fa2e081
export VIDEO_FLOW=5a1e0000-0000-4000-8000-0000000000f2
export AUDIO_FLOW=5a1e0000-0000-4000-8000-0000000000a2
OTHER_FLOW=5a1e0000-0000-4000-8000-0000000000ff
MXL_ROOT=$(mktemp -d /dev/shm/mxl-writer-nmos-lane.XXXXXX)
FLOW_CONFIG=$(mktemp -d)
export MXL_ROOT FLOW_CONFIG
mkdir -p "${MXL_ROOT}/domain" "${MXL_ROOT}/domains/${SECOND_ID}"
for d in "domain:${PRIMARY_ID}" "domains/${SECOND_ID}:${SECOND_ID}"; do
  printf '{"id":"%s","label":"lane","description":"conformance lane domain","tags":{}}\n' \
    "${d#*:}" >"${MXL_ROOT}/${d%%:*}/domain_def.json"
done
chmod -R 0777 "${MXL_ROOT}"

# The definitions in the shape the mxl-writer chart renders them, the audio
# sample rate without a denominator as it does.
cat >"${FLOW_CONFIG}/video-flow.json" <<EOF
{"description": "Video output for ${VIDEO_FLOW}", "id": "${VIDEO_FLOW}",
 "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["lane:Video"]}, "format": "urn:x-nmos:format:video",
 "label": "Video output for ${VIDEO_FLOW}", "parents": [], "media_type": "video/v210",
 "grain_rate": {"numerator": 50, "denominator": 1}, "frame_width": 648, "frame_height": 360,
 "interlace_mode": "progressive", "colorspace": "BT709",
 "components": [{"name": "Y", "width": 648, "height": 360, "bit_depth": 10},
                {"name": "Cb", "width": 324, "height": 360, "bit_depth": 10},
                {"name": "Cr", "width": 324, "height": 360, "bit_depth": 10}]}
EOF
cat >"${FLOW_CONFIG}/audio-flow.json" <<EOF
{"description": "Audio output for ${AUDIO_FLOW}", "id": "${AUDIO_FLOW}",
 "tags": {"urn:x-nmos:tag:grouphint/v1.0": ["lane:Audio"]}, "format": "urn:x-nmos:format:audio",
 "label": "Audio output for ${AUDIO_FLOW}", "parents": [], "media_type": "audio/float32",
 "sample_rate": {"numerator": 48000}, "channel_count": 2, "bit_depth": 32}
EOF
chmod -R a+r "${FLOW_CONFIG}"

OFF_NAME="mxl-writer-nmos-off-$$"
cleanup() {
  docker rm -f "$OFF_NAME" >/dev/null 2>&1 || true
  docker compose --profile probe down -v --remove-orphans >/dev/null 2>&1 || true
  # The flows belong to the container's root, so they are removed from inside one.
  docker run --rm -v "${MXL_ROOT}:/r" --entrypoint sh "$WRITER_IMAGE" -c 'rm -rf /r/*' >/dev/null 2>&1 || true
  rm -rf "${MXL_ROOT}" "${FLOW_CONFIG}"
}
trap cleanup EXIT
docker compose down -v --remove-orphans >/dev/null 2>&1 || true
docker compose up -d registry writer status

in_lane() { docker compose exec -T registry curl -s "$@"; }
rc=0
fail() { echo "FAIL $*"; rc=1; }

# until_ok SECONDS CMD... runs CMD once a second until it succeeds or time is up.
until_ok() {
  local n=$1; shift
  for _ in $(seq 1 "$n"); do "$@" && return 0; sleep 1; done
  return 1
}

senders_listed() { [ "$(in_lane "$QUERY/senders/" | jq length 2>/dev/null)" = 2 ]; }
until_ok 120 senders_listed || { fail "the Node never registered two senders"; docker compose logs writer | tail -30; exit 1; }

sender_id() { in_lane "$QUERY/senders/" | jq -r --arg n "$1" '.[] | select(.tags["urn:x-nvnmos:tag:name"][0] == $n) | .id' || true; }
VIDEO_TX=$(sender_id video)
AUDIO_TX=$(sender_id audio)

# writing FLOW: whether mxl-writer-status saw a commit within the last second.
writing() { [ "$(in_lane "$STATUS" | jq -r --arg f "$1" '.flows[] | select(.id == $f) | .writing' 2>/dev/null)" = true ]; }
not_writing() { ! writing "$1"; }
# A flow quiet for a moment proves nothing -- a writer started again straight
# away would pass that -- so it has to stay quiet.
stays_quiet() { until_ok 15 not_writing "$1" && sleep 3 && not_writing "$1"; }

# Every Connection API href the Node advertises has to be reachable at the
# address it was given; a controller tries them in order.
hrefs=$(in_lane "$QUERY/devices/" | jq -r '.[].controls[].href' | sort -u || true)
if [ -z "$hrefs" ] || grep -v "^http://$NODE:" <<<"$hrefs" >/dev/null; then
  fail "advertised hrefs"; sed 's/^/    /' <<<"${hrefs:-(none)}"
else
  echo "PASS advertised hrefs"
fi

# One Sender per flow, as BCP-007-03 has it, each active from the start with
# the flow the writer writes and the domain it writes into -- the second one,
# not the primary.
senders_rc=0
for pair in "$VIDEO_TX:$VIDEO_FLOW:video:Video" "$AUDIO_TX:$AUDIO_FLOW:audio:Audio"; do
  IFS=: read -r tx flow role group_role <<<"$pair"
  [ -n "$tx" ] || { fail "the $role sender is missing"; senders_rc=1; continue; }
  got=$(in_lane "$QUERY/senders/$tx" | jq -c '{t: .transport, b: .interface_bindings, m: .manifest_href, h: .tags["urn:x-nmos:tag:grouphint/v1.0"], l: .label}' || true)
  want=$(jq -nc --arg r "$role" --arg g "$group_role" '{t: "urn:x-nmos:transport:mxl", b: [], m: null, h: ["lane:" + $g], l: "lane " + $r}')
  [ "$got" = "$want" ] || { fail "$role sender is $got, want $want"; senders_rc=1; }
  got=$(in_lane "$CONN/$tx/active" | jq -c '{m: .master_enable, f: .transport_params[0].mxl_flow_id, d: .transport_params[0].mxl_domain_id}' || true)
  want=$(jq -nc --arg f "$flow" --arg d "$SECOND_ID" '{m: true, f: $f, d: $d}')
  [ "$got" = "$want" ] || { fail "$role /active is $got, want $want"; senders_rc=1; }
  code=$(in_lane -o /dev/null -w '%{http_code}' "$CONN/$tx/transportfile")
  [ "$code" = 404 ] || { fail "$role /transportfile answered $code"; senders_rc=1; }
done
video_flow=$(in_lane "$QUERY/senders/$VIDEO_TX" | jq -r .flow_id || true)
got=$(in_lane "$QUERY/flows/$video_flow" | jq -c '[.media_type, .frame_width, .frame_height, .grain_rate]' || true)
[ "$got" = '["video/v210",648,360,{"denominator":1,"numerator":50}]' ] || { fail "the video Flow states $got"; senders_rc=1; }
[ "$video_flow" != "$VIDEO_FLOW" ] || { fail "the IS-04 Flow reuses the MXL flow id"; senders_rc=1; }
audio_flow=$(in_lane "$QUERY/senders/$AUDIO_TX" | jq -r .flow_id || true)
got=$(in_lane "$QUERY/flows/$audio_flow" | jq -c '[.media_type, .sample_rate, .bit_depth]' || true)
[ "$got" = '["audio/float32",{"denominator":1,"numerator":48000},32]' ] || { fail "the audio Flow states $got"; senders_rc=1; }
audio_source=$(in_lane "$QUERY/flows/$audio_flow" | jq -r .source_id || true)
got=$(in_lane "$QUERY/sources/$audio_source" | jq -c '.channels | length' || true)
[ "$got" = 2 ] || { fail "the audio Source lists $got channels"; senders_rc=1; }
[ $senders_rc -eq 0 ] && echo "PASS senders advertise the flows, the domain, the format and the group written"

write_rc=0
for flow in "$VIDEO_FLOW" "$AUDIO_FLOW"; do
  until_ok 30 writing "$flow" || { fail "flow $flow is not written"; write_rc=1; }
done
[ $write_rc -eq 0 ] && echo "PASS both flows are written from the start"

# phase NAME: the ball's rising crossings and the ticks' onsets, each in ms
# into its TAI second, all of them within 0.5 ms of a whole second, and at
# least one of each.
phase() {
  local got
  got=$(docker compose run --rm -T probe 2>/dev/null | tail -1)
  if jq -e '(.crossings | length) > 0 and (.ticks | length) > 0 and
            ([.crossings[], .ticks[]] | all(. < 0.5 or . > 999.5))' <<<"$got" >/dev/null 2>&1; then
    echo "PASS phase $1: $got"
  else
    fail "phase $1: $got"
  fi
}
phase "with NMOS on"

patch() {
  in_lane -o /dev/null -w '%{http_code}' -X PATCH -H 'Content-Type: application/json' "$CONN/$1/staged" -d "$2"
}
enable() { patch "$1" "{\"master_enable\":$2,\"activation\":{\"mode\":\"activate_immediate\"}}"; }

# BCP-007-03: an activation to false stops a Sender's MXL write, one to true
# starts it, and each Sender has its own.
toggle_rc=0
[ "$(enable "$VIDEO_TX" false)" = 200 ] || { fail "deactivating the video sender was refused"; toggle_rc=1; }
stays_quiet "$VIDEO_FLOW" || { fail "the video flow is still written after deactivation"; toggle_rc=1; }
writing "$AUDIO_FLOW" || { fail "deactivating video stopped the audio flow"; toggle_rc=1; }
[ "$(in_lane "$CONN/$VIDEO_TX/active" | jq .master_enable || true)" = false ] || { fail "/active still enabled"; toggle_rc=1; }
[ "$(enable "$VIDEO_TX" true)" = 200 ] || { fail "activating the video sender was refused"; toggle_rc=1; }
until_ok 30 writing "$VIDEO_FLOW" || { fail "the video flow is not written again"; toggle_rc=1; }
got=$(in_lane "$CONN/$VIDEO_TX/active" | jq -c '[.master_enable, .transport_params[0].mxl_flow_id, .transport_params[0].mxl_domain_id]' || true)
[ "$got" = "[true,\"$VIDEO_FLOW\",\"$SECOND_ID\"]" ] || { fail "/active after activation is $got"; toggle_rc=1; }
[ "$(enable "$AUDIO_TX" false)" = 200 ] || { fail "deactivating the audio sender was refused"; toggle_rc=1; }
stays_quiet "$AUDIO_FLOW" || { fail "the audio flow is still written after deactivation"; toggle_rc=1; }
writing "$VIDEO_FLOW" || { fail "deactivating audio stopped the video flow"; toggle_rc=1; }
[ "$(enable "$AUDIO_TX" true)" = 200 ] || { fail "activating the audio sender was refused"; toggle_rc=1; }
until_ok 30 writing "$AUDIO_FLOW" || { fail "the audio flow is not written again"; toggle_rc=1; }
[ $toggle_rc -eq 0 ] && echo "PASS each sender starts and stops its own write"
phase "after the audio sender was stopped and started"

# The flow a booking writes is fixed by its configuration: another flow or
# domain is refused, and an activation that changes nothing is not.
fixed_rc=0
code=$(patch "$VIDEO_TX" "{\"transport_params\":[{\"mxl_flow_id\":\"$OTHER_FLOW\"}],\"activation\":{\"mode\":\"activate_immediate\"}}")
[ "$code" = 400 ] || { fail "staging another mxl_flow_id returned $code"; fixed_rc=1; }
code=$(patch "$VIDEO_TX" "{\"transport_params\":[{\"mxl_domain_id\":\"$PRIMARY_ID\"}]}")
[ "$code" = 400 ] || { fail "staging the primary domain returned $code"; fixed_rc=1; }
code=$(patch "$VIDEO_TX" "{\"master_enable\":true,\"transport_params\":[{\"mxl_flow_id\":\"$VIDEO_FLOW\",\"mxl_domain_id\":\"$SECOND_ID\"}],\"activation\":{\"mode\":\"activate_immediate\"}}")
[ "$code" = 200 ] || { fail "a no-op activation returned $code"; fixed_rc=1; }
writing "$VIDEO_FLOW" || { fail "a no-op activation stopped the video flow"; fixed_rc=1; }
[ $fixed_rc -eq 0 ] && echo "PASS another flow or domain is refused, a no-op activation is not"

# The ids derive from the seed, so a controller's connections survive a
# restart; the restart also comes back with every Sender active.
docker compose restart writer >/dev/null
same_ids() { [ "$(sender_id video)" = "$VIDEO_TX" ] && [ "$(sender_id audio)" = "$AUDIO_TX" ] && writing "$VIDEO_FLOW" && writing "$AUDIO_FLOW"; }
if until_ok 60 same_ids; then
  echo "PASS a restarted Node keeps its resource ids"
else
  fail "after a restart the senders are $(sender_id video) and $(sender_id audio)"
fi
phase "after a restart"

suites=("$@")
[ ${#suites[@]} -gt 0 ] || suites=(IS-04-01 IS-05-01 IS-05-02 BCP-007-03-01)

for suite in "${suites[@]}"; do
  case "$suite" in
    IS-04-01)      args=(--host "$NODE" --port "$PORT" --version v1.3) ;;
    IS-05-01)      args=(--host "$NODE" --port "$PORT" --version v1.2) ;;
    IS-05-02)      args=(--host "$NODE" "$NODE" --port "$PORT" "$PORT" --version v1.3 v1.2) ;;
    BCP-007-03-01) args=(--host "$NODE" "$NODE" --port "$PORT" "$PORT" --version v1.3 v1.2) ;;
    *) echo "unknown suite $suite" >&2; exit 2 ;;
  esac
  log="$(mktemp)"
  docker compose run --rm testing suite "$suite" "${args[@]}" >"$log" 2>&1 || true
  junit=$(sed -n '/----- BEGIN JUNIT -----/,/----- END JUNIT -----/p' "$log")
  # The tool exits non-zero on warnings too. A warning is advice, so only a
  # failure of type Fail, an error, no report at all, or a report in which
  # nothing passed fails the suite.
  cases=$(grep -o '<testcase' <<<"$junit" | wc -l)
  unpassed=$(grep -oE '<(skipped|failure|error)[ >]' <<<"$junit" | wc -l)
  if ! grep -q '<testsuite' <<<"$junit"; then
    echo "FAIL $suite (no report written)"
    tail -20 "$log" | sed 's/^/    /'
    rc=1
  elif [ "$cases" -le "$unpassed" ]; then
    echo "FAIL $suite (nothing passed)"
    rc=1
  elif grep -qE '<failure type="Fail"|<error ' <<<"$junit"; then
    echo "FAIL $suite"
    rc=1
  else
    echo "PASS $suite"
  fi
  grep -oE '<(failure|error) [^>]*message="[^"]*' <<<"$junit" | sed 's/^/    /' || true
  rm -f "$log"
done

# A writer that dies while its Sender is active takes the Node with it, so the
# pod restarts rather than advertising a flow nobody writes. The suites leave
# Senders deactivated, so both are activated first.
enable "$VIDEO_TX" true >/dev/null
enable "$AUDIO_TX" true >/dev/null
until_ok 30 writing "$VIDEO_FLOW" || fail "the video flow is not written after the suites"
docker compose exec -T writer sh -c 'kill -9 $(pidof -s mxl-gst-testsrc)' >/dev/null 2>&1 || true
exited() { [ "$(docker inspect -f '{{.State.Status}}' "$(docker compose ps -aq writer)")" = exited ]; }
if until_ok 10 exited && [ "$(docker inspect -f '{{.State.ExitCode}}' "$(docker compose ps -aq writer)")" = 1 ]; then
  echo "PASS a dead writer stops the Node with exit status 1"
else
  fail "a dead writer left the Node $(docker inspect -f '{{.State.Status}} {{.State.ExitCode}}' "$(docker compose ps -aq writer)")"
fi

# Without NMOS the image runs the test source alone, as its entrypoint,
# with the same arguments; the content falls on the same media time.
docker run -d --name "$OFF_NAME" --network none -v "${MXL_ROOT}:/run/mxl" -v "${FLOW_CONFIG}:/etc/flow-config:ro" \
  "$WRITER_IMAGE" $(docker compose config --format json | jq -r '.services.writer.command | join(" ")') >/dev/null
sleep 3
phase "with NMOS off"
exit $rc
