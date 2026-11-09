/*
 * queue.c -- the window manager lock, message queues, cross-process
 * SendMessage, timers, and the message loop calls.
 *
 * Every process has its own queue in the shared arena. PostMessage puts a
 * message in the queue of the process the window belongs to and wakes it.
 * SendMessage to our own window is a call; to another process's window it
 * waits while that process runs the window procedure (in its GetMessage or
 * any other wait) -- answering messages sent to us meanwhile, so two
 * processes sending to each other don't deadlock. WM_COPYDATA's data is
 * copied into the arena on the way: the IPC primitive for passing data.
 *
 * The lock: one per window system (kernel lock 0), held from a process's
 * first UI call until it waits, like Win95's Win16Mutex.
 */
#include "uipriv.h"

int ui_pid;
static bool held;                       /* we own the window manager lock */
static bool rehold;
static struct pqueue *myq;

_Static_assert(sizeof(struct ui_shared) <= USER_ARENA_SIZE, "shared arena too small");

/* ---- the lock ------------------------------------------------------------ */

void ui_enter(void)
{
    if (!held) {
        AcquireLock(0);
        held = true;
    }
}

void ui_leave(void)
{
    if (held) {
        held = false;
        ReleaseLock(0);
    }
}

static void before_wait(void) { rehold = held; ui_leave(); }
static void after_wait(void)  { if (rehold) ui_enter(); }

void ui_wait(uint32_t ms, bool input)
{
    WaitForWake(ms, input ? WAIT_INPUT : 0);    /* the hooks leave and re-enter */
}

static struct pqueue *queue_of(int pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (S->queues[i].pid == pid)
            return &S->queues[i];
    return NULL;
}

void InitUI(void)
{
    ui_pid = GetCurrentProcessId();
    SetWaitHooks(before_wait, after_wait);
    ui_screen_init();
    if (S->magic != UI_MAGIC) {                 /* the first process sets it up */
        memset(S, 0, sizeof *S);
        S->magic = UI_MAGIC;
        S->work = (RECT){ 0, 0, 640, 480 };
        S->in.cur_x = 320;
        S->in.cur_y = 240;
        S->exits_seen = GetProcessExitCount();
        gfx_set_palette(theme_palette);
        ui_init_classes();
        ui_invalidate_screen(0, 0, 640, 480);
    }
    myq = queue_of(ui_pid);
    if (!myq && (myq = queue_of(0)) != NULL) {
        memset(myq, 0, sizeof *myq);
        myq->pid = ui_pid;
    }
}

/* ---- posting --------------------------------------------------------------- */

static bool push(struct pqueue *q, HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    if (msg == WM_MOUSEMOVE && q->head != q->tail) {    /* merge moves */
        MSG *last = &q->q[(q->tail + QMAX - 1) % QMAX];
        if (last->message == WM_MOUSEMOVE && last->hwnd == h) {
            last->wParam = w;
            last->lParam = l;
            return true;
        }
    }
    unsigned next = (q->tail + 1) % QMAX;
    if (next == q->head)
        return false;
    q->q[q->tail] = (MSG){ h, msg, w, l, GetTickCount() };
    q->tail = next;
    return true;
}

bool PostMessage(HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    struct wnd *n = h ? wnd_from(h) : NULL;
    if (h && !n)
        return false;
    int pid = n ? n->pid : ui_pid;
    struct pqueue *q = queue_of(pid);
    if (!q || !push(q, h, msg, w, l))
        return false;
    if (pid != ui_pid)
        WakeProcess(pid);
    return true;
}

void PostQuitMessage(int code)
{
    if (myq) {
        myq->quit = true;
        myq->quit_code = code;
    }
}

/* ---- sending ------------------------------------------------------------------ */

/* Run the messages other processes are waiting on us to handle. */
static void answer_sent(void)
{
    for (int i = 0; i < SEND_MAX; i++) {
        struct sent *s = &S->sends[i];
        if (!s->from || s->to != ui_pid || s->taken)
            continue;
        s->taken = true;
        struct wnd *w = wnd_from(s->hwnd);
        s->result = wnd_mine(w) ? w->proc(s->hwnd, s->msg, s->wp, s->lp) : 0;
        s->done = true;
        WakeProcess(s->from);
    }
}

static LRESULT send_across(struct wnd *w, unsigned msg, WPARAM wp, LPARAM lp)
{
    int to = w->pid;
    struct sent *s = NULL;
    while (!s) {
        if (!IsProcessAlive(to))
            return 0;
        for (int i = 0; i < SEND_MAX && !s; i++)
            if (!S->sends[i].from)
                s = &S->sends[i];
        if (!s) {                               /* all slots busy: wait a moment */
            answer_sent();
            ui_wait(55, false);
        }
    }
    memset(s, 0, offsetof(struct sent, data));
    s->from = ui_pid;
    s->to = to;
    s->hwnd = w;
    s->msg = msg;
    s->wp = wp;
    s->lp = lp;
    if (msg == WM_COPYDATA && lp) {             /* the data goes with it */
        const COPYDATASTRUCT *src = (const COPYDATASTRUCT *)lp;
        uint32_t n = src->cbData < COPYDATA_MAX ? src->cbData : COPYDATA_MAX;
        if (n && src->lpData)
            memcpy(s->data, src->lpData, n);
        s->cds = (COPYDATASTRUCT){ src->dwData, n, s->data };
        s->lp = (LPARAM)&s->cds;
    }
    WakeProcess(to);
    while (!s->done) {
        answer_sent();
        if (!IsProcessAlive(to))
            break;
        ui_wait(110, false);
    }
    LRESULT r = s->done ? s->result : 0;
    s->from = 0;
    return r;
}

LRESULT SendMessage(HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    struct wnd *n = wnd_from(h);
    if (!n)
        return 0;
    if (wnd_mine(n))
        return n->proc(h, msg, w, l);
    return send_across(n, msg, w, l);
}

LRESULT CallWindowProc(WNDPROC proc, HWND h, unsigned msg, WPARAM w, LPARAM l)
{
    struct wnd *n = wnd_from(h);
    return proc && wnd_mine(n) ? proc(h, msg, w, l) : 0;
}

/* ---- timers ------------------------------------------------------------------ */

int SetTimer(HWND h, int id, uint32_t ms)
{
    struct wnd *w = wnd_from(h);
    if (!w || !wnd_mine(w) || !ms) return 0;
    int slot = -1;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (S->timers[i].w == w && S->timers[i].id == id) { slot = i; break; }
        if (!S->timers[i].w && slot < 0) slot = i;
    }
    if (slot < 0) return 0;
    S->timers[slot] = (struct timer){ w, id, ms, GetTickCount() + ms };
    return id ? id : 1;
}

bool KillTimer(HWND h, int id)
{
    for (int i = 0; i < TIMER_MAX; i++)
        if (S->timers[i].w == (struct wnd *)h && S->timers[i].id == id) {
            S->timers[i].w = NULL;
            return true;
        }
    return false;
}

/* Post due WM_TIMERs for our windows; -> ms until the next one is due. */
static uint32_t run_timers(void)
{
    uint32_t now = GetTickCount(), next = WAIT_INFINITE;
    for (int i = 0; i < TIMER_MAX; i++) {
        struct timer *t = &S->timers[i];
        if (!t->w || !wnd_mine(t->w))
            continue;
        if ((int32_t)(now - t->due) >= 0) {
            t->due = now + t->interval;
            bool pending = false;                   /* WM_TIMERs don't pile up */
            for (unsigned q = myq->head; q != myq->tail; q = (q + 1) % QMAX)
                if (myq->q[q].message == WM_TIMER && myq->q[q].hwnd == t->w &&
                    (int)myq->q[q].wParam == t->id)
                    pending = true;
            if (!pending)
                push(myq, t->w, WM_TIMER, (WPARAM)t->id, 0);
        }
        uint32_t left = t->due - now;
        if ((int32_t)left > 0 && left < next)
            next = left;
    }
    return next;
}

/* ---- processes that ended ------------------------------------------------------- */

void ui_cleanup_dead(void)
{
    uint32_t exits = GetProcessExitCount();
    if (exits == S->exits_seen)
        return;
    S->exits_seen = exits;
    for (int i = 0; i < WND_MAX; i++) {
        struct wnd *w = &S->wnds[i];
        if (w->magic == WND_MAGIC && !w->parent && !IsProcessAlive(w->pid))
            ui_forget_window(w);
    }
    for (int i = 0; i < PROC_MAX; i++)
        if (S->queues[i].pid && !IsProcessAlive(S->queues[i].pid)) {
            menu_process_gone(S->queues[i].pid);
            S->queues[i].pid = 0;
        }
    for (int i = 0; i < S->nclasses; i++)
        if (S->classes[i].pid && !IsProcessAlive(S->classes[i].pid))
            S->classes[i].name[0] = '\0';
    for (int i = 0; i < SEND_MAX; i++) {
        struct sent *s = &S->sends[i];
        if (s->from && !IsProcessAlive(s->from))
            s->from = 0;
        else if (s->from && !IsProcessAlive(s->to)) {
            s->done = true;
            WakeProcess(s->from);
        }
    }
    if (S->in.modal_pid && !IsProcessAlive(S->in.modal_pid)) {
        S->in.modal_pid = 0;
        S->in.modal_running = false;
        S->in.outline_on = false;
    }
    if (S->mt.owner && !wnd_from(S->mt.owner))
        memset(&S->mt, 0, sizeof S->mt);
}

/* ---- getting and dispatching messages ---------------------------------------------- */

static bool input_for_us(void)
{
    return !S->in.modal_pid || S->in.modal_pid == ui_pid;
}

void ui_reserve_input(int pid)
{
    S->in.modal_pid = pid;
    if (pid != ui_pid)
        WakeProcess(pid);
}

static uint32_t next_timer;

bool PeekMessage(MSG *m, bool remove)
{
    if (!myq)
        return false;
    ui_cleanup_dead();
    answer_sent();
    if (input_for_us())
        ui_pump_input();
    next_timer = run_timers();
    while (myq->head != myq->tail) {
        *m = myq->q[myq->head];
        if (m->message == WM_NULL && !m->hwnd) {     /* dropped */
            myq->head = (myq->head + 1) % QMAX;
            continue;
        }
        if (remove)
            myq->head = (myq->head + 1) % QMAX;
        return true;
    }
    if (ui_paint_pending())
        ui_paint();
    if (myq->quit) {
        *m = (MSG){ NULL, WM_QUIT, (WPARAM)myq->quit_code, 0, GetTickCount() };
        if (remove)
            myq->quit = false;
        return true;
    }
    return false;
}

bool GetMessage(MSG *m)
{
    while (!PeekMessage(m, true))
        ui_wait(next_timer, input_for_us());
    return m->message != WM_QUIT;
}

LRESULT DispatchMessage(const MSG *m)
{
    struct wnd *w = wnd_from(m->hwnd);
    LRESULT r = wnd_mine(w) ? w->proc(m->hwnd, m->message, m->wParam, m->lParam) : 0;
    if (S->in.modal_pid == ui_pid && !S->in.modal_running)
        S->in.modal_pid = 0;                    /* reserved input nobody used */
    return r;
}

bool TranslateMessage(const MSG *m)
{
    if (m->message != WM_KEYDOWN)
        return false;
    char ch = kbd_char((int)m->wParam, (m->lParam & (1 << 25)) != 0);
    if (!ch)
        return false;
    if ((m->lParam & (1 << 27)) && m->wParam >= 'A' && m->wParam <= 'Z')
        ch = (char)(m->wParam - 'A' + 1);           /* Ctrl+letter: a control code */
    return PostMessage(m->hwnd, WM_CHAR, (WPARAM)(uint8_t)ch, m->lParam);
}
