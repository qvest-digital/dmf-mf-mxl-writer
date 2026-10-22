# dmf-mf-mxl-writer

Producer-side container image for the mxl-k8s demo pipeline.

Bundles `mxl-gst-testsrc`, `mxl-info` and `mxl-writer-status`, built from
the libmxl source of the `ghcr.io/qvest-digital/go-mxl-runtime` image it runs
on, so the writer and the mxl-k8s gateway DaemonSet that reads its flows link
the same libmxl. The entrypoint is `mxl-gst-testsrc`.

## Image

`ghcr.io/qvest-digital/dmf-mf-mxl-writer:<tag>`

Tags follow `<git-sha>` (every build), `latest` (main only), and `<version>` on release.

`ghcr.io/qvest-digital/mxl-dmf-writer` is the name this repository had before the
rename. It keeps the tags it already holds and receives no further build; it can
go once nothing resolves it.

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
complete; a header still incomplete after 5 s is dropped, one over 8 KiB gets
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

## Build locally

```bash
docker build -t dmf-mf-mxl-writer:dev .
```

The build runs `mxl-writer-status-tests` and fails with them. They cover
`tools/mxl-writer-status/status.hpp` and the HTTP server in `server.hpp`.

Override defaults with `--build-arg`:

```bash
docker build \
  --build-arg GO_MXL_TAG=<tag> \
  --build-arg MXL_SRC=<repository URL> \
  --build-arg MXL_REF=<commit> \
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

The image builds `MXL_SRC` @ `MXL_REF` and then applies `patches/*.patch`
on top. 0001-0005 fix producer pacing specific to this pipeline, 0006 adds
the overlay-format setting and 0007 makes the audio tone describable. They
touch `mxl-gst-testsrc` only and are not upstreamed.

To bump the stock base:
1. Set `MXL_SRC` and `MXL_REF` to what go-mxl's `.github/libmxl.version`
   names at the new `GO_MXL_TAG`.
2. Re-apply the series: `git apply --3way patches/*.patch` (resolve any
   conflicts against the new base, and drop a patch the new base makes
   redundant).
3. Re-test the pipeline end-to-end before shipping.
