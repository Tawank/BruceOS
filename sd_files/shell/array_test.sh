# Manual smoke test for BruceOS shell's bash-style indexed arrays.
#
# Run it from the shell: /shell/array_test.sh
# (or "shell /shell/array_test.sh" from a parent process)
#
# Every check prints "ok - <what>" or "FAIL - <what> (got X, want Y)", and
# the script exits 0 only if every single one passed -- check "$?" (or just
# scroll up looking for FAIL) after it finishes.

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

echo "== literal assignment, one line =="
colors=(red green blue)
check "colors[0]" "${colors[0]}" "red"
check "colors[1]" "${colors[1]}" "green"
check "colors[2]" "${colors[2]}" "blue"
check "#colors[@]" "${#colors[@]}" "3"

echo "== literal assignment, multiple lines =="
numbers=(
	one
	two
	three
)
check "numbers[0]" "${numbers[0]}" "one"
check "numbers[2]" "${numbers[2]}" "three"
check "#numbers[@]" "${#numbers[@]}" "3"

echo "== index as an arithmetic expression =="
i=1
check "colors[i+1]" "${colors[i+1]}" "blue"

echo "== append (+=) =="
colors+=(yellow)
check "colors[3] after +=" "${colors[3]}" "yellow"
check "#colors[@] after +=" "${#colors[@]}" "4"

echo "== single-element assignment =="
colors[0]=purple
check "colors[0] after element assign" "${colors[0]}" "purple"

echo "== scalar vs. array-element length =="
greeting=hello
check "#greeting (scalar)" "${#greeting}" "5"
check "#colors[0] (array element)" "${#colors[0]}" "6"

echo "== unset one element leaves a hole, not the whole array =="
unset colors[1]
check "#colors[@] after unset colors[1]" "${#colors[@]}" "3"
check "colors[2] unaffected by the hole" "${colors[2]}" "blue"

echo "== reindex/compaction via \"\${arr[@]}\" closes the hole =="
colors=("${colors[@]}")
check "#colors[@] after reindex" "${#colors[@]}" "3"
sum=
for c in "${colors[@]}"; do
	sum="$sum$c,"
done
check "order survives reindex" "$sum" "purple,blue,yellow,"

echo "== \${#arr[@]} works inside ((...)) too =="
total=0
for ((j = 0; j < ${#colors[@]}; j++)); do
	total=$((total + 1))
done
check "((...)) saw the same length" "$total" "3"

echo "== empty literal, and unsetting the whole array =="
empty=()
check "#empty[@]" "${#empty[@]}" "0"
unset colors
check "#colors[@] after unset colors" "${#colors[@]}" "0"

echo ""
echo "$pass passed, $fail failed"
if [ "$fail" != "0" ]; then
	exit 1
fi
