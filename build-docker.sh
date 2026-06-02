#!/bin/sh
set -eu

image="${FCONCAT_DOCKER_IMAGE:-fconcat}"
tag="${FCONCAT_DOCKER_TAG:-latest}"

if [ "${1:-}" = "extract" ]; then
    docker run --rm -v "$(pwd):/out" "${image}:${tag}" sh -c 'cp /usr/local/bin/fconcat /out/fconcat'
    echo "Extracted ./fconcat"
    exit 0
fi

docker build -t "${image}:${tag}" .

cat <<EOF
Built ${image}:${tag}

Run:
  docker run --rm -v "\$(pwd):/data" ${image}:${tag} /data/input /data/output.txt

Extract binary:
  ./build-docker.sh extract
EOF
