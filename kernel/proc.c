// For studying purpose, @driedoutjerky has put comments with `// *`.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// * Array of cpus
struct cpu cpus[NCPU]; 

// * Array of process list.
struct proc proc[NPROC];

// * Babysitter for children who don't have parents.
struct proc *initproc;

// * Next pid to be assigned.
int nextpid = 1;

// * Lock for allocation of pid. 
struct spinlock pid_lock;
extern uint TIME_SLICE_UNIT; 

// TODO: Project 2, weight table

static int nice_weights[] = {
[0]    88761,
[5]      29154,
[10]  9548,
[15]  3121,
[20]   1024,
[25]    335,
[30]   110,
[35]    35,
};

// Project 03: setoff() from file.c
extern int setoff(struct file *f, int off);

// * Starting point address for initialized processes that never got switched before.
// * This is defined further down in the code, however declared here for usage in different functions. 
extern void forkret(void);

// * Frees process p. 
static void freeproc(struct proc *p);

// * Bridge between user mode and kernel mode. 
// * Contains  start address of the trampoline code page. 
extern char trampoline[]; // trampoline.S

// * PROJECT_01 meminfo()
// * Implemented in kalloc.c
extern int kfreemem(void);

// * PROJECT_03 mmap()
extern void* kmmap(uint64  addr,int length);

extern void eligible_check(void);

// * But can't we implement meminfo() in proc.c rather having original code in kalloc.c?
// * We'll track the used page count here.
// * HOWEVER, we can't count the page here, because this counter only
// * accounts for page usage in proc.c, not considering other global page states.
// * Therefore we discard this method of counting emptyPage in proc.c
// * emptyPage = 0

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.

void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    // * emptyPage++;
  }
}

// initialize the proc table.

// * Initializes...
// * 1. `pid_lock`
// * 2. `wait_lock`
// * For every process slot:
// * 1. Initializes `p->lock`
// * 2. Sets `p->state = UNUSED`
// * 3. Precomputes `p->kstack`
// * So table exists, every slot has its own lock, and all are initially free.

void
procinit(void)
{

  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.

// * CPU ID comes from `tp` via `r_tp()`
// * xv6 keeps each CPU's hart ID in the `tp` register. 
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.

// * If timer interrupt caused migration to another CPU while using
// * previously returned CPU pointer, it could become stale. 

struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.

// * 1. `push_off()`: Disables interrupts
// * 2. Get current CPU via `mycpu()` and reads `c->proc`
// * 3. `pop_off()`: Enables interrupts
// * 4. Returns the current process pointer. 

struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// * PID allocation

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.

// * Find a free process slot(i.e. state is UNUSED)
// * 1. Looping through proc[]
// * 2. Acquires `p->lock` for each loop and checks whether
// * `p->state == UNUSED`
// * 3. If not, release lock and repeat step 1. 
// * 4. When found free process slot, jumps to `found:`

static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // for getnice(), initialize the value of nice 
  p->nice = 20;
  // TODO: Initialize the value for EEVDF in project 02
  p->runtime = 0; 
  p->vruntime = 0; 
  p->vdeadline = 0;
  p->timeslice = 5;
  p->is_eligible = 1; 

  p->proc_start_ticks = ticks;
  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    //emptyPage--;
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.

// * this does NOT free the kernel stack.
// * QUESTION: It doesn't need to clear out kernel stack necessarily then?
static void
freeproc(struct proc *p)
{
  if(p->trapframe){
    kfree((void*)p->trapframe);
    // * emptyPage++; 
  }
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.

// * A space for user memory, however it's not declared empty, but puts trampoline and trapframe code. 

pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.

// * This creates `initproc` and this CANNOT be exited.

void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.

// * Resizes the current process's user address space.

int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) { // * The process is not allowed to go beyond into the trapframe/trampoline region.
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){ // If can't, free`np` and lock as well. ref: xv6: a simple, Unix-like teaching operating system
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  // * QUESTION: What is the purpose of this? 
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  // TODO: Project 02 inherit and initialize fields 
  np->vruntime = p->vruntime;
  np->nice = p->nice;
  np->runtime = 0; // initialized to 0
  np->timeslice = 5; // set to default (5)
  np->vdeadline = p->vruntime + TIME_SLICE_UNIT  * nice_weights[20]/nice_weights[p->nice];


  release(&np->lock);
  eligible_check();

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit"); // * This shouldn't be executed in correct execution.
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void
eligible_check(void)
{
  struct proc *p;

  int challenger = 0;
  int vZero = 0;
  int weightSum = 0;

  for(p=proc;p<&proc[NPROC];p++){
    acquire(&p->lock);
    if(p->state != RUNNABLE) {
      release(&p->lock);
      continue;
    }
    if(vZero == 0) {
      vZero = p->vruntime;
    } else if(vZero > p->vruntime) {
      vZero = p->vruntime;
    }

    weightSum += nice_weights[p->nice];
    release(&p->lock);
  }

  for(p=proc;p<&proc[NPROC];p++) {
    acquire(&p->lock);
    if(p->state != RUNNABLE) {
      release(&p->lock);
      continue;
    }
    challenger += (p->vruntime - vZero) * nice_weights[p->nice];
    release(&p->lock); 
  }

  for(p=proc;p<&proc[NPROC];p++) {
    acquire(&p->lock);
    if(p->state != RUNNABLE) {
      release(&p->lock);
      continue;
    }
    if(challenger >= (p->vruntime - vZero) * weightSum) {
      p->is_eligible = 1;
    } else {
      p->is_eligible = 0;
    }
    release(&p->lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    intr_on();
    intr_off();

    int found = 0;

    int tempCount = 0; //If it's 0, the candidate must be initialized.
    struct proc *candidate = 0;
    
    eligible_check();
    for(p=proc;p<&proc[NPROC];p++) {
      acquire(&p->lock);
      if(p->is_eligible == 1){ // Lag determination
        if(candidate == p) {
          release(&p->lock);
          continue;
        }
        if(p->state == RUNNABLE) {
          if((tempCount==0) && (candidate != p)){
            tempCount = 1;
            candidate =  p; 
            // printf("candidate initialized [%p]\n", candidate);
            // printf("|candidate information [%p] |\n%s\t%d\t%s\n",candidate, candidate->name, candidate->pid, states[candidate->state]);
            // printf("|p information [%p]|\n%s\t%d\t%s\n",p, 
            // p->name, p->pid, states[p->state]);
          } 
          else if (candidate->vdeadline > p->vdeadline) {
            candidate = p;
            // printf("candidate has been changed\n");
          }
        } 
      }
      release(&p->lock);
    }

    if(tempCount==0) {
      continue;
    }

    acquire(&candidate->lock);
    candidate->state = RUNNING;
    c->proc = candidate;
    swtch(&c->context, &candidate->context);
    c->proc = 0;
    found = 1;
    release(&candidate->lock);
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        p->timeslice = 5;
        p->vdeadline = p->vruntime + TIME_SLICE_UNIT  * nice_weights[20]/nice_weights[p->nice];
      }
      release(&p->lock);
    }
  }
  eligible_check();
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
// TODO: Implement function in project 01

int
getnice(int pid)
{
  // the pointer of the process
  struct proc *p; 

  // loop the table of process
  for(p = proc; p < &proc[NPROC]; p++){
    //first get the lock to prevent the situation where the nice value is changing while checking the value 
    acquire(&p->lock);

    // if the process is not what we are finding
    if(p->pid != pid) {
      // release the lock
      release(&p->lock);
      //just pass 
      continue; 
    }
    // if the process is what we are finding and it is not UNUSED state
    if(p->state != UNUSED) {

      // save the nice value of the process whose pid was what we are looking for
      int nice = p->nice;
      release(&p->lock);
      // return the nice value
      return nice; 
    } else {
      //release the lock
      release(&p->lock);

      //we found the process with given pid but that is not valid process
      // to prevent the case: pid == 0 which can be both valid process and unused process's default pid
      return -1;
    }
  }
  // printf("[ERROR] Invalid pid (there is no process of pid : %d)\n", pid); (debugging)

  // there is no process corresponding to pid, return -1
  return -1;
}

int 
setnice(int pid, int value)
{
  // check the value that we intended to change satisfies the valid nice value range (0 - 39)
  if(0<=value&&value<=39){
    // the pointer variable of the process
    struct proc *p;

    // Scan the process table to find the target process
    for(p = proc; p < &proc[NPROC]; p++){
      // Acquire the lock so this process entry can be checked safely
      acquire(&p->lock);

      // if the process is not what we are finding
      if(p->pid != pid) {
        // release the lock
        release(&p->lock);
        // continue scanning 
        continue; 
      }
      // if the process is what we are finding and it is not UNUSED state
      if(p->state != UNUSED) {

       // set the nice value that we wanted to change
        p->nice = value;
        p->vdeadline = p->vruntime + p->timeslice * nice_weights[20]/nice_weights[p->nice];

        //release the lock
        release(&p->lock);
        // return with success value, 0
        return 0;
      } 
      
      // if the process with given pid is not valid process (UNUSED) (case: pid == 0)
      else {

        //release the lock
        release(&p->lock);

        //we found the process with given pid but that is not valid process
        // to prevent the case: pid == 0 which can be both valid process and unused process's default pid
        return -1;
      }
      
    }
  } 
  // Since the input value is not in the valid nice value range, return -1
  else {
    // printf("[ERROR] Invalid nice value: %d\n", value); // (debugging)
    return -1;
  }
  // there is no process corresponding to the given pid with valid nice value, return -1
  // printf("[ERROR] Invalid pid (there is no process of pid : %d)\n", pid); // (debugging)
  return -1;
}
void 
ps(int pid) 
{   
  // match digit value of state and its meaning
  static char *states[] = {
  [UNUSED]    "UNUSED   ",
  [USED]      "USED     ",
  [SLEEPING]  "SLEEPING ",
  [RUNNABLE]  "RUNNABLE ",
  [RUNNING]   "RUNNING  ",
  [ZOMBIE]    "ZOMBIE   "
  };
  // flag to check whether printing all is mandatory
  int isAll = 0;
  // flag to check whether we found the process
  int isFound = 0;
  
  // pid == 0 means we should print all!
  if(pid==0){
    isAll = 1;
  }

  // pointer to save temperal process
  struct proc *p;

  // Scan the process table to find the target process (NPROC is the number of whole process)
  for(p = proc; p < &proc[NPROC]; p++){
    // Acquire the lock so this process entry can be checked safely
    acquire(&p->lock);

    // filter UNUSED processes  
    if(p->state == UNUSED){
      
      // release the lock
      release(&p->lock);

      // continue scanning
      continue;
    }

    // if the process was what we were looking for or we should print all processes
    if(p->pid == pid || isAll){
      
      // if this is the first time to print
      if(!isFound){
        // print the header of process table
        printf("name   pid   state      priority   runtime/weight   runtime      vruntime  is_eligible  total tick  vdeadline\n");

        // make sure the header is not printed repeatedly
        isFound = 1;
      }
      // TODO: Project 2 calculate runtime / nice_weights
      int ratio = 1000*p->runtime/nice_weights[p->nice];
      // print the information of the process
      printf("%s\t%d   %s\t%d\t   %d\t            %ld ms\t  %ld\t    %d\t          %d\t    %ld\n", 
        p->name, p->pid, states[p->state], p->nice, 
        ratio, p->runtime*1000, p->vruntime, 
        p->is_eligible, (ticks - p->proc_start_ticks), p->vdeadline);

      // release the lock
      release(&p->lock);

    } 
    
    // the process was not what we were looking for and we don't have to print all processes 
    else {
      // release the lock
      release(&p->lock);
    }
  
  }
  // * It's void function. I don't think we need `return;` Check the `void voiddump(void)` function for comparison - @driedoutjerky
  //return;
}

// * Below method is wrong as well in terms of meminfo().
// * p->sz is jsut the size of a process's user memory region, not total
// * physical memory consumed by the kernel and system. 
//int 
//meminfo()
//{
//  int usedSz = 0;
//  struct proc *p;
//  for(p = proc; p < &proc[NPROC]; p++){
//    if(p->state==UNUSED) {
//      break;
//    }
//    usedSz = usedSz + p->sz;
//  }
//  return PHYSTOP - usedSz;
//
//}

int
meminfo(void)
{
  return kfreemem();
}

int
waitpid(int pid)
{
  struct proc *pp; // Child
  struct proc *p = myproc(); // Parent
  int gotKids; //

  acquire(&wait_lock); // Checking relation between parent and child so wait_lock is acquired.
  
  for(;;){
    gotKids = 0; // Does that child with that pid exist?
    for(pp=proc;pp<&proc[NPROC];pp++){
      if(pp->pid==pid && pp->parent==p){
        acquire(&pp->lock); // make sure the child isn't still in exit() or swtch().
        gotKids = 1;

        if(pp->state==ZOMBIE){
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return 0;
        }
        release(&pp->lock);
      }
    }
    if(!gotKids || killed(p)){ // You're not the father OR You've been killed.
      release(&wait_lock);
      return -1;
    }
    sleep(p, &wait_lock);
  }
}

int
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
  //Check contradiction between flags + fd before mmap begins
  if(flags == MAP_ANONYMOUS && fd != -1) return 0;

  struct proc *p = myproc();

  acquire(&p->lock);
  if(p->mmappagecount >= MAXMMAP){
    release(&p->lock);
    return 0; //MAXMMAP exception
  }
  release(&p->lock);

  //1. check addr, length if page aligned
  // -> Now being checked in kmmap()

  //2. compute mapping start address: MMAPBASE + addr
  uint64 startaddr = (uint64)MMAPBASE + addr;

  //3. request kalloc() n times, where n =  length/PGSIZE;
  // HOWEVER there's no way for kalloc() to receive addr and begin from that point.
  // Therefore in kalloc.c, function kmmap() has been implemented.

  void* allocaddr = kmmap(startaddr, length);
  if(allocaddr == 0){
    return 0; //failed to allocate.
  }
  //4.Check flags: MAP_POPULATE or MAP_ANONYMOUS
  if(flags == MAP_POPULATE){
    mappages(p->pagetable,(uint64)addr,length,(uint64)allocaddr,prot); 
  }
  else if(flags == MAP_ANONYMOUS){
    //don't mappages, instead mappages through page fault handler
  }
  else{
    return 0; //failed to check flag.
  }

  //5. deal with fd and offset. OFFSET IMPLEMENTED. Yippee
  if(fd != -1 && offset>=0){
    if(p->ofile[fd]){
      if(setoff(p->ofile[fd], offset) < 0) return 0; //Couldn't set offset of file

      int maxCounter = 0;
      uint64 targetAddr = (uint64)allocaddr;

      for(;;){
        if(maxCounter > length/PGSIZE) break;
        int data = fileread(p->ofile[fd], targetAddr, PGSIZE);
        if(data==0) break;
        targetAddr += PGSIZE;
        maxCounter++;
      }
    }
  }
  
  //Make sure to increment 1 on  p->mmappagecount after success of mmap().
  p->mmappagecount++;

  int resultaddr = (uint64)allocaddr;
  return resultaddr; 
}

int
munmap(uint64 addr)
{
  return 0;
}

int
freemem()
{
  return 0;
}

