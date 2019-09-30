#!/bin/sh

# This script generates the version string, as printed by "dte -V".

set -eu
export VPREFIX="$1"

# These values are filled automatically for git-archive(1) tarballs.
# See: "export-subst" in gitattributes(5).
distinfo_commit_full='cae7c45ea5a563c022001a54d1eee71c268f62b4'
distinfo_commit_short='cae7c45e'
distinfo_author_date='2019-09-29 16:51:55 +0100'
if expr "$distinfo_commit_short" : '[0-9a-f]\{7,40\}$' >/dev/null; then
    echo "${VPREFIX}-g${distinfo_commit_short}-dist"
    exit 0
fi

git_describe_ver=$(git describe --match="v$VPREFIX" 2>/dev/null || true)
if test -n "$git_describe_ver"; then
    echo "${git_describe_ver#v}"
    exit 0
fi

echo "$VPREFIX-unknown"
