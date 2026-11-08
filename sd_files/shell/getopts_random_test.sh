# Manual smoke test for BruceOS shell's "getopts" builtin and "$RANDOM".
#
# Run it from the shell: /shell/getopts_random_test.sh
#
# Every check prints "ok - <what>" or "FAIL - <what> (got X, want Y)", and
# the script exits 0 only if every single one passed.
#
# Everything below runs at the top level, in one function ("check") rather
# than one per scenario: $OPTIND is a plain global scalar that persists
# across calls, exactly like real bash's own -- a script normally resets it
# (or localizes it with "local OPTIND" inside a function, the way
# xmas.ysap.sh's own main()/tput() do) before reusing getopts on a fresh
# argument list, and each "OPTIND=1" below is doing exactly that. Passing
# arguments to getopts explicitly (its own "[arg ...]" form, often via an
# array's "${name[@]}") rather than relying on "$1"/"$@" also sidesteps
# needing a wrapper function - and a dedicated one per scenario would add
# up fast against this shell's SHELL__MAX_FUNCTIONS cap (8) anyway.

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

echo "== getopts: a flag and a next-word argument =="
OPTIND=1
seen=
while getopts 'hc:' opt -h -c value; do
	case "$opt" in
		h) seen="${seen}h" ;;
		c) seen="${seen}c=$OPTARG" ;;
	esac
done
check "flag + next-word arg" "$seen" "hc=value"

echo "== getopts: argument jammed onto the option itself =="
OPTIND=1
last_optarg=
while getopts 'c:' opt -cjammed; do
	last_optarg="$OPTARG"
done
check "jammed arg (-cvalue)" "$last_optarg" "jammed"

echo "== getopts: unrecognized option =="
OPTIND=1
getopts 'h' opt -x
check "illegal option sets NAME to ?" "$opt" "?"

echo "== getopts: unrecognized option, silent mode =="
OPTIND=1
OPTARG=
getopts ':h' opt -x
check "silent mode puts the bad char in OPTARG" "$OPTARG" "x"

echo "== getopts: missing required argument =="
OPTIND=1
getopts 'c:' opt -c
check "missing arg sets NAME to ?" "$opt" "?"

OPTIND=1
getopts ':c:' opt -c
check "silent mode sets NAME to : instead" "$opt" ":"

echo "== getopts: loop ends at the first non-option, leaves it for shift =="
OPTIND=1
n=0
args=(-h plainarg)
while getopts 'h' opt "${args[@]}"; do
	n=$((n + 1))
done
check "options actually seen" "$n" "1"
check "non-option word left for the caller" "${args[OPTIND - 1]}" "plainarg"

echo "== getopts: -- ends option processing and is consumed =="
OPTIND=1
n=0
args=(-h -- after)
while getopts 'h' opt "${args[@]}"; do
	n=$((n + 1))
done
check "options actually seen" "$n" "1"
check "-- consumed, next word is the leftover" "${args[OPTIND - 1]}" "after"

echo "== \$RANDOM =="
r1=$RANDOM
r2=$RANDOM
if [ "$r1" -ge 0 ] && [ "$r1" -le 32767 ] && [ "$r2" -ge 0 ] && [ "$r2" -le 32767 ]; then
	pass=$((pass + 1))
	echo "ok   - \$RANDOM is in range (got $r1 and $r2)"
else
	fail=$((fail + 1))
	echo "FAIL - \$RANDOM out of range (got $r1 and $r2)"
fi
if ((RANDOM >= 0 && RANDOM <= 32767)); then
	pass=$((pass + 1))
	echo "ok   - \$RANDOM is in range inside ((...))"
else
	fail=$((fail + 1))
	echo "FAIL - \$RANDOM out of range inside ((...))"
fi

echo ""
echo "$pass passed, $fail failed"
if [ "$fail" != "0" ]; then
	exit 1
fi
