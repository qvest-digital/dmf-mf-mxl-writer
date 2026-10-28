# dmf-mf-mxl-writer

Producer-side container image for the mxl-k8s demo pipeline.

Bundles `mxl-gst-testsrc`, `mxl-info`, `mxl-writer-status` and `mxl-writer-nmos`, built from
the libmxl source of the `ghcr.io/qvest-digital/go-mxl-runtime` image it runs
on, so the writer and the mxl-k8s gateway DaemonSet that reads its flows link
the same libmxl. The entrypoint is `mxl-gst-testsrc`.

## Image

`ghcr.io/qvest-digital/dmf-mf-mxl-writer:<tag>`

Tags follow `<git-sha>` (every build), `latest` (main only), and `<version>` on release.

`ghcr.io/qvest-digital/mxl-dmf-writer` is the name this repository had before the
rename. It keeps the tags it already holds and receives no further build; it can
go once nothing resolves it.

## Write metrics

`mxl-gst-testsrc --metrics-port <port>` serves, on every interface or on
`--metrics-address`, what each flow's writer calls returned, counted by
[libmxl-monitoring](https://github.com/marquies/libmxl-monitoring):

| path | answers |
| --- | --- |
| `/metrics` | Prometheus text, `mxl_writer_*`, one series per flow id |
| `/healthz` | 200 while the endpoint runs |

`mxl_writer_writes_total{result}` counts whole grains and committed samples as
`ok`. A grain filled as invalid because no frame arrived in time, and silence
written for samples that never arrived, count as `input_late`; failed opens
and commits as `permission_error` or `other_error`, and a create that found no
space left as `out_of_space`. `mxl_writer_open_failures_total{reason}` counts
creates that failed: `domain`, `permission` or `other`. A port that cannot be
bound ends the process with status 1; without the option nothing listens.
`mxl-writer-nmos` takes the option too and serves the flows of all its
processes on that one port (see [NMOS](#nmos)).

## Ring status

`mxl-writer-status` serves the ring buffers of the flows the writer produces,
from the same image with the command overridden:

```
mxl-writer-status -d <domain> -f <flow-id> [-f <flow-id> ...] [-p 9102]
```

It opens each flow with a libmxl flow reader and reads only the flow's config
and runtime info, never a grain or a sample. The reader maps the flow's data
read-only. For video, data and event flows it also opens the flow's `access`
file read-write, which libmxl touches only when a grain is read, so the
`lastReadTime` it derives from that file keeps reporting real consumers. On a
read-only mount that open fails, which libmxl ignores. The tool reads the
domain directly and needs no intent shim (`LD_PRELOAD`). A domain or flow that
is not there yet is retried every second, and a flow removed or created anew
under it, before or after its first commit, is opened again within a second.

The header is read every 2 ms. Every change of head index or write time counts
as a commit. A grain commit is timed by the write time the flow records rather
than by when the change was seen, so video and data rates and intervals are on
the flow's own clock. libmxl's continuous writer moves the head without
stamping a write time, so a sample commit is timed by the read that saw it,
within those 2 ms.

| path | answers |
| --- | --- |
| `/` | the ring view, a page drawing each flow's ring and commit timing from `/stream` |
| `/stream` | server-sent events, 20 batches a second: per flow its ring length, slot payload, period, span of one commit, a reader's grace, head and media-clock index, and every commit since the last batch as `[head, write ms, ms after media time, step, kind]`; at most 8 at once |
| `/status` | JSON, one object per flow: state, label, declared format, ring length, payload and duration, batch hints, head, head against the media clock, head rate, write age, read age for grains and events, counters, last error |
| `/metrics` | Prometheus text, `mxl_writer_status_*` |
| `/healthz` | 200 while the process serves |

One thread serves every connection. A request is answered once its header is
complete; a header still incomplete after 5 s is dropped, one over 64 KiB gets
431, a malformed one 400 and any method but `GET` 405. A client that connects
and sends nothing does not delay anybody else's request.

The ring view draws a slot per grain, or per sample span for audio, coloured by
when it was committed against its media timestamp: ahead of it, within one
grain or one commit batch of samples, within the grace a reader at the head
allows (the rule `mxl-reader` applies), or later. Grains the head moved past
without writing get a colour of their own. Markers
show the head and the current media-clock index. Below it: the number of
slots, the payload of one slot (a grain of every plane, or one float sample of
every channel; a data grain has no fixed size), the payload of the whole ring,
and the media time it holds. Two strips plot each commit of the last six
seconds against its media timestamp and the interval between commits.

| metric | |
| --- | --- |
| `mxl_writer_status_flow_info{media_type,unit}`, `_flow_state{state}`, `_flow_writing` | what the flow is, where its watch stands (`starting`, `domain_unavailable`, `flow_not_found`, `permission_denied`, `flow_error`, `open`), and whether a commit landed within the last second |
| `mxl_writer_status_ring_length`, `_ring_seconds`, `_ring_bytes` | the ring, in the flow's unit, in media time, in payload bytes |
| `mxl_writer_status_head_index`, `_head_rate`, `_nominal_rate` | the head, how fast it advanced over the last second of commits, and the declared rate |
| `mxl_writer_status_head_lead_seconds` | head index minus the current media-clock index; negative is behind the clock |
| `mxl_writer_status_write_age_seconds` | wall clock minus the last commit |
| `mxl_writer_status_read_age_seconds` | wall clock minus the flow's last read time, for grains and events only. libmxl sets it to the creation time until a consumer reads, and a sample read never moves it, so audio flows have none |
| `mxl_writer_status_commits_total`, `_skipped_total`, `_backward_steps_total`, `_stalls_total`, `_opens_total`, `_lost_total` | commits seen, grains the head moved past unwritten, backward steps of the head, timeouts without a commit, opens, and flows removed or replaced while open |

An id that is not a lowercase flow UUID exits with status 2; a port that
cannot be bound exits with status 1.

## NMOS

`mxl-writer-nmos` runs the test source as an NMOS Node: it registers over IS-04
and serves an IS-05 Connection API with one BCP-007-03 MXL Sender per flow,
through NvNmos. It takes `mxl-gst-testsrc`'s own command line in place of the
entrypoint, and its configuration from the environment:

```
mxl-writer-nmos -d <domain> [-v <video flow json> ...] [-a <audio flow json> ...] [-g <group>] [--metrics-port <port>]
```

| Variable | Default | Description |
|---|---|---|
| `NMOS_HOST_ADDRESS` | *(required)* | Address the Node's APIs are reached at, which the registry hands to controllers |
| `NMOS_HOST_NAME` | *(the pod's DNS name)* | DNS name the Node's APIs are reached at. It must contain a dot and must not be an address. Required outside Kubernetes. The Node advertises its APIs under the host name, then the address |
| `NMOS_POD_NAMESPACE` | *(required where `NMOS_HOST_NAME` is unset)* | Namespace of the pod. The Node's host name is then the pod's DNS name, `<address with dashes>.<namespace>.pod.<cluster domain>`, which CoreDNS serves with `pods insecure` or `pods verified`. The cluster domain is taken from the `<namespace>.svc.` entry in the last search line of `/etc/resolv.conf` |
| `NMOS_SEED` | *(required)* | Resource ids derive from it, so it must survive a restart and differ between instances |
| `NMOS_HTTP_PORT` | *(library default)* | Port of the Node and Connection APIs |
| `NMOS_REGISTRY_HOST` / `NMOS_REGISTRY_PORT` | *(unset -- DNS-SD)* | A fixed IS-04 Registration API, which disables discovery |
| `NMOS_SYSTEM_HOST` / `NMOS_SYSTEM_PORT` | *(the registry's)* | A fixed IS-09 System API. nmos-cpp serves it beside the registry |
| `NMOS_DNS_DOMAIN` | *(the host name's domain, else the library default)* | DNS-SD domain to browse for a registry where none is fixed |
| `NMOS_LABEL` / `NMOS_DESCRIPTION` | `MXL test writer` / `Test patterns written into MXL flows` | Node and Device label and description. A Sender is labelled `<label> video` or `<label> audio` |

Only the `-x value` and `--name value` forms are taken. An option the test
source does not take, an option without its value, a video option without
`-v` or an audio option without `-a` exits with status 2, as does a flow id that
is not a lowercase UUID, a missing `NMOS_HOST_ADDRESS` or `NMOS_SEED`, or no
host name from `NMOS_HOST_NAME` or `NMOS_POD_NAMESPACE`.

| Sender | Flow | Format |
|---|---|---|
| `video` | the `id` of the `-v` definition | its `media_type`, `video/v210` |
| `audio` | the `id` of the `-a` definition | its `media_type`, `audio/float32` |

Each Sender carries the group hint `<group>:Video` or `<group>:Audio`; the flow
itself keeps the `<group>:video` or `<group>:audio` the test source writes into
it. Its Flow and Source state the definition's format. The IS-04 Flow id derives from the seed and is not the MXL
flow id, which IS-05 carries as `mxl_flow_id`. A Sender binds no interface, has
no manifest, and its `/transportfile` answers 404.

`mxl_domain_id` is the `id` in `<domain>/domain_def.json`, the file the
platform writes into every MXL domain. A directory named as a UUID must carry
that UUID in its file, or the Node does not start. Until the file is readable
the Node is retried every 10 s; the flows do not wait for it.

IS-05 constrains `mxl_flow_id` and `mxl_domain_id` to the one flow and the one
domain each Sender writes, so `auto` resolves to them, an activation that
names them changes nothing, and any other value is refused with 400.

- Each flow is written by an `mxl-gst-testsrc` process of its own, running the
  options that apply to that flow and the ones both share. What a flow carries
  at a media time does not depend on when its process started (see
  [Producer-pacing patches](#producer-pacing-patches)), so the two flows stay
  in step across restarts and toggles.
- Every Sender starts active.
- An activation that clears `master_enable` stops that flow's process, which
  releases the flow; the other flow is untouched. One that sets it starts the
  process again. A deactivation is not persisted: after a restart every Sender
  is active again.
- A process that exits while its Sender is active stops the Node with status 1,
  so the container restarts rather than advertise a flow nobody writes.
- With `--metrics-port <port>` the Node serves `/metrics` and `/healthz` on
  that port, on every interface. Each process serves its own flow's metrics
  on loopback, video on the port above it and audio two above, and every
  scrape of the Node collects them, 500 ms each at most, into one Prometheus
  text. A process that is stopped or does not answer leaves its flow out of
  that scrape; the Node's port stays up. A port above 65533 exits with status
  2, one that cannot be bound with status 1.
- `SIGTERM` stops the Node and then every process, against one 10 s
  deadline after which a process still running is killed. As the container's
  PID 1 it also reaps any process a writer leaves behind.

NvNmos's log lines are passed through with each distinct line printed at most
once a minute. Repeats in between are counted, and the count of those since the
last print is printed with the next one; a line that stops repeating gets its
last count, and how long ago it was last seen, once it has been quiet for a
minute. A Node that finds no registry therefore says so once a minute rather
than several times a second, and an outage that ends shows how long it lasted.

The Connection API is unauthenticated, as IS-05 is: whoever reaches the Node's
port can stop and start the flows.

`test/nmos/run.sh` runs the Node against a real nmos-cpp registry, reads the
flows through `mxl-writer-status`, and checks what the Senders advertise, that
each starts and stops its own write, that another flow or domain is refused,
that a restart keeps the resource ids, and that a dead writer stops the Node.
Then it runs the AMWA NMOS Testing Tool suites IS-04-01, IS-05-01, IS-05-02 and
BCP-007-03-01. It also checks, through `test/nmos/phase-probe.cpp`, that the
ball crosses the middle of the frame and the ticks start on whole TAI seconds
with NMOS on, after a restart, after a Sender was stopped and started, and with
NMOS off, that a video thread starved of CPU at 1080p restarts at most once
while it is slow and is back on the media time once it has the CPU again, and
that one whose source cannot be realigned keeps writing and tries again. It
needs docker, curl and jq, and runs locally, not in CI.

## Build locally

```bash
docker build -t dmf-mf-mxl-writer:dev .
```

The build runs `mxl-writer-status-tests` and `mxl-writer-nmos-tests` and fails
with them. They cover `tools/mxl-writer-status/status.hpp`, the HTTP server in
`server.hpp`, and `tools/mxl-writer-nmos/senders.hpp` and `repeats.hpp`.

Override defaults with `--build-arg`:

```bash
docker build \
  --build-arg GO_MXL_TAG=<tag> \
  --build-arg MXL_SRC=<repository URL> \
  --build-arg MXL_REF=<commit> \
  --build-arg MONITORING_REF=<libmxl-monitoring commit> \
  --build-arg VERSION=<what mxl-writer-status reports> \
  -t dmf-mf-mxl-writer:dev .
```

## Tag bump policy

`GO_MXL_TAG` must match the `ARG GO_MXL_TAG` in
[`qvest-digital/mxl-k8s`](https://github.com/qvest-digital/mxl-k8s)'s
`docker/gateway.Dockerfile`, so the producer and the gateway run the same
libmxl and libfabric.

`MXL_SRC` and `MXL_REF` are the repository and commit that go-mxl's
`.github/libmxl.version` names at `GO_MXL_TAG`, the libmxl
`go-mxl-runtime:${GO_MXL_TAG}` was built from. Bump all three together.
Renovate rewrites `GO_MXL_TAG` only; its pull requests need `MXL_SRC` and
`MXL_REF` set by hand.

## Producer-pacing patches

The image builds `MXL_SRC` @ `MXL_REF` and then applies `patches/*.patch` on
top. 0001-0005 fix producer pacing specific to this pipeline, 0006 adds the
overlay-format setting and 0007 makes the audio tone describable. 0008 seeks
every test source, before it plays, to the TAI time of the first grain or sample
it is committed at, so the ball and the ticks are a function of the media time:
the same in every process and after every restart. A sine's phase is not, since
no seek reaches it. 0009 commits each audio batch at its own sample range, which
libmxl takes by its exclusive end; before, audio landed one batch, 10 ms at 48
kHz, ahead of video. 0010 realigns the video source once its thread keeps real
time again after falling more than 30 grains behind: 0004 writes such frames at
the clock, every one of them, but the source's counter stayed behind, so the
picture carried an older media time for as long as the process ran. Once the lag
has stopped growing for 2 s the video pipeline starts again at the next grain's
media time, at most once per 10 s. A start that cannot put the source on the
media time leaves its frames written at the clock and is tried again 10 s later;
`MXL_TESTSRC_REFUSE_REALIGN` set makes it fail, for tests. A pipeline started
again that produces nothing for 5 s, or any other error that ends a flow's
thread, ends the process with status 1. 0011 records every writer call through
libmxl-monitoring, at `MONITORING_REF`, and adds `--metrics-port` (see
[Write metrics](#write-metrics)). They touch `mxl-gst-testsrc` only and are not
upstreamed.

To bump the stock base:
1. Set `MXL_SRC` and `MXL_REF` to what go-mxl's `.github/libmxl.version`
   names at the new `GO_MXL_TAG`.
2. Re-apply the series: `git apply --3way patches/*.patch` (resolve any
   conflicts against the new base, and drop a patch the new base makes
   redundant).
3. Re-test the pipeline end-to-end before shipping.
