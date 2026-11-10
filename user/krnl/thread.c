/*
 * thread.c -- threads and their synchronization, as in Win32.
 *
 * Threads are the VMM's (SYS_THREAD_*): each has its own stack, with a
 * guard page below it, and a TIB at FS:0. Everything that waits is built
 * on the VMM's futex: block while a word still holds a value, wake those
 * blocked on it. Critical sections, events, mutexes and semaphores are
 * words in the process's memory, so taking a free one never enters the
 * kernel.
 *
 * Only the process's first thread may use the window system (UI.DLL):
 * when it waits, UI.DLL lets go of its lock; other threads' waits don't
 * touch it.
 */
#include "kpriv.h"

/* ---- atomics (486: XADD, CMPXCHG, XCHG) -------------------------------------- */

int32_t InterlockedIncrement(volatile int32_t *p)  { return __sync_add_and_fetch(p, 1); }
int32_t InterlockedDecrement(volatile int32_t *p)  { return __sync_sub_and_fetch(p, 1); }
int32_t InterlockedExchange(volatile int32_t *p, int32_t v) { return __sync_lock_test_and_set(p, v); }
int32_t InterlockedExchangeAdd(volatile int32_t *p, int32_t v) { return __sync_fetch_and_add(p, v); }
int32_t InterlockedCompareExchange(volatile int32_t *p, int32_t v, int32_t cmp)
{
    return __sync_val_compare_and_swap(p, cmp, v);
}

/* ---- futex -------------------------------------------------------------------- */

static int futex_wait(volatile void *addr, uint32_t expected, uint32_t ms)
{
    return (int)syscall3(SYS_FUTEX_WAIT, (uint32_t)(uintptr_t)addr, expected, ms);
}

static void futex_wake(volatile void *addr, int n)
{
    syscall3(SYS_FUTEX_WAKE, (uint32_t)(uintptr_t)addr, (uint32_t)n, 0);
}

/* WaitForMultipleObjects sleeps on this; anything that becomes signalled
 * bumps it. */
static volatile uint32_t signal_gen;

void k_signal(void)
{
    __sync_add_and_fetch(&signal_gen, 1);
    futex_wake(&signal_gen, 0x7FFFFFFF);
}

/* Milliseconds left of a wait that began at start, or WAIT_FOREVER. */
static uint32_t left(uint32_t ms, uint32_t start)
{
    if (ms == INFINITE)
        return WAIT_FOREVER;
    uint32_t gone = GetTickCount() - start;
    return gone >= ms ? 0 : ms - gone;
}

/* ---- critical sections ----------------------------------------------------------- */

/* lock: 0 free, 1 held, 2 held and someone may be waiting (Drepper's
 * "Futexes are tricky", mutex 2); recursive for its owner. */
void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    cs->lock = 0;
    cs->owner = 0;
    cs->recursion = 0;
}

void DeleteCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }

bool TryEnterCriticalSection(CRITICAL_SECTION *cs)
{
    uint32_t me = tib()->tid;
    if (cs->owner == me) {
        cs->recursion++;
        return true;
    }
    if (__sync_val_compare_and_swap(&cs->lock, 0, 1) != 0)
        return false;
    cs->owner = me;
    cs->recursion = 1;
    return true;
}

void EnterCriticalSection(CRITICAL_SECTION *cs)
{
    uint32_t me = tib()->tid;
    if (cs->owner == me) {
        cs->recursion++;
        return;
    }
    int32_t c = __sync_val_compare_and_swap(&cs->lock, 0, 1);
    if (c != 0) {
        if (c != 2)
            c = __sync_lock_test_and_set(&cs->lock, 2);
        while (c != 0) {
            futex_wait(&cs->lock, 2, WAIT_FOREVER);
            c = __sync_lock_test_and_set(&cs->lock, 2);
        }
    }
    cs->owner = me;
    cs->recursion = 1;
}

void LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    if (cs->owner != tib()->tid || --cs->recursion > 0)
        return;
    cs->owner = 0;
    if (__sync_fetch_and_sub(&cs->lock, 1) != 1) {
        cs->lock = 0;
        futex_wake(&cs->lock, 1);
    }
}

/* ---- threads ------------------------------------------------------------------------ */

/* A new thread starts here with [ESP] = fn, [ESP+4] = arg. */
__asm__(".text\n"
        "k_thread_start:\n"
        "    popl %eax\n"                   /* fn; now [ESP] = arg, as for a call */
        "    calll *%eax\n"
        "    pushl %eax\n"                  /* its result is the exit code */
        "    calll ExitThread\n"
        "1:  jmp 1b\n");
void k_thread_start(void);

HANDLE CreateThread(void *security, uint32_t stack_size, LPTHREAD_START_ROUTINE fn, void *arg,
                    uint32_t flags, uint32_t *tid)
{
    (void)security;
    struct thread_args a = { (uint32_t)(uintptr_t)k_thread_start, (uint32_t)(uintptr_t)fn,
                             (uint32_t)(uintptr_t)arg, stack_size,
                             (flags & CREATE_SUSPENDED) ? THREAD_SUSPENDED : 0 };
    int rc = (int)syscall3(SYS_THREAD_CREATE, (uint32_t)(uintptr_t)&a, 0, 0);
    if (rc < 0) {
        k_error((uint32_t)-rc);
        return 0;
    }
    if (tid)
        *tid = (uint32_t)rc;
    return (HANDLE)(H_THREAD | (uint32_t)rc);
}

void ExitThread(uint32_t code)
{
    k_signal();                                 /* WaitForMultipleObjects looks again */
    syscall3(SYS_THREAD_EXIT, code, 0, 0);
    for (;;)
        ;
}

uint32_t ResumeThread(HANDLE h)
{
    if (H_TAG(h) != H_THREAD || (int)syscall3(SYS_THREAD_RESUME, H_ID(h), 0, 0) < 0) {
        k_error(ERROR_INVALID_HANDLE);
        return 0xFFFFFFFFu;
    }
    return 1;                                   /* it was suspended once */
}

bool TerminateThread(HANDLE h, uint32_t code)
{
    uint32_t id = h == GetCurrentThread() ? tib()->tid : H_ID(h);
    if ((h != GetCurrentThread() && H_TAG(h) != H_THREAD) ||
        (int)syscall3(SYS_THREAD_KILL, id, code, 0) < 0) {
        k_error(ERROR_INVALID_HANDLE);
        return false;
    }
    k_signal();
    return true;
}

bool GetExitCodeThread(HANDLE h, uint32_t *code)
{
    int c = 0, rc;
    if (H_TAG(h) != H_THREAD || (rc = (int)syscall3(SYS_THREAD_WAIT, H_ID(h), 0, (uint32_t)(uintptr_t)&c)) < 0) {
        k_error(ERROR_INVALID_HANDLE);
        return false;
    }
    *code = rc == 0 ? (uint32_t)c : STILL_ACTIVE;
    return true;
}

uint32_t GetCurrentThreadId(void)  { return tib()->tid; }
HANDLE   GetCurrentThread(void)    { return (HANDLE)-2; }       /* pseudo-handles, as in Win32 */
HANDLE   GetCurrentProcess(void)   { return (HANDLE)-1; }
void    *GetThreadInformationBlock(void) { return tib(); }

/* ---- thread-local storage ---------------------------------------------------------- */

static CRITICAL_SECTION tls_lock;
static uint32_t tls_used[TLS_SLOTS / 32];

uint32_t TlsAlloc(void)
{
    EnterCriticalSection(&tls_lock);
    uint32_t slot = TLS_OUT_OF_INDEXES;
    for (uint32_t i = 0; i < TLS_SLOTS && slot == TLS_OUT_OF_INDEXES; i++)
        if (!(tls_used[i / 32] & (1u << (i % 32)))) {
            tls_used[i / 32] |= 1u << (i % 32);
            slot = i;
        }
    LeaveCriticalSection(&tls_lock);
    if (slot == TLS_OUT_OF_INDEXES)
        k_error(ERROR_NOT_ENOUGH_MEMORY);
    else
        tib()->tls[slot] = 0;
    return slot;
}

static bool tls_ok(uint32_t i)
{
    if (i < TLS_SLOTS && (tls_used[i / 32] & (1u << (i % 32))))
        return true;
    k_error(ERROR_INVALID_PARAMETER);
    return false;
}

bool TlsFree(uint32_t i)
{
    if (!tls_ok(i))
        return false;
    EnterCriticalSection(&tls_lock);
    tls_used[i / 32] &= ~(1u << (i % 32));
    LeaveCriticalSection(&tls_lock);
    return true;
}

void *TlsGetValue(uint32_t i)
{
    if (!tls_ok(i))
        return NULL;
    k_error(0);
    return (void *)(uintptr_t)tib()->tls[i];
}

bool TlsSetValue(uint32_t i, void *v)
{
    if (!tls_ok(i))
        return false;
    tib()->tls[i] = (uint32_t)(uintptr_t)v;
    return true;
}

/* ---- events, mutexes, semaphores ------------------------------------------------------ */

#define OBJ_MAX 64
enum { O_FREE, O_EVENT, O_MUTEX, O_SEMAPHORE };

struct object {
    int               type;
    volatile int32_t  state;            /* event: 1 set; semaphore: count; mutex: 0 free, 1 held */
    int32_t           max;              /* semaphore */
    volatile uint32_t owner;            /* mutex: its thread */
    int32_t           recursion;
    bool              manual;           /* event: stays set until reset */
};

static struct object objects[OBJ_MAX];
static CRITICAL_SECTION objects_lock;

static HANDLE new_object(int type, int32_t state, int32_t max, bool manual)
{
    EnterCriticalSection(&objects_lock);
    for (int i = 0; i < OBJ_MAX; i++)
        if (objects[i].type == O_FREE) {
            objects[i] = (struct object){ type, state, max, 0, 0, manual };
            LeaveCriticalSection(&objects_lock);
            return (HANDLE)(H_OBJECT | (uint32_t)i);
        }
    LeaveCriticalSection(&objects_lock);
    k_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static struct object *object_of(HANDLE h, int type)
{
    if (H_TAG(h) == H_OBJECT && H_ID(h) < OBJ_MAX && objects[H_ID(h)].type != O_FREE &&
        (!type || objects[H_ID(h)].type == type))
        return &objects[H_ID(h)];
    k_error(ERROR_INVALID_HANDLE);
    return NULL;
}

HANDLE CreateEvent(void *security, bool manual_reset, bool initial, const char *name)
{
    (void)security; (void)name;
    return new_object(O_EVENT, initial ? 1 : 0, 1, manual_reset);
}

bool SetEvent(HANDLE h)
{
    struct object *o = object_of(h, O_EVENT);
    if (!o)
        return false;
    o->state = 1;
    futex_wake(&o->state, o->manual ? 0x7FFFFFFF : 1);
    k_signal();
    return true;
}

bool ResetEvent(HANDLE h)
{
    struct object *o = object_of(h, O_EVENT);
    if (o)
        o->state = 0;
    return o != NULL;
}

HANDLE CreateSemaphore(void *security, int32_t initial, int32_t max, const char *name)
{
    (void)security; (void)name;
    if (max <= 0 || initial < 0 || initial > max) {
        k_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return new_object(O_SEMAPHORE, initial, max, false);
}

bool ReleaseSemaphore(HANDLE h, int32_t n, int32_t *previous)
{
    struct object *o = object_of(h, O_SEMAPHORE);
    if (!o)
        return false;
    for (;;) {
        int32_t c = o->state;
        if (n <= 0 || c + n > o->max) {
            k_error(ERROR_TOO_MANY_POSTS);
            return false;
        }
        if (__sync_bool_compare_and_swap(&o->state, c, c + n)) {
            if (previous)
                *previous = c;
            break;
        }
    }
    futex_wake(&o->state, n);
    k_signal();
    return true;
}

HANDLE CreateMutex(void *security, bool initial_owner, const char *name)
{
    (void)security; (void)name;
    HANDLE h = new_object(O_MUTEX, 0, 1, false);
    if (h && initial_owner) {
        struct object *o = &objects[H_ID(h)];
        o->state = 1;
        o->owner = tib()->tid;
        o->recursion = 1;
    }
    return h;
}

bool ReleaseMutex(HANDLE h)
{
    struct object *o = object_of(h, O_MUTEX);
    if (!o)
        return false;
    if (o->owner != tib()->tid) {
        k_error(ERROR_NOT_OWNER);
        return false;
    }
    if (--o->recursion > 0)
        return true;
    o->owner = 0;
    o->state = 0;
    futex_wake(&o->state, 1);
    k_signal();
    return true;
}

bool k_close_object(HANDLE h)
{
    if (H_TAG(h) == H_THREAD || H_TAG(h) == H_PROCESS)
        return true;                            /* nothing to free */
    struct object *o = object_of(h, 0);
    if (!o)
        return false;
    EnterCriticalSection(&objects_lock);
    o->type = O_FREE;
    LeaveCriticalSection(&objects_lock);
    return true;
}

/* ---- waiting ------------------------------------------------------------------------------ */

/* Take h if it is signalled (an auto-reset event resets, a semaphore
 * counts down, a mutex becomes ours). *word and *seen: what to sleep on. */
static int try_take(HANDLE h, volatile void **word, uint32_t *seen)
{
    *word = NULL;
    if (H_TAG(h) == H_THREAD || H_TAG(h) == H_PROCESS) {
        int sys = H_TAG(h) == H_THREAD ? SYS_THREAD_WAIT : SYS_PROC_WAIT;
        int rc = (int)syscall3((uint32_t)sys, H_ID(h), 0, 0);
        return rc < 0 ? -1 : rc == 0;
    }
    struct object *o = object_of(h, 0);
    if (!o)
        return -1;
    uint32_t me = tib()->tid;
    for (;;) {
        int32_t s = o->state;
        *word = &o->state;
        *seen = (uint32_t)s;
        switch (o->type) {
        case O_EVENT:
            if (!s)
                return 0;
            if (o->manual || __sync_bool_compare_and_swap(&o->state, 1, 0))
                return 1;
            continue;
        case O_SEMAPHORE:
            if (s <= 0)
                return 0;
            if (__sync_bool_compare_and_swap(&o->state, s, s - 1))
                return 1;
            continue;
        case O_MUTEX:
            if (o->owner == me) {
                o->recursion++;
                return 1;
            }
            if (s || !__sync_bool_compare_and_swap(&o->state, 0, 1))
                return 0;
            o->owner = me;
            o->recursion = 1;
            return 1;
        }
        return -1;
    }
}

uint32_t WaitForSingleObject(HANDLE h, uint32_t ms)
{
    uint32_t start = GetTickCount(), result = WAIT_TIMEOUT;
    bool hooked = false;
    for (;;) {
        volatile void *word;
        uint32_t seen;
        int got = try_take(h, &word, &seen);
        if (got < 0) {
            result = WAIT_FAILED;
            break;
        }
        if (got) {
            result = WAIT_OBJECT_0;
            break;
        }
        uint32_t rest = left(ms, start);
        if (!rest)
            break;
        if (!hooked && on_main_thread()) {
            k_before_wait();
            hooked = true;
        }
        if (word)
            futex_wait(word, seen, rest);
        else                                    /* a thread or a process: the VMM waits */
            syscall3(H_TAG(h) == H_THREAD ? SYS_THREAD_WAIT : SYS_PROC_WAIT, H_ID(h), rest, 0);
    }
    if (hooked)
        k_after_wait();
    return result;
}

/* Wait for one (or, wait_all, each in turn) of n handles. Waiting for all
 * takes them one at a time rather than all at once. */
uint32_t WaitForMultipleObjects(uint32_t n, const HANDLE *hs, bool wait_all, uint32_t ms)
{
    if (!n || n > MAXIMUM_WAIT_OBJECTS || !hs) {
        k_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    uint32_t start = GetTickCount(), result = WAIT_TIMEOUT, taken = 0;
    bool hooked = false, polls = false;
    for (uint32_t i = 0; i < n; i++)
        polls |= H_TAG(hs[i]) == H_PROCESS;     /* nothing bumps signal_gen for these */
    for (;;) {
        uint32_t gen = signal_gen;
        bool fail = false;
        int first = -1;
        for (uint32_t i = 0; i < n; i++) {
            if (wait_all && (taken & (1u << i)))
                continue;
            volatile void *word;
            uint32_t seen;
            int got = try_take(hs[i], &word, &seen);
            if (got < 0)
                fail = true;
            else if (got && !wait_all) {
                first = (int)i;
                break;
            } else if (got) {
                taken |= 1u << i;
            }
        }
        if (fail) {
            result = WAIT_FAILED;
            break;
        }
        if (first >= 0) {
            result = WAIT_OBJECT_0 + (uint32_t)first;
            break;
        }
        if (wait_all && taken == (n == 32 ? 0xFFFFFFFFu : (1u << n) - 1)) {
            result = WAIT_OBJECT_0;
            break;
        }
        uint32_t rest = left(ms, start);
        if (!rest)
            break;
        if (!hooked && on_main_thread()) {
            k_before_wait();
            hooked = true;
        }
        futex_wait(&signal_gen, gen, polls && rest > 55 ? 55 : rest);
    }
    if (hooked)
        k_after_wait();
    return result;
}

/* Sleep: the first thread waits as before (input and WakeProcess end it
 * early, for UI.DLL); others sleep on their own TIB word. */
void k_thread_sleep(uint32_t ms)
{
    if (!ms) {
        syscall3(SYS_YIELD, 0, 0, 0);
        return;
    }
    uint32_t start = GetTickCount(), rest;
    while ((rest = left(ms, start)) != 0)
        futex_wait(&tib()->sleep_word, tib()->sleep_word, rest);
}
