// This file is a part of Julia. License is MIT: https://julialang.org/license

// Windows
// Note that this file is `#include`d by "signal-handling.c"
#include <mmsystem.h> // hidden by LEAN_AND_MEAN

static const size_t sig_stack_size = 131072; // 128k reserved for backtrace_fiber for stack overflow handling

// Copied from MINGW_FLOAT_H which may not be found due to a collision with the builtin gcc float.h
// eventually we can probably integrate this into OpenLibm.
#if defined(_COMPILER_GCC_)
void __cdecl __MINGW_NOTHROW _fpreset (void);
void __cdecl __MINGW_NOTHROW fpreset (void);
#else
void __cdecl _fpreset (void);
void __cdecl fpreset (void);
#endif
#define _FPE_INVALID        0x81
#define _FPE_DENORMAL       0x82
#define _FPE_ZERODIVIDE     0x83
#define _FPE_OVERFLOW       0x84
#define _FPE_UNDERFLOW      0x85
#define _FPE_INEXACT        0x86
#define _FPE_UNEMULATED     0x87
#define _FPE_SQRTNEG        0x88
#define _FPE_STACKOVERFLOW  0x8a
#define _FPE_STACKUNDERFLOW 0x8b
#define _FPE_EXPLICITGEN    0x8c    /* raise( SIGFPE ); */

static char *strsignal(int sig)
{
    switch (sig) {
    case SIGINT:         return "SIGINT"; break;
    case SIGILL:         return "SIGILL"; break;
    case SIGABRT_COMPAT: return "SIGABRT_COMPAT"; break;
    case SIGFPE:         return "SIGFPE"; break;
    case SIGSEGV:        return "SIGSEGV"; break;
    case SIGTERM:        return "SIGTERM"; break;
    case SIGBREAK:       return "SIGBREAK"; break;
    case SIGABRT:        return "SIGABRT"; break;
    }
    return "?";
}

static void jl_try_throw_sigint(void)
{
    jl_task_t *ct = jl_current_task;
    jl_safepoint_enable_sigint();
    jl_wake_libuv();
    int force = jl_check_force_sigint();
    if (force || (!ct->ptls->defer_signal && ct->ptls->io_wait)) {
        jl_safepoint_consume_sigint();
        if (force)
            jl_safe_printf("WARNING: Force throwing a SIGINT\n");
        // Force a throw
        jl_clear_force_sigint();
        jl_throw(jl_interrupt_exception);
    }
}

void __cdecl crt_sig_handler(int sig, int num)
{
    CONTEXT Context;
    switch (sig) {
    case SIGFPE:
        fpreset();
        signal(SIGFPE, (void (__cdecl *)(int))crt_sig_handler);
        switch(num) {
        case _FPE_INVALID:
        case _FPE_OVERFLOW:
        case _FPE_UNDERFLOW:
        default:
            jl_errorf("Unexpected FPE Error 0x%X", num);
            break;
        case _FPE_ZERODIVIDE:
            jl_throw(jl_diverror_exception);
            break;
        }
        break;
    case SIGINT:
        signal(SIGINT, (void (__cdecl *)(int))crt_sig_handler);
        if (!jl_ignore_sigint()) {
            if (exit_on_sigint)
                jl_exit(130); // 128 + SIGINT
            jl_try_throw_sigint();
        }
        break;
    default: // SIGSEGV, SIGTERM, SIGILL, SIGABRT
        if (sig == SIGSEGV) { // restarting jl_ or profile
            jl_jmp_buf *saferestore = jl_get_safe_restore();
            if (saferestore) {
                signal(sig, (void (__cdecl *)(int))crt_sig_handler);
                jl_longjmp(*saferestore, 1);
                return;
            }
        }
        memset(&Context, 0, sizeof(Context));
        RtlCaptureContext(&Context);
        if (sig == SIGILL)
            jl_show_sigill(&Context);
        jl_critical_error(sig, 0, &Context, jl_get_current_task());
        raise(sig);
    }
}

// StackOverflowException needs extra stack space to record the backtrace
// so we keep one around, shared by all threads
static uv_mutex_t backtrace_lock;
static win32_ucontext_t collect_backtrace_fiber;
static win32_ucontext_t error_return_fiber;
static PCONTEXT stkerror_ctx;
static jl_ptls_t stkerror_ptls;
static int have_backtrace_fiber;
static void JL_NORETURN start_backtrace_fiber(void)
{
    // print the warning (this mysteriously needs a lot of stack for the WriteFile syscall)
    stack_overflow_warning();
    // collect the backtrace
    stkerror_ptls->bt_size =
        rec_backtrace_ctx(stkerror_ptls->bt_data, JL_MAX_BT_SIZE, stkerror_ctx,
                          NULL /*current_task?*/);
    // switch back to the execution fiber
    jl_setcontext(&error_return_fiber);
    abort();
}

void restore_signals(void)
{
    // turn on ctrl-c handler
    SetConsoleCtrlHandler(NULL, 0);
}

int jl_simulate_longjmp(jl_jmp_buf mctx, bt_context_t *c);

static void jl_throw_in_ctx(jl_task_t *ct, jl_value_t *excpt, PCONTEXT ctxThread)
{
    jl_jmp_buf *saferestore = jl_get_safe_restore();
    if (saferestore) { // restarting jl_ or profile
        if (!jl_simulate_longjmp(*saferestore, ctxThread))
            abort();
        return;
    }
    assert(ct && excpt);
    jl_ptls_t ptls = ct->ptls;
    ptls->bt_size = 0;
    if (excpt != jl_stackovf_exception) {
        ptls->bt_size = rec_backtrace_ctx(ptls->bt_data, JL_MAX_BT_SIZE, ctxThread,
                                          ct->gcstack);
    }
    else if (have_backtrace_fiber) {
        uv_mutex_lock(&backtrace_lock);
        stkerror_ctx = ctxThread;
        stkerror_ptls = ptls;
        jl_swapcontext(&error_return_fiber, &collect_backtrace_fiber);
        uv_mutex_unlock(&backtrace_lock);
    }
    ptls->sig_exception = excpt;
    ptls->io_wait = 0;
    jl_handler_t *eh = ct->eh;
    if (eh != NULL) {
        asan_unpoison_task_stack(ct, &eh->eh_ctx);
        if (!jl_simulate_longjmp(eh->eh_ctx, ctxThread))
            abort();
    }
    else {
        jl_no_exc_handler(excpt, ct);
    }
}

HANDLE hMainThread = INVALID_HANDLE_VALUE;

// Try to throw the exception in the master thread.
static void jl_try_deliver_sigint(void)
{
    jl_ptls_t ptls2 = jl_atomic_load_relaxed(&jl_all_tls_states)[0];
    jl_lock_profile();
    jl_safepoint_enable_sigint();
    jl_wake_libuv();
    if ((DWORD)-1 == SuspendThread(hMainThread)) {
        // error
        jl_safe_printf("error: SuspendThread failed\n");
        jl_unlock_profile();
        return;
    }
    jl_unlock_profile();
    int force = jl_check_force_sigint();
    if (force || (!ptls2->defer_signal && ptls2->io_wait)) {
        jl_safepoint_consume_sigint();
        if (force)
            jl_safe_printf("WARNING: Force throwing a SIGINT\n");
        // Force a throw
        jl_clear_force_sigint();
        CONTEXT ctxThread;
        memset(&ctxThread, 0, sizeof(CONTEXT));
        ctxThread.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!GetThreadContext(hMainThread, &ctxThread)) {
            // error
            jl_safe_printf("error: GetThreadContext failed\n");
            return;
        }
        jl_task_t *ct = jl_atomic_load_relaxed(&ptls2->current_task);
        jl_throw_in_ctx(ct, jl_interrupt_exception, &ctxThread);
        ctxThread.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!SetThreadContext(hMainThread, &ctxThread)) {
            jl_safe_printf("error: SetThreadContext failed\n");
            // error
            return;
        }
    }
    if ((DWORD)-1 == ResumeThread(hMainThread)) {
        jl_safe_printf("error: ResumeThread failed\n");
        // error
        return;
    }
}

static BOOL WINAPI sigint_handler(DWORD wsig) //This needs winapi types to guarantee __stdcall
{
    int sig;
    //windows signals use different numbers from unix (raise)
    switch(wsig) {
        case CTRL_C_EVENT: sig = SIGINT; break;
        //case CTRL_BREAK_EVENT: sig = SIGTERM; break;
        // etc.
        default: sig = SIGTERM; break;
    }
    if (!jl_ignore_sigint()) {
        if (exit_on_sigint)
            jl_exit(128 + sig); // 128 + SIGINT
        jl_try_deliver_sigint();
    }
    return 1;
}

LONG WINAPI jl_exception_handler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
    if (ExceptionInfo->ExceptionRecord->ExceptionFlags != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    jl_task_t *ct = jl_get_current_task();
    if (ct != NULL && ct->ptls != NULL && ct->ptls->gc_state != JL_GC_STATE_WAITING) {
        jl_ptls_t ptls = ct->ptls;
        switch (ExceptionInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            if (ct->eh != NULL) {
                fpreset();
                jl_throw_in_ctx(ct, jl_diverror_exception, ExceptionInfo->ContextRecord);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            break;
        case EXCEPTION_STACK_OVERFLOW:
            if (ct->eh != NULL) {
                ptls->needs_resetstkoflw = 1;
                jl_throw_in_ctx(ct, jl_stackovf_exception, ExceptionInfo->ContextRecord);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            break;
        case EXCEPTION_ACCESS_VIOLATION:
            if (jl_addr_is_safepoint(ExceptionInfo->ExceptionRecord->ExceptionInformation[1])) {
                jl_set_gc_and_wait(ct);
                // Do not raise sigint on worker thread
                if (ptls->tid != 0)
                    return EXCEPTION_CONTINUE_EXECUTION;
                if (ptls->defer_signal) {
                    jl_safepoint_defer_sigint();
                }
                else if (jl_safepoint_consume_sigint()) {
                    jl_clear_force_sigint();
                    jl_throw_in_ctx(ct, jl_interrupt_exception, ExceptionInfo->ContextRecord);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (jl_get_safe_restore()) {
                jl_throw_in_ctx(NULL, NULL, ExceptionInfo->ContextRecord);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (ct->eh != NULL) {
                if (ExceptionInfo->ExceptionRecord->ExceptionInformation[0] == 1) { // writing to read-only memory (e.g. mmap)
                    jl_throw_in_ctx(ct, jl_readonlymemory_exception, ExceptionInfo->ContextRecord);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        default:
            break;
        }
    }
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        jl_safe_printf("\n");
        jl_show_sigill(ExceptionInfo->ContextRecord);
    }
    jl_safe_printf("\nPlease submit a bug report with steps to reproduce this fault, and any error messages that follow (in their entirety). Thanks.\nException: ");
    switch (ExceptionInfo->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        jl_safe_printf("EXCEPTION_ACCESS_VIOLATION"); break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        jl_safe_printf("EXCEPTION_ARRAY_BOUNDS_EXCEEDED"); break;
    case EXCEPTION_BREAKPOINT:
        jl_safe_printf("EXCEPTION_BREAKPOINT"); break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        jl_safe_printf("EXCEPTION_DATATYPE_MISALIGNMENT"); break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        jl_safe_printf("EXCEPTION_FLT_DENORMAL_OPERAND"); break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        jl_safe_printf("EXCEPTION_FLT_DIVIDE_BY_ZERO"); break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        jl_safe_printf("EXCEPTION_FLT_INEXACT_RESULT"); break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        jl_safe_printf("EXCEPTION_FLT_INVALID_OPERATION"); break;
    case EXCEPTION_FLT_OVERFLOW:
        jl_safe_printf("EXCEPTION_FLT_OVERFLOW"); break;
    case EXCEPTION_FLT_STACK_CHECK:
        jl_safe_printf("EXCEPTION_FLT_STACK_CHECK"); break;
    case EXCEPTION_FLT_UNDERFLOW:
        jl_safe_printf("EXCEPTION_FLT_UNDERFLOW"); break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        jl_safe_printf("EXCEPTION_ILLEGAL_INSTRUCTION"); break;
    case EXCEPTION_IN_PAGE_ERROR:
        jl_safe_printf("EXCEPTION_IN_PAGE_ERROR"); break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        jl_safe_printf("EXCEPTION_INT_DIVIDE_BY_ZERO"); break;
    case EXCEPTION_INT_OVERFLOW:
        jl_safe_printf("EXCEPTION_INT_OVERFLOW"); break;
    case EXCEPTION_INVALID_DISPOSITION:
        jl_safe_printf("EXCEPTION_INVALID_DISPOSITION"); break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        jl_safe_printf("EXCEPTION_NONCONTINUABLE_EXCEPTION"); break;
    case EXCEPTION_PRIV_INSTRUCTION:
        jl_safe_printf("EXCEPTION_PRIV_INSTRUCTION"); break;
    case EXCEPTION_SINGLE_STEP:
        jl_safe_printf("EXCEPTION_SINGLE_STEP"); break;
    case EXCEPTION_STACK_OVERFLOW:
        jl_safe_printf("EXCEPTION_STACK_OVERFLOW"); break;
    default:
        jl_safe_printf("UNKNOWN"); break;
    }
    jl_safe_printf(" at 0x%zx -- ", (size_t)ExceptionInfo->ExceptionRecord->ExceptionAddress);
    jl_print_native_codeloc((uintptr_t)ExceptionInfo->ExceptionRecord->ExceptionAddress);

    jl_critical_error(0, 0, ExceptionInfo->ContextRecord, ct);
    static int recursion = 0;
    if (recursion++)
        exit(1);
    else
        jl_exit(1);
}

JL_DLLEXPORT void jl_install_sigint_handler(void)
{
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)sigint_handler,1);
}

static volatile HANDLE hBtThread = 0;

int jl_thread_suspend_and_get_state(int tid, int timeout, bt_context_t *ctx)
{
    (void)timeout;
    jl_ptls_t ptls2 = jl_atomic_load_relaxed(&jl_all_tls_states)[tid];
    if (ptls2 == NULL) // this thread is not alive
        return 0;
    jl_task_t *ct2 = jl_atomic_load_relaxed(&ptls2->current_task);
    if (ct2 == NULL) // this thread is already dead
        return 0;
    HANDLE hThread = ptls2->system_id;
    if ((DWORD)-1 == SuspendThread(hThread))
        return 0;
    assert(sizeof(*ctx) == sizeof(CONTEXT));
    memset(ctx, 0, sizeof(CONTEXT));
    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(hThread, ctx)) {
        if ((DWORD)-1 == ResumeThread(hThread))
            abort();
        return 0;
    }
    return 1;
}

void jl_thread_resume(int tid)
{
    jl_ptls_t ptls2 = jl_atomic_load_relaxed(&jl_all_tls_states)[tid];
    HANDLE hThread = ptls2->system_id;
    if ((DWORD)-1 == ResumeThread(hThread)) {
        fputs("failed to resume main thread! aborting.", stderr);
        abort();
    }
}

void jl_lock_stackwalk(void)
{
    uv_mutex_lock(&jl_in_stackwalk);
    jl_lock_profile();
}

void jl_unlock_stackwalk(void)
{
    jl_unlock_profile();
    uv_mutex_unlock(&jl_in_stackwalk);
}

void jl_with_stackwalk_lock(void (*f)(void*), void *ctx)
{
    jl_lock_stackwalk();
    f(ctx);
    jl_unlock_stackwalk();
}


static DWORD WINAPI profile_bt( LPVOID lparam )
{
    // Note: illegal to use jl_* functions from this thread except for profiling-specific functions
    while (1) {
        DWORD timeout_ms = nsecprof / (GIGA / 1000);
        Sleep(timeout_ms > 0 ? timeout_ms : 1);
        if (profile_running) {
            if (jl_profile_is_buffer_full()) {
                jl_profile_stop_timer(); // does not change the thread state
                SuspendThread(GetCurrentThread());
                continue;
            }
            else if (profile_all_tasks) {
                // Don't take the stackwalk lock here since it's already taken in `jl_rec_backtrace`
                jl_profile_task();
            }
            else {
                // TODO: bring this up to parity with other OS by adding loop over tid here
                jl_lock_stackwalk();
                CONTEXT ctxThread;
                if (!jl_thread_suspend_and_get_state(0, 0, &ctxThread)) {
                    jl_unlock_stackwalk();
                    fputs("failed to suspend main thread. aborting profiling.", stderr);
                    jl_profile_stop_timer();
                    break;
                }
                // Get backtrace data
                profile_bt_size_cur += rec_backtrace_ctx((jl_bt_element_t*)profile_bt_data_prof + profile_bt_size_cur,
                        profile_bt_size_max - profile_bt_size_cur - 1, &ctxThread, NULL);

                jl_ptls_t ptls = jl_atomic_load_relaxed(&jl_all_tls_states)[0]; // given only profiling hMainThread

                // META_OFFSET_THREADID store threadid but add 1 as 0 is preserved to indicate end of block
                profile_bt_data_prof[profile_bt_size_cur++].uintptr = ptls->tid + 1;

                // META_OFFSET_TASKID store task id (never null)
                profile_bt_data_prof[profile_bt_size_cur++].jlvalue = (jl_value_t*)jl_atomic_load_relaxed(&ptls->current_task);

                // META_OFFSET_CPUCYCLECLOCK store cpu cycle clock
                profile_bt_data_prof[profile_bt_size_cur++].uintptr = cycleclock();

                // store whether thread is sleeping (don't ever encode a state as `0` since is preserved to indicate end of block)
                int state = jl_atomic_load_relaxed(&ptls->sleep_check_state) == 0 ? PROFILE_STATE_THREAD_NOT_SLEEPING : PROFILE_STATE_THREAD_SLEEPING;
                profile_bt_data_prof[profile_bt_size_cur++].uintptr = state;

                // Mark the end of this block with two 0's
                profile_bt_data_prof[profile_bt_size_cur++].uintptr = 0;
                profile_bt_data_prof[profile_bt_size_cur++].uintptr = 0;
                jl_unlock_stackwalk();
                jl_thread_resume(0);
                jl_check_profile_autostop();
            }
        }
    }
    uv_mutex_unlock(&jl_in_stackwalk);
    jl_profile_stop_timer();
    hBtThread = NULL;
    return 0;
}

static volatile TIMECAPS timecaps;

JL_DLLEXPORT int jl_profile_start_timer(uint8_t all_tasks)
{
    if (hBtThread == NULL) {

        TIMECAPS _timecaps;
        if (MMSYSERR_NOERROR != timeGetDevCaps(&_timecaps, sizeof(_timecaps))) {
            fputs("failed to get timer resolution", stderr);
            return -2;
        }
        timecaps = _timecaps;

        hBtThread = CreateThread(
            NULL,                   // default security attributes
            0,                      // use default stack size
            profile_bt,             // thread function name
            0,                      // argument to thread function
            0,                      // use default creation flags
            0);                     // returns the thread identifier
        if (hBtThread == NULL)
            return -1;
        (void)SetThreadPriority(hBtThread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    else {
        if ((DWORD)-1 == ResumeThread(hBtThread)) {
            fputs("failed to resume profiling thread.", stderr);
            return -2;
        }
    }
    if (profile_running == 0) {
        // Failure to change the timer resolution is not fatal. However, it is important to
        // ensure that the timeBeginPeriod/timeEndPeriod is paired.
        if (TIMERR_NOERROR != timeBeginPeriod(timecaps.wPeriodMin))
            timecaps.wPeriodMin = 0;
    }
    profile_all_tasks = all_tasks;
    profile_running = 1; // set `profile_running` finally
    return 0;
}
JL_DLLEXPORT void jl_profile_stop_timer(void)
{
    uv_mutex_lock(&bt_data_prof_lock);
    if (profile_running && timecaps.wPeriodMin)
        timeEndPeriod(timecaps.wPeriodMin);
    profile_running = 0;
    profile_all_tasks = 0;
    uv_mutex_unlock(&bt_data_prof_lock);
}

void jl_install_default_signal_handlers(void)
{
    if (signal(SIGFPE, (void (__cdecl *)(int))crt_sig_handler) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGFPE");
    }
    if (signal(SIGILL, (void (__cdecl *)(int))crt_sig_handler) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGILL");
    }
    if (signal(SIGINT, (void (__cdecl *)(int))crt_sig_handler) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGINT");
    }
    if (signal(SIGSEGV, (void (__cdecl *)(int))crt_sig_handler) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGSEGV");
    }
    if (signal(SIGTERM, (void (__cdecl *)(int))crt_sig_handler) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGTERM");
    }
    if (signal(SIGABRT, (void (__cdecl *)(int))crt_sig_handler) == SIG_ERR) {
        jl_error("fatal error: Couldn't set SIGABRT");
    }
    SetUnhandledExceptionFilter(jl_exception_handler);
}

void jl_install_thread_signal_handler(jl_ptls_t ptls)
{
    if (!have_backtrace_fiber) {
        size_t ssize = sig_stack_size;
        void *stk = jl_malloc_stack(&ssize, NULL);
        if (stk == NULL)
            jl_errorf("fatal error allocating signal stack: mmap: %s", strerror(errno));
        collect_backtrace_fiber.uc_stack.ss_sp = (void*)stk;
        collect_backtrace_fiber.uc_stack.ss_size = ssize;
        jl_makecontext(&collect_backtrace_fiber, start_backtrace_fiber);
        uv_mutex_init(&backtrace_lock);
        have_backtrace_fiber = 1;
    }
}
