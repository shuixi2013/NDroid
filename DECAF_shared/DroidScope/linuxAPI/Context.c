/**
 * Copyright (C) <2011> <Syracuse System Security (Sycure) Lab>
 *
 * This library is free software; you can redistribute it and/or 
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Context.c
 * The shadow process list and the shadow module list are implemented here. 
 * The flow is something like this:
 * 1. We create an initial process list.
 * 2. We monitor specific system calls (and their respective core functions, e.g. do_fork)
 *   and wait for them to return. Upon return - we update the process list.
 * 2A. While we HOPE to catch all changes to the process list, we might not so
 *       - We create a temporary process list
 *       - Anything NOT in OLD one but in the new one is a new process
 *       - Anything NOT in the NEW one but in the old one has ended
 * 3. The same procedure is used to udpate the modules. The only difference is 
 *      that modules are updated on demand.
 * NOTE: We assume a small number of processes and thus assume that linear
 *   search is perfectly fine. This is different from the PROCMOD approach
 *   that uses multiple hashtables. Hashtables can also be implemented here
 *   as well, but its not too important yet.
 * NOTE: This also means that our shadow process list is very comprehensive.
 *   It is easier and less performance intensive to just keep a pointer to
 *   the guest's task_struct structure per process or thread and read the fields
 *   (e.g. pid, uid, etc.) on demand. However, we update these whenenver the
 *   process list is updated. (Will this reduce the number of changes to the TLB
 *   due since we are reading the memory locations closer to when they were last
 *   used? - I wonder)
 *  Created on: Sep 16, 2011
 *      Author: lok
 */

#include "LinuxAPI.h"
#include "linuxAPI/ProcessInfo.h"
#include "DECAF_shared/DECAF_callback.h"
#include "DECAF_shared/utils/SimpleCallback.h"
#include "DECAF_shared/utils/OutputWrapper.h"

/* NDROID START */
#include "DroidScope/NDroid/ND_manager.h"
/* NDROID END */

gpid_t curProcessPID = (-1);
static gpa_t curProcessPGD = 0;

inline gpid_t getCurrentPID()
{
  return (curProcessPID);
}

inline gpa_t getCurrentPGD()
{
  return (curProcessPGD);
}

int bSkipNextPGDUpdate = 0;
static DECAF_Handle contextBBHandle;
static DECAF_Handle contextCSHandle;

/************************************************************************
 * Start of implementation section for "Callbacks"
 ************************************************************************/
static SimpleCallback_t DroidScope_callbacks[DECAF_PROCESSES_LAST_CB];

DECAF_Handle DECAF_Processes_register_callback(DECAF_Processes_callback_type_t cb_type, DECAF_Processes_callback_func_t cb_func, int *cb_cond)
{
  if ((cb_type > DECAF_PROCESSES_LAST_CB) || (cb_type < 0))
  {
    return (DECAF_NULL_HANDLE);
  }
 
  return (SimpleCallback_register(&DroidScope_callbacks[cb_type], (SimpleCallback_func_t)cb_func, cb_cond));
}

DECAF_errno_t DECAF_Processes_unregister_callback(DECAF_Processes_callback_type_t cb_type, DECAF_Handle handle)
{
  if ((cb_type > DECAF_PROCESSES_LAST_CB) || (cb_type < 0))
  {
    return (-1);
  }

  return (SimpleCallback_unregister(&DroidScope_callbacks[cb_type], handle));
}

/************************************************************************
 * Start of implementation section for updating process and module structures
 ************************************************************************/

void updateProcessModuleList(CPUState* env, gpid_t pid)
{
  DECAF_Processes_Callback_Params params;
  target_ulong task = 0;
  target_ulong i = 0;
  char name[MAX_PROCESS_INFO_NAME_LEN];

  if (pid == 0)
  {
    return;
  }

	/* NDROID START */
	/**
	 * Comment added by Chenxiong0_14N
	 * find the task_struct with 'pid'
	 */
	/* NDROID END */
  task = DECAF_get_current_process(env);
  i = task;
  do
  {
    if (pid == DECAF_get_pid(env, i))
    {
      break;
    }
    i = DECAF_get_next_task_struct(env, i);
  } while ( (i != 0) && (i != task) );

  if ((i == 0) || (i == task))
  {
    return;
  }

  target_ulong mmap_first = DECAF_get_first_mmap(env, i);
  target_ulong mmap_i = mmap_first;
  target_ulong vmfile = 0;
  target_ulong flags = 0;
  target_ulong vmstart = 0;
  target_ulong vmend = 0;
  target_ulong startbrk = 0;
  target_ulong brk = 0;
  target_ulong startstack = 0;
  target_ulong mm = 0;

  do
  {
    vmstart = DECAF_get_vm_start(env, mmap_i);
    vmend = DECAF_get_vm_end(env, mmap_i);
    flags = DECAF_get_vm_flags(env, mmap_i);
    mm = DECAF_get_mm(env, i);
    startbrk = DECAF_get_start_brk(env, mm);
    brk = DECAF_get_brk(env, mm);
    startstack = DECAF_get_start_stack(env, mm);

    //from mm.h
    //#define VM_READ         0x00000001      /* currently active flags */
    //#define VM_WRITE        0x00000002
    //#define VM_EXEC         0x00000004
    //#define VM_SHARED       0x00000008

    vmfile = DECAF_get_vm_file(env, mmap_i);
    if (vmfile != 0)
    {
      //get_mod_dname(mmap_i, name, 128);
      DECAF_get_mod_full_dname(env, mmap_i, name, 128);
    }
    else
    {
      name[0] = '\0';
      //get_mod_iname(mmap_i, name, 128);
      DECAF_get_mod_full_iname(env, mmap_i, name, 128);
      if (strlen(name) <= 0)
      {
        if (vmstart <= startbrk && vmend >= brk)
        {
          sprintf(name, "[heap]");
        }
        else if (vmstart <= startstack && vmend >= startstack)
        {
          sprintf(name, "[stack]");
        }
      }
    }

    int ret = updateModule(pid, vmstart, vmend, flags, name);
    if (ret == 0xF) //if this is a new module
    {
      params.lm.pid = pid;
      params.lm.pgd = getCurrentPGD();
      params.lm.name = name; //TODO: THIS IS NOT THREAD SAFE!!!
      params.lm.full_name = name;
      params.lm.base = vmstart;
      params.lm.size = vmend-vmstart;
      SimpleCallback_dispatch(&DroidScope_callbacks[DECAF_PROCESSES_LOAD_MODULE_CB], &params);
    }
    else if (ret > 0)
    {
      params.mu.pid = pid;
      params.mu.startAddr = vmstart;
      params.mu.mask = ret;
      SimpleCallback_dispatch(&DroidScope_callbacks[DECAF_PROCESSES_MODULES_UPDATED_CB], &params);
    }
    mmap_i = DECAF_get_next_mmap(env, mmap_i);
  } while ((mmap_i != 0) && (mmap_i != mmap_first));

}

inline void update_mod(CPUState* env, gpid_t pid)
{
  updateProcessModuleList(env, pid);
}

inline void linux_print_mod(Monitor* mon, gpid_t pid)
{
  //We can pass in NULL since printModuleList uses DECAF_fprintf
  printModuleList(NULL, pid);
}


/**
 * @param val the current pgd-cr3 value
 *
 * The task list can be obtained in two ways.
 * 1. Using next_task_struct which uses the global symbol for init_task as the starting point
 * 2. Using the current_task_struct structure. This should just "work" although there are some
 *   special considerations in some special cases. current_task_struct returns the actual
 *   task struct - which can either be a process task struct or a thread task struct (if it
 *   is single threaded). Either way, it is guaranteed that the next pointer in task struct
 *   will either point to the next process task struct or init_task. Which means the loop
 *   should still work.
 *
 * Perhaps it is helpful to illustrate how the process/thread list really works
 * There are two fields in the task_struct that are of interest
 * 1. The next pointer that points to the next task struct
 * 2. The thread_group field (which is of type struct list_head { list_head* next, prev })
 *    This means task_struct.thred_group will automatically give you next. The tricky thing
 *    is that this list points to the thread_group field of the next task_struct that belongs
 *    to this group. See the figure below.
 * To put things together, lets assume that we have two running processes, 30 and 31.
 * 31 is single threaded and 30 is multi-threaded with two additional threads 32 and 33.
 * Given that we always have init_task, we should have a total of 5 task_structs, 1 for init
 *   2 for the processes and 2 for the threads.
 * The following is a graphical representation of the "process list"
 *
 * ,--------------------------------------------------------------------,
 * |     _____________          _____________          _____________    |
 * |---> | pid = 0   |    ,---> | pid = 30  |    ,---> | pid = 31  |    |
 * |     | tgid = 0  |    |     | tgid = 30 |    |     | tgid = 31 |    |
 * |     | next      | ---'     | next      | ---|     | next      | ---'
 * | ,-> | t-group   | -,   ,-> | t-group   | -, | ,-> | t-group   | --,
 * | |   |___________|  |  /    |___________|  | | |   |___________|   |
 * | '------------------' /                    | | '-------------------'
 * |                     / ,-------------------' |
 * |                    |  |    _____________    |
 * |                    |  |    | pid = 32  |    |
 * |                    |  |    | tgid = 30 |    |
 * |                    |  |    | next      | ---' (points to real next)
 * |                    |  '--> | t-group   | --,
 * |                    |       |___________|   |
 * |                    |  ,--------------------'
 * |                    |  |    _____________
 * |                    |  |    | pid = 33  |
 * |                    |  |    | tgid = 30 |
 * |                    |  |    | next      | ----, (points to init_task)
 * |                    |  '--> | t-group   | --, |
 * |                    |       |___________|   | |
 * |                    '-----------------------' |
 * '----------------------------------------------'
 *
 * Some things to emphasize (again?)
 * 1. thread_group.next (represented by t-group) points to the next thread_group field!
 * 2. next of the process task struct in the process list is guaranteed to point to the next
 *   task struct. The next in the thread task_struct might point to next or init_task
 * 3. According to online references, the pid's are always unique - this is why thread ids
 *   for the process 30 are 30 (the main thread) 32 and 33 (the other two threads).
 * 4. The tgid shows the real pid.
 *
 * The above example does not include the "thread_info" structure. Each task_struct
 *   is associated with its own thread_info structure which is pointed to by the "stack" field of the
 *   task_struct. To DECAF_get the stack address of the task - we will have to go into the thread info structure
 *   and look at the cpu_context field to grab the stack pointer. More info on the cpu_context and copy_thread
 *   (called from copy_process called from do_fork) can be found in arch/ARCH/kernel/process.c
 */
gva_t updateProcessListByTask(CPUState* env, gva_t task, int updateMask, int bNeedMark)
{
  DECAF_Processes_Callback_Params params;

  gpid_t pid;
  gpid_t parentPid;
  gpid_t tgid;
  gpid_t glpid;
  target_ulong uid;
  target_ulong gid;
  target_ulong euid;
  target_ulong egid;

  gpid_t t_pid;
  gpid_t t_tgid;

  gpa_t pgd;
  char name[MAX_PROCESS_INFO_NAME_LEN];
  char argName[MAX_PROCESS_INFO_NAME_LEN];
  gva_t i = task;

  argName[0] = '\0';
  name[0] = '\0';

  pid = DECAF_get_pid(env, i);
  tgid = DECAF_get_tgid(env, i);
  glpid = DECAF_get_group_leader_pid(env, i);
  uid = DECAF_get_uid(env, i);
  gid = DECAF_get_gid(env, i);
  euid = DECAF_get_euid(env, i);
  egid = DECAF_get_egid(env, i);
  parentPid = DECAF_get_parent_pid(env, i);
  pgd = pgd_strip(DECAF_get_pgd(env, i));

  int ret = 0;

  if (curProcessPGD == pgd)
  {
    if (DECAF_get_arg_name(env, i, argName, MAX_PROCESS_INFO_NAME_LEN) < 0)
    {
      argName[0] = '\0';
    }
  }
  if (DECAF_get_name(env, i, name, MAX_PROCESS_INFO_NAME_LEN) < 0) //get the name
  {
    name[0] = '\0';
  }

	/* NDROID START */
	/**
	 * Comment added by Chenxiong0_14N
	 * The logic of this "if" condition expression confuses me:
	 * 1. processMark(pid) == 1 means the process with 'pid' doesn't exist
	 * 2. findProcessByPID(pid) == NULL also means 'pid' doesn't exist
	 * So, it seems that bNeedMark is useless.
	 */
	/* NDROID END */
  //update the info if needed
  if ( ((bNeedMark) && (processMark(pid) == 1))
       || ((!bNeedMark) && (findProcessByPID(pid) == NULL)) 
     ) // i.e. it doesn't exist
  {
    addProcess(i, pid, parentPid, tgid, glpid, uid, gid, euid, egid, pgd, (argName[0] == '\0') ? NULL : argName, (name[0] == '\0') ? NULL : name);
		/* NDROID START */
		if(ND_TRACING_STATE == ND_WAITING && uid == ND_GLOBAL_TRACING_UID){
			DECAF_printf("Process with uid <%d> starts!\n", uid);
			nd_manager_trace_uid(NULL, uid);
		}
		/* NDROID END */

		/* NDROID START */
		//because process with 'pid' has been added to 'processInfoMap', so that 
		//call 'processMark' again would add the new added process to 'processInfoMapTemp'
		/* NDROID END */
    processMark(pid);
    params.cp.pid = pid;
    params.cp.pgd = pgd;
    SimpleCallback_dispatch(&DroidScope_callbacks[DECAF_PROCESSES_CREATE_PROCESS_CB], &params);
    //force a module and thread update
    updateMask |= UPDATE_THREADS | UPDATE_MODULES;
  }
  else
  {
    ret = updateProcess(i, pid, parentPid, tgid, glpid, uid, gid, euid, egid, pgd, (argName[0] == '\0') ? NULL : argName, (name[0] == '\0') ? NULL : name);
    if (ret > 0)
    {
      params.pu.pid = pid;
      params.pu.mask = ret;
      SimpleCallback_dispatch(&DroidScope_callbacks[DECAF_PROCESSES_PROCESS_UPDATED_CB], &params);
    }
  }

  if (updateMask & UPDATE_THREADS)
  {
    //update (repopulate) the threads
    gva_t j = i;
    clearThreads(pid);
    do
    {
      if ((j != 0) && (j != -1))
      {
        t_pid = DECAF_get_pid(env, j);
        t_tgid = DECAF_get_tgid(env, j);

        //run through the thread group
        gva_t parentTI = DECAF_get_stack(env, j);
        addThread(t_tgid, t_pid, parentTI);
      }

      j = DECAF_get_thread_group(env, j);
      if ( (j == -1) || (j == 0) )
      {
        break;
      }
      j -= task_struct_thread_group_offset;//this gives you the next one immediately
    } while ( i != j );
  } //end bUpdateThreads

  //update (repopulate) the module list
  if (updateMask & UPDATE_MODULES)
  {
    updateProcessModuleList(env, pid);
  }

  i = DECAF_get_next_task_struct(env, i);
  return (i);
}

void updateProcessList(CPUState* env, gpa_t newpgd, int updateMask)
{
  if (env == NULL)
  {
    return;
  }

  if (env->regs[15] < 0xC0000000)
  {
    return;
  }

  DECAF_Processes_Callback_Params params;

  gva_t task = DECAF_get_current_process(env);

  if (task == 0)
  {
    return;
  }

  gpa_t pgd;
  gpid_t pid;

  gva_t i = task;
  //begin the marking process
  //TODO: This is not thread safe - might lose some information
  processMarkBegin();
  do
  {
    pgd = pgd_strip(DECAF_get_pgd(env, i));
    pid = DECAF_get_pid(env,i);
		//DECAF_fprintf(NULL, "DNDROID--updateProcessList: task=%i, pid=%i", task, pid);
    //see if this is the new process, if it is, then update the current PID
    if (pgd == pgd_strip(newpgd))
    {
      curProcessPID = pid;
      curProcessPGD = pgd;
    }

		/* NDROID START */
		/**
		 * Comment added by Chenxiong0_14N
		 * update task and update threads or modules depends on 'updateMask'
		 * The 'updateMask' would union with 'UPDATE_THREADS' and 'UPDATE_MODULES'
		 * if the process is newly created.
		 */	
		/* NDROID END */
    i = updateProcessListByTask(env, i, updateMask, 1);
  } while ( (i != task) && ( i != 0) );

  gpid_t* pids = NULL;
  size_t len = 0;
  size_t j = 0;

  //mark the end as well as get an array of all the affected pids
  processMarkEnd(&pids, &len);
  for (j = 0; j < len; j++)
  {
    params.rp.pid = pids[j];
    SimpleCallback_dispatch(&DroidScope_callbacks[DECAF_PROCESSES_REMOVE_PROCESS_CB], &params);
  }

  if (len > 0)
  {
    free(pids);
  }

  return;
}

inline void get_symbol(CPUState* env, gpid_t pid, gva_t addr)
{
  char name[128];
  name[0] = '\0';
  getNearestSymbol(name, 128, pid, addr);
  DECAF_printf("%08x is in %s\n", addr, name);
}

inline void get_symbol_address(Monitor* mon, int pid, const char* strModule, const char* strName)
{
  DECAF_printf("%s is at [%x]\n", strName, getSymbolAddress(pid, strModule, strName));
}

inline void linux_ps(Monitor* mon)
{
  //We can pass in NULL since printProcessList uses DECAF_fprintf
  printProcessList(NULL);
}

inline void linux_pt(Monitor* mon)
{
  //We can pass in NULL since printThreadsList uses DECAF_fprintf
  printThreadsList(NULL);
}

//reg 0 is c2_base0 and 1 is c2_base1
void Context_PGDWriteCallback(DECAF_Callback_Params* params)
{
  struct timeval t;
  gettimeofday(&t, NULL);

  DEFENSIVE_CHECK0(params == NULL);

	//DECAF_fprintf(NULL, "PGDWriteCallback--pgd.env=%p, pgd.curPGD=%i, pgd.newPGD=%i, pgd.c2_base=%i\n", params->pgd.env, params->pgd.curPGD, params->pgd.newPGD, (int) params->pgd.c2_base);

  //TODO: Keep a record of what the current PGD is and the new PGD is
  // so that we don't do unnecessary updates - this applies to the
  // skipupdates flag that is set when system calls are made as well
  if (!bSkipNextPGDUpdate)
  {
    updateProcessList(params->pgd.env, params->pgd.newPGD, UPDATE_PROCESSES | UPDATE_THREADS);
  }

  //reset this flag
  bSkipNextPGDUpdate = 0;
}


static gva_t Context_retAddr = 0;
//LOK: My tests have shown that do_fork -> then update on a PGD write is a perfect choice. Should change the logic to do that.
// it seems to cover many more cases than do_fork and schedule()
//TODO: have to fix the potential problem where this is called twice before the return is processed
// in which case the process name will not be updated properly
void contextBBCallback(DECAF_Callback_Params* params)
{
  static gva_t taskAddr = INV_ADDR;
  static int updateMask = 0;
  gpid_t pid = -1;
  TranslationBlock* tb = NULL;
  CPUState* env = NULL;

  DEFENSIVE_CHECK0(params == NULL);

  env = params->bb.env;
  tb = params->bb.tb;
  
  if (NULL == tb)
  {
    return;
  }

  if ( (tb->pc == SET_TASK_COMM_ADDR) || (tb->pc == DO_PRCTL_ADDR) )//set_task_comm
  {
    //In this case, we just update the name when the function returns
    //TODO: Fix i386 support 
    //TODO: Make sure this taskAddr is NOT the thread's task 
#ifdef TARGET_ARM
    taskAddr = env->regs[0];
    Context_retAddr = env->regs[14];
#elif TARGET_I386
    taskAddr = env->regs[R_EAX];
    DECAF_read_mem(env, env->regs[R_ESP], &Context_retAddr, sizeof(Context_retAddr));
#endif
  }
  else if ( (tb->pc == DO_EXECVE_ADDR) || (tb->pc == DO_CLONE_ADDR) )//do_execve
  {
    //we OR the update mask since its possible for the system call
    // to call another test - e.g. do_fork - and without declaring
    // the updateMask as static and using |= the flags will be
    // overwritten
    //TODO: Implement a STACK for the return addresses!!!
    //in this case we update the process, threads and module lists
    updateMask |= UPDATE_PROCESSES | UPDATE_THREADS | UPDATE_MODULES;
#ifdef TARGET_ARM
    Context_retAddr = env->regs[14];
#endif
  }
  else if (tb->pc == DO_FORK_ADDR) //do_fork
  {
    //In this case we just update the process and threads lists 
    updateMask |= UPDATE_PROCESSES | UPDATE_THREADS;
#ifdef TARGET_ARM
    Context_retAddr = env->regs[14];
#endif
  }
	/* NDROID START */
	else if (tb->pc == DO_MMAP2_ADDR) //mmap2
	{
		updateMask |= UPDATE_MODULES;
#ifdef TARGET_ARM
    Context_retAddr = env->regs[14];
#endif
	}
	/* NDROID END */

  if (tb->pc == Context_retAddr)
  {
    if (taskAddr != INV_ADDR)
    //if we need to update the names only
    {
      pid = DECAF_get_pid(env, taskAddr);
      if (pid != -1)
      {
        //if we found the PID then just read the names and update
        updateProcessListByTask(env, taskAddr, UPDATE_PROCESSES | UPDATE_THREADS | UPDATE_MODULES, 0);
      }
      taskAddr = INV_ADDR;
    } 
    else
    {
      updateProcessList(env, getCurrentPGD(), updateMask);
    }

    //since we updated the list already - lets skip the next PGD
    //write update
		/* NDROID START */
		//After conducting experiments, we should not skip the next PGD update handling,
		//or in callbacks (e.g., instruction_begin_callback), curPID will refer other 
		//process (e.g., system_server) rather than the running process.
    //bSkipNextPGDUpdate = 1;
		/* NDORID END */
    Context_retAddr = 0;
    DECAF_flushTranslationBlock_env(env, Context_retAddr);
  }

  if (Context_retAddr != 0)
  {
    //instead of registering for a new callback - we will just update our
    //conditions list and flush the entry for retAddr
   DECAF_flushTranslationBlock_env(env, Context_retAddr);
  }

  return;
}

int contextCondFunc (DECAF_callback_type_t cbType, gva_t curPC, gva_t nextPC)
{
  DEFENSIVE_CHECK1(cbType != DECAF_BLOCK_BEGIN_CB, 0);

  if ( (curPC == SET_TASK_COMM_ADDR)
       || (curPC == DO_FORK_ADDR)
       || (curPC == DO_EXECVE_ADDR)
       || (curPC == DO_PRCTL_ADDR)
       || (curPC == DO_CLONE_ADDR)
			 /* NDROID START */
			 || (curPC == DO_MMAP2_ADDR)
			 /* NDORID END */
       || ((Context_retAddr != INV_ADDR) && (curPC == Context_retAddr))
     )
  {
    return (1);
  }

  return (0);
}

void context_init()
{
  int i = 0;
  for (i = 0; i < DECAF_PROCESSES_LAST_CB; i++)
  {
    SimpleCallback_init(&DroidScope_callbacks[i]);
  }

  contextCSHandle = DECAF_register_callback(DECAF_PGD_WRITE_CB, &Context_PGDWriteCallback, NULL);
  contextBBHandle = DECAF_register_callback(DECAF_BLOCK_BEGIN_CB, &contextBBCallback, &contextCondFunc);
}

void context_close()
{
  int i = 0; 

  for (i = 0; i < DECAF_PROCESSES_LAST_CB; i++)
  {
    SimpleCallback_clear(&DroidScope_callbacks[i]);
  }
  if (contextCSHandle != DECAF_NULL_HANDLE)
  {
    DECAF_unregister_callback(DECAF_PGD_WRITE_CB, contextCSHandle);
    contextCSHandle = DECAF_NULL_HANDLE;
  }
  if (contextBBHandle != DECAF_NULL_HANDLE)
  {
    DECAF_unregister_callback(DECAF_BLOCK_BEGIN_CB, contextBBHandle);
    contextBBHandle = DECAF_NULL_HANDLE;
  }
}

