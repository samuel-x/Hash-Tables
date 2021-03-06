/* * * * * * * * *
* Dynamic hash table using a combination of extendible hashing and cuckoo
* hashing with a single keys per bucket, resolving collisions by switching keys 
* between two tables with two separate hash functions and growing the tables 
* incrementally in response to cycles
*
* created for COMP20007 Design of Algorithms - Assignment 2, 2017
* by Samuel Xu
* Uses code retrieved from xtndbl1.c and linear.c created by 
* Matt Farrugia <matt.farrugia@unimelb.edu.au>
*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "xuckoo.h"
/*
// Use colours for debugging
#include <windows.h>
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN    "\x1b[36m"
#define RESET   "\x1b[0m"
*/
#define EMPTY 0
// macro to calculate the rightmost n bits of a number x
#define rightmostnbits(n, x) (x) & ((1 << (n)) - 1)
// a bucket stores a single key (full=true) or is empty (full=false)
// it also knows how many bits are shared between possible keys, and the first 
// table address that references it

typedef struct bucket {
	int id;		// a unique id for this bucket, equal to the first address
				// in the table which points to it
	int depth;	// how many hash value bits are being used by this bucket
	bool full;	// does this bucket contain a key
	int64 key;	// the key stored in this bucket
} Bucket;

typedef struct stats {
	int nbuckets;	// how many distinct buckets does the table point to
	int nkeys;		// how many keys are being stored in the table
	int time;		// how much CPU time has been used to insert/lookup keys
					// in this table
} Stats;

// an inner table is an extendible hash table with an array of slots pointing 
// to buckets holding up to 1 key, along with some information about the number 
// of hash value bits to use for addressing
typedef struct inner_table {
	Bucket **buckets;	// array of pointers to buckets
	int size;			// how many entries in the table of pointers (2^depth)
	int depth;			// how many bits of the hash value to use (log2(size))
	int nkeys;			// how many keys are being stored in the table
} InnerTable;

// a xuckoo hash table is just two inner tables for storing inserted keys
struct xuckoo_table {
	InnerTable *table1;
	InnerTable *table2;
	Stats stats;
};


bool try_xuck_insert(XuckooHashTable *table, int64 key, int orig_pos, 
						int64 orig_key, int loop, int orig_table);

// Function takes an address and a depth and makes a new bucket
static Bucket *new_bucket(int first_address, int depth) {
	// malloc bucket
	Bucket *bucket = malloc(sizeof *bucket);
	assert(bucket);

	// Set initial values
	bucket->id = first_address;
	bucket->depth = depth;
	bucket->full = false;

	return bucket;
}

// Function creates a new inner table
static InnerTable *new_inner_table() {
	// malloc inner table
	InnerTable *table = malloc(sizeof *table);
	assert(table);

	// set initial values and return
	table->size = 1;
	// Make a new bucket
	table->buckets = malloc(sizeof *table->buckets);
	assert(table->buckets);
	table->buckets[0] = new_bucket(0, 0);
	
	table->depth = 0;
	table->nkeys = 0;

	return table;
};

// Doubles a table's size
static void double_table(InnerTable *table) {
	// upsize the table
	int size = table->size * 2;
	assert(size < MAX_TABLE_SIZE && "error: table has grown too large!");

	// get a new array of twice as many bucket pointers, and copy pointers down
	table->buckets = realloc(table->buckets, (sizeof *table->buckets) * size);
	assert(table->buckets);
	int i;
	for (i = 0; i < table->size; i++) {
		table->buckets[table->size + i] = table->buckets[i];
	}

	// finally, increase the table size and the depth we are using to hash keys
	table->size = size;
	table->depth++;
}

// Reinserts a key to the table
static void reinsert_key(InnerTable *table, int64 key, int table_no) {
	int address;
	// calculate the address
	if (table_no == 1) {
		address = rightmostnbits(table->depth, h1(key));	
	}
	else {
		address = rightmostnbits(table->depth, h2(key));	
	}
	// Just insert, because we know there's space.
	table->buckets[address]->key = key;
	table->buckets[address]->full = true;
}

// split the bucket in 'table' at address 'address', growing table if necessary
static void split_bucket(XuckooHashTable *table, int address, int table_no) {
	// set the inner table depending on the table_no for later code
	InnerTable *inner_table;
	if (table_no == 1) {
		inner_table = table->table1;
	}
	else {
		inner_table = table->table2;
	}

	// FIRST,
	// do we need to grow the table?
	if (inner_table->buckets[address]->depth == inner_table->depth) {
		// yep, this bucket is down to its last pointer
		double_table(inner_table);
	}
	// either way, now it's time to split this bucket

	// SECOND,
	// create a new bucket and update both buckets' depth
	Bucket *bucket = inner_table->buckets[address];
	int depth = bucket->depth;
	int first_address = bucket->id;

	int new_depth = depth + 1;
	bucket->depth = new_depth;

	// new bucket's first address will be a 1 bit plus the old first address
	int new_first_address = 1 << depth | first_address;
	Bucket *newbucket = new_bucket(new_first_address, new_depth);
	
	// THIRD,
	// redirect every second address pointing to this bucket to the new bucket
	// construct addresses by joining a bit 'prefix' and a bit 'suffix'
	// (defined below)

	// suffix: a 1 bit followed by the previous bucket bit address
	int bit_address = rightmostnbits(depth, first_address);
	int suffix = (1 << depth) | bit_address;

	// prefix: all bitstrings of length equal to the difference between the new
	// bucket depth and the table depth
	// use a for loop to enumerate all possible prefixes less than maxprefix:
	int maxprefix = 1 << (inner_table->depth - new_depth);

	int prefix;
	for (prefix = 0; prefix < maxprefix; prefix++) {
		
		// construct address by joining this prefix and the suffix
		int a = (prefix << new_depth) | suffix;

		// redirect this table entry to point at the new bucket
		inner_table->buckets[a] = newbucket;
	}

	// FINALLY,
	// filter the key from the old bucket into its rightful place in the new 
	// table (which may be the old bucket, or may be the new bucket)

	// remove and reinsert the key
	int64 key = bucket->key;
	bucket->full = false;
	reinsert_key(inner_table, key, table_no);
	table->stats.nbuckets++;
}

// initialise an extendible cuckoo hash table
XuckooHashTable *new_xuckoo_hash_table() {
	XuckooHashTable *cuckoo = malloc(sizeof* cuckoo);
	assert(cuckoo != NULL);
	// Create two new inner tables (use helpter function here)
	cuckoo->table1 = new_inner_table();
	//printf("Successfully made table 1!\n");
	cuckoo->table2 = new_inner_table();
	//printf("Successfully made table 2!\n");
	// Then create a cuckoo table and link these to the inner tables
	//printf("Successfully made cuckoo table!\n");
	// set 
	cuckoo->stats.time = 0;
	cuckoo->stats.nkeys = 0;
	cuckoo->stats.nbuckets = 0;
	return cuckoo;
}


// free all memory associated with 'table'
void free_xuckoo_hash_table(XuckooHashTable *table) {
	assert(table);

	// loop backwards through the array of pointers, freeing buckets only as we
	// reach their first reference
	// (if we loop through forwards, we wouldn't know which reference was last)
	int i;
	for (i = table->table1->size-1; i >= 0; i--) {
		if (table->table1->buckets[i]->id == i) {
			free(table->table1->buckets[i]);
		}
	}
	for (i = table->table2->size-1; i >= 0; i--) {
		if (table->table2->buckets[i]->id == i) {
			free(table->table2->buckets[i]);
		}
	}

	// free the array of bucket pointers
	free(table->table1->buckets);
	free(table->table2->buckets);
	free(table->table1);
	free(table->table2);
	
	// free the table struct itself
	free(table);	
}


// insert 'key' into 'table', if it's not in there already
// returns true if insertion succeeds, false if it was already in there
bool xuckoo_hash_table_insert(XuckooHashTable *table, int64 key) {
	assert(table);
	int start_time = clock(); // start timing
	// is this key already there?
	if (xuckoo_hash_table_lookup(table, key) == true) {
		return false;
	}
	// Initialise variables
	int hash;
	int address;
	// Check size of each, if table1 has less or equal keys (according to spec)
	// then insert to table2 instead.
	if (table->table1->nkeys <= table->table2->nkeys) {
		hash = h1(key);
		address = rightmostnbits(table->table1->depth, hash);
		try_xuck_insert(table, key, address, key, 0, 1);
	}
	else {
		hash = h2(key);
		address = rightmostnbits(table->table2->depth, hash);
		try_xuck_insert(table, key, address, key, 1, 2);
	}
	// add time elapsed to total CPU time before returning
	table->stats.time += clock() - start_time;
	return true;
}


// lookup whether 'key' is inside 'table'
// returns true if found, false if not
bool xuckoo_hash_table_lookup(XuckooHashTable *table, int64 key) {
	assert(table);
	int start_time = clock(); // start timing

	// calculate table address for this key
	int address = rightmostnbits(table->table1->depth, h1(key));
	int address2 = rightmostnbits(table->table2->depth, h2(key));
	// look for the key in that bucket (unless it's empty)
	bool found = false;
	if (table->table1->buckets[address]->full) {
		// found it?
		found = table->table1->buckets[address]->key == key;
	}
	if (table->table2->buckets[address2]->full && found == false) {
		// found it?
		found = table->table2->buckets[address2]->key == key;
	}

	// add time elapsed to total CPU time before returning result
	table->stats.time += clock() - start_time;
	return found;
}


// print the contents of 'table' to stdout
void xuckoo_hash_table_print(XuckooHashTable *table) {
	assert(table != NULL);

	printf("--- table ---\n");

	// loop through the two tables, printing them
	InnerTable *innertables[2] = {table->table1, table->table2};
	int t;
	for (t = 0; t < 2; t++) {
		// print header
		printf("table %d\n", t+1);

		printf("  table:               buckets:\n");
		printf("  address | bucketid   bucketid [key]\n");
		
		// print table and buckets
		int i;
		for (i = 0; i < innertables[t]->size; i++) {
			// table entry
			printf("%9d | %-9d ", i, innertables[t]->buckets[i]->id);

			// if this is the first address at which a bucket occurs, print it
			if (innertables[t]->buckets[i]->id == i) {
				printf("%9d ", innertables[t]->buckets[i]->id);
				if (innertables[t]->buckets[i]->full) {
					printf("[%llu]", innertables[t]->buckets[i]->key);
				} else {
					printf("[ ]");
				}
			}

			// end the line
			printf("\n");
		}
	}
	printf("--- end table ---\n");
}


// print some statistics about 'table' to stdout
void xuckoo_hash_table_stats(XuckooHashTable *table) {
	assert(table);

	printf("--- table stats ---\n");

	// print some stats about state of the table
	printf("current tab 1 size: %d\n", table->table1->size);
	printf("current tab 2 size: %d\n", table->table2->size);
	printf("    number of keys: %d\n", table->stats.nkeys);
	printf(" number of buckets: %d\n", table->stats.nbuckets);

	// also calculate CPU usage in seconds and print this
	float seconds = table->stats.time * 1.0 / CLOCKS_PER_SEC;
	printf("    CPU time spent: %.6f sec\n", seconds);
	
	printf("--- end stats ---\n");
}

// Recursive function which performs cuckoo hash
bool try_xuck_insert(XuckooHashTable *table, int64 key, int orig_pos, 
						int64 orig_key, int loop, int orig_table){
	int address; 
	int hash;
	int table_no;
	// Check which table the function is currently in. Assign variables
	// accordingly.
	loop++;
	InnerTable *inner_table;
	if (loop % 2 == 0) {
		inner_table = table->table2;
		hash = h2(key);
		table_no = 2;
	}
	else {
		inner_table = table->table1;
		hash = h1(key);
		table_no = 1;
	}
	address = rightmostnbits(inner_table->depth, hash);
	// If there is a long cuckoo chain (according to spec) then split.
	if (((address == orig_pos) && (key == orig_key) && loop > 3) || 
		loop > table->table1->size+table->table2->size) {
		// split bucket in the current table
		split_bucket(table, address, table_no);

		// and recalculate address because we might now need more bits
		address = rightmostnbits(inner_table->depth, hash);
		// reinsert key after there is space made
		//loop = 0;
		return xuckoo_hash_table_insert(table, key);
	}
	// check if there is already something in the position, if there is,
	// then push new value into that bucket, and take the key to be rehashed
	// and try inserting the rehash key into the opposite table
	if (inner_table->buckets[address]->full == true){
		int64 rehash_key = inner_table->buckets[address]->key;
		inner_table->buckets[address]->key = key;
		return try_xuck_insert(table, rehash_key, orig_pos, orig_key, 
							loop, orig_table);
	}
	else {
		// otherwise, just insert the key and return true
		inner_table->buckets[address]->key = key;
		inner_table->buckets[address]->full = true;
		inner_table->nkeys++;
		table->stats.nkeys++;
		return true;
	}
	//return true;
}
