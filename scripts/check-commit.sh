#!/bin/bash
# Pre-commit scan of the staged tree (run it before every commit): no local paths, IPs, credentials, ROM-derived or built files, and
# only the two expected submodule gitlinks. Exit status 1 when anything is found.
cd "$(git rev-parse --show-toplevel)" || exit 2
bad=0
fail() { echo "FAIL: $*"; bad=1; }

echo "staged: $(git diff --cached --name-only | wc -l) files, $(git ls-files -s | wc -l) in the index"

# gitlinks: exactly the two submodules, with https URLs and no local path in .gitmodules
links=$(git ls-files -s | awk '$1 == "160000" {print $4}' | sort | tr '\n' ' ')
[ "$links" = "ext/mupen64plus-rsp-hle ext/parallel-rsp " ] || fail "gitlinks are '$links'"
if [ -f .gitmodules ]; then
    grep -E '^\s*url\s*=' .gitmodules | grep -qvE 'url = https://github\.com/' && fail ".gitmodules has a url that is not https://github.com/..."
    grep -E '^\s*(url|path)\s*=\s*(/|\.|file:|~)' .gitmodules && fail ".gitmodules mentions a local path"
fi

# files that must never be tracked
git ls-files | grep -E '\.(z64|n64|v64|rom|so|o|a|ppm|fdump|raw|xz)$|(^|/)(build[^/]*|out|work|test-output|test-data)/' | grep -v '^ext/' && fail "built / ROM-derived / bulk file tracked"
big=$(git ls-files -z | xargs -0 -I{} sh -c 'f="{}"; [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 1048576 ] && echo "$f"')
[ -n "$big" ] && fail "files over 1 MiB: $big"

# content of the index (what a commit would contain), not the working tree; the licence texts and the submodules are not scanned
scan() { # description, regex
    local hits
    hits=$(git grep --cached -nIE "$2" -- . ':!LICENSES' ':!COPYING' ':!scripts/check-commit.sh' 2>/dev/null)
    [ -n "$hits" ] && { fail "$1"; echo "$hits" | head -20; }
}
scan "local path (home, workspace mount, scratchpad)" '(^|[ "=:])/home/[a-z]|/Users/|/mnt/[0-9a-f]{8}-|/tmp/claude|noxwell|beebono@'
scan "IP address" '(^|[^0-9.])([0-9]{1,3}\.){3}[0-9]{1,3}([^0-9.]|$)'
scan "credential-looking text" '[Pp]ass(word|wd)|sshpass|api[_-]?key|secret|BEGIN [A-Z ]*PRIVATE KEY|happygaming|rocknix[^a-zA-Z/-]'
scan "ssh user@host" '(root|spruce)@[A-Za-z0-9]'

[ $bad = 0 ] && echo "OK: nothing found"
exit $bad
