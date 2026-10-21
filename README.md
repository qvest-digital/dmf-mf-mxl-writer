# dmf-mf-mxl-writer

Producer-side container image for the mxl-k8s demo pipeline.

Bundles `mxl-gst-testsrc` + `mxl-info`, built from the libmxl source of
the `ghcr.io/qvest-digital/go-mxl-runtime` image it runs on, so the writer
and the mxl-k8s gateway DaemonSet that reads its flows link the same
libmxl.

## Image

`ghcr.io/qvest-digital/dmf-mf-mxl-writer:<tag>`

Tags follow `<git-sha>` (every build), `latest` (main only), and `<version>` on release.

`ghcr.io/qvest-digital/mxl-dmf-writer` is the name this repository had before the
rename. It keeps the tags it already holds and receives no further build; it can
go once nothing resolves it.

## Build locally

```bash
docker build -t dmf-mf-mxl-writer:dev .
```

Override defaults with `--build-arg`:

```bash
docker build \
  --build-arg GO_MXL_TAG=<tag> \
  --build-arg MXL_SRC=<repository URL> \
  --build-arg MXL_REF=<commit> \
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
