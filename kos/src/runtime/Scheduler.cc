/******************************************************************************
    Copyright © 2012-2015 Martin Karsten

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/
#include "runtime/RuntimeImpl.h"
#include "runtime/Scheduler.h"
#include "runtime/Stack.h"
#include "runtime/Thread.h"
#include "kernel/Output.h"
#include "world/Access.h"
#include "machine/Machine.h"
#include "devices/Keyboard.h"
	   
/* A2 */
// in miliseconds
mword Scheduler::schedMinGranularity;
mword Scheduler::defaultEpochLength;

// in ticks
mword Scheduler::epochLengthTicks;
mword Scheduler::defaultEpochLengthTicks = Scheduler::epochLengthTicks;
mword Scheduler::schedMinGranularityTicks;

// running time of a task
mword startTime;
mword waitTime;

mword timeSlice=1;      // time for a task
mword totalPriority=0;  // total sum of priorities in the tree
/* A2*/

/***********************************
    Used as a node in the tree to 
	reference the thread instance
	Created by: Adam Fazekas (Fall 2015)
***********************************/
mword smallestRuntime=9999999;

class ThreadNode
{
	friend class Scheduler;
	Thread *th;
	
	public:
		bool operator < (ThreadNode other) const 
		{
			return th->vRuntime < other.th->vRuntime;
		}
		bool operator == (ThreadNode other) const 
		{
			return th->vRuntime == other.th->vRuntime;
		}
		bool operator > (ThreadNode other) const 
		{
			return th->vRuntime > other.th->vRuntime;
		}
    
	//this is how we want to do it
	ThreadNode(Thread *t)
	{
		th = t;
	}
};	   
	   
/***********************************
			Constructor
***********************************/	   
Scheduler::Scheduler() : readyCount(0), preemption(0), resumption(0), partner(this) {
	
//Initialize the idle thread
	//(It keeps the CPU awake when there are no other threads currently running)
	Thread* idleThread = Thread::create((vaddr)idleStack, minimumStack);
	idleThread->setAffinity(this)->setPriority(idlePriority);

	// use low-level routines, since runtime context might not exist
	idleThread->stackPointer = stackInit(idleThread->stackPointer, &Runtime::getDefaultMemoryContext(), (ptr_t)Runtime::idleLoop, this, nullptr, nullptr);
	
	//Initialize the tree that contains the threads waiting to be served
	readyTree = new Tree<ThreadNode>();
	
	//Add the idle thread to the tree
	readyTree->insert(*(new ThreadNode(idleThread)));
	readyCount += 1;
    
    /* A2 */
	idleThread->vRuntime=0;
	timeSlice=Scheduler::epochLengthTicks/readyCount;
    totalPriority+=idleThread->priority +1;
    /* A2 */
}

/***********************************
		Static functions
***********************************/      
static inline void unlock() {}

template<typename... Args>
static inline void unlock(BasicLock &l, Args&... a) {
  l.release();
  unlock(a...);
}	   

/***********************************
    Gets called whenever a thread 
	should be added to the tree
***********************************/
void Scheduler::enqueue(Thread& t) {
    /* A2 */
    // increase total priority in the tree
    totalPriority+=t.priority + 1;  
    
    // start reading waiting time
    t.startTime = CPU::readTSC();

    // take the maximum of the epoch length and (readyCount*minGranularity)
    if (Scheduler::epochLengthTicks>=((readyCount+1)*Scheduler::schedMinGranularityTicks))
    {
        Scheduler::epochLengthTicks=Scheduler::defaultEpochLengthTicks;
    }
    else
    {
        Scheduler::epochLengthTicks=((readyCount+1)*(Scheduler::schedMinGranularityTicks));
    }
    
    // check is the thread was suspended
    if(!t.isAwake)
    {
        // update vRuntime
        t.vRuntime = smallestRuntime;
        
        // set to awake
        t.isAwake = true;
    }
    /* A2 */
    
    GENASSERT1(t.priority < maxPriority, t.priority);
    readyLock.acquire();
    readyTree->insert(*(new ThreadNode(&t)));	
    bool wake = (readyCount == 0);
    readyCount += 1;						
    readyLock.release();
    Runtime::debugS("Thread ", FmtHex(&t), " queued on ", FmtHex(this));
    if (wake) Runtime::wakeUp(this);
}

/***********************************
    Gets triggered at every RTC
	interrupt (per Scheduler)
***********************************/
void Scheduler::preempt(){		// IRQs disabled, lock count inflated
	//Get current running thread
	Thread* currentThread = Runtime::getCurrThread();
    
    /* A2 */
    // get the start and end running times
    if(currentThread->runStart < currentThread->runEnd)
    {
        currentThread->runEnd = CPU::readTSC();
    }
    else
    {
        currentThread->runStart = CPU::readTSC();
    }
    /* A2 */
    
	//Get its target scheduler
	Scheduler* target = currentThread->getAffinity();						
	
	//Check if the thread should move to a new scheduler
	//(based on the affinity)
	if(target != this && target){						
		//Switch the served thread on the target scheduler
		switchThread(target);				
	}
	//Check if it is time to switch the thread on the current scheduler
	if(switchTest(currentThread)){
		//Switch the served thread on the current scheduler
		switchThread(this);	
	}
}

/***********************************
    Checks if it is time to stop
	serving the current thread
	and start serving the next
	one
***********************************/
bool Scheduler::switchTest(Thread* t)
{	
	//increas Vruntime properly (by equation give)
	//check time served so far in regards to minGran
		//if it is less than mingran return false
		//if more than mingram
			//check if time slice has past
	//no need for while loop or to mess with isAwake.

    // get the runTime
    mword runTime = (t->runEnd) - (t->runStart);
	
    // update vRuntime
    t->vRuntime+=runTime/(t->priority+1);
    
	if(t->vRuntime<smallestRuntime)
	{
		smallestRuntime=t->vRuntime;
        return false;
	}
    
    // check if the runTime is greater than minGran
    if(runTime>=Scheduler::schedMinGranularityTicks)
    {
        // switch thread
        return true;
    }
    
    // check if the runTime is greater than timeSlice
    if (runTime>=timeSlice)
    {
        // switch thread
        return true;	
    }
    
    // don't switch thread
    return false; 
}

/***********************************
    Switches the current running
	thread with the next thread
	waiting in the tree
***********************************/
template<typename... Args>
inline void Scheduler::switchThread(Scheduler* target, Args&... a) 
{   
    //somewhere in here minVRuntime should be changed.
    preemption += 1;
    CHECK_LOCK_MIN(sizeof...(Args));
    Thread* nextThread;
    readyLock.acquire();

    if(!readyTree->empty())
    {
        nextThread = readyTree->popMinNode()->th;
        
        /* A2 */
        // calculate time slice
        timeSlice=((Scheduler::epochLengthTicks*nextThread->priority+1)/(totalPriority+1)); 
        
        // start TSC
        nextThread->waitTime += CPU::readTSC() - nextThread->startTime;
        
        // update minvRuntime
        smallestRuntime = nextThread->vRuntime;
        
        // check if suspended (if args is > 0 that means there are locks passed in)
        if(sizeof...(Args) > 0)
        {
            nextThread->vRuntime -= smallestRuntime;
            nextThread->isAwake = false;
        }

        // decrease total priority
        totalPriority -= nextThread->priority-1;
        /* A2 */

        readyCount -= 1;
        goto threadFound;
    }

    readyLock.release();
    GENASSERT0(target);
    GENASSERT0(!sizeof...(Args));
    return;                                         // return to current thread

threadFound:
  readyLock.release();
  resumption += 1;
  Thread* currThread = Runtime::getCurrThread();
  
  /* A2 */
  waitTime = CPU::readTSC() - startTime;
  /* A2 */
  
  GENASSERTN(currThread && nextThread && nextThread != currThread, currThread, ' ', nextThread);
  if (target) currThread->nextScheduler = target; // yield/preempt to given processor
  else currThread->nextScheduler = this;          // suspend/resume to same processor
  unlock(a...);                                   // ...thus can unlock now
  CHECK_LOCK_COUNT(1);
  Runtime::debugS("Thread switch <", (target ? 'Y' : 'S'), ">: ", FmtHex(currThread), '(', FmtHex(currThread->stackPointer), ") to ", FmtHex(nextThread), '(', FmtHex(nextThread->stackPointer), ')');

  Runtime::MemoryContext& ctx = Runtime::getMemoryContext();
  Runtime::setCurrThread(nextThread);
  Thread* prevThread = stackSwitch(currThread, target, &currThread->stackPointer, nextThread->stackPointer);
  // REMEMBER: Thread might have migrated from other processor, so 'this'
  //           might not be currThread's Scheduler object anymore.
  //           However, 'this' points to prevThread's Scheduler object.
  Runtime::postResume(false, *prevThread, ctx);
  if (currThread->state == Thread::Cancelled) 
  {
    currThread->state = Thread::Finishing;
    switchThread(nullptr);
    unreachable();
  }
}

/***********************************
    Gets triggered when a thread is 
	suspended
***********************************/
void Scheduler::suspend(BasicLock& lk) 
{
  Runtime::FakeLock fl;
  switchThread(nullptr, lk);
}
void Scheduler::suspend(BasicLock& lk1, BasicLock& lk2) {
  Runtime::FakeLock fl;
  switchThread(nullptr, lk1, lk2);
}

/***********************************
    Gets triggered when a thread is 
	awake after suspension
***********************************/
void Scheduler::resume(Thread& t) {
  /* A2 */
  t.vRuntime+=smallestRuntime;
  /* A2 */

  GENASSERT1(&t != Runtime::getCurrThread(), Runtime::getCurrThread());
  if (t.nextScheduler) t.nextScheduler->enqueue(t);
  else Runtime::getScheduler()->enqueue(t);
}

/***********************************
    Gets triggered when a thread is 
	done but not destroyed yet
***********************************/
void Scheduler::terminate() {
  /* A2
  KOUT::outl();
  KOUT::out1("Total wait time: ");
  KOUT::out1(waitTime);
  KOUT::outl();
  A2 */
  Runtime::RealLock rl;
  Thread* thr = Runtime::getCurrThread();
  GENASSERT1(thr->state != Thread::Blocked, thr->state);
  thr->state = Thread::Finishing;
  switchThread(nullptr);
  unreachable();
}

/***********************************
		Other functions
***********************************/      
extern "C" Thread* postSwitch(Thread* prevThread, Scheduler* target) {
  CHECK_LOCK_COUNT(1);
  if fastpath(target) Scheduler::resume(*prevThread);
  return prevThread;
}

extern "C" void invokeThread(Thread* prevThread, Runtime::MemoryContext* ctx, funcvoid3_t func, ptr_t arg1, ptr_t arg2, ptr_t arg3) {
  Runtime::postResume(true, *prevThread, *ctx);
  func(arg1, arg2, arg3);
  Runtime::getScheduler()->terminate();
}

int priority(int task)
{
	return 1;
}
