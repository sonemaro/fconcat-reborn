#!/bin/sh
# fconcat installer
# Usage: curl -fsSL https://raw.githubusercontent.com/sonemaro/fconcat-reborn/main/.github/scripts/install.sh | sh
#
# Environment variables:
#   FCONCAT_VERSION      - Install a specific version (e.g., v1.0.0)
#   FCONCAT_INSTALL_DIR  - Installation directory (default: /usr/local/bin)

set -eu

REPO="sonemaro/fconcat-reborn"
BINARY_NAME="fconcat"
INSTALL_DIR="${FCONCAT_INSTALL_DIR:-/usr/local/bin}"

# Colors for output (disabled if not a terminal)
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

info() {
    printf "${BLUE}==>${NC} %s\n" "$1"
}

success() {
    printf "${GREEN}==>${NC} %s\n" "$1"
}

warn() {
    printf "${YELLOW}WARNING:${NC} %s\n" "$1"
}

error() {
    printf "${RED}ERROR:${NC} %s\n" "$1" >&2
    exit 1
}

# Detect OS and architecture
detect_platform() {
    OS="$(uname -s)"
    ARCH="$(uname -m)"

    case "$OS" in
        Linux*)  OS="linux" ;;
        Darwin*) OS="macos" ;;
        *)       error "Unsupported operating system: $OS" ;;
    esac

    case "$ARCH" in
        x86_64|amd64)   ARCH="amd64" ;;
        aarch64|arm64)  ARCH="arm64" ;;
        *)              error "Unsupported architecture: $ARCH" ;;
    esac

    echo "${OS}-${ARCH}"
}

# Get the latest release version from GitHub API
get_latest_version() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null | \
            grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/'
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null | \
            grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/'
    else
        error "Neither curl nor wget found. Please install one of them."
    fi
}

# Download a file
download() {
    url="$1"
    output="$2"
    
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$output"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$url" -O "$output"
    else
        error "Neither curl nor wget found. Please install one of them."
    fi
}

# Main installation function
install() {
    info "Detecting platform..."
    PLATFORM="$(detect_platform)"
    info "Platform: $PLATFORM"

    # Get version
    VERSION="${FCONCAT_VERSION:-}"
    if [ -z "$VERSION" ]; then
        info "Fetching latest version..."
        VERSION="$(get_latest_version)"
    fi
    
    if [ -z "$VERSION" ]; then
        error "Could not determine version. Please set FCONCAT_VERSION environment variable."
    fi
    
    info "Version: $VERSION"

    # Construct download URL
    DOWNLOAD_URL="https://github.com/${REPO}/releases/download/${VERSION}/${BINARY_NAME}-${PLATFORM}"
    info "Downloading from: $DOWNLOAD_URL"

    # Create temp directory
    TMP_DIR="$(mktemp -d)"
    trap 'rm -rf "$TMP_DIR"' EXIT

    # Download binary
    download "$DOWNLOAD_URL" "${TMP_DIR}/${BINARY_NAME}"
    
    # Make executable
    chmod +x "${TMP_DIR}/${BINARY_NAME}"

    # Verify binary works
    info "Verifying binary..."
    if ! "${TMP_DIR}/${BINARY_NAME}" --version >/dev/null 2>&1; then
        error "Downloaded binary failed verification. It may be corrupted or incompatible."
    fi

    # Get version string for display
    INSTALLED_VERSION=$("${TMP_DIR}/${BINARY_NAME}" --version 2>&1 | head -n1 || echo "$VERSION")

    # Install binary
    info "Installing to ${INSTALL_DIR}..."
    
    # Create install directory if it doesn't exist
    if [ ! -d "$INSTALL_DIR" ]; then
        if [ -w "$(dirname "$INSTALL_DIR")" ]; then
            mkdir -p "$INSTALL_DIR"
        else
            sudo mkdir -p "$INSTALL_DIR"
        fi
    fi
    
    if [ -w "$INSTALL_DIR" ]; then
        mv "${TMP_DIR}/${BINARY_NAME}" "${INSTALL_DIR}/"
    else
        info "Requesting sudo access to install to ${INSTALL_DIR}..."
        sudo mv "${TMP_DIR}/${BINARY_NAME}" "${INSTALL_DIR}/"
    fi

    # Verify installation
    if command -v "$BINARY_NAME" >/dev/null 2>&1; then
        success "fconcat installed successfully!"
        echo ""
        echo "  Version:  $INSTALLED_VERSION"
        echo "  Location: $(command -v $BINARY_NAME)"
        echo ""
        echo "Get started:"
        echo "  fconcat --help"
        echo "  fconcat ./src output.txt"
        echo ""
    else
        warn "Installation complete but 'fconcat' not found in PATH."
        echo ""
        echo "Binary installed to: ${INSTALL_DIR}/${BINARY_NAME}"
        echo ""
        echo "Add ${INSTALL_DIR} to your PATH, or run:"
        echo "  ${INSTALL_DIR}/${BINARY_NAME} --help"
        echo ""
    fi
}

# Check for help flag
case "${1:-}" in
    -h|--help)
        echo "fconcat installer"
        echo ""
        echo "Usage:"
        echo "  curl -fsSL https://raw.githubusercontent.com/sonemaro/fconcat-reborn/main/.github/scripts/install.sh | sh"
        echo ""
        echo "Environment variables:"
        echo "  FCONCAT_VERSION      Install a specific version (e.g., v1.0.0)"
        echo "  FCONCAT_INSTALL_DIR  Installation directory (default: /usr/local/bin)"
        echo ""
        echo "Examples:"
        echo "  # Install latest version"
        echo "  curl -fsSL ... | sh"
        echo ""
        echo "  # Install specific version"
        echo "  curl -fsSL ... | FCONCAT_VERSION=v1.0.0 sh"
        echo ""
        echo "  # Install to custom directory"
        echo "  curl -fsSL ... | FCONCAT_INSTALL_DIR=~/.local/bin sh"
        exit 0
        ;;
esac

install
