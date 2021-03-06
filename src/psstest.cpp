/*******************************************************************************
 * src/psstest.cpp
 *
 * Parallel string sorting test program
 *
 *******************************************************************************
 * Copyright (C) 2012-2013 Timo Bingmann <tb@panthema.net>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstddef>
#include <cmath>
#include <string>
#include <vector>
#include <list>
#include <deque>
#include <set>
#include <map>
#include <stack>
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <iomanip>

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <omp.h>
#include <getopt.h>
#include <gmpxx.h>
#include <numa.h>

#include <boost/static_assert.hpp>
#include <boost/array.hpp>
#include <boost/type_traits/integral_constant.hpp>

#include <tlx/string/parse_si_iec_units.hpp>

#include "src/config.h"

#include "tools/stats_writer.hpp"
#include "tools/timer.hpp"
#include "tools/timer_array.hpp"

#ifdef MALLOC_COUNT
#include "tools/malloc_count.h"
#include "tools/stack_count.h"
#include "tools/memprofile.hpp"

static const char* memprofile_path = "memprofile.txt";
#endif

// *** Global Input Data Structures ***

size_t gopt_inputsize = 0;
size_t gopt_inputsize_minlimit = 0;
size_t gopt_inputsize_maxlimit = 0;
size_t gopt_repeats = 1;
size_t gopt_repeats_inner = 1;
std::vector<const char*> gopt_algorithm;
std::vector<const char*> gopt_algorithm_full;
std::vector<const char*> gopt_algorithm_exclude;
int gopt_timeout = 0;

const char* g_datapath = NULL;       // path of input
std::string g_dataname;              // stripped name of input
const char* g_string_data = NULL;    // pointer to first string
size_t g_string_datasize = 0;        // total size of string data (without padding)
char* g_string_databuff = NULL;      // pointer to data buffer with padding
size_t g_string_buffsize = 0;        // total size of string data with padding
size_t g_string_count = 0;           // number of strings.
size_t g_string_dprefix = 0;         // calculated distinguishing prefix
size_t g_string_lcpsum = 0;          // sum over LCP array

const char* gopt_inputwrite = NULL;  // argument -i, --input
const char* gopt_output = NULL;      // argument -o, --output

bool gopt_suffixsort = false;        // argument --suffix
bool gopt_threads = false;           // argument --threads
bool gopt_all_threads = false;       // argument --all-threads
bool gopt_some_threads = false;      // argument --some-threads
bool gopt_no_check = false;          // argument --no-check
bool gopt_mlockall = false;          // argument --mlockall
bool gopt_segment_threads = false;   // argument --segment-threads
bool gopt_segment_one_thread = false; // argument --segment-1thread

std::vector<size_t> gopt_threadlist; // argument --thread-list

size_t g_smallsort = 0;

bool gopt_forkrun = false;
bool gopt_forkdataload = false;

bool gopt_sequential_only = false;            // argument --sequential
bool gopt_parallel_only = false;              // argument --parallel

const size_t g_stacklimit = 64 * 1024 * 1024; // increase from 8 MiB

// for -M mmap_segment:
std::vector<size_t> g_numa_chars;             // offsets of character on NUMA nodes
std::vector<size_t> g_numa_strings;           // number to first strings on NUMA nodes
std::vector<size_t> g_numa_string_count;      // pointer to strings on NUMA nodes

// *** Tools and Algorithms

#include "tools/membuffer.hpp"
#include "tools/lcgrandom.hpp"
#include "tools/contest.hpp"
#include "tools/input.hpp"
#include "tools/checker.hpp"
#include "tools/stringtools.hpp"

#include "sequential/inssort.hpp"
#include "sequential/bs-mkqs.hpp"

#include "sequential/bingmann-radix_sort.hpp"
#include "sequential/bingmann-mkqs.hpp"
#include "sequential/bingmann-lcp_inssort.hpp"
#include "sequential/bingmann-lcp_mergesort_binary.hpp"
#include "sequential/bingmann-lcp_mergesort_kway.hpp"

#include "parallel/eberle-ps5-parallel-toplevel-merge.hpp"
#include "parallel/eberle-parallel-lcp-mergesort.hpp"

#include "rantala/tools/debug.hpp"
#include "rantala/tools/get_char.hpp"
#include "rantala/tools/median.hpp"
#include "rantala/tools/vector_malloc.hpp"
#include "rantala/tools/vector_realloc.hpp"
#include "rantala/tools/vector_block.hpp"
#include "rantala/tools/vector_bagwell.hpp"
#include "rantala/tools/vector_brodnik.hpp"
#include "rantala/tools/insertion_sort.hpp"

#include "rantala/msd_a.hpp"
#include "rantala/msd_a2.hpp"
#include "rantala/msd_ce.hpp"
#include "rantala/msd_ci.hpp"
#include "rantala/msd_dyn_block.hpp"
#include "rantala/msd_dyn_vector.hpp"

#undef debug

Contest * getContestSingleton()
{
    static Contest* c = NULL;
    if (!c) c = new Contest;
    return c;
}

static inline bool gopt_algorithm_select(const Contestant* c)
{
    if (gopt_sequential_only && c->is_parallel()) return false;
    if (gopt_parallel_only && !c->is_parallel()) return false;

    for (size_t ai = 0; ai < gopt_algorithm_exclude.size(); ++ai) {
        if (strstr(c->m_algoname, gopt_algorithm_exclude[ai]) != NULL)
            return false;
    }

    if (gopt_algorithm.size() || gopt_algorithm_full.size())
    {
        // iterate over gopt_algorithm list as a filter
        for (size_t ai = 0; ai < gopt_algorithm.size(); ++ai) {
            if (strstr(c->m_algoname, gopt_algorithm[ai]) != NULL)
                return true;
        }

        // iterate over gopt_algorithm_full list as a exact match filter
        for (size_t ai = 0; ai < gopt_algorithm_full.size(); ++ai) {
            if (strcmp(c->m_algoname, gopt_algorithm_full[ai]) == 0)
                return true;
        }

        return false;
    }

    return true;
}

static inline void maybe_inputwrite()
{
    if (!gopt_inputwrite) return;

    std::cout << "Writing unsorted input to " << gopt_inputwrite << std::endl;
    std::ofstream f(gopt_inputwrite);

    if (!gopt_suffixsort)
    {
        const char* sbegin = g_string_data;
        for (size_t i = 0; i < g_string_datasize; ++i)
        {
            if (g_string_data[i] == 0) {
                f << sbegin << "\n";
                sbegin = g_string_data + i + 1;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < g_string_datasize; ++i)
            f << g_string_data + i << "\n";
    }
}

void Contest::run_contest(const char* path)
{
    g_datapath = path;

    if (!gopt_forkdataload)
    {
        // read input datafile
        if (!input::load(g_datapath))
            return;

        g_string_dprefix = 0;
        g_string_lcpsum = 0;

        maybe_inputwrite();
        std::cout << "Sorting " << g_string_count << " strings composed of " << g_string_datasize << " bytes." << std::endl;
    }

    std::stable_sort(m_list.begin(), m_list.end(), sort_contestants);

    // iterate over all contestants
    for (list_type::iterator c = m_list.begin(); c != m_list.end(); ++c)
    {
        if (gopt_algorithm_select(*c))
            (*c)->run();
    }
}

bool Contest::exist_contestant(const char* algoname)
{
    for (size_t i = 0; i < m_list.size(); ++i) {
        if (strcmp(m_list[i]->m_algoname, algoname) == 0)
            return true;
    }

    return false;
}

void Contest::register_contestant(Contestant* c)
{
    if (exist_contestant(c->m_algoname))
        // duplicate registration
        return;

    m_list.push_back(c);
}

void Contest::list_contentants()
{
    std::cout << "Available string sorting algorithms:" << std::endl;

    std::stable_sort(m_list.begin(), m_list.end(), sort_contestants);

    size_t w_algoname = 0;
    for (list_type::iterator c = m_list.begin(); c != m_list.end(); ++c)
    {
        if (!gopt_algorithm_select(*c)) continue;
        w_algoname = std::max(w_algoname, strlen((*c)->m_algoname));
    }

    // iterate over all contestants
    for (list_type::iterator c = m_list.begin(); c != m_list.end(); ++c)
    {
        if (!gopt_algorithm_select(*c)) continue;
        std::cout << std::left << std::setw(w_algoname)
                  << (*c)->m_algoname << "  " << (*c)->m_description
                  << std::endl;
    }

    if (w_algoname == 0)
        std::cout << "Selected algorithm set is empty." << std::endl;
}

void Contestant_UCArray::run_forked()
{
    if (gopt_memory_type == "mmap_segment" &&
        g_num_threads < (int)g_numa_nodes)
    {
        std::cout << "Skipping because threads=" << g_num_threads
                  << " less than numa_nodes=" << g_numa_nodes << std::endl;
        return;
    }

    if (!gopt_forkrun) return prepare_run();

    pid_t p = fork();
    if (p == 0)
    {
        std::cout << "fork() ------------------------------------------------------------" << std::endl;

        if (gopt_forkdataload)
        {
            // read input datafile
            if (!input::load(g_datapath))
                return;

            g_string_dprefix = 0;
            g_string_lcpsum = 0;

            maybe_inputwrite();
            std::cout << "Sorting " << g_string_count << " strings composed of " << g_string_datasize << " bytes." << std::endl;
        }

        if (gopt_timeout) alarm(gopt_timeout); // terminate child program after gopt_timeout seconds
        prepare_run();

        input::free_stringdata();
        exit(0);
    }

    int status = 0;
    wait(&status);

    if (WIFEXITED(status)) {
        //std::cout << "Child normally with return code " << WEXITSTATUS(status) << std::endl;
    }
    else if (WIFSIGNALED(status)) {
        std::cout << "Child terminated abnormally with signal " << WTERMSIG(status) << std::endl;

        // write out exit status information to results file

        g_stats >> "algo" << m_algoname
            >> "data" << input::strip_datapath(g_datapath)
            >> "memory_type" << gopt_memory_type
            >> "char_count" << gopt_inputsize;

        if (WTERMSIG(status) == SIGALRM)
        {
            g_stats >> "status" << "timeout"
                >> "timeout" << gopt_timeout;
        }
        else if (WTERMSIG(status) == SIGSEGV)
        {
            g_stats >> "status" << "segfault";
        }
        else if (WTERMSIG(status) == SIGABRT)
        {
            g_stats >> "status" << "aborted";
        }
        else
        {
            g_stats >> "status" << "SIG" << WTERMSIG(status);
        }

        std::cout << g_stats << std::endl;
    }
    else {
        std::cout << "Child wait returned with status " << status << std::endl;

        g_stats >> "algo" << m_algoname
            >> "data" << g_dataname
            >> "char_count" << g_string_datasize
            >> "string_count" << g_string_count
            >> "status" << "weird";

        std::cout << g_stats << std::endl;
    }

    if (gopt_output) exit(0);
    g_stats.clear();
}

void Contestant_UCArray::real_run(
    membuffer<uint8_t*>& stringptr,
    std::vector<uintptr_t>& lcp, std::vector<uint8_t>& charcache)
{
    if (!gopt_segment_threads && !gopt_segment_one_thread)
    {
        if (m_prepare_func)
            m_prepare_func(stringptr.data(), stringptr.size());

        if (is_lcp_func())
            m_run_lcp_func(stringptr.data(), lcp.data(), stringptr.size());
        else if (is_lcp_cache_func())
            m_run_lcp_cache_func(
                stringptr.data(), lcp.data(), charcache.data(), stringptr.size());
        else
            m_run_func(stringptr.data(), stringptr.size());
    }
    else {
        // segment input
        size_t nthr = std::thread::hardware_concurrency();

        std::vector<std::pair<size_t, size_t> > ranges(nthr);
        stringtools::calculateRanges(ranges.data(), nthr, stringptr.size());

        std::vector<std::thread> threads(nthr);
        for (size_t i = 0; i < nthr; ++i) {
            if (gopt_segment_one_thread && i != 0)
                continue;

            threads[i] = std::thread(
                [this, &stringptr, &lcp, &charcache, &ranges, i]() {
                size_t begin, length;
                std::tie(begin, length) = ranges[i];

                if (m_prepare_func)
                    m_prepare_func(stringptr.data(), stringptr.size());

                if (is_lcp_func())
                    m_run_lcp_func(
                        stringptr.data() + begin, lcp.data() + begin, length);
                else if (is_lcp_cache_func())
                    m_run_lcp_cache_func(
                        stringptr.data() + begin, lcp.data() + begin,
                        charcache.data() + begin, length);
                else
                    m_run_func(stringptr.data() + begin, length);
            });
        }

        for (size_t i = 0; i < nthr; ++i) {
            if (gopt_segment_one_thread && i != 0)
                continue;

            threads[i].join();
        }
    }
}

void Contestant_UCArray::prepare_run()
{
    typedef unsigned char* string;

    // lock process into memory (on Linux)
    if (!gopt_mlockall) {
        // skip
    }
    else if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::cout << "Error locking process into memory: " << strerror(errno) << std::endl;
    }
    else {
        std::cout << "Successfully locked process into memory." << std::endl;
    }

    // create unsigned char* array from offsets
    membuffer<string> stringptr(g_string_count);

    ClockTimer strptr_timer;

    // make sure sentinels are there
    if (g_numa_chars.size() == 0)
        g_numa_chars.push_back(0);
    if (g_numa_chars.back() != g_string_datasize)
        g_numa_chars.push_back(g_string_datasize);

    ssize_t numNumaNodes = g_numa_nodes;
    if (numNumaNodes < 1) numNumaNodes = 1;

    // set up NUMA string set variables
    ssize_t numaNode = -1;
    g_numa_strings.resize(numNumaNodes);
    g_numa_string_count.resize(numNumaNodes);

    if (!gopt_suffixsort)
    {
        size_t j = 0;
        for (size_t i = 0; i < g_string_datasize; ++i)
        {
            if (i == 0 || g_string_data[i - 1] == 0) {
                assert(j < stringptr.size());
                stringptr[j] = (string)g_string_data + i;

                // stepped over boundary to next NUMA node
                while (i >= g_numa_chars[numaNode + 1]) {
                    numaNode++;
                    g_numa_strings[numaNode] = j;
                    g_numa_string_count[numaNode] = 0;
                }
                assert(numaNode >= 0 && numaNode < numNumaNodes);
                g_numa_string_count[numaNode]++;

                ++j;
            }
        }
        assert(j == g_string_count);
    }
    else
    {
        assert(g_string_count == g_string_datasize);
        for (size_t i = 0; i < g_string_datasize; ++i)
        {
            stringptr[i] = (string)g_string_data + i;

            // stepped over boundary to next NUMA node
            while (i >= g_numa_chars[numaNode + 1]) {
                numaNode++;
                g_numa_strings[numaNode] = i;
                g_numa_string_count[numaNode] = 0;
            }
            assert(numaNode >= 0 && numaNode < numNumaNodes);
            g_numa_string_count[numaNode]++;
        }
    }

    std::cout << "Wrote string pointer array in " << strptr_timer.elapsed()
              << " seconds" << std::endl;

    if (gopt_memory_type == "mmap_segment")
    {
        size_t sum = 0;

        for (size_t n = 0; n < g_numa_strings.size(); ++n)
        {
            std::cout << "NUMA string set[" << n << "] ="
                      << " string offset " << g_numa_strings[n]
                      << " count " << g_numa_string_count[n]
                      << " char offset "
                      << stringptr[g_numa_strings[n]] - (string)g_string_data
                      << std::endl;
            sum += g_numa_string_count[n];
        }

        assert(sum == g_string_count);
    }

    // save permutation check evaluation result
    PermutationCheck pc;
    if (!gopt_no_check) pc = PermutationCheck(stringptr);

    std::cout << "Running " << m_algoname << " - " << m_description
              << std::endl;

    g_stats >> "algo" << m_algoname
        >> "data" << g_dataname
        >> "memory_type" << gopt_memory_type
        >> "char_count" << g_string_datasize
        >> "string_count" << stringptr.size();

    if (g_smallsort)
        g_stats >> "smallsort" << g_smallsort;

    // enable nested parallel regions
    omp_set_nested(true);
    omp_set_num_threads(g_num_threads);

    if (g_num_threads)
    {
        // dummy parallel region to start up threads
        std::atomic<unsigned int> thrsum;
#pragma omp parallel
        {
            thrsum += omp_get_thread_num();
        }
    }

#ifdef MALLOC_COUNT
    //MemProfile memprofile( m_algoname, memprofile_path );
    size_t memuse = malloc_count_current();
    void* stack = stack_count_clear();
    malloc_count_reset_peak();
#endif

    std::vector<uintptr_t> lcp;
    if (is_lcp_func() || is_lcp_cache_func()) {
        lcp.resize(g_string_count);
        // must keep lcp[0] unchanged
        lcp[0] = 42;
        std::fill(lcp.begin() + 1, lcp.end(), -1);
    }

    std::vector<uint8_t> charcache;
    if (is_lcp_cache_func()) {
        charcache.resize(g_string_count);
        charcache[0] = 0;
        std::fill(charcache.begin() + 1, charcache.end(), -1);
    }

    ClockIntervalBase<CLOCK_MONOTONIC> timer;
    ClockIntervalBase<CLOCK_PROCESS_CPUTIME_ID> cpu_timer;

    if (gopt_repeats_inner == 1)
    {
        cpu_timer.start(), timer.start();
        {
            real_run(stringptr, lcp, charcache);
        }
        timer.stop(), cpu_timer.stop();
    }
    else
    {
        // copy unsorted array
        membuffer<string> stringptr_copy;
        stringptr_copy.copy(stringptr_copy);

        cpu_timer.start(), timer.start();

        for (size_t rep = 0; rep < gopt_repeats_inner; ++rep)
        {
            // restore array
            if (rep != 0)
            {
                std::copy(stringptr_copy.begin(), stringptr_copy.end(),
                          stringptr.begin());

                if (is_lcp_func()) {
                    // must keep lcp[0] unchanged
                    lcp[0] = 42;
                    std::fill(lcp.begin() + 1, lcp.end(), -1);
                }
                if (is_lcp_cache_func()) {
                    charcache[0] = 0;
                    std::fill(charcache.begin() + 1, charcache.end(), -1);
                }
            }

            real_run(stringptr, lcp, charcache);
        }
        timer.stop(), cpu_timer.stop();
    }

#ifdef MALLOC_COUNT
    std::cout << "Max stack usage: " << stack_count_usage(stack) << std::endl;
    std::cout << "Max heap usage: " << malloc_count_peak() - memuse << std::endl;
    //memprofile.finish();

    g_stats >> "heapuse" << (malloc_count_peak() - memuse)
        >> "stackuse" << stack_count_usage(stack)
        >> "memleak" << (malloc_count_current() - memuse);

    if (memuse < malloc_count_current())
    {
        std::cout << "MEMORY LEAKED: " << (malloc_count_current() - memuse)
                  << " B" << std::endl;
    }
#endif

    g_stats >> "time" << timer.delta() / gopt_repeats_inner
        >> "cpu_time" << cpu_timer.delta() / gopt_repeats_inner;
    (std::cout << timer.delta() << "\tchecking ").flush();

    if (gopt_repeats_inner != 1)
        g_stats >> "repeats_inner" << gopt_repeats_inner;

    if (!gopt_no_check)
    {
        bool ok = check_sorted_order(stringptr, pc);
        if (ok && is_lcp_func()) {
            ok = stringtools::verify_lcp(
                stringptr.data(), lcp.data(), stringptr.size(), 42);
        }
        if (ok && is_lcp_cache_func()) {
            ok = stringtools::verify_lcp_cache(
                stringptr.data(), lcp.data(), charcache.data(), stringptr.size(), 42);
        }
        if (ok) {
            std::cout << "ok" << std::endl;
            g_stats >> "status" << "ok";
        }
        else {
            g_stats >> "status" << "failed";
        }

        if (!g_string_dprefix)
            g_string_dprefix = calc_distinguishing_prefix(stringptr, g_string_lcpsum);

        g_stats
        >> "dprefix" << g_string_dprefix
            >> "dprefix_percent" << (g_string_dprefix * 100.0 / g_string_datasize)
            >> "lcpsum" << g_string_lcpsum
            >> "avg-lcpsum" << g_string_lcpsum / (double)g_string_count;
    }
    else
    {
        std::cout << "skipped" << std::endl;
    }

    // print timing data out to results file
    std::cout << g_stats << std::endl;
    g_stats.clear();

    if (gopt_output)
    {
        std::cout << "Writing sorted output to " << gopt_output << std::endl;
        std::ofstream f(gopt_output);
        for (size_t i = 0; i < stringptr.size(); ++i)
            f << stringptr[i] << "\n";
        f.close();
        exit(0);
    }
}

void Contestant_UCArray::run()
{
    // sequential algorithm
    g_num_threads = 0;

    for (size_t r = 0; r < gopt_repeats; ++r)
        run_forked();
}

void Contestant_UCArray_Parallel::run()
{
    if (gopt_threadlist.size())
    {
        for (size_t pi = 0; pi < gopt_threadlist.size(); ++pi)
        {
            size_t p = gopt_threadlist[pi];

            for (size_t r = 0; r < gopt_repeats; ++r)
            {
                g_stats.clear();
                g_num_threads = p;
                std::cout << "threads=" << p << std::endl;
                g_stats >> "threads" << p;

                Contestant_UCArray::run_forked();
            }
        }
    }
    else
    {
        int nprocs = omp_get_num_procs();
        int p = (gopt_threads ? 1 : nprocs);

        static const int somethreads16[] = { 2, 4, 6, 8, 12, 16, 0 };
        static const int somethreads32[] = { 2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 0 };
        static const int somethreads48[] = { 2, 3, 6, 9, 12, 18, 24, 30, 36, 42, 48, 0 };
        static const int somethreads64[] = { 2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 0 };

        const int* somethreads = NULL;

        if (gopt_some_threads)
        {
            if (nprocs == 16) somethreads = somethreads16;
            else if (nprocs == 32) somethreads = somethreads32;
            else if (nprocs == 48) somethreads = somethreads48;
            else if (nprocs == 64) somethreads = somethreads64;
            else
                gopt_all_threads = 1;
        }

        while (1)
        {
            for (size_t r = 0; r < gopt_repeats; ++r)
            {
                g_stats.clear();
                g_num_threads = p;
                std::cout << "threads=" << p << std::endl;
                g_stats >> "threads" << p;

                Contestant_UCArray::run_forked();
            }

            if (p == nprocs) break;

            if (somethreads)
                p = *somethreads++;
            else if (!gopt_all_threads)
                p = std::min(nprocs, 2 * p);
            else
                p = std::min(nprocs, p + 1);
        }
    }
}

static inline void increase_stacklimit(size_t stacklimit)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_STACK, &rl)) {
        std::cout << "Error getrlimit(RLIMIT_STACK): " << strerror(errno) << std::endl;
    }
    else if (rl.rlim_cur < stacklimit)
    {
        rl.rlim_cur = stacklimit;
        if (setrlimit(RLIMIT_STACK, &rl)) {
            std::cout << "Error increasing stack limit with setrlimit(RLIMIT_STACK): " << strerror(errno) << std::endl;
        }
        else {
            std::cout << "Successfully increased stack limit to " << stacklimit << std::endl;
        }
    }
}

void print_usage(const char* prog)
{
    std::cout << "Usage: " << prog << " [options] filename" << std::endl
              << "Options:" << std::endl
              << "  -a, --algo <match>     Run only algorithms containing this substring, can be used multile times. Try \"list\"." << std::endl
              << "  -A, --algoname <name>  Run only algorithms fully matching this string, can be used multile times. Try \"list\"." << std::endl
              << "      --all-threads      Run linear thread increase test from 1 to max_processors." << std::endl
              << "  -D, --datafork         Fork before running algorithm and load data within fork!" << std::endl
              << "  -e, --exclude <name>   Skip algorithms containing name!" << std::endl
              << "  -F, --fork             Fork before running algorithm, but load data before fork!" << std::endl
              << "  -i, --input <path>     Write unsorted input strings to file, usually for checking." << std::endl
              << "  -M, --memory <type>    Load string data into <type> memory, see -M list for details." << std::endl
              << "      --mlockall         Perform call of mlockall() to locked program into memory." << std::endl
              << "  -N, --no-check         Skip checking of sorted order and distinguishing prefix calculation." << std::endl
              << "      --numa-nodes <n>   Fake number of NUMA nodes on system." << std::endl
              << "  -o, --output <path>    Write sorted strings to output file, terminate after first algorithm run." << std::endl
              << "      --parallel         Run only parallelized algorithms." << std::endl
              << "  -r, --repeat <num>     Repeat experiment a number of times." << std::endl
              << "  -R, --repeat-inner <n> Repeat inner experiment loop a number of times and divide by repetition count." << std::endl
              << "  -s, --size <size>      Limit the input size to this number of characters." << std::endl
              << "  -S, --maxsize <size>   Run through powers of two for input size limit." << std::endl
              << "      --segment-threads  Run sequential algorithms in parallel on segments of input." << std::endl
              << "      --segment-1thread  Run sequential algorithms in parallel on segments of input." << std::endl
              << "      --sequential       Run only sequential algorithms." << std::endl
              << "      --some-threads     Run specific selected thread counts from 1 to max_processors." << std::endl
              << "      --suffix           Suffix sort the input file." << std::endl
              << "  -T, --timeout <sec>    Abort algorithms after this timeout (default: disabled)." << std::endl
              << "      --threads          Run tests with doubling number of threads from 1 to max_processors." << std::endl
              << "      --thread-list <#>  Run tests with number of threads in list (comma or space separated)." << std::endl
    ;
}

int main(int argc, char* argv[])
{
    enum {
        OPT_SUFFIX = 1,
        OPT_SEGMENT_THREADS,
        OPT_SEGMENT_ONE_THREAD,
        OPT_SEQUENTIAL,
        OPT_PARALLEL,
        OPT_THREADS,
        OPT_ALL_THREADS,
        OPT_SOME_THREADS,
        OPT_THREAD_LIST,
        OPT_MLOCKALL,
        OPT_NUMA_NODES
    };

    static const struct option longopts[] = {
        { "help", no_argument, 0, 'h' },
        { "algo", required_argument, 0, 'a' },
        { "algoname", required_argument, 0, 'A' },
        { "exclude", required_argument, 0, 'e' },
        { "fork", no_argument, 0, 'F' },
        { "datafork", no_argument, 0, 'D' },
        { "input", required_argument, 0, 'i' },
        { "memory", required_argument, 0, 'M' },
        { "no-check", no_argument, 0, 'N' },
        { "output", required_argument, 0, 'o' },
        { "repeat", required_argument, 0, 'r' },
        { "repeat-inner", required_argument, 0, 'R' },
        { "size", required_argument, 0, 's' },
        { "maxsize", required_argument, 0, 'S' },
        { "timeout", required_argument, 0, 'T' },
        { "suffix", no_argument, 0, OPT_SUFFIX },
        { "segment-threads", no_argument, 0, OPT_SEGMENT_THREADS },
        { "segment-1thread", no_argument, 0, OPT_SEGMENT_ONE_THREAD },
        { "sequential", no_argument, 0, OPT_SEQUENTIAL },
        { "parallel", no_argument, 0, OPT_PARALLEL },
        { "threads", no_argument, 0, OPT_THREADS },
        { "all-threads", no_argument, 0, OPT_ALL_THREADS },
        { "some-threads", no_argument, 0, OPT_SOME_THREADS },
        { "thread-list", required_argument, 0, OPT_THREAD_LIST },
        { "mlockall", no_argument, 0, OPT_MLOCKALL },
        { "numa-nodes", required_argument, 0, OPT_NUMA_NODES },
        { 0, 0, 0, 0 },
    };

#ifndef GIT_VERSION_SHA1
#define GIT_VERSION_SHA1 "unknown"
#endif

    {
        char hostname[128];
        gethostname(hostname, sizeof(hostname));
        std::cout << "Running parallel-string-sorting " << GIT_VERSION_SHA1
                  << " on " << hostname << std::endl;

        std::cout << "Called as";
        for (int i = 0; i < argc; ++i)
            std::cout << " " << argv[i];
        std::cout << std::endl;
    }

    g_numa_nodes = numa_num_configured_nodes();

#ifdef MALLOC_COUNT
    if (truncate(memprofile_path, 0)) {
        perror("Cannot truncate memprofile datafile");
    }
#endif

    while (1)
    {
        int index;
        int argi = getopt_long(argc, argv, "hs:S:a:A:e:r:R:o:i:T:DFNM:", longopts, &index);

        if (argi < 0) break;

        switch (argi)
        {
        case 'h':
            print_usage(argv[0]);
            return 0;

        case 'a':
            if (strcmp(optarg, "list") == 0)
            {
                getContestSingleton()->list_contentants();
                return 0;
            }
            gopt_algorithm.push_back(optarg);
            std::cout << "Option -a: selecting algorithms containing " << optarg << std::endl;
            break;

        case 'A':
            if (strcmp(optarg, "list") == 0)
            {
                getContestSingleton()->list_contentants();
                return 0;
            }
            if (!getContestSingleton()->exist_contestant(optarg))
            {
                std::cout << "Option -A: unknown algorithm " << optarg << std::endl;
                return 0;
            }
            gopt_algorithm_full.push_back(optarg);
            std::cout << "Option -A: selecting algorithm " << optarg << std::endl;
            break;

        case 'D':
            gopt_forkrun = gopt_forkdataload = true;
            std::cout << "Option -D: forking before each algorithm run and loading data after fork." << std::endl;
            break;

        case 'e':
            gopt_algorithm_exclude.push_back(optarg);
            std::cout << "Option -e: excluding algorithms containing " << optarg << std::endl;
            break;

        case 'F':
            gopt_forkrun = true;
            std::cout << "Option -F: forking before each algorithm run, but load data before fork." << std::endl;
            break;

        case 'i':
            gopt_inputwrite = optarg;
            std::cout << "Option -i: will write input strings to \"" << gopt_inputwrite << "\"" << std::endl;
            break;

        case 'M':
            gopt_memory_type = optarg;
            if (!input::check_memory_type(gopt_memory_type))
                return 0;
            std::cout << "Option -M: loading input strings into \"" << gopt_memory_type << "\" memory" << std::endl;
            break;

        case 'N':
            gopt_no_check = true;
            std::cout << "Option --no-check: skipping checking of sorted order and distinguishing prefix calculation." << std::endl;
            break;

        case 'o':
            gopt_output = optarg;
            std::cout << "Option -o: will write output strings to \"" << gopt_output << "\"" << std::endl;
            break;

        case 'r':
            gopt_repeats = atoi(optarg);
            std::cout << "Option -r: repeat string sorting algorithms " << gopt_repeats << " times. " << std::endl;
            break;

        case 'R':
            gopt_repeats_inner = atoi(optarg);
            std::cout << "Option -R: repeat inner loop " << gopt_repeats_inner << " times. " << std::endl;
            break;

        case 's':
            if (!tlx::parse_si_iec_units(optarg, &gopt_inputsize_minlimit)) {
                std::cout << "Option -s: invalid size parameter: " << optarg << std::endl;
                exit(EXIT_FAILURE);
            }
            std::cout << "Option -s: limiting input size to " << gopt_inputsize_minlimit << std::endl;
            break;

        case 'S':
            if (!tlx::parse_si_iec_units(optarg, &gopt_inputsize_maxlimit)) {
                std::cout << "Option -S: invalid maxsize parameter: " << optarg << std::endl;
                exit(EXIT_FAILURE);
            }
            std::cout << "Option -S: limiting maximum input size to " << gopt_inputsize_maxlimit << std::endl;
            break;

        case 'T':
            gopt_timeout = atoi(optarg);
            std::cout << "Option -T: aborting algorithms after " << gopt_timeout << " seconds timeout." << std::endl;
            break;

        case OPT_SUFFIX: // --suffix
            gopt_suffixsort = true;
            std::cout << "Option --suffix: running as suffix sorter on input file." << std::endl;
            break;

        case OPT_SEQUENTIAL: // --sequential
            gopt_sequential_only = true;
            std::cout << "Option --sequential: running only sequential algorithms." << std::endl;
            break;

        case OPT_PARALLEL: // --parallel
            gopt_parallel_only = true;
            std::cout << "Option --parallel: running only parallelized algorithms." << std::endl;
            break;

        case OPT_THREADS: // --threads
            gopt_threads = true;
            std::cout << "Option --threads: running test with exponentially increasing thread count." << std::endl;
            break;

        case OPT_ALL_THREADS: // --all-threads
            gopt_threads = gopt_all_threads = true;
            std::cout << "Option --all-threads: running test with linear increasing thread count." << std::endl;
            break;

        case OPT_SOME_THREADS: // --some-threads
            gopt_threads = gopt_some_threads = true;
            std::cout << "Option --some-threads: running test with specifically selected thread counts." << std::endl;
            break;

        case OPT_THREAD_LIST: // --thread-list
        {
            char* endptr = optarg;
            while (*endptr) {
                size_t p = strtoul(endptr, &endptr, 10);
                if (!endptr) break;
                gopt_threadlist.push_back(p);
                std::cout << "Option --thread-list: added p = " << p << " to list of thread counts." << std::endl;
                if (!*endptr++) break;
            }
            break;
        }
        case OPT_MLOCKALL: // --mlockall
            gopt_mlockall = true;
            std::cout << "Option --mlockall: calling mlockall() to lock memory." << std::endl;
            break;

        case OPT_NUMA_NODES: // --numa-nodes <n>
            g_numa_nodes = atoi(optarg);
            std::cout << "Option --numa-nodes: set number of (fake) NUMA nodes to " << g_numa_nodes << "." << std::endl;
            break;

        case OPT_SEGMENT_THREADS: // --segment-threads
            gopt_segment_threads = gopt_no_check = true;
            std::cout << "Option --segment-threads: running sequential algorithms in parallel on segments of the input. This implies skipping checking." << std::endl;
            break;

        case OPT_SEGMENT_ONE_THREAD: // --segment-1thread
            gopt_segment_one_thread = gopt_no_check = true;
            std::cout << "Option --segment-one_thread: running sequential algorithms in one thread on segments of the input. This implies skipping checking." << std::endl;
            break;

        default:
            std::cout << "Invalid parameter: " << argi << std::endl;
            print_usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (optind == argc) { // no input data parameter given
        print_usage(argv[0]);
        return 0;
    }

    increase_stacklimit(g_stacklimit);

    std::cout << "Using CLOCK_MONOTONIC with resolution: " << ClockIntervalBase<CLOCK_MONOTONIC>::resolution() << std::endl;
    std::cout << "Using CLOCK_PROCESS_CPUTIME_ID with resolution: " << ClockIntervalBase<CLOCK_PROCESS_CPUTIME_ID>::resolution() << std::endl;

    if (gopt_inputsize_maxlimit < gopt_inputsize_minlimit)
        gopt_inputsize_maxlimit = gopt_inputsize_minlimit;

    numa_set_strict(1);

    for ( ; optind < argc; ++optind)
    {
        // iterate over input size range
        for (gopt_inputsize = gopt_inputsize_minlimit; gopt_inputsize <= gopt_inputsize_maxlimit;
             gopt_inputsize *= 2)
        {
            // iterate over small sort size
            //for (g_smallsort = 1*1024*1024; g_smallsort <= 1*1024*1024; g_smallsort *= 2)
            {
                getContestSingleton()->run_contest(argv[optind]);
            }

            if (gopt_inputsize == 0) break;
        }
    }

    input::free_stringdata();

    return 0;
}

/******************************************************************************/
