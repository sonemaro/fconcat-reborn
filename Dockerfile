# CentOS 7 for glibc 2.17
FROM centos:7 AS builder

RUN sed -i 's/mirrorlist/#mirrorlist/g' /etc/yum.repos.d/CentOS-*.repo && \
    sed -i 's|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo

# Install development tools
RUN yum groupinstall -y "Development Tools" && \
    yum install -y \
    gcc \
    gcc-c++ \
    make \
    git \
    glibc-devel \
    glibc-static \
    # For Windows cross-compilation (optional)
    mingw64-gcc \
    mingw32-gcc \
    && yum clean all

# Set working directory
WORKDIR /app

# Copy project files
COPY . .

# Build with static linking for maximum compatibility
RUN make clean-all && \
    make release LDFLAGS="-static -pthread" SECURITY=0

# Runtime stage
FROM centos:7 AS runtime

# Fix yum repositories for CentOS 7 EOL by pointing to the vault
RUN sed -i 's/mirrorlist/#mirrorlist/g' /etc/yum.repos.d/CentOS-*.repo && \
    sed -i 's|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' /etc/yum.repos.d/CentOS-*.repo

# Install minimal runtime
RUN yum install -y glibc && yum clean all

# Copy binary
COPY --from=builder /app/fconcat /usr/local/bin/fconcat

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/fconcat"]
CMD ["--help"]