# libssh2 ESP-IDF wrapper

This temporary in-tree component replaces `skuodi/libssh2_esp` 1.1.0, whose
vendored libssh2 1.11.1 does not compile with ESP-IDF 6 and mbedTLS 4.

The wrapper pins the mbedTLS PSA work from upstream libssh2 pull request 2284
at commit `74bb307bd52a26c8b579ed0e0b5c197388bb8a46`. Move this directory into the
maintained `libssh2_esp` fork once its permanent repository is available, then
replace this local component with a pinned managed dependency.
