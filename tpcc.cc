// Copyright 2008,2009,2010 Massachusetts Institute of Technology.
// All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#define __STDC_FORMAT_MACROS
#include <climits>
#include <cstdio>
#include <inttypes.h>

#include "clock.h"
#include "randomgenerator.h"
#include "tpccclient.h"
#include "tpccgenerator.h"
#include "tpcctables.h"

#include "m5ops.h"


void * log_ptr;

static const int NUM_TRANSACTIONS = 4096;

int main(int argc, const char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "tpcc [num warehouses]\n");
        exit(1);
    }
    /*
    log_ptr = (int **) malloc(1 * sizeof(int *));

    for (int i = 0; i < 1; i++) {
      log_ptr[i] = (int *) aligned_alloc(4096, 512 * 4096);
    }
    */

    log_ptr = (void *) aligned_alloc(4096, 8 * 4096);
    
    m5_gg_log_alloc((uint64_t) log_ptr,
		    (uint64_t) 8 * 4096);
    __dmb();
    
    long num_warehouses = strtol(argv[1], NULL, 10);
    if (num_warehouses == LONG_MIN || num_warehouses == LONG_MAX) {
        fprintf(stderr, "Bad warehouse number (%s)\n", argv[1]);
        exit(1);
    }
    if (num_warehouses <= 0) {
        fprintf(stderr, "Number of warehouses must be > 0 (was %ld)\n", num_warehouses);
        exit(1);
    }
    if (num_warehouses > Warehouse::MAX_WAREHOUSE_ID) {
        fprintf(stderr, "Number of warehouses must be <= %d (was %ld)\n", Warehouse::MAX_WAREHOUSE_ID, num_warehouses);
        exit(1);
    }

    TPCCTables* tables = (TPCCTables*) aligned_alloc(64, sizeof(TPCCTables));
    new (tables) TPCCTables();
      //new TPCCTables();
    SystemClock* clock = (SystemClock*) aligned_alloc(64, sizeof(SystemClock));
    new (clock) SystemClock();
      //new SystemClock();

    // Create a generator for filling the database.
    tpcc::RealRandomGenerator* random = (tpcc::RealRandomGenerator*) aligned_alloc(64, sizeof(tpcc::RealRandomGenerator));
    new (random) tpcc::RealRandomGenerator();
      //new tpcc::RealRandomGenerator();
    tpcc::NURandC cLoad = tpcc::NURandC::makeRandom(random);
    random->setC(cLoad);

    // Generate the data
    printf("Loading %ld warehouses... ", num_warehouses);
    fflush(stdout);
    char now[Clock::DATETIME_SIZE+1];
    clock->getDateTimestamp(now);
    TPCCGenerator generator(random, now, Item::NUM_ITEMS, District::NUM_PER_WAREHOUSE,
            Customer::NUM_PER_DISTRICT, NewOrder::INITIAL_NUM_PER_DISTRICT);
    int64_t begin = clock->getMicroseconds();
    generator.makeItemsTable(tables);
    for (int i = 0; i < num_warehouses; ++i) {
        generator.makeWarehouse(tables, i+1);
    }
    int64_t end = clock->getMicroseconds();
    printf("%" PRId64 " ms\n", (end - begin + 500)/1000);

    // Change the constants for run
    random = (tpcc::RealRandomGenerator*) aligned_alloc(64, sizeof(tpcc::RealRandomGenerator));
    new (random) tpcc::RealRandomGenerator();
    //new tpcc::RealRandomGenerator();
    random->setC(tpcc::NURandC::makeRandomForRun(random, cLoad));

    // Client owns all the parameters
    TPCCClient client(clock, random, tables, Item::NUM_ITEMS, static_cast<int>(num_warehouses),
            District::NUM_PER_WAREHOUSE, Customer::NUM_PER_DISTRICT);
    printf("Running... ");
    fflush(stdout);
    begin = clock->getMicroseconds();
    m5_dump_stats(0,0);
    m5_reset_stats(0, 0);
    for (int i = 0; i < NUM_TRANSACTIONS; ++i) {
        client.doOne();
    }
    m5_dump_stats(0, 0);
    end = clock->getMicroseconds();
    
    int64_t microseconds = end - begin;
    printf("%d transactions in %" PRId64 " ms = %f txns/s\n", NUM_TRANSACTIONS,
            (microseconds + 500)/1000, NUM_TRANSACTIONS / (double) microseconds * 1000000.0);

    return 0;
}
