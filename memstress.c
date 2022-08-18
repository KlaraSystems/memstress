/*-
 * Copyright (c) 2022 Tom Jones <thj@freebsd.org>
 * Copyright (c) 2022 Klara Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <err.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <vm/vm_param.h>

#define PAGESZ 4096

#include "libroute.h"

void usage(void) __dead2;

void routestress(bool, bool, int, int, int, int);
void histstore(uint64_t *, uint64_t *, uint64_t);
void histprint(const char **, uint64_t *);
bool vm_veto(int);

void freepages(char *, int, int);
char *allocate_pages(int, int , int , int);

static int veto_count;
static int failure_count;

void
usage(void)
{
	printf("usage: memstress [Hhmnrvwxyz]\n");
	printf("       memstress: apply memory pressure either through\n");
	printf("       memory allocations or route creation\n");
	printf("       -H	display a route creation time histogram\n");
	printf("       -h	print help (this message)\n");
	printf("       -m	perform memory allocation test\n");
	printf("       -n	allocation size in pages for memory tests\n");
	printf("                (default: vm.stats.vm.page_count)\n");
	printf("       -r	perform route addition tests\n");
	printf("       -v 	veto size in pages (default: vm.v_free_target)\n");
	printf("                Size in pages that would veto an allocation/route addition\n");
	printf("       -w	wait between allocation/addition before freeing\n");
	printf("       -x	number of addresses in x part of subnet\n");
	printf("       -y	number of addresses in y part of subnet\n");
	printf("       -z	number of addresses in z part of subnet\n");
	printf("       \n");
	printf("       Route addition tests will add routes from 10.z.y.x addresses via 1.0.0.1\n");
	printf("       this needs to be configured on an interface or the test will fail immediately\n");

	exit(0);
}

int
main(int argc, char **argv)
{
	const char *errstr;
	char *pagetable = NULL;
	bool Hflag, mflag, rflag;
	bool work, wait;
	int ch, err;
	int page_count, free_target;
	int x, y, z;
	size_t len;

	mflag = false;
	rflag = false;
	work = true;
	wait = false;
	x = y = z = 100;

	veto_count = 0;
	failure_count = 0;


	/* reuse free_target to find v_free_count , we don't use it for the veto */
	len = sizeof(free_target);
	err = sysctlbyname("vm.stats.vm.v_free_count", &free_target, &len, NULL, 0);
	if (err == -1) {
		perror("getting vm.stats.vm.v_free_count");
		exit(1);
	}
	printf("v_free_count at start is %d pages (%ld MB)\n", free_target, ((uint64_t)free_target*PAGESZ)/1048576);

	/* reuse free_target to find v_free_severe, we don't use it for the veto */
	len = sizeof(free_target);
	err = sysctlbyname("vm.v_free_severe", &free_target, &len, NULL, 0);
	if (err == -1) {
		perror("getting vm.v_free_severe");
		exit(1);
	}
	printf("v_free_severe is %d pages (%ld MB)\n", free_target, ((uint64_t)free_target*PAGESZ)/1048576);

	len = sizeof(free_target);
	err = sysctlbyname("vm.v_free_target", &free_target, &len, NULL, 0);
	if (err == -1) {
		perror("getting vm.v_free_target");
		exit(1);
	}
	printf("free_target is %d pages (%ld MB)\n", free_target, ((uint64_t)free_target*PAGESZ)/1048576);

	len = sizeof(page_count);
	err = sysctlbyname("vm.stats.vm.v_page_count", &page_count, &len, NULL, 0);
	if (err == -1) {
		perror("getting vm.stats.vm.v_page_count");
		exit(1);
	}
	printf("page_count is %d pages (%ld MB)\n", page_count, ((uint64_t)free_target*PAGESZ)/1048576);

	while ((ch = getopt(argc, argv, "Hhmn:rv:wx:y:z:")) != -1) {
		switch (ch) {
		case 'H':
			Hflag = true;
			break;
		case 'h':
			usage();
		case 'm':
			mflag = true;
			rflag = false;
			break;
		case 'n':
			page_count = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				perror("reading page count");
				exit(0);
			}
			break;
		case 'r':
			rflag = true;
			mflag = false;
			break;
		case 'v':
			free_target = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				perror("reading veto target");
				exit(0);
			}
			break;
		case 'w':
			wait = true;
			break;
		case 'x':
			x = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				perror("reading x");
				exit(0);
			}
			if (x > 254) {
				printf("ERROR: x can't be larger than 254 (%d)\n", z);
				exit(0);
			}
			break;
		case 'y':
			y = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				perror("reading y");
				exit(0);
			}
			if (y > 254) {
				printf("ERROR: y can't be larger than 254 (%d)\n", z);
				exit(0);
			}
			break;
		case 'z':
			z = strtonum(optarg, 0, INT_MAX, &errstr);
			if (errstr != NULL) {
				perror("reading z");
				exit(0);
			}
			if (z > 254) {
				printf("ERROR: z can't be larger than 254 (%d)\n", z);
				exit(0);
			}
			break;
		default:
			printf("no idea what %c is for\n", ch);
			exit(1);
		}

	}

	if (!mflag && !rflag)
		usage();

	if (mflag) {
		printf("allocating %d pages ( %ld MB) and touching them\n", 
			page_count, ((uint64_t)page_count*PAGESZ)/1048576);

		pagetable = allocate_pages(PAGESZ, page_count, work, free_target);
		if (wait) {
			printf("Press enter to trigger memory reclaim\n");
			getchar();
		}

		/* Free mmap'd pages */
		printf("freeing allocated pages\n");
		freepages(pagetable, PAGESZ, page_count);
	} else if (rflag) {
		routestress(wait, Hflag, free_target, x, y, z);
	}
	printf("would have vetoed %d times (%d failures)\n", veto_count, failure_count);
}

char *
allocate_pages(int pagesize, int count, int work, int veto)
{
	char *pagetable = NULL;
	char *page = NULL;

	pagetable = calloc(count, sizeof(char *));

	if (pagetable == NULL) {
		perror("allocate pages");
		return NULL;
	}

	page = pagetable;
	for (int i = 0; i < count; page++, i++) {
		vm_veto(veto);

		page = mmap(NULL, pagesize, PROT_READ|PROT_WRITE, 
			MAP_ANON|MAP_PRIVATE, -1, 0);

		if (page == MAP_FAILED) {
			perror("allocation failed for ...");
			failure_count++;
			continue;
		}

		if (work)
			memset(page++, 44, pagesize);
	}

	return pagetable;
}

void
freepages(char *page, int pagesize, int count)
{
	for (int i = 0; i < count; page++, i++) {
		if (page != NULL || page != MAP_FAILED)
			munmap(page, pagesize);
	}
}

void
routestress(bool wait, bool printhist, int veto, int xroutes, int yroutes, int zroutes)
{
	rt_handle *handle;
	struct sockaddr *sa_dest, *sa_gateway;
	struct timespec start, end, elapsed;
	char buf[] = "xxx.xxx.xxx.xxx";
	int error;

#define ONEMS 1000000
	const char *bucketlabels[11]  
		= { "1ms", "2ms", "3ms", "4ms", "5ms", "6ms", "7ms", "8ms", "9ms", "10ms", "100ms" };
	uint64_t bucketcuts[11]  
		= { ONEMS, 2*ONEMS, 3*ONEMS,
		4*ONEMS, 5*ONEMS, 6*ONEMS, 7*ONEMS,
		8*ONEMS, 9*ONEMS, 10*ONEMS, 100*ONEMS};

	uint64_t buckets[11];
	memset(buckets, 0, sizeof(uint64_t) * 11);

	handle = libroute_open(0);

	if (handle == NULL)
		err(1, "libroute_open failed");

	sa_gateway = str_to_sockaddr(handle, "1.0.0.1");
	printf("adding %d routes in blocks of %d\n", zroutes * xroutes * yroutes, yroutes);
	for (int z = 0; z < zroutes; z++) {
		for (int y = 0; y < yroutes; y++) {
			vm_veto(veto);

			timespec_get(&start, TIME_UTC);
			for (int x = 2; x < xroutes; x++) {
				snprintf(buf, sizeof(buf), "10.%d.%d.%d", z, y, x);
				sa_dest = str_to_sockaddr(handle, buf);
				error = libroute_add(handle, sa_dest, sa_gateway);
				if (error == -1) {
					err(1, "Failed to add route");
					failure_count++;
				}
			}

			timespec_get(&end, TIME_UTC);
			timespecsub(&end, &start, &elapsed);	
			if (elapsed.tv_sec > 0)
				printf("it took more than 1 second to add %d routes", yroutes);
			histstore(bucketcuts, buckets, (uint64_t)elapsed.tv_nsec);
		}
	}

	if (wait) {
		printf("Press enter to route flushing\n");
		getchar();
	}
	printf("flushing created routes\n");
	for (int z = 0; z < zroutes; z++) {
		for (int x = 0; x < xroutes; x++) {
			for (int y = 2; y < yroutes; y++) {
				snprintf(buf, sizeof(buf), "10.%d.%d.%d", z, x, y);
				sa_dest = str_to_sockaddr(handle, buf);
				error = libroute_del(handle, sa_dest);
				if (error == -1) {
					err(1, "Failed to delete route");
				}
			}
		}
	}
	libroute_close(handle);
	if (printhist)
		histprint(bucketlabels, buckets);
}


void 
histstore(uint64_t *bc, uint64_t *b, uint64_t value)
{
	if (value >= bc[10]) {
		b[10]++;
		return;
	}

	for (int i = 0; i < 11; i++) {
		if (value < bc[i]) {
			b[i]++;
			break;
		}
	}
}


void 
histprint(const char **labels, uint64_t *b)
{
	int total = 0;
	for (int i = 0; i < 11; i++) {
		printf("%s: %lu\n", labels[i], b[i]);
		total += b[i];
	}
	printf("%d total readings in histogram\n", total);
}

bool 
vm_veto(int target)
{
	size_t len;
	int free_pages;
	int err = 0;

	free_pages = 0;
	len = sizeof(free_pages);
	err = sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &len, NULL, 0);

	if (err == -1)
		perror("free_pages error");

	if (free_pages >= target)
		return false;
	veto_count++;
	return true;
}
