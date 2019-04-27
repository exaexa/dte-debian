#!/bin/sh

# This script generates the version string, as printed by "dte -V".

set -eu
export VPREFIX="$1"

# These values are filled automatically for git-archive(1) tarballs.
# See: "export-subst" in gitattributes(5).
distinfo_commit_full='53804e55dcd0ba47dbdc61500e0366a6e5cf8b52'
distinfo_commit_short='53804e55'
distinfo_author_date='2019-04-23 07:52:22 +0100'
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
