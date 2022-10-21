#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "debug.h"
#include "interrupt.h"
#include "print.h"
#include "memory.h"
#include "process.h"
#include "sync.h"


struct task_struct* main_thread;    // 主线程PCB
struct task_struct* idle_thread;    // idle线程
struct list thread_ready_list;	    // 就绪队列
struct list thread_all_list;	    // 所有任务队列
struct lock pid_lock;		    // 分配pid锁
static struct list_elem* thread_tag;// 用于保存队列中的线程结点

extern void switch_to(struct task_struct* cur, struct task_struct* next);

static void idle(void* arg UNUSED){
    while (true)
    {
        thread_block(TASK_BLOCKED);
        //执行hlt时必须要保证目前处在开中断的情况下
        asm volatile ("sti; hlt" : : : "memory");
    }
    
}
/* 获取当前线程pcb指针 */
struct task_struct* running_thread(){
    uint32_t esp;
    asm ("mov %%esp, %0" : "=g" (esp));//esp指向PCB的页中
    return (struct task_struct*) (esp & 0xfffff000);
}

/* 分配pid */
static pid_t allocate_pid(void){
    static pid_t next_pid = 0;
    lock_acquire(&pid_lock);
    next_pid ++;
    lock_release(&pid_lock);
    return next_pid;
}

/* 由kernel_thread去执行function(func_arg) */
static void kernel_thread(thread_func* function, void* func_arg ){
    /* 执行function前要开中断,避免后面的时钟中断被屏蔽,而无法调度其它线程,导致线程独占CPU */
    intr_enable();
    function(func_arg);
}

/* 初始化线程基本信息 */
void init_thread(struct task_struct* pthread, char* name, int prio) {
    memset(pthread,0,sizeof(*pthread));
    pthread->pid = allocate_pid();
    strcpy(pthread->name,name);

    if (pthread == main_thread )
    { 
       pthread->status = TASK_RUNNING;
    }else{
        pthread->status = TASK_READY;
    }
    pthread->priority = prio;
    //线程的内核栈初始化位PCB所在页的最高地址处
    pthread->self_kstack = (uint32_t*) ((uint32_t)pthread + PG_SIZE);
    pthread->ticks = prio;
    pthread->elapsed_ticks = 0;
    pthread->pgdir = NULL;
    pthread->stack_magic = 0x20020905;//防止内核栈入栈过多破坏PCB低处的线程数据
}

/* 初始化线程栈thread_stack,将待执行的函数和参数放到thread_stack中相应的位置 */
void thread_create(struct task_struct* pthread, thread_func function, void* func_arg) {
    /* 先预留中断使用栈的空间,可见thread.h中定义的结构 */
    pthread->self_kstack -= (uint32_t)sizeof(struct intr_stack);
    /* 再留出线程栈空间,可见thread.h中定义 */
    pthread->self_kstack -= (uint32_t)sizeof(struct thread_stack);
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->func_arg = func_arg;
    kthread_stack->function = function;
    kthread_stack->ebp = kthread_stack->ebx = kthread_stack->edi = kthread_stack->esi = 0;

}
/* 创建一优先级为prio的线程,线程名为name,线程所执行的函数是function(func_arg) */
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg){
    /* pcb都位于内核空间,包括用户进程的pcb也是在内核空间 */
    struct task_struct* thread = get_kernel_pages(1);
    
    init_thread(thread,name,prio);
    thread_create(thread,function,func_arg);

    /* 确保之前不在队列中 */
    ASSERT(!elem_find(&thread_ready_list,&thread->general_tag));
    list_append(&thread_ready_list,&thread->general_tag);

    /* 确保之前不在队列中 */
    ASSERT(!elem_find(&thread_all_list,&thread->all_list_tag));
    list_append(&thread_all_list,&thread->all_list_tag);

    return thread;
}

/* 将kernel中的main函数完善为主线程 */
static void make_main_thread(void) {
    /* 因为main线程早已运行,咱们在loader.S中进入内核时的mov esp,0xc009f000,
就是为其预留了pcb,地址为0xc009e000,因此不需要通过get_kernel_page另分配一页*/
    main_thread = running_thread();
    init_thread(main_thread,"main",31);
    
    /* main函数是当前线程,当前线程正在运行，不在thread_ready_list中,
 * 所以只将其加在thread_all_list中. */
    ASSERT(!elem_find(&thread_all_list,&main_thread->all_list_tag));
    list_append(&thread_all_list,&main_thread->all_list_tag);
}

/* 实现任务调度 */
void schedule() {

    //任务调度都一定在中断中（即关中断时进行）
    ASSERT(intr_get_status() == INTR_OFF);

    struct task_struct* cur = running_thread();
    if (cur->status==TASK_RUNNING)
    {
        ASSERT(!elem_find(&thread_ready_list,&cur->general_tag));
        list_append(&thread_ready_list,&cur->general_tag);
        cur->ticks = cur->priority;
        cur->status = TASK_READY;
    }else {
        /* 若此线程需要某事件发生后才能继续上cpu运行,
      不需要将其加入队列,因为当前线程不在就绪队列中。*/
    }

    ASSERT(!list_empty(&thread_ready_list));
    thread_tag = NULL;
    thread_tag = list_pop(&thread_ready_list);
    struct task_struct* next = elem2entry(struct task_struct , general_tag , thread_tag);
    next->status = TASK_RUNNING;

    process_activate(next);
    switch_to(cur,next);
}

/* 当前线程将自己阻塞,标志其状态为stat. */
void thread_block(enum task_status stat){
    /* stat取值为TASK_BLOCKED,TASK_WAITING,TASK_HANGING,也就是只有这三种状态才不会被调度*/
    ASSERT(((stat == TASK_BLOCKED) || (stat == TASK_HANGING) || (stat == TASK_WAITING)));
    enum intr_status old_status = intr_disable();
    struct task_struct* cur_thread = running_thread();
    cur_thread->status = stat;
    schedule();//将任务调度下来,切换到其他线程
    //解除阻塞后继续运行
    intr_set_status(old_status);
}

/* 将线程pthread解除阻塞 */
void thread_unblock(struct task_struct* pthread){
    enum intr_status old_status = intr_disable();
    ASSERT (((pthread->status == TASK_BLOCKED) || (pthread->status == TASK_WAITING) || (pthread->status == TASK_HANGING)));
    if (pthread->status!=TASK_READY)
    {
        //因为一个线程运行时不在Ready_list中，被阻塞时（运行期间）同样也不会在
        ASSERT(!elem_find(&thread_ready_list,&pthread->general_tag));
        if (elem_find(&thread_ready_list,&pthread->general_tag))
        {
            PANIC("thread_unblock: blocked thread in ready_list\n");
        }
        list_push(&thread_ready_list,&pthread->general_tag);
        pthread->status = TASK_READY;
    }
    intr_set_status(old_status);
}
/* 主动让出cpu,换其它线程运行 */
void thread_yield(void){
    struct task_struct* cur = running_thread();
    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
    list_append(&thread_ready_list, &cur->general_tag);
    cur->status = TASK_READY;
    schedule();
    intr_set_status(old_status);
}

/* 初始化线程环境 */
void thread_init(void){
    put_str("thread_init start\n");
    list_init(&thread_all_list);
    list_init(&thread_ready_list);
    lock_init(&pid_lock);
    make_main_thread();

    idle_thread = thread_start("idle", 10, idle, NULL);
    put_str("thread_init done\n");
}