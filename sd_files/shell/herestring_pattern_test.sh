# Manual smoke test for BruceOS shell's "${NAME/pat/repl}" pattern
# substitution, "$'...'" ANSI-C quoting, "<<<" here-strings, and
# "read -r -d ''" -- the exact feature set xmas.ysap.sh (curl xmas.ysap.sh |
# shell) needs to slurp its multi-line tree template into one scalar, walk it
# line by line with "while IFS= read -r line; do ... done <<< \"$TREE\"",
# and splice ANSI color codes into each line.
#
# Run it from the shell: /shell/herestring_pattern_test.sh
#
# Every check prints "ok - <what>" or "FAIL - <what> (got X, want Y)", and
# the script exits 0 only if every single one passed.

pass=0
fail=0

check() {
	what="$1"
	got="$2"
	want="$3"
	if [ "$got" = "$want" ]; then
		pass=$((pass + 1))
		echo "ok   - $what"
	else
		fail=$((fail + 1))
		echo "FAIL - $what (got \"$got\", want \"$want\")"
	fi
}

echo "== pattern substitution =="
s='a b a b a'
check 'first match only' "${s/a/X}" 'X b a b a'
check 'every match' "${s//a/X}" 'X b X b X'
RED=RR
RST=GG
t='a 1 b'
check 'pattern/replacement both expand their own variables' "${t// 1 / ${RED}o${RST} }" 'a RRoGG b'
s=hello
check 'no match passes through unchanged' "${s/zzz/Q}" 'hello'
s='a-b-c'
check 'empty replacement deletes the match' "${s/-/}" 'ab-c'

echo "== \$'...' ANSI-C quoting =="
t=$'a\tb'
check 'tab counts as one character' "${#t}" "3"
t=$'\x41\x42'
check 'hex escape' "$t" "AB"
t=$'\101\102'
check 'octal escape' "$t" "AB"
t=$'esc:\e'
check 'escape char counts as one character' "${#t}" "5"
t=$'\q'
check 'unrecognized escape keeps its backslash' "$t" '\q'

echo "== here-strings and read -d =="
read -r line <<< hello
check 'plain here-string' "$line" "hello"
x=world
read -r line <<< "hi $x"
check 'here-string word is expanded first' "$line" "hi world"
read -r -d '' t <<< $'one\ntwo'
check 'read -d slurps a multi-line here-string into one scalar' "$t" $'one\ntwo'

echo "== IFS= applies to read/while-read even as a builtin =="
t=$'  a\n  b'
sum=
while IFS= read -r line; do
	sum="$sum[$line]"
done <<< "$t"
check 'IFS= preserves leading/trailing whitespace per line' "$sum" '[  a][  b]'

sum=
while read -r line; do
	sum="$sum[$line]"
done <<< "$t"
check 'default IFS (no override) still trims whitespace' "$sum" '[a][b]'

echo ""
echo "$pass passed, $fail failed"
if [ "$fail" != "0" ]; then
	exit 1
fi
