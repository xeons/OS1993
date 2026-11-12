/*
 * thrtest.c -- tests of threads and the process, memory and file APIs.
 *
 * Without arguments: the checks, one line per failure and a total
 * (tools/qemu-threads-test.py reads the debug log). With one:
 *
 *   child N      exit with code N (for the process checks)
 *   stack        a thread recurses until it hits its guard page
 *   readonly     write to a page made read-only
 *   exitthread   the first thread ends; the other ends the process (code 5)
 *   exitprocess  a second thread calls exit(9) while the first waits
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os1993.h"

static int failures, checks;

#define CHECK(cond, what) \
    do { checks++; if (!(cond)) { failures++; printf("THR: FAIL %s (line %d, error %lu)\n", what, __LINE__, \
                                                    (unsigned long)GetLastError()); } } while (0)

static HANDLE spawn(LPTHREAD_START_ROUTINE fn, void *arg)
{
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}

/* ---- counting under a critical section ---------------------------------------- */

static CRITICAL_SECTION cs;
static volatile int32_t counted, atomics;

static uint32_t count_up(void *arg)
{
    int n = (int)(uintptr_t)arg;
    for (int i = 0; i < 2000; i++) {
        EnterCriticalSection(&cs);
        int v = counted;
        if (i % 97 == 0)
            Sleep(0);                               /* let the others try to get in */
        counted = v + 1;
        LeaveCriticalSection(&cs);
        InterlockedIncrement(&atomics);
    }
    return (uint32_t)n * 10;
}

static void test_counting(void)
{
    HANDLE t[4];
    for (int i = 0; i < 4; i++)
        t[i] = spawn(count_up, (void *)(uintptr_t)(i + 1));
    CHECK(t[0] && t[3], "CreateThread: four threads");
    bool codes = true;
    for (int i = 0; i < 4; i++) {
        uint32_t code = 0;
        codes &= WaitForSingleObject(t[i], INFINITE) == WAIT_OBJECT_0 && GetExitCodeThread(t[i], &code) &&
                 code == (uint32_t)(i + 1) * 10;
        CloseHandle(t[i]);
    }
    CHECK(codes, "WaitForSingleObject joins them; GetExitCodeThread has what they returned");
    CHECK(counted == 8000, "a critical section keeps 8000 increments whole");
    CHECK(atomics == 8000, "InterlockedIncrement");
    int32_t x = 5;
    CHECK(InterlockedCompareExchange(&x, 9, 5) == 5 && x == 9 && InterlockedCompareExchange(&x, 1, 5) == 9 &&
          InterlockedExchange(&x, 3) == 9 && InterlockedExchangeAdd(&x, 4) == 3 && x == 7, "Interlocked*");
    CHECK(TryEnterCriticalSection(&cs) && TryEnterCriticalSection(&cs), "TryEnterCriticalSection, recursively");
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
}

/* ---- events ------------------------------------------------------------------------ */

static HANDLE ping, pong, gate;
static volatile int32_t rounds, through;

static uint32_t ponger(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        WaitForSingleObject(ping, INFINITE);
        rounds++;
        SetEvent(pong);
    }
    return 0;
}

static uint32_t at_gate(void *arg)
{
    (void)arg;
    WaitForSingleObject(gate, INFINITE);
    InterlockedIncrement(&through);
    return 0;
}

static void test_events(void)
{
    ping = CreateEvent(NULL, false, false, NULL);
    pong = CreateEvent(NULL, false, false, NULL);
    HANDLE t = spawn(ponger, NULL);
    bool ok = true;
    for (int i = 0; i < 100 && ok; i++) {
        SetEvent(ping);
        ok = WaitForSingleObject(pong, 2000) == WAIT_OBJECT_0 && rounds == i + 1;
    }
    CHECK(ok, "auto-reset events: 100 rounds of ping-pong");
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);

    gate = CreateEvent(NULL, true, false, NULL);
    HANDLE w[3];
    for (int i = 0; i < 3; i++)
        w[i] = spawn(at_gate, NULL);
    Sleep(100);
    CHECK(through == 0, "a manual-reset event holds its waiters");
    SetEvent(gate);
    CHECK(WaitForMultipleObjects(3, w, true, 2000) == WAIT_OBJECT_0 && through == 3,
          "... setting it lets all three through (WaitForMultipleObjects, all)");
    for (int i = 0; i < 3; i++)
        CloseHandle(w[i]);
    CHECK(WaitForSingleObject(gate, 0) == WAIT_OBJECT_0, "... and it stays set");
    ResetEvent(gate);
    uint32_t start = GetTickCount();
    uint32_t r = WaitForSingleObject(gate, 150);
    uint32_t took = GetTickCount() - start;
    CHECK(r == WAIT_TIMEOUT && took >= 100 && took < 600, "a wait times out on time");
    CloseHandle(ping);
    CloseHandle(pong);
    CloseHandle(gate);
}

/* ---- semaphores: a bounded buffer ---------------------------------------------------- */

static HANDLE slots, items;
static CRITICAL_SECTION buf_cs;
static int buffer[4], head, tail;
static volatile int32_t consumed_sum;

static uint32_t producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 200; i++) {
        WaitForSingleObject(slots, INFINITE);
        EnterCriticalSection(&buf_cs);
        buffer[head++ % 4] = i;
        LeaveCriticalSection(&buf_cs);
        ReleaseSemaphore(items, 1, NULL);
    }
    return 0;
}

static uint32_t consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100; i++) {
        WaitForSingleObject(items, INFINITE);
        EnterCriticalSection(&buf_cs);
        int v = buffer[tail++ % 4];
        LeaveCriticalSection(&buf_cs);
        ReleaseSemaphore(slots, 1, NULL);
        InterlockedExchangeAdd(&consumed_sum, v);
    }
    return 0;
}

static void test_semaphores(void)
{
    slots = CreateSemaphore(NULL, 4, 4, NULL);
    items = CreateSemaphore(NULL, 0, 4, NULL);
    HANDLE t[3] = { spawn(producer, NULL), spawn(consumer, NULL), spawn(consumer, NULL) };
    CHECK(WaitForMultipleObjects(3, t, true, 10000) == WAIT_OBJECT_0, "producer and consumers finish");
    CHECK(consumed_sum == 200 * 201 / 2, "a bounded buffer with semaphores: every item once");
    for (int i = 0; i < 3; i++)
        CloseHandle(t[i]);
    int32_t prev = -1;
    CHECK(!ReleaseSemaphore(slots, 1, &prev) && GetLastError() == ERROR_TOO_MANY_POSTS,
          "a semaphore can't go over its maximum");
    CloseHandle(slots);
    CloseHandle(items);
}

/* ---- mutexes, TLS, errno -------------------------------------------------------------- */

static HANDLE mutex;
static uint32_t tls;
static volatile int32_t tls_good, errno_good, release_refused;

static uint32_t other_thread(void *arg)
{
    int me = (int)(uintptr_t)arg;
    if (!ReleaseMutex(mutex) && GetLastError() == ERROR_NOT_OWNER)
        InterlockedIncrement(&release_refused);
    TlsSetValue(tls, (void *)(uintptr_t)(me * 1000));
    errno = me;
    SetLastError((uint32_t)me + 100);
    Sleep(50);                                      /* the others set theirs meanwhile */
    if (errno == me && GetLastError() == (uint32_t)me + 100)   /* before TlsGetValue clears it */
        InterlockedIncrement(&errno_good);
    if ((uintptr_t)TlsGetValue(tls) == (uintptr_t)(me * 1000))
        InterlockedIncrement(&tls_good);
    return 0;
}

static void test_per_thread(void)
{
    mutex = CreateMutex(NULL, true, NULL);
    CHECK(WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0, "a mutex is recursive for its owner");
    tls = TlsAlloc();
    CHECK(tls != TLS_OUT_OF_INDEXES, "TlsAlloc");
    TlsSetValue(tls, (void *)7);
    errno = ERANGE;
    HANDLE t[3];
    for (int i = 0; i < 3; i++)
        t[i] = spawn(other_thread, (void *)(uintptr_t)(i + 1));
    WaitForMultipleObjects(3, t, true, INFINITE);
    for (int i = 0; i < 3; i++)
        CloseHandle(t[i]);
    CHECK(release_refused == 3, "only its owner can release a mutex");
    CHECK(tls_good == 3 && (uintptr_t)TlsGetValue(tls) == 7, "TLS slots are each thread's own");
    CHECK(errno_good == 3 && errno == ERANGE, "errno and GetLastError are each thread's own");
    CHECK(ReleaseMutex(mutex) && ReleaseMutex(mutex) && !ReleaseMutex(mutex), "ReleaseMutex, twice");
    CloseHandle(mutex);
    TlsFree(tls);
}

/* ---- suspended, terminated, sleeping ---------------------------------------------------- */

static volatile int32_t ran, spins;

static uint32_t set_ran(void *arg) { (void)arg; ran = 1; return 3; }

static uint32_t spin(void *arg)
{
    (void)arg;
    while (spins >= 0)                              /* until TerminateThread */
        InterlockedIncrement(&spins);
    return 0;
}

static uint32_t nap(void *arg) { (void)arg; Sleep(300); return 0; }

static void test_control(void)
{
    uint32_t tid = 0;
    HANDLE t = CreateThread(NULL, 0, set_ran, NULL, CREATE_SUSPENDED, &tid);
    Sleep(100);
    CHECK(t && tid && !ran, "CREATE_SUSPENDED: it doesn't run ...");
    ResumeThread(t);
    CHECK(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0 && ran, "... until ResumeThread");
    CloseHandle(t);

    t = spawn(spin, NULL);
    Sleep(100);
    uint32_t code = 0;
    CHECK(GetExitCodeThread(t, &code) && code == STILL_ACTIVE, "a running thread is STILL_ACTIVE");
    CHECK(TerminateThread(t, 44) && WaitForSingleObject(t, 2000) == WAIT_OBJECT_0 &&
          GetExitCodeThread(t, &code) && code == 44 && spins > 0, "TerminateThread ends a busy thread");
    CloseHandle(t);

    uint32_t start = GetTickCount();
    HANDLE n[3] = { spawn(nap, NULL), spawn(nap, NULL), spawn(nap, NULL) };
    WaitForMultipleObjects(3, n, true, INFINITE);
    uint32_t took = GetTickCount() - start;
    CHECK(took >= 250 && took < 800, "three threads sleep at the same time");
    for (int i = 0; i < 3; i++)
        CloseHandle(n[i]);

    HANDLE any[2] = { CreateEvent(NULL, false, false, NULL), CreateEvent(NULL, false, true, NULL) };
    CHECK(WaitForMultipleObjects(2, any, false, 0) == WAIT_OBJECT_0 + 1, "WaitForMultipleObjects: which one");
    CloseHandle(any[0]);
    CloseHandle(any[1]);

    HANDLE many[16];
    int made = 0;
    for (int i = 0; i < 16; i++)
        if ((many[made] = CreateThread(NULL, 0, set_ran, NULL, CREATE_SUSPENDED, NULL)))
            made++;
    CHECK(made == 15 && GetLastError() == ERROR_TOO_MANY_THREADS, "16 threads at most: the 16th is refused");
    for (int i = 0; i < made; i++) {
        ResumeThread(many[i]);
        WaitForSingleObject(many[i], INFINITE);
        CloseHandle(many[i]);
    }
    Sleep(200);                                     /* the VMM frees their slots */
    t = spawn(set_ran, NULL);
    CHECK(t && WaitForSingleObject(t, 2000) == WAIT_OBJECT_0, "... and ended threads make room again");
    CloseHandle(t);
}

/* ---- the C heap from several threads ------------------------------------------------------- */

static volatile int32_t heap_good;

static uint32_t churn(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    char *blocks[32] = { 0 };
    size_t sizes[32] = { 0 };
    bool ok = true;
    for (int i = 0; i < 400; i++) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)(seed >> 16) % 32;
        if (blocks[k]) {
            for (size_t j = 0; j < sizes[k]; j++)
                ok &= blocks[k][j] == (char)(k + (int)(uintptr_t)arg);
            free(blocks[k]);
            blocks[k] = NULL;
        } else {
            sizes[k] = (seed >> 8) % 500 + 1;
            blocks[k] = malloc(sizes[k]);
            if (blocks[k])
                memset(blocks[k], k + (int)(uintptr_t)arg, sizes[k]);
            ok &= blocks[k] != NULL;
        }
    }
    for (int k = 0; k < 32; k++)
        free(blocks[k]);
    if (ok)
        InterlockedIncrement(&heap_good);
    return 0;
}

static uint32_t chatter(void *arg)
{
    for (int i = 0; i < 20; i++)
        printf("THR: line from thread %d, number %d of 20\n", (int)(uintptr_t)arg, i);
    return 0;
}

static void test_shared_crt(void)
{
    HANDLE t[4];
    for (int i = 0; i < 4; i++)
        t[i] = spawn(churn, (void *)(uintptr_t)(i + 1));
    WaitForMultipleObjects(4, t, true, INFINITE);
    for (int i = 0; i < 4; i++)
        CloseHandle(t[i]);
    CHECK(heap_good == 4, "malloc and free from four threads at once");
    CHECK(HeapValidate(GetProcessHeap(), 0, NULL), "... the heap is whole after");
    for (int i = 0; i < 3; i++)
        t[i] = spawn(chatter, (void *)(uintptr_t)(i + 1));
    WaitForMultipleObjects(3, t, true, INFINITE);
    for (int i = 0; i < 3; i++)
        CloseHandle(t[i]);
}

/* ---- memory ------------------------------------------------------------------------------- */

static void test_memory(void)
{
    char *r = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE, PAGE_READWRITE);
    CHECK(r && ((uintptr_t)r & 0xFFFF) == 0, "VirtualAlloc reserves 1 MB, 64 KB aligned");
    MEMORY_BASIC_INFORMATION mbi;
    CHECK(VirtualQuery(r, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE && mbi.RegionSize == 1 << 20,
          "VirtualQuery: reserved");
    char *c = VirtualAlloc(r + 0x8000, 0x2000, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == r + 0x8000, "committing two pages inside it");
    c[0] = 'a';
    c[0x1FFF] = 'z';
    CHECK(VirtualQuery(c, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.RegionSize == 0x2000 &&
          mbi.Protect == PAGE_READWRITE, "VirtualQuery: those two are committed");
    uint32_t old = 0;
    CHECK(VirtualProtect(c, 0x1000, PAGE_READONLY, &old) && old == PAGE_READWRITE && c[0] == 'a' &&
          VirtualQuery(c, &mbi, sizeof mbi) && mbi.Protect == PAGE_READONLY && mbi.RegionSize == 0x1000,
          "VirtualProtect: read-only, keeping the contents");
    CHECK(!VirtualProtect(r, 0x1000, PAGE_READONLY, &old), "... not a page that isn't committed");
    CHECK(VirtualFree(c, 0x2000, MEM_DECOMMIT) && VirtualQuery(c, &mbi, sizeof mbi) && mbi.State == MEM_RESERVE,
          "VirtualFree decommits");
    CHECK(!VirtualFree(r + 0x1000, 0, MEM_RELEASE), "MEM_RELEASE wants the region's base");
    CHECK(VirtualFree(r, 0, MEM_RELEASE) && VirtualQuery(r, &mbi, sizeof mbi) && mbi.State == MEM_FREE,
          "... and releases it");
    char *both = VirtualAlloc(NULL, 10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(both && both[9999] == 0, "reserve and commit at once: zeroed pages");
    VirtualFree(both, 0, MEM_RELEASE);

    HANDLE h = HeapCreate(0, 0, 64 * 1024);
    CHECK(h != 0, "HeapCreate: up to 64 KB");
    char *blocks[200];
    int got = 0;
    for (int i = 0; i < 200; i++)
        if ((blocks[got] = HeapAlloc(h, HEAP_ZERO_MEMORY, 400)))
            got++;
    CHECK(got > 120 && got < 200 && GetLastError() == ERROR_NOT_ENOUGH_MEMORY,
          "it grows as needed, and stops at its maximum");
    CHECK(HeapSize(h, 0, blocks[0]) >= 400 && blocks[5][399] == 0, "HeapSize, HEAP_ZERO_MEMORY");
    for (int i = 0; i < got; i++)
        HeapFree(h, 0, blocks[i]);
    CHECK(HeapValidate(h, 0, NULL) && HeapAlloc(h, 0, 60000) != NULL, "freed blocks merge back into one");
    CHECK(HeapDestroy(h), "HeapDestroy");
    char *p = LocalAlloc(LMEM_ZEROINIT, 100);
    p = LocalReAlloc(p, 5000, 0);
    CHECK(p && LocalSize(p) >= 5000 && HeapSize(GetProcessHeap(), 0, p) == LocalSize(p),
          "LocalAlloc is the process heap's");
    LocalFree(p);
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);
    CHECK(ms.dwTotalPhys >= 8u << 20 && ms.dwAvailPhys < ms.dwTotalPhys && ms.dwMemoryLoad <= 100,
          "GlobalMemoryStatus");
}

/* ---- processes ------------------------------------------------------------------------------ */

static void test_processes(void)
{
    int pid = CreateProcess("THRTEST.BIN", "child 7");
    CHECK(pid > 0, "CreateProcess");
    HANDLE p = OpenProcess(0, false, pid);
    CHECK(p != 0, "OpenProcess");
    CHECK(WaitForSingleObject(p, 20000) == WAIT_OBJECT_0, "WaitForSingleObject waits for it to end");
    uint32_t code = 0;
    CHECK(GetExitCodeProcess(p, &code) && code == 7, "GetExitCodeProcess: its exit code");
    CloseHandle(p);
    CHECK(GetExitCodeProcess(GetCurrentProcess(), &code) && code == STILL_ACTIVE, "... ours is still active");
    char name[16];
    CHECK(GetModuleFileName(NULL, name, sizeof name) && !strcasecmp(name, "THRTEST.BIN") &&
          GetCommandLine()[0] == '\0', "GetModuleFileName, GetCommandLine");
}

/* ---- files ------------------------------------------------------------------------------------- */

static volatile int32_t files_good;

static uint32_t file_worker(void *arg)
{
    char name[16], data[256], back[256];
    int me = (int)(uintptr_t)arg;
    snprintf(name, sizeof name, "THR%d.TMP", me);
    for (int i = 0; i < 256; i++)
        data[i] = (char)(i * me);
    HANDLE f = CreateFile(name, GENERIC_WRITE, CREATE_ALWAYS);
    uint32_t n = 0;
    bool ok = f != INVALID_HANDLE_VALUE;
    for (int i = 0; ok && i < 8; i++)
        ok = WriteFile(f, data, sizeof data, &n) && n == sizeof data;
    CloseHandle(f);
    f = CreateFile(name, GENERIC_READ, OPEN_EXISTING);
    for (int i = 0; ok && i < 8; i++)
        ok = ReadFile(f, back, sizeof back, &n) && n == sizeof back && !memcmp(back, data, sizeof data);
    CloseHandle(f);
    DeleteFile(name);
    if (ok)
        InterlockedIncrement(&files_good);
    return 0;
}

static void test_files(void)
{
    HANDLE f = CreateFile("TRUNC.TMP", GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
    uint32_t n;
    WriteFile(f, "0123456789abcdefghij", 20, &n);
    CHECK(SetFilePointer(f, 10, FILE_BEGIN) == 10 && SetEndOfFile(f) && GetFileSize(f) == 10,
          "SetEndOfFile cuts the file at its pointer");
    CHECK(FlushFileBuffers(f), "FlushFileBuffers");
    SYSTEMTIME st = { 2001, 2, 0, 3, 4, 5, 6, 0 }, back;
    FILETIME ft, ft2;
    CHECK(SystemTimeToFileTime(&st, &ft) && FileTimeToSystemTime(&ft, &back) && back.year == 2001 &&
          back.month == 2 && back.day == 3 && back.hour == 4 && back.minute == 5 && back.second == 6 &&
          back.day_of_week == 6, "SystemTimeToFileTime and back (2001-02-03 was a Saturday)");
    CHECK(SetFileTime(f, NULL, NULL, &ft), "SetFileTime");
    CloseHandle(f);
    f = CreateFile("TRUNC.TMP", GENERIC_READ, OPEN_EXISTING);
    CHECK(GetFileTime(f, NULL, NULL, &ft2) && FileTimeToSystemTime(&ft2, &back) && back.year == 2001 &&
          back.month == 2 && back.day == 3 && back.hour == 4 && back.minute == 5 && back.second == 6,
          "GetFileTime: the time set (to DOS's 2 seconds)");
    CloseHandle(f);
    CHECK(CopyFile("TRUNC.TMP", "COPY.TMP", false), "CopyFile");
    f = CreateFile("COPY.TMP", GENERIC_READ, OPEN_EXISTING);
    char buf[32] = { 0 };
    CHECK(ReadFile(f, buf, sizeof buf, &n) && n == 10 && !memcmp(buf, "0123456789", 10) &&
          GetFileTime(f, NULL, NULL, &ft2) && ft2.dwLowDateTime == ft.dwLowDateTime,
          "... the same bytes, and the same date");
    CloseHandle(f);
    CHECK(!CopyFile("TRUNC.TMP", "COPY.TMP", true) && GetLastError() == ERROR_FILE_EXISTS,
          "... fail_if_exists");
    CHECK(SetFileAttributes("COPY.TMP", FILE_ATTRIBUTE_READONLY) &&
          (GetFileAttributes("COPY.TMP") & FILE_ATTRIBUTE_READONLY) && !DeleteFile("COPY.TMP"),
          "SetFileAttributes: a read-only file can't be deleted");
    CHECK(SetFileAttributes("COPY.TMP", 0) && DeleteFile("COPY.TMP") && DeleteFile("TRUNC.TMP"),
          "... until it isn't");
    char full[128], *part = NULL;
    uint32_t len = GetFullPathName("X.TMP", sizeof full, full, &part);
    CHECK(len > 6 && full[1] == ':' && full[2] == '\\' && part && !strcmp(part, "X.TMP"),
          "GetFullPathName");
    CHECK(GetFullPathName("X.TMP", 4, full, NULL) == len + 1, "... too small a buffer: the size needed");
    uint32_t spc, bps, fr, total;
    CHECK(GetDiskFreeSpace(NULL, &spc, &bps, &fr, &total) && bps == 512 && spc >= 1 && total >= fr && total > 0,
          "GetDiskFreeSpace");
    HANDLE t[3];
    for (int i = 0; i < 3; i++)
        t[i] = spawn(file_worker, (void *)(uintptr_t)(i + 1));
    WaitForMultipleObjects(3, t, true, INFINITE);
    for (int i = 0; i < 3; i++)
        CloseHandle(t[i]);
    CHECK(files_good == 3, "three threads write and read files at once");
}

/* ---- the deliberate ones -------------------------------------------------------------------------- */

#pragma GCC diagnostic ignored "-Winfinite-recursion"     /* on purpose: the guard page */
static uint32_t deep(void *arg)
{
    volatile char pad[512];
    pad[0] = (char)(uintptr_t)arg;
    return deep((void *)(uintptr_t)(pad[0] + 1)) + pad[0];
}

static uint32_t outlive(void *arg)
{
    (void)arg;
    Sleep(300);
    printf("THR: the second thread outlived the first\n");
    return 5;
}

static uint32_t quitter(void *arg)
{
    (void)arg;
    Sleep(100);
    exit(9);
}

static int deliberate(const char *what, const char *arg)
{
    if (!strcmp(what, "child")) {
        return atoi(arg ? arg : "0");
    } else if (!strcmp(what, "stack")) {
        HANDLE t = spawn(deep, NULL);
        WaitForSingleObject(t, INFINITE);
    } else if (!strcmp(what, "readonly")) {
        char *p = VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
        uint32_t old;
        VirtualProtect(p, 4096, PAGE_READONLY, &old);
        p[0] = 1;
    } else if (!strcmp(what, "swap")) {             /* more memory than there is: the swap file */
        uint32_t mb = arg ? (uint32_t)atoi(arg) : 20, pages = mb * 256, bad = 0;
        uint32_t *p = VirtualAlloc(NULL, mb << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            printf("THR: swap: no %u MB to commit\n", (unsigned)mb);
            return 2;
        }
        for (uint32_t i = 0; i < pages; i++) {
            p[i * 1024] = i * 2654435761u;
            p[i * 1024 + 1023] = ~i;
        }
        for (uint32_t pass = 0; pass < 2; pass++)   /* forwards, then backwards */
            for (uint32_t k = 0; k < pages; k++) {
                uint32_t i = pass ? pages - 1 - k : k;
                if (p[i * 1024] != i * 2654435761u || p[i * 1024 + 1023] != ~i || p[i * 1024 + 512] != 0)
                    bad++;
            }
        printf("THR: swap: %u MB written and read back twice, %u bad pages\n", (unsigned)mb, (unsigned)bad);
        VirtualFree(p, 0, MEM_RELEASE);
        return bad ? 3 : 0;
    } else if (!strcmp(what, "exitthread")) {
        spawn(outlive, NULL);
        ExitThread(0);
    } else if (!strcmp(what, "exitprocess")) {
        HANDLE t = spawn(quitter, NULL);
        WaitForSingleObject(t, INFINITE);
        printf("THR: the first thread went on\n");
        return 1;
    }
    printf("THR: %s survived\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return deliberate(argv[1], argc > 2 ? argv[2] : NULL);
    test_counting();
    test_events();
    test_semaphores();
    test_per_thread();
    test_control();
    test_shared_crt();
    test_memory();
    test_processes();
    test_files();
    printf("THR: %d of %d checks passed\n", checks - failures, checks);
    return failures;
}
