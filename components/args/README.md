# Args

BruceOS-local fork of [dmulholl/args](https://github.com/dmulholl/args), vendored
from [Tawank/args](https://github.com/Tawank/args).

This version is adapted for firmware use: it returns parse statuses instead of
exiting, uses routed Bruce stdio, stores parser internals in one task-owned Core
memory arena, generates command help, and supports named required and optional
positional arguments. Parsers can also retain a variadic positional remainder
for loader and command-forwarding use cases.
