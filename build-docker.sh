#!/bin/bash
# build-docker.sh

set -e

IMAGE_NAME="fconcat-compat"
TAG="latest"

if [[ "$1" == "extract" ]]; then
  echo "Extracting 'fconcat' binary from Docker image..."
  docker run --rm -v "$(pwd)":/output ${IMAGE_NAME}:${TAG} sh -c 'cp /usr/local/bin/fconcat /output/'
  echo "✅ Binary extracted to $(pwd)/fconcat"
  exit 0
fi

echo "Building fconcat with older glibc for compatibility..."

# Build the Docker image
docker build -t ${IMAGE_NAME}:${TAG} .

echo "Build complete!"
echo ""
echo "Usage examples:"
echo "  # Run fconcat directly:"
echo "  docker run --rm -v \$(pwd):/data ${IMAGE_NAME}:${TAG} /data/input /data/output"
echo ""
echo "  # Interactive shell:"
echo "  docker run --rm -it -v \$(pwd):/data ${IMAGE_NAME}:${TAG} /bin/bash"
echo ""
echo "  # Extract binary to host:"
echo "  ./build-docker.sh extract"
echo ""
echo "  # Check glibc version:"
echo "  docker run --rm ${IMAGE_NAME}:${TAG} sh -c 'ldd --version'"