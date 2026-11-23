#!/bin/sh
# Records what a build stage compiled from source in a CycloneDX
# fragment. No package manager tracks these, so an SBOM generated from
# the finished image (Trivy, Syft) does not list them; the fragment
# travels in the image beside the installed files and is merged into the
# image SBOM by CI.
#
#   sbom.sh git OUT NAME CHECKOUT [PATCH...]
#       a git checkout of a GitHub repository, at its HEAD commit, with the
#       patches applied on top. The bom-ref carries NAME as well as the
#       commit: the mxl checkout is the commit go-mxl records as libmxl,
#       and CI merges components by bom-ref.
#   sbom.sh vcpkg OUT INSTALLED_DIR
#       every port vcpkg installed into INSTALLED_DIR, from the
#       vcpkg.spdx.json it writes per port; vcpkg-* helper ports run
#       only at build time and are left out
#
# Each call adds its components to OUT, creating it if missing.
set -eu

cmd=$1 out=$2
shift 2

[ -f "$out" ] || {
	mkdir -p "$(dirname "$out")"
	printf '%s\n' '{"bomFormat":"CycloneDX","specVersion":"1.6","version":1,"components":[]}' >"$out"
}

add() {
	jq --slurpfile new /dev/stdin '.components += $new[0]' "$out" >"$out.tmp"
	mv "$out.tmp" "$out"
}

case $cmd in
git)
	name=$1 dir=$2
	shift 2
	url=$(git -C "$dir" remote get-url origin)
	repo=${url#https://github.com/}
	repo=${repo%.git}
	sha=$(git -C "$dir" rev-parse HEAD)
	version=$(git -C "$dir" describe --tags --always)
	for p in "$@"; do jq -Rs '{type: "unofficial", diff: {text: {contentType: "text/x-diff", content: .}}}' "$p"; done |
		jq -s --arg n "$name" --arg v "$version" --arg r "$repo" --arg s "$sha" --arg u "$url" '[{
			type: "library", "bom-ref": "pkg:github/\($r | ascii_downcase)@\($s)#\($n)",
			name: $n, version: $v, purl: "pkg:github/\($r | ascii_downcase)@\($s)",
			externalReferences: [{type: "vcs", url: $u}]
		} + (if length > 0 then {pedigree: {patches: .}} else {} end)]' | add
	;;
vcpkg)
	find "$1" -path '*/share/*/vcpkg.spdx.json' -exec cat {} + |
		jq -s '[.[] | .packages as $p | $p[0] | select(.name | startswith("vcpkg-") | not) |
			($p[1:] | map(select(.downloadLocation != "NONE"))) as $src |
			($src | map(select(.name | endswith(".patch") | not)) | first) as $up |
			(first(($up.downloadLocation // "") | capture("^git\\+https://github\\.com/(?<repo>[^@]+)@(?<ref>.+)$")) // null) as $gh |
			{
				type: "library",
				"bom-ref": "vcpkg:\(.name)@\(.versionInfo)",
				name: .name,
				version: (.versionInfo | sub("#.*"; "")),
				properties: [{name: "vcpkg:port-version", value: .versionInfo}]
			}
			+ (if $gh then {purl: "pkg:github/\($gh.repo | ascii_downcase)@\($gh.ref)"} else {} end)
			+ (if $gh then {externalReferences: [{type: "vcs", url: "https://github.com/\($gh.repo)"}]}
				elif $up then {externalReferences: [{type: "source-distribution", url: $up.downloadLocation}]}
				else {} end)
			+ (if .licenseConcluded != "NOASSERTION" then {licenses: [{expression: .licenseConcluded}]} else {} end)
			+ ([$src[] | select(.name | endswith(".patch")) | {
					type: (if $gh and (.downloadLocation | ascii_downcase
						| startswith("https://github.com/\($gh.repo | ascii_downcase)/commit/"))
						then "backport" else "unofficial" end),
					diff: {url: .downloadLocation}}]
				| if length > 0 then {pedigree: {patches: .}} else {} end)
		]' | add
	;;
*)
	echo "usage: sbom.sh git|vcpkg OUT ..." >&2
	exit 2
	;;
esac
