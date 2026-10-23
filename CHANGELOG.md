# Changelog

## [1.0.0-rc.10](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.9...v1.0.0-rc.10) (2026-10-23)


### Bug Fixes

* **testsrc:** realign the video source once it keeps up again ([8f2fb98](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/8f2fb981366e3c0a100190e5e2179d4d3ccb8624))

## [1.0.0-rc.9](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.8...v1.0.0-rc.9) (2026-10-22)


### Bug Fixes

* **nmos:** print a repeating NvNmos log line once a minute ([#21](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/21)) ([5453492](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/54534922325d31d18be4ffc03bef1711aa24fc5d))

## [1.0.0-rc.8](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.7...v1.0.0-rc.8) (2026-10-22)


### Features

* **nmos:** register the writer's flows as BCP-007-03 senders ([61e20b9](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/61e20b976ab2f5f3d29c2ac8ed8a9706dab89fb9))
* **testsrc:** draw the ball and the ticks from the media time ([61e20b9](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/61e20b976ab2f5f3d29c2ac8ed8a9706dab89fb9))


### Bug Fixes

* **testsrc:** commit each audio batch at its own sample range ([61e20b9](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/61e20b976ab2f5f3d29c2ac8ed8a9706dab89fb9))

## [1.0.0-rc.7](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.6...v1.0.0-rc.7) (2026-10-22)


### Features

* **status:** serve the writer's ring buffers on port 9102 ([#17](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/17)) ([a67b970](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/a67b970f951ec8091c01ba0bef2fecf046575f86))

## [1.0.0-rc.6](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.5...v1.0.0-rc.6) (2026-10-22)


### Bug Fixes

* **deps:** move onto go-mxl 1.1.0-rc.7 and its libmxl ([#15](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/15)) ([dee912a](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/dee912a24212db4c021e1c8f56f0fbc301bd3ff6))

## [1.0.0-rc.5](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.4...v1.0.0-rc.5) (2026-10-21)


### Bug Fixes

* **deps:** build against the libmxl the gateway runs ([#13](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/13)) ([dda3f4e](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/dda3f4e0618335f1c7dcb98ec0552b11871bf92e))

## [1.0.0-rc.4](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.3...v1.0.0-rc.4) (2026-08-04)


### Dependencies

* move onto go-mxl 1.0.0-rc.12 and libmxl main ([#11](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/11)) ([161bc86](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/161bc862358146c655224a3ada9e88bd97a63490))

## [1.0.0-rc.3](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.2...v1.0.0-rc.3) (2026-07-31)


### Features

* **patches:** make the audio tone describable ([#9](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/9)) ([571e4b7](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/571e4b74f86487a9d470979e1607f88ab3b962bd))

## [1.0.0-rc.2](https://github.com/qvest-digital/dmf-mf-mxl-writer/compare/v1.0.0-rc.1...v1.0.0-rc.2) (2026-07-30)


### Continuous Integration

* publish the image under the renamed package ([#7](https://github.com/qvest-digital/dmf-mf-mxl-writer/issues/7)) ([7f48ce1](https://github.com/qvest-digital/dmf-mf-mxl-writer/commit/7f48ce17863de9a402a9f63d6bb92f974d756d9a))

## [1.0.0-rc.1](https://github.com/qvest-digital/mxl-dmf-writer/compare/v1.0.0-rc.0...v1.0.0-rc.1) (2026-07-17)


### Features

* bump MXL_REF to d6d2922 (MXL_OVERLAY_FORMAT env) ([#3](https://github.com/qvest-digital/mxl-dmf-writer/issues/3)) ([a2ca7dc](https://github.com/qvest-digital/mxl-dmf-writer/commit/a2ca7dc2a8e65ee9aec6dedcaceb4c4949e6c19c))
* initial mxl-dmf-writer image scaffold ([4bb1a8c](https://github.com/qvest-digital/mxl-dmf-writer/commit/4bb1a8c9a06b8ef66763975f4c887e0cbbc66b76))
* produce showcase flows at 480x270, not 1080p ([db5fa14](https://github.com/qvest-digital/mxl-dmf-writer/commit/db5fa14c2a03bab5e5486bfa5cd6aea2c412cf33))


### Bug Fixes

* build writer from the I420-overlay pacing fix (kills ~50% backfill) ([#1](https://github.com/qvest-digital/mxl-dmf-writer/issues/1)) ([c91dc64](https://github.com/qvest-digital/mxl-dmf-writer/commit/c91dc64df3d29feb09e178520187482cad27fd41))
* **build:** build testsrc from fork branch with appsink drift fix ([dad0136](https://github.com/qvest-digital/mxl-dmf-writer/commit/dad0136b8509c3dddce3137f135c174b8241d627))
* **build:** pin MXL_REF to fork commit 004e894 (wall-clock producer fix) ([476db9b](https://github.com/qvest-digital/mxl-dmf-writer/commit/476db9b6d6df66a7991f687c86e8948620aefef7))
* **ci:** full clone + checkout (shallow SHA fetch fails, exit 128) ([#2](https://github.com/qvest-digital/mxl-dmf-writer/issues/2)) ([8567416](https://github.com/qvest-digital/mxl-dmf-writer/commit/85674165d5ea94ff4f67fcd509584537d4a795a7))
* **image:** add pango runtime libs + gstreamer1.0-x for textoverlay/clockoverlay ([180c873](https://github.com/qvest-digital/mxl-dmf-writer/commit/180c8734dfc39c9788655fa960a0b9fced5ac968))
* **image:** install testsrc at /usr/bin + ship v210/audio flow JSON templates ([616ee4e](https://github.com/qvest-digital/mxl-dmf-writer/commit/616ee4e872b131fb94828c490ae91381ce1f04b8))
* writer width 1296 (divisible by 48) instead of 1284 ([#4](https://github.com/qvest-digital/mxl-dmf-writer/issues/4)) ([e5cf194](https://github.com/qvest-digital/mxl-dmf-writer/commit/e5cf194066bf509a55409c367940206ff4b7024c))


### Build System

* build testsrc from stock dmf-mxl + patches/, lock-step go-mxl rc.9 ([058b3be](https://github.com/qvest-digital/mxl-dmf-writer/commit/058b3be5d3a6d033226d00139573d33b655354d7))
* vendor the 6 producer-pacing patches (owned qvest delta, not upstreamed) ([d8ca3d5](https://github.com/qvest-digital/mxl-dmf-writer/commit/d8ca3d5f3250e5dd72112a2d32b0fa0bee0935e2))


### Continuous Integration

* add release-please (versioned rc tags on merge to main) ([6cf5a5b](https://github.com/qvest-digital/mxl-dmf-writer/commit/6cf5a5b9146c1c12be7c991b4db5daea9a425eb7))
* publish a version-tagged writer image on release ([ea856f1](https://github.com/qvest-digital/mxl-dmf-writer/commit/ea856f1b9ef6acbf3832df925ed1d004f907a466))
* **renovate:** track the go-mxl ARG via an inline marker ([db2255d](https://github.com/qvest-digital/mxl-dmf-writer/commit/db2255d5847cd9478686da16ef1227ad2090fc70))
