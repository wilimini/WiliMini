#!/bin/sh
# Copies one GitHub release (notes + assets) to Codeberg (Forgejo API).
# Usage: codeberg-release-sync.sh <tag>
# Env: GH_REPO (owner/name on GitHub), GH_TOKEN, CODEBERG_REPO (owner/name),
#      CODEBERG_TOKEN. Idempotent: an existing release is updated in place and
#      assets already present are skipped.
set -eu
tag=${1:?usage: $0 <tag>}
: "${GH_REPO:?}" "${CODEBERG_REPO:?}" "${CODEBERG_TOKEN:?}"
api="https://codeberg.org/api/v1/repos/$CODEBERG_REPO"
auth="Authorization: token $CODEBERG_TOKEN"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

gh release view "$tag" --repo "$GH_REPO" --json name,body,isPrerelease > "$work/rel.json"
gh release download "$tag" --repo "$GH_REPO" --dir "$work/assets"

python3 -I - "$work/rel.json" "$tag" > "$work/payload.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
print(json.dumps({"tag_name": sys.argv[2], "name": r["name"] or sys.argv[2],
                  "body": r["body"] or "", "prerelease": r["isPrerelease"],
                  "draft": False}))
PY

code=$(curl -sS -o "$work/existing.json" -w '%{http_code}' -H "$auth" "$api/releases/tags/$tag")
if [ "$code" = 200 ]; then
    id=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$work/existing.json")
    curl -fsS -X PATCH -H "$auth" -H 'Content-Type: application/json' \
        -d @"$work/payload.json" "$api/releases/$id" > /dev/null
else
    curl -fsS -X POST -H "$auth" -H 'Content-Type: application/json' \
        -d @"$work/payload.json" "$api/releases" > "$work/existing.json"
    id=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$work/existing.json")
fi

curl -fsS -H "$auth" "$api/releases/$id/assets" > "$work/have.json"
for f in "$work"/assets/*; do
    name=$(basename "$f")
    if python3 -I -c 'import json,sys; sys.exit(0 if sys.argv[2] in [a["name"] for a in json.load(open(sys.argv[1]))] else 1)' "$work/have.json" "$name"; then
        echo "skip $name (already on Codeberg)"
        continue
    fi
    echo "upload $name"
    curl -fsS -X POST -H "$auth" -F "attachment=@$f" \
        "$api/releases/$id/assets?name=$name" > /dev/null
done
echo "Codeberg release $tag synced"
