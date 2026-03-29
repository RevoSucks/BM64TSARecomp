// process.c
#include "patches.h"
//#include "process.h"
#include "process_funcs.h"

// memory externs
extern void errstop(const char *fmt, ...);
extern s32 GetMemoryAllocSize(s32);
extern void InitMemory(void *, s32);
extern void *AllocMemory(void *, s32);
extern void *malloc(s32);
extern s32 getsp();
extern void FreeMemory(void*);
extern void free(void*);

// process.c

typedef struct jump_buf
{
    u32* sp;
    void *func;
    u32 regs[21];
} jmp_buf;

typedef struct HeapNode {
    /* 0x00 */ s32 size;
    /* 0x04 */ u8 heap_constant;
    /* 0x05 */ u8 active;
    /* 0x08 */ struct HeapNode* prev;
    /* 0x0C */ struct HeapNode* next;
} HeapNode;

typedef void (*process_func)();

#define EXEC_PROCESS_DEFAULT 0
#define EXEC_PROCESS_SLEEPING 1
#define EXEC_PROCESS_WATCH 2
#define EXEC_PROCESS_DEAD 3
#define EXEC_PROCESS_UNK4 4 // paused?

typedef struct Process {
    /*0x00*/ struct Process *next;
    /*0x04*/ struct Process *youngest_child;
    /*0x08*/ struct Process *oldest_child;
    /*0x0C*/ struct Process *relative;
    /*0x10*/ struct Process *parent_oldest_child;
    /*0x14*/ struct Process *new_process;
    /*0x18*/ void *heap;
    /*0x1C*/ u32 stat;
    /*0x20*/ process_func func;
    /*0x24*/ u32 exec_mode;
    /*0x28*/ u16 priority;
    char filler2A[0x2];
    /*0x2C*/ s32 sleep_time;
    /*0x30*/ void *base_sp;

    // Replacing jmpBuf (was 0x5C bytes) with coroutine state
    /*0x34*/ s32 coro_created;    // Has native coroutine been created?
    /*0x38*/ s32 yield_value;     // Value passed during yield/resume
    /*0x3C*/ int id;
    /*0x40*/ s32 stack_size;
    /*0x44*/ u8 _padding[0x4C];   // Maintain struct layout

    /*0x90*/ process_func destructor;
} Process; // size:0x94

extern void *process_jmp_buf;
extern Process *top_process;
extern Process *current_process;
extern u16 process_count;

extern void *D_800A59C8;

// FUNCTIONS

void InitProcess(void);
Process *CreateChildProcess(process_func func, u16 priority, s32 stack_size, s32 extra_data_size, Process *root);
Process* CreateProcess(process_func func, u16 priority, u32 stack_size, s32 extra_data_size);
void LinkProcess(Process** root, Process* process); // INLINE
void LinkChildProcess(Process* root, Process *child);
void WatchChildProcess(void);
void EndProcess(void);
void TerminateProcess(Process *process); // INLINE
void UnlinkProcess(Process *process); // INLINE
void SleepVProcess(void);
void SleepProcess(s32 time);
void SetProcessDestruct(void *destructor_func);
void CallProcess(s32 time);
void AllocProcessMemory(s32 size);
Process* GetCurrentProcess(void);
void FreeProcessMemory(void *ptr);
void SleepPrioProcess(u16 priority_min, u16 priority_max, s32 time);
void SleepProcessP(Process* process, s32 time);
void KillPrioProcess(u16 arg0, u16 arg1); // REQUIRES O3 ABI
void KillProcess(Process *process);
void KillChildProcess(Process* process); // this cannot be static, there is a table that links against this
void UnlinkChildProcess(Process* process);
s32 SetKillStatusProcess(Process *process);
void WakeupPrioProcess(u16 priority_min, u16 priority_max);
void WakeupProcess(Process* process);
void SetProcessCheck(void);
void CheckProcessStruct(void);
void CheckProcessStackBroken(void);
s32 CheckProcessStack(void);
s32 GetProcessStackR(void);

static s32 yield_to_scheduler(s32 reason) {
    GetCurrentProcess()->yield_value = reason;
    return recomp_process_yield(reason);
}

#define MAX_PROC_IDS 256

u8 allocated_process_ids[MAX_PROC_IDS] = {0};

static int allocate_process_id(void) {
    for(int i = 0; i < MAX_PROC_IDS; i++) {
        if (allocated_process_ids[i] == 0) {
            // this ID is free.
            allocated_process_ids[i] = 1;
            return i;
        }
    }
    return 0; // uh oh
}

static void free_process_id(int i) {
    allocated_process_ids[i] = 0; // mark as free
}

RECOMP_PATCH void InitProcess(void) {
    recomp_process_init();

    process_count = 0;
    top_process = 0;
}

// INLINE
RECOMP_PATCH void LinkProcess(Process** root, Process* process) {
    Process* src_process = *root;
    if (src_process != NULL && (src_process->priority >= process->priority)) {
        while (src_process->next != NULL && src_process->next->priority >= process->priority) {
            src_process = src_process->next;
        }

        process->next = src_process->next;
        process->youngest_child = src_process;
        src_process->next = process;
        if (process->next) {
            process->next->youngest_child = process;
        }
    } else {
        process->next = (*root);
        process->youngest_child = NULL;
        *root = process;
        if (src_process != NULL) {
            src_process->youngest_child = process;
        }
    }
    process->oldest_child = NULL;
    process->relative = NULL;
}

RECOMP_PATCH void UnlinkProcess(Process *process) {
    if (process->next) {
        process->next->youngest_child = process->youngest_child;
    }

    if (process->youngest_child) {
        process->youngest_child->next = process->next;
    } else {
        top_process = process->next;
    }
}

static int total_created_processes = 0;

RECOMP_PATCH Process* CreateProcess(process_func func, u16 priority, u32 stack_size, s32 extra_data_size) {
    s32 alloc_size;
    HeapNode *process_heap;
    Process* process;

    if (stack_size == 0) {
        stack_size = 0x1000;
    } else if (stack_size < 0x2000) {
        stack_size = 0x2000;
    }

    alloc_size = GetMemoryAllocSize(sizeof(Process)) + GetMemoryAllocSize(stack_size) + extra_data_size;

    process_heap = (HeapNode *)malloc(alloc_size);
    if (process_heap == NULL) {
        errstop("process : create malloc error\n");
    }
    InitMemory(process_heap, alloc_size);

    process = (Process*)AllocMemory(process_heap, sizeof(Process));
    process->heap = process_heap;
    process->stat = 0;
    process->exec_mode = EXEC_PROCESS_DEFAULT;
    process->priority = priority;
    process->sleep_time = 0;
    process->base_sp = AllocMemory(process_heap, stack_size);
    process->stack_size = stack_size;
    ((s32*)process->base_sp)[0] = 0xDBDB7272;
    //process->prc_jump.func = func;
    process->func = func;
    //process->prc_jump.sp = (u32 *)process->base_sp + stack_size - 8;
    process->destructor = NULL;
    process->id = allocate_process_id();
    LinkProcess(&top_process, process);

    // Native coroutine will be created lazily when first scheduled
    process->coro_created = 0;
    process->yield_value = 0;

    process_count++;
    return process;
}

RECOMP_PATCH void LinkChildProcess(Process* root, Process *child) {
    UnlinkChildProcess(child);

    if (root->oldest_child != NULL) {
        root->oldest_child->new_process = child;
    }
    child->parent_oldest_child = root->oldest_child;
    child->new_process = NULL;
    root->oldest_child = child;
    child->relative = root;
}

RECOMP_PATCH void UnlinkChildProcess(Process* process) {
    if (process->relative != NULL) {
        if (process->parent_oldest_child != NULL) {
            process->parent_oldest_child->new_process = process->new_process;
        }
        if (process->new_process != NULL) {
            process->new_process->parent_oldest_child = process->parent_oldest_child;
        } else {
            process->relative->oldest_child = process->parent_oldest_child;
        }
        process->relative = NULL;
    }
}

RECOMP_PATCH Process *CreateChildProcess(process_func func, u16 priority, s32 stack_size, s32 extra_data_size, Process *root) {
    Process *proc = CreateProcess(func, priority, stack_size, extra_data_size);

    LinkChildProcess(root, proc);
    return proc;
}

RECOMP_PATCH void WatchChildProcess(void) {
    Process* process = GetCurrentProcess();
    if (process->oldest_child) {
        process->exec_mode = EXEC_PROCESS_WATCH;
        // TODO: setjmp/longjmp
        /*
        if (!setjmp(&process->prc_jump)) {
            longjmp(&process_jmp_buf, 1);
        }
        */
    }
}

RECOMP_PATCH Process* GetCurrentProcess(void) {
    return current_process;
}

RECOMP_PATCH void KillChildProcess(Process* process) {
    Process* child_process = process->oldest_child;

    for(child_process = process->oldest_child; child_process; child_process = child_process->parent_oldest_child) {
        if (child_process->oldest_child != 0) {
            KillChildProcess(child_process);
        }
        SetKillStatusProcess(child_process);
    }
    process->oldest_child = NULL;
}

RECOMP_PATCH void TerminateProcess(Process *process) {
    if (process->destructor) {
        process->destructor();
    }

    s32 terminated_self = (process == GetCurrentProcess()); // i think this is correct...

    // Destroy the native coroutine if it was created
    // (but not our own while we're still in it)
    if (process->coro_created && !terminated_self) {
        recomp_process_coro_destroy(process->id);
    }
    process->coro_created = 0;

    // Update process state
    process->exec_mode = EXEC_PROCESS_DEAD;

    free_process_id(process->id);

    UnlinkProcess(process);
    process_count--;

    // If we terminated ourselves, yield back to scheduler
    if (terminated_self) {
        //D_802AC344 = prev;
        yield_to_scheduler(YIELD_TERMINATE);
        // Should never return here
    }

    //longjmp(&process_jmp_buf, 2);
}

RECOMP_PATCH void EndProcess(void) {
    Process *process = GetCurrentProcess();

    KillChildProcess(process);
    UnlinkChildProcess(process);
    TerminateProcess(process);
}

RECOMP_PATCH void SleepProcess(s32 time) {
    Process* process = GetCurrentProcess();
    int res;
    jmp_buf *jmp;

    if (time != 0 && process->exec_mode != EXEC_PROCESS_DEAD) {
        process->exec_mode = EXEC_PROCESS_SLEEPING;
        process->sleep_time = time;
    }

    //jmp = &process->prc_jump;
    // TODO setjmp
    //res = setjmp(jmp);

    if (!res) {
        //longjmp(&process_jmp_buf, 1);
    }
}

RECOMP_PATCH void SleepVProcess(void) {
    SleepProcess(0);
}

RECOMP_PATCH void SetProcessDestruct(void *destructor_func) {
    Process *process = GetCurrentProcess();
    process->destructor = destructor_func;
}

RECOMP_PATCH void CallProcess(s32 time) {
    Process* cur_proc_local;
    s32 ret;
    s32 yield_reason;

    current_process = top_process;
    //ret = setjmp(&process_jmp_buf); TODO

    while (1)
    {
        switch (ret)
        {
            case 2:
                free(current_process->heap);
            case 1:
                if (((u8*)current_process->heap)[4] != 0xA5) {
                    //errstop("stack overlap error.(process pointer %x)\n", current_process);
                    cur_proc_local = current_process;
                }
                current_process = current_process->next;
                break;
        }

        cur_proc_local = current_process;
        if (cur_proc_local == 0) {
            break;
        }

        switch (cur_proc_local->exec_mode){
            case EXEC_PROCESS_UNK4:
                ret = 1;
                break;
            case EXEC_PROCESS_SLEEPING:
                if (cur_proc_local->sleep_time > 0 && (cur_proc_local->sleep_time -= time) <= 0) {
                    cur_proc_local->sleep_time = 0;
                    cur_proc_local->exec_mode = EXEC_PROCESS_DEFAULT;
                }
                ret = 1;
                break;

            case EXEC_PROCESS_WATCH:
                if (cur_proc_local->oldest_child != 0) {
                    ret = 1;
                }
                else {
                    cur_proc_local->exec_mode = EXEC_PROCESS_DEFAULT;
                    ret = 0;
                }
                break;

            case EXEC_PROCESS_DEAD:
                //cur_proc_local->prc_jump.func = EndProcess;

            case EXEC_PROCESS_DEFAULT:
                // Create native coroutine on first run
                if (!cur_proc_local->coro_created) {
                    // Calculate initial MIPS stack pointer (stack grows downward)
                    // sp should point to the bottom of the stack area (highest address)
                    u32 mips_sp = (u32)cur_proc_local->base_sp + cur_proc_local->stack_size;
            
                    recomp_process_coro_create(
                        cur_proc_local->id,
                        (u32)cur_proc_local->func,
                        cur_proc_local->stack_size,
                        mips_sp
                    );
                    cur_proc_local->coro_created = 1;
                }

                // Switch to this process and wait for it to yield
                yield_reason = recomp_process_switch_to(cur_proc_local->id, 1);

                // Handle special yield reasons
                if (yield_reason == YIELD_STACKCHECK) {
                    // Process requested a stack check
                    s32 ret = CheckProcessStack();
                    if (ret == 0) {
                        ret = -1;
                    }
                    // Store result and resume process
                    cur_proc_local->yield_value = ret;
                    yield_reason = recomp_process_switch_to(cur_proc_local->id, ret);
                }
                // TODO
                //longjmp(&cur_proc_local->prc_jump, 1);
                break;
        }
    }
}

RECOMP_PATCH void AllocProcessMemory(s32 size) {
    Process *process = GetCurrentProcess();

    AllocMemory(process->heap, size);
}

RECOMP_PATCH void FreeProcessMemory(void *ptr) {
    FreeMemory(ptr);
}

RECOMP_PATCH void SleepPrioProcess(u16 priority_min, u16 priority_max, s32 time) {
    Process *process;

    for(process = top_process; process; process = process->next) {
        if (process->priority >= priority_min && priority_max >= process->priority && time && process->exec_mode != EXEC_PROCESS_DEAD) {
            if (time == -1) {
                process->exec_mode = EXEC_PROCESS_UNK4;
            } else {
                process->exec_mode = EXEC_PROCESS_SLEEPING;
                process->sleep_time = time;
            }
        }
    }
}

RECOMP_PATCH void SleepProcessP(Process* process, s32 time) {
    if ((time != 0) && (process->exec_mode != 3)) {
        if (time == -1) {
            process->exec_mode = 4;
            return;
        }
        process->exec_mode = 1;
        process->sleep_time = time;
    }
}

RECOMP_PATCH void KillPrioProcess(u16 arg0, u16 arg1) {
    Process* process = top_process;

    for(process = top_process; process; process = process->next) {
        if (process->priority >= arg0 && arg1 >= process->priority) {
            KillChildProcess(process);
            UnlinkChildProcess(process);
            SetKillStatusProcess(process);
        }
    }
}

RECOMP_PATCH void KillProcess(Process *process) {
    KillChildProcess(process);
    UnlinkChildProcess(process);
    SetKillStatusProcess(process);
}

RECOMP_PATCH s32 SetKillStatusProcess(Process *process) {
    if (process->exec_mode != 3) {
        WakeupProcess(process);
        process->exec_mode = 3;
        return 0;
    }
    return -1;
}

RECOMP_PATCH void WakeupPrioProcess(u16 priority_min, u16 priority_max) {
    Process* process = top_process;

    for(process = top_process; process; process = process->next) {
        if (process->priority >= priority_min && priority_max >= process->priority) {
            WakeupProcess(process);
        }
    }
}

RECOMP_PATCH void WakeupProcess(Process* process) {
    if (process->exec_mode == 1) {
        process->sleep_time = 0;
        return;
    }
    if (process->exec_mode == 4) {
        if (process->sleep_time != 0) {
            process->exec_mode = 1;
            return;
        }
        process->exec_mode = 0;
    }
}

RECOMP_PATCH void SetProcessCheck(void) {
    memcpy(&D_800A59C8, current_process, sizeof(Process));
}

RECOMP_PATCH void CheckProcessStruct(void) {
    u8 *var_s0 = &D_800A59C8;
    u8 *cur_process = (u8*)current_process; // didnt use GetCurrentProcess....
    int i;

    for(i = 0; i < sizeof(Process); i++) {
        if ((*var_s0++) != (*cur_process++)) {
            errstop("Process : Structure compare failed\n");
        }
    }
}

RECOMP_PATCH void CheckProcessStackBroken(void) {

}

RECOMP_PATCH s32 CheckProcessStack(void) {
    if ((getsp() - (s32)current_process->base_sp) < 5) {
        return 1;
    }
    return 0;
}

RECOMP_PATCH s32 GetProcessStackR(void) {
    return getsp() - (s32)current_process->base_sp;
}
