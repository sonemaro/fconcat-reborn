#!/bin/sh
set -eu

mode="${1:-raw}"
bin="${BENCH_BIN:-./fconcat}"
root="${BENCH_ROOT:-$HOME/projects}"
iterations="${BENCH_ITERATIONS:-}"
output="${BENCH_OUTPUT:-$HOME/fconcat-bench-real-output.txt}"
warmup="${BENCH_WARMUP:-1}"

case "$mode" in
    raw)
        iterations="${iterations:-7}"
        ;;
    real)
        iterations="${iterations:-3}"
        ;;
    check)
        iterations="${iterations:-1}"
        ;;
    *)
        echo "usage: BENCH_ROOT=/path BENCH_ITERATIONS=N $0 raw|real|check" >&2
        exit 2
        ;;
esac

case "$iterations" in
    ''|*[!0-9]*)
        echo "BENCH_ITERATIONS must be a positive integer: $iterations" >&2
        exit 2
        ;;
esac

if [ "$iterations" -lt 1 ]; then
    echo "BENCH_ITERATIONS must be >= 1" >&2
    exit 2
fi

if [ ! -x "$bin" ]; then
    echo "BENCH_BIN is not executable: $bin" >&2
    exit 1
fi

if [ ! -d "$root" ]; then
    echo "BENCH_ROOT is not a directory: $root" >&2
    exit 1
fi

now_ns="$(date +%s%N)"
case "$now_ns" in
    *N*)
        echo "date +%s%N is not supported on this platform" >&2
        exit 1
        ;;
esac

file_count="$(find "$root" -type f 2>/dev/null | wc -l | tr -d ' ')"
root_kb="$(du -sk "$root" 2>/dev/null | awk '{print $1}')"
uname_text="$(uname -sm 2>/dev/null || echo unknown)"

echo "fconcat benchmark"
echo "mode=$mode"
echo "bin=$bin"
echo "root=$root"
echo "files=$file_count"
echo "du_kb=$root_kb"
echo "platform=$uname_text"
echo "iterations=$iterations"
echo "warmup=$warmup"

if [ "$mode" = "check" ]; then
    echo "BENCH_CHECK: OK"
    exit 0
fi

if [ "$mode" = "raw" ]; then
    dest="/dev/null"
else
    echo "output=$output"
    dest="$output"
fi

if [ "$warmup" = "1" ]; then
    if [ "$mode" = "real" ]; then
        rm -f "$output"
    fi
    "$bin" "$root" "$dest" >/dev/null
    if [ "$mode" = "real" ]; then
        rm -f "$output"
    fi
elif [ "$warmup" != "0" ]; then
    echo "BENCH_WARMUP must be 0 or 1" >&2
    exit 2
fi

i=1
min_ns=
max_ns=0
sum_ns=0
last_bytes=0

while [ "$i" -le "$iterations" ]; do
    if [ "$mode" = "real" ]; then
        rm -f "$output"
    fi

    start_ns="$(date +%s%N)"
    "$bin" "$root" "$dest" >/dev/null
    status="$?"
    end_ns="$(date +%s%N)"

    if [ "$status" -ne 0 ]; then
        exit "$status"
    fi

    elapsed_ns="$((end_ns - start_ns))"
    sum_ns="$((sum_ns + elapsed_ns))"
    if [ -z "$min_ns" ] || [ "$elapsed_ns" -lt "$min_ns" ]; then
        min_ns="$elapsed_ns"
    fi
    if [ "$elapsed_ns" -gt "$max_ns" ]; then
        max_ns="$elapsed_ns"
    fi

    if [ "$mode" = "real" ]; then
        last_bytes="$(wc -c < "$output" | tr -d ' ')"
        awk -v i="$i" -v ns="$elapsed_ns" -v bytes="$last_bytes" \
            'BEGIN { printf "run %d %.3f bytes %s\n", i, ns / 1000000000, bytes }'
    else
        awk -v i="$i" -v ns="$elapsed_ns" \
            'BEGIN { printf "run %d %.3f\n", i, ns / 1000000000 }'
    fi

    i="$((i + 1))"
done

avg_ns="$((sum_ns / iterations))"
if [ "$mode" = "real" ]; then
    awk -v min="$min_ns" -v avg="$avg_ns" -v max="$max_ns" -v bytes="$last_bytes" \
        'BEGIN { printf "summary min %.3f avg %.3f max %.3f bytes %s\n", min / 1000000000, avg / 1000000000, max / 1000000000, bytes }'
    if [ "${BENCH_KEEP_OUTPUT:-0}" != "1" ]; then
        rm -f "$output"
    fi
else
    awk -v min="$min_ns" -v avg="$avg_ns" -v max="$max_ns" \
        'BEGIN { printf "summary min %.3f avg %.3f max %.3f\n", min / 1000000000, avg / 1000000000, max / 1000000000 }'
fi
