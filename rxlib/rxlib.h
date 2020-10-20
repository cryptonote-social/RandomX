// Copyright 2020 cryptonote.social. All rights reserved. Use of this source code is governed by
// the license found in the LICENSE file.
#include <stdbool.h>
#include <stdint.h>

// return values:
//   1: success
//   2: success, but no huge pages.
//   -1: unexpected failure
int init_rxlib(int threads);

bool seed_rxlib(const char* seed_hash, uint32_t len, int init_threads);

int64_t rx_hash_until(const char* blob, uint32_t len, uint64_t diff, int thread, char* hash_output, char* nonce_output, uint32_t* stopper);

int rx_add_thread();
int rx_remove_thread();
