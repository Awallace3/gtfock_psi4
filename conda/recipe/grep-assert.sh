#!/usr/bin/env bash
# Shared absence assertion for the packaging gates. grep exits 2 on an
# unreadable or missing operand, which `! grep ...` cannot distinguish from
# "scanned and found nothing", so an unread file would satisfy the assertion.
# Only status 1 proves the operand was scanned and is clean.

GTF_CUDA_LINK_PATTERN='(^|[[:space:]/])(libcuda|libcudart|libcublas|libcudnn|libcufft|libcufile|libcupti|libcurand|libcusolver|libcusparse|libcutensor|libnv[^[:space:]/]*|libnccl)[^[:space:]/]*\.so'

gtf_grep_absent() {
    local label=$1
    shift
    local output status
    output=$(grep "$@" 2>&1) && status=0 || status=$?
    if ((status == 0)); then
        echo "$label:" >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if ((status != 1)); then
        echo "$label: scan failed with grep status $status" >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
}
