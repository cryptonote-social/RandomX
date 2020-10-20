// Copyright 2020 cryptonote.social. All rights reserved. Use of this source code is governed by
// the license found in the LICENSE file.
#include <atomic>
#include <cfenv>
#include <string>
#include <thread>

#include <stdint.h>

#include "randomx.h"
#include "checkhash.h"
#include "dataset.hpp"

static randomx_dataset* dataset = nullptr;

static std::vector<randomx_vm*> vm;

static std::atomic<uint32_t> atomic_nonce(1);

// only call when all existing threads are stopped
extern "C" int rx_add_thread() {
  randomx_flags flags, hugepages_flags;
  flags = RANDOMX_FLAG_DEFAULT | RANDOMX_FLAG_HARD_AES | RANDOMX_FLAG_JIT | RANDOMX_FLAG_FULL_MEM;
  flags |= randomx_get_flags();
  hugepages_flags = flags | RANDOMX_FLAG_LARGE_PAGES;

  auto v = randomx_create_vm(hugepages_flags, nullptr, dataset);
  if (v == nullptr) {
    std::cerr << "# rxlib: Failed to allocate rx vm w/ hugepages." << std::endl;
    v = randomx_create_vm(flags, nullptr, dataset);
    if (v == nullptr) {
      std::cerr << "# rxlib: Failed to allocate rx vm" << std::endl;
      return -1;
    }
  }
  vm.push_back(v);
  return vm.size();
}

// only call when all existing threads are stopped
extern "C" int rx_remove_thread() {
  if (vm.size() <= 1) {
    std::cerr << "# rxlib: Number of threads can't be below 1." << std::endl;
    return -1;
  }
  randomx_destroy_vm(vm[vm.size()-1]);
  vm.pop_back();
  return vm.size();
}

extern "C" bool seed_rxlib(const char* seed_hash, uint32_t len, int init_threads) {
  randomx_flags flags =
    RANDOMX_FLAG_DEFAULT | RANDOMX_FLAG_HARD_AES | RANDOMX_FLAG_JIT | RANDOMX_FLAG_FULL_MEM | randomx_get_flags();
  randomx_cache* cache = randomx_alloc_cache(flags);  
  if (cache == nullptr) {
    std::cerr << "# rxlib: Failed to allocate rx cache" << std::endl;
    return false;
  }
  randomx_init_cache(cache, seed_hash, len);
  uint32_t items = randomx_dataset_item_count();

  if (init_threads == 1) {
    std::cerr << "# rxlib: initializing rx dataset..." << std::endl;
    randomx_init_dataset(dataset, cache, 0, items);
  } else {
    std::cerr << "# rxlib: initializing rx dataset (" << init_threads << ")..." << std::endl;
	std::vector<std::thread> thread;
    auto t_items = items / init_threads;
    auto remainder = items % init_threads;
    uint32_t startItem = 0;
    for (int i = 0; i < init_threads; ++i) {
      auto count = t_items + (i == init_threads - 1 ? remainder : 0);
      thread.push_back(std::thread(randomx_init_dataset, dataset, cache, startItem, count));
      startItem += count;
    }
    for (std::thread& t : thread) t.join();
  }
  std::cerr << "# rxlib: rx dataset initialized" << std::endl;

  randomx_release_cache(cache);
  return true;
}

extern "C" int init_rxlib(int threads) {
  randomx_flags flags =
    RANDOMX_FLAG_DEFAULT | RANDOMX_FLAG_HARD_AES | RANDOMX_FLAG_JIT | RANDOMX_FLAG_FULL_MEM | randomx_get_flags();
  randomx_flags hugepages_flags = flags | RANDOMX_FLAG_LARGE_PAGES;

  bool hugepages_success = false;
  if (dataset == nullptr) {
    // Allocate a dataset if it hasn't been allocated already.
    dataset = randomx_alloc_dataset(hugepages_flags);
    if (dataset == nullptr) {
      std::cerr << "# rxlib: Failed to allocate rx dataset w/ hugepages" << std::endl;
      dataset = randomx_alloc_dataset(flags);
      if (dataset == nullptr) {
        std::cerr << "# rxlib: Failed to allocate rx dataset" << std::endl;
        return -1;
      }
    } else {
      hugepages_success = true;
    }
  }

  if (vm.size() == 0) {
    // Create vms if we haven't created one already.
    for (int i=0; i<threads; ++i) {
      auto v = randomx_create_vm(hugepages_flags, nullptr, dataset);
      if (v == nullptr) {
        std::cerr << "# rxlib: Failed to allocate rx vm w/ hugepages" << std::endl;
        v = randomx_create_vm(flags, nullptr, dataset);
        if (v == nullptr) {
          std::cerr << "# rxlib: Failed to allocate rx vm" << std::endl;
          return -1;
        }
      }
      vm.push_back(v);
    }
  }

  return hugepages_success ? 1 : 2;
}

int64_t do_hashing(randomx_vm* vm, char* blob, uint32_t len, uint64_t difficulty, char* hash_output, char* nonce_output, std::atomic<uint32_t> *stop) {
  void* noncePtr = blob + 39;
  int64_t hashes = 0;
  auto nonce = atomic_nonce.fetch_add(1);
  auto prevnonce = nonce;

  store32(noncePtr, nonce);

  randomx_calculate_hash_first(vm, blob, len);

  while (true) {
    auto s = stop->load();
    if (s) {
      // TODO: If we are not moving on to a fresh job we might want to check the state of the last
      // hash before returning just in case it meets difficulty?  This would probably be very rare.
      return -hashes;
    }
    prevnonce = nonce;
    nonce = atomic_nonce.fetch_add(1);
	store32(noncePtr, nonce);

    randomx_calculate_hash_next(vm, blob, len, hash_output);

    hashes++;
    if (check_hash_64(hash_output, difficulty)) {
      store32(nonce_output, prevnonce);
      return hashes;
    }
  }
}

extern "C" int64_t rx_hash_until(const char* blob, uint32_t len, uint64_t difficulty, int thread, char* hash_output, char* nonce_output, uint32_t* stopper) {
  int64_t hashes = 0;
  auto stop = reinterpret_cast<std::atomic<uint32_t>* >(stopper);

  fenv_t fpstate;
  fegetenv(&fpstate);

  char mutable_blob[len];
  memcpy(mutable_blob, blob, len);
  hashes = do_hashing(vm[thread], mutable_blob, len, difficulty, hash_output, nonce_output, stop);
  fesetenv(&fpstate);
  return hashes;
}
