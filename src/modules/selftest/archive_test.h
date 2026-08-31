#pragma once

#include <stdbool.h>

bool selftest__run_archive_tar_gz_roundtrip_case(void);
bool selftest__run_archive_zip_roundtrip_case(void);
bool selftest__run_archive_tar_gz_corrupt_case(void);
bool selftest__run_archive_tar_gz_slip_rejection_case(void);
