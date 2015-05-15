#include "VirtualMachine.h"
#include "Machine.h"
#include <iostream>
#include <unistd.h>
#include <vector>
#include <string>
using namespace std;

typedef struct ThreadControlBlock{
		TVMThreadID t_id;
		TVMThreadPriority t_priority;
		TVMThreadState t_state;
		TVMMemorySize t_mem_size;
		void* stackPtr;		
		TVMThreadEntry t_entry;
		void* entryParam;		
		SMachineContext t_context;
		TVMTick t_ticks;
		vector<TVMMutexID> t_mutexs_held;
		int fd;		
} TCB;

typedef struct MemoryPool{
		TVMMemorySize pool_mem_size;
		TVMMemoryPoolID pool_mem_id; 
		uint8_t *base; 
} MP; 

typedef struct MutexBlock{
	TVMMutexID m_id;
	TVMThreadID m_owner_id;
	vector<TVMThreadID> m_high_waiting_threads;
	vector<TVMThreadID> m_normal_waiting_threads;
	vector<TVMThreadID> m_low_waiting_threads;
} Mutex;

// Global Variables ------------------------------------------------------
vector<TCB> allThreads;
vector<Mutex> allMutexs;
vector<TVMThreadID> file_waiting_threads;
vector<TVMThreadID> highPrio_rdy_threads, normalPrio_rdy_threads, lowPrio_rdy_threads;
vector<TVMThreadID> sleeping_threads;
TVMThreadID runningThreadID;
TVMThreadID idleThreadID;
const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;

extern "C" {
	
// Function Prototypes ---------------------------------------------------
TVMMainEntry VMLoadModule(const char *module);
TVMStatus VMThreadSleep(TVMTick tick);
void theCallbackFn(void* calldata);
void idleFunction(void* calldata);
void schedule(void);
void enqueue(TVMThreadID argThreadID);
void dequeueAll(TVMThreadID argThreadID);
void removeFromFileWaitingThreadQueue (TVMThreadID argThreadID);
void skeletonEntry(void* param);
void fileCallback(void *calldata, int result);
void enqueueToMutex(TVMMutexID mutex, TVMThreadID thread);
TVMThreadID getNextMutexOwner(TVMMutexID arg);

// Function Definitions --------------------------------------------------

TVMStatus VMStart(int tickms, TVMMemorySize heapsize,  int machinetickms, TVMMemorySize sharedsize, int argc,char *argv[]) {
	const int timeout = 50000;
	// Create Main Thread & set it as runningThreadID
	TCB mainThread;
	mainThread.t_id = allThreads.size();
	mainThread.t_priority = VM_THREAD_PRIORITY_NORMAL;
	runningThreadID = mainThread.t_id;
	mainThread.t_state = VM_THREAD_STATE_RUNNING;
	allThreads.push_back(mainThread);	

	
	// Setup idleThread
	TCB idleThread;
	idleThread.t_id = allThreads.size();
	idleThread.t_priority = VM_THREAD_PRIORITY_LOW;
	idleThread.t_state = VM_THREAD_STATE_READY;
	idleThread.t_entry = &idleFunction;
	idleThread.t_mem_size = 64000;
	idleThread.stackPtr = new uint8_t[idleThread.t_mem_size];	
	
	// This creates the context for idleThread
	MachineContextCreate(&(idleThread.t_context), &skeletonEntry, 0, idleThread.stackPtr, idleThread.t_mem_size);
	
	allThreads.push_back(idleThread);		
	idleThreadID = idleThread.t_id;
	// DO NOT ENQUEUE idle thread. Treat it as the ELSE thing to do in schedule.

	
	// Set the fn ptr to the address returned from the VMLoadModule
	TVMMainEntry vmMainPtr = VMLoadModule((const char*) argv[0]);	
	
	// Make sure the address returned isn't an error
	if(!vmMainPtr) {
		return VM_STATUS_FAILURE;
	}
	
	// Initialize machine with timeout
	MachineInitialize(timeout);
		
	// Set pointer to callback functions.
	TMachineAlarmCallback callbackPtr = &theCallbackFn;
		
	// Set MachineRequestAlarm
	MachineRequestAlarm(tickms*1000, callbackPtr, 0);

	// Enable Signals
	MachineEnableSignals();
		
	// Call the function pointed at by the VMLoadModule

	vmMainPtr (argc, argv);

	// Once you successfully return from whatever VMMain was pointer to
	// you can return, saying it was a success.
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int filedescriptor, void *data, int *length) {

	if (!data || !length)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	
	int returnCode = write(filedescriptor, data, *length);
	
	//State changes to waiting 
	
	//Add to the queue 
	
	//Call MachineFileWrite 
	//Call SCheduler 
	
	// *length = running thread FD 
	
	if(returnCode < 0)
		return VM_STATUS_FAILURE;
	else 
		return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadSleep(TVMTick tick) {

	if(tick == VM_TIMEOUT_INFINITE) {
		return VM_STATUS_ERROR_INVALID_PARAMETER;	
	}
	
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	
	if(tick == VM_TIMEOUT_IMMEDIATE) {
		// 1. Mark running process as ready (currently its "running")
		allThreads[runningThreadID].t_state = VM_THREAD_STATE_READY;
		
		// 2. Put it at the back of the readyqueue
		enqueue(runningThreadID);
		
		// 3. Schedule
		schedule();
		
		// 4. Resume Signals
		MachineResumeSignals(&OldState);
		return VM_STATUS_SUCCESS;
	}	
	

	
	// 1. Set current thread (in runningThreads)'s state to waiting	
	allThreads[runningThreadID].t_state = VM_THREAD_STATE_WAITING;
	// 2. Set current thread (in runningThreads)'s tick to the argTick
	allThreads[runningThreadID].t_ticks = tick;
	// 3. Add to sleeping vector
	sleeping_threads.push_back(runningThreadID);
	// 4. schedule() // probably to idle
	//cout << "Sleep is calling schedule()" << endl;

	schedule();	// running already set to WAITING
	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;	
}

void theCallbackFn(void* calldata) { 
	std::vector<TVMThreadID>::const_iterator i;
	
	// iterate through the vector of sleeping threads
	for(i = sleeping_threads.begin(); i != sleeping_threads.end(); ++i) {
		
		// decrement each threads' tick by 1	
		(allThreads[(*i)].t_ticks)--;
		
		// if any of the vector's thread's tick hits zero
		if(allThreads[(*i)].t_ticks == 0) {
			//cout << "CallBack(): Hey! thread# " << (*i) << "just woke up!" << endl;
			
			// make it's state ready
			allThreads[(*i)].t_state = VM_THREAD_STATE_READY;
				
			//push to ready queue
			enqueue(*i);			
			
			// Change running thread's state
			allThreads[runningThreadID].t_state = VM_THREAD_STATE_READY;
			
			//Schedule
			schedule();
			
			return;
		}
	}
}

void schedule() {

	// running thread's state should ALREADY BE MODIFIED BEFORE CALLING SCHEDULE
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	
	if(!highPrio_rdy_threads.empty()) {
		if(runningThreadID == highPrio_rdy_threads.front()) {
			// cout << "First high priority thread is already running." << endl;
			MachineResumeSignals(&OldState);
			return;
		}
		
		// Otherwise switch context.
		//cout << "Time to switch to a different high priority thread!" << endl;
		
		TVMThreadID temp = runningThreadID;
		runningThreadID = highPrio_rdy_threads.front();
		
		highPrio_rdy_threads.erase(highPrio_rdy_threads.begin());		// Taking it out of READY QUEUE only!
		
		MachineResumeSignals(&OldState);
		MachineContextSwitch(&allThreads[temp].t_context, &allThreads[runningThreadID].t_context);
		// DO NOTHING AFTER CONTEXT SWITCH ERROR. NO WAY EVER!
	}
	
	else if(!normalPrio_rdy_threads.empty()) {
		if(runningThreadID == normalPrio_rdy_threads.front()) {
		//	cout << "First normal priority thread is already running." << endl;
 			MachineResumeSignals(&OldState);
			return;
		}
		
		// Otherwise switch context.
		//cout << "Time to switch to a different normal priority thread!" << endl;
		
		TVMThreadID temp = runningThreadID;
		runningThreadID = normalPrio_rdy_threads.front();

		//cout << "\n Current thread id = " << temp << "\nTrying to switch to ID: " << runningThreadID << endl;
		
		normalPrio_rdy_threads.erase(normalPrio_rdy_threads.begin());		// Taking it out of READY QUEUE only!
		MachineResumeSignals(&OldState);
		MachineContextSwitch(&allThreads[temp].t_context, &allThreads[runningThreadID].t_context);
		// DO NOTHING AFTER CONTEXT SWITCH ERROR. NO WAY EVER!
	}
	
	else if (!lowPrio_rdy_threads.empty()) {
		if(runningThreadID == lowPrio_rdy_threads.front()) {
			//cout << "First low priority thread is already running." << endl;
 			MachineResumeSignals(&OldState);
			return;
		}
		
		// Otherwise switch context.
		//cout << "Time to switch to a different low priority thread!" << endl;
		
		TVMThreadID temp = runningThreadID;
		runningThreadID = lowPrio_rdy_threads.front();
		
		lowPrio_rdy_threads.erase(lowPrio_rdy_threads.begin());		// Taking it out of READY QUEUE only!
		MachineResumeSignals(&OldState);
		MachineContextSwitch(&allThreads[temp].t_context, &allThreads[runningThreadID].t_context);
		// DO NOTHING AFTER CONTEXT SWITCH ERROR. NO WAY EVER!
	}
	
	else {
		// If all are empty, switch to idle thread.
	//	cout << "Time to switch to idle thread!" << endl;	
	
		TVMThreadID temp = runningThreadID;
		runningThreadID = idleThreadID;		
		
		MachineResumeSignals(&OldState);
		MachineContextSwitch(&allThreads[temp].t_context, &allThreads[runningThreadID].t_context);
	}	

}	// End of schedule()

void skeletonEntry(void* param) {

	// 1. Enable Signals
	MachineEnableSignals();
		
	// 2. Call Entry
	void* myParam = allThreads[runningThreadID].entryParam;
	allThreads[runningThreadID].t_entry(myParam);
		
	// 3. Terminate Thread
	VMThreadTerminate(runningThreadID);
	return;
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid) {
	// Return error if  entry ot tid is NULL
	if(!entry || !tid)
	return VM_STATUS_ERROR_INVALID_PARAMETER;
	
	// 1. Suspend signals
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	
	// 2. Initiate Thread			
	TCB newThread;
	newThread.t_id = allThreads.size();
	newThread.t_priority = prio;
	newThread.t_state = VM_THREAD_STATE_DEAD;
	newThread.t_mem_size = memsize;
	newThread.stackPtr = new uint8_t[memsize];
	newThread.t_entry = entry;
	newThread.entryParam = param;
	
	// 3. Store thread object
	allThreads.push_back(newThread);
	
	// 4. Store thread id in param's id
	*tid = newThread.t_id;
	
	// 6. Resume signals and return
	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadActivate(TVMThreadID thread) {

	int found = 0;
	vector<TCB>::iterator i;
	
	for(i = allThreads.begin(); i != allThreads.end(); ++i) {
		if(i->t_id == thread) {
			found = 1;
			break;
		}
	}
	if(!found)
		return VM_STATUS_ERROR_INVALID_ID;
	if(i->t_state != VM_THREAD_STATE_DEAD)
		return VM_STATUS_ERROR_INVALID_STATE;
	// Note: By now, iterator i should have the correct thread.

	// 1. Suspend signals
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	
	// 2. Create Context for the thread we are activating. 	// Good Example: MachineContextCreate(&(idleThread.t_context), &skeletonEntry, 0, idleThread.stackPtr, idleThread.t_mem_size);	
	MachineContextCreate(&(i->t_context), &skeletonEntry, i->entryParam, i->stackPtr, i->t_mem_size);
	
	// 3. Set Thread State to Ready.
	i->t_state = VM_THREAD_STATE_READY;
	
	// 4. Push into queue according to priority.
	enqueue(i->t_id);
	
	// 5. Reschedule if needed.
	if(i->t_priority > allThreads[runningThreadID].t_priority) {
		allThreads[runningThreadID].t_priority = VM_THREAD_STATE_READY;
		enqueue(allThreads[runningThreadID].t_id);
		schedule();
	}
	
	// 6. Resume signals.
	MachineResumeSignals(&OldState);
	
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadID(TVMThreadIDRef threadref) {

	if(!threadref)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	
	vector<TCB>::iterator i;
	for(i = allThreads.begin(); i != allThreads.end(); ++i) {
		if(i->t_id == runningThreadID) {
			*threadref = runningThreadID;
			return VM_STATUS_SUCCESS;
		}
	}
	return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef state){

	if(!state)
		return VM_STATUS_ERROR_INVALID_PARAMETER;	
	
	vector<TCB>::iterator i;
	for(i = allThreads.begin(); i != allThreads.end(); ++i) {
		if(i->t_id == thread) {
			*state = i->t_state;
			return VM_STATUS_SUCCESS;
		}
	}
	return VM_STATUS_ERROR_INVALID_ID;
}

TVMStatus VMThreadTerminate(TVMThreadID thread) {
	vector<TCB>::iterator i;
	int found = 0;
	for(i = allThreads.begin(); i != allThreads.end(); ++i) {
		if(i->t_id == thread) {
			found = 1;
			break;
		}
	}
	if(!found)
		return VM_STATUS_ERROR_INVALID_ID;
	if(i->t_state == VM_THREAD_STATE_DEAD)
		return VM_STATUS_ERROR_INVALID_STATE;
	
	// 1. Set state to dead.
	i->t_state = VM_THREAD_STATE_DEAD;
	
	// 2. Dequeue from all except allThreads.
	dequeueAll(i->t_id);
	
	// 3. Reschedule!!
	schedule();
	
	return VM_STATUS_SUCCESS;
}

void dequeueAll(TVMThreadID argThreadID) {
	// This function weill remove the arg thread from 
	// sleep, and all priority queues.
	
	vector<TVMThreadID>::iterator i;
	int found = 0;
	
	// 1. Deal with sleeping_threads.	
	for(i = sleeping_threads.begin(); i != sleeping_threads.end(); ++i) {
		if(*i == argThreadID)
			break;			
	}
	sleeping_threads.erase(i);
	
	// 2. Deal with priority queues	
	TVMThreadPriority priority = allThreads[argThreadID].t_priority;	
	
	if(priority == VM_THREAD_PRIORITY_HIGH) {
		for(i = highPrio_rdy_threads.begin(); i != highPrio_rdy_threads.end(); ++i) {
			if(*i == argThreadID) {
				found = 1;
				break;		
			}				
		}
		if(found)
			highPrio_rdy_threads.erase(i);
	}
	else if (priority == VM_THREAD_PRIORITY_NORMAL){
		for(i = normalPrio_rdy_threads.begin(); i != normalPrio_rdy_threads.end(); ++i) {
			if(*i == argThreadID) {
				found = 1;
				break;			
			}
		}
		if(found) 
			normalPrio_rdy_threads.erase(i);		
	}
	else if (priority == VM_THREAD_PRIORITY_LOW) {
		for(i = lowPrio_rdy_threads.begin(); i != lowPrio_rdy_threads.end(); ++i) {
			if(*i == argThreadID) {
				found = 1;				
				break;
			}
		}
		if(found)
			lowPrio_rdy_threads.erase(i);
	}
	else {
		cout << "dequeueAll Error: Invalid Priority of passed thread." << endl;
	}	
	return;
}

void idleFunction(void* calldata) {
	while(1){
		// do nothing
	}
	return;
}

void enqueue(TVMThreadID argThreadID){

	TVMThreadPriority priority = allThreads[argThreadID].t_priority;
	
	if(priority == VM_THREAD_PRIORITY_LOW) {
		lowPrio_rdy_threads.push_back(argThreadID);
	}
	else if (priority == VM_THREAD_PRIORITY_NORMAL) {
		normalPrio_rdy_threads.push_back(argThreadID);
	}
	else if (priority == VM_THREAD_PRIORITY_HIGH) {
		highPrio_rdy_threads.push_back(argThreadID);
	}
	else {
		cout << "Error: Invalid thread priority in enqueue()." << endl;
		return;
	}
	/*
	string prio;
	if(priority == 1)
		prio = "low";
	else if (priority == 2)
		prio = "normal";
	else if (priority == 3)
		prio = "high";
	else
		prio = "error";
	
	//cout << "Enqueue: Added thread  with id# " << argThreadID << 
	//" to " << prio << " ready queue." << endl;
	*/
	return;
}

void removeFromFileWaitingThreadQueue (TVMThreadID argThreadID) {

	vector<TVMThreadID>::iterator i;
	int found = 0;

	for(i = file_waiting_threads.begin(); i != file_waiting_threads.end(); ++i) {
		if(*i == argThreadID)
			break;			
	}
	if (found)
		file_waiting_threads.erase(i);
	return;
}



void fileCallback(void *calldata, int result) {
	cout << "You are in call back!" << endl;
	
	TCB* argThread = (TCB*) calldata;
       
  argThread->t_state = VM_THREAD_STATE_READY;
  argThread->fd = result;
        
	enqueue(argThread->t_id);
				
	// Reschedule
	allThreads[runningThreadID].t_state = VM_THREAD_STATE_READY;
	enqueue(allThreads[runningThreadID].t_id);
  schedule();

/*
My old implementation
	cout << "You are in file callback." << endl;
	
	TVMThreadID arg = *((TVMThreadIDRef) calldata);
	allThreads[arg].fd = result;
	allThreads[arg].t_state = VM_THREAD_STATE_READY;
	
	removeFromFileWaitingThreadQueue(allThreads[arg].t_id);
	enqueue(allThreads[arg].t_id);

	schedule();	
	*/
	
/*	       threadControlBlock *thr = (threadControlBlock*) calldata;
        thr->state = VM_THREAD_STATE_READY;
        thr->result = result;
        if (thr->prio == VM_THREAD_PRIORITY_LOW)
                lowQueue.push(thr);
 
        else if (thr->prio == VM_THREAD_PRIORITY_NORMAL)
                normalQueue.push(thr);
 
        else if (thr->prio == VM_THREAD_PRIORITY_HIGH)
                highQueue.push(thr);
        Schedule();
*/
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
	
	if(!filename || !filedescriptor)					// Check for errors
		return VM_STATUS_ERROR_INVALID_PARAMETER;

	TMachineSignalState OldState; 						// Suspend Signals
	MachineSuspendSignals(&OldState);
	
	allThreads[runningThreadID].t_state = VM_THREAD_STATE_WAITING;		// mark running thread as waiting
	file_waiting_threads.push_back(runningThreadID);									// put it in the waiting queue
	
	
	TMachineFileCallback fooPtr = &fileCallback;
	TCB* currentThreadPtr = &allThreads[runningThreadID];
	
	
	MachineFileOpen(filename, flags, mode, fooPtr, currentThreadPtr);
	
	//Reschedule
	allThreads[runningThreadID].t_state = VM_THREAD_STATE_READY;
	enqueue(allThreads[runningThreadID].t_id);
	schedule();						
	
	*filedescriptor = currentThreadPtr->fd;

	MachineResumeSignals(&OldState);

	if(allThreads[runningThreadID].fd == 0)
		return VM_STATUS_SUCCESS;
	else
		return VM_STATUS_FAILURE;
	
		
		/*         TVMStatus Status = VM_STATUS_SUCCESS;
        if (filename == NULL || filedescriptor == NULL)
        {
                Status = VM_STATUS_ERROR_INVALID_PARAMETER;
                return Status;
        }
        TMachineSignalState Oldstate;
        MachineSuspendSignals(&Oldstate);
        threadControlBlock *myThread = threads[curThreadID];
        myThread->state = VM_THREAD_STATE_WAITING;
        MachineFileOpen(filename, flags, mode, MyMachineFileCallback, myThread);
        Schedule();
        *filedescriptor = myThread->result;
 
        if (!(myThread->result > 0))
                return VM_STATUS_FAILURE;
 
        MachineResumeSignals(&Oldstate);
        return Status;
}
 
 
 
void MyMachineFileCallback(void *calldata, int result){
        threadControlBlock *thr = (threadControlBlock*) calldata;
        thr->state = VM_THREAD_STATE_READY;
        thr->result = result;
        if (thr->prio == VM_THREAD_PRIORITY_LOW)
                lowQueue.push(thr);
 
        else if (thr->prio == VM_THREAD_PRIORITY_NORMAL)
                normalQueue.push(thr);
 
        else if (thr->prio == VM_THREAD_PRIORITY_HIGH)
                highQueue.push(thr);
        Schedule();
*/

}

TVMStatus VMFileClose(int filedescriptor){
	TMachineSignalState OldState; 
	MachineSuspendSignals(&OldState);
	
	//State changes to waiting 
	
	//Add to the queue 
	
	//Call machine file to close 
	//MachineFileClose 
	//Call scheduler 
	
	//See if result value, FD,  is 0, 
	//If result value is 0 than success
	//Otherwise failure
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
	TMachineSignalState OldState; 
	MachineSuspendSignals(&OldState);
	
	if(!data || !length)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
		
	//State changes to waiting 
	
	//Add to the queue 
	
	//Call MachineFileRead 
	//Call scheduler 
	
	//If result, FD, is less than 0 
	// return VM_STATUS_FAILURE;
	// *length = running thread FD  
	//otherwise success 
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
	TMachineSignalState OldState; 
	MachineSuspendSignals(&OldState);
	
	//State changes to waiting 
	
	//Add to the queue 
	
	//Call MachineFileSeek 
	//Call scheduler 
	
	//See if result, FD,  of value is 
	//less than 0, if <0 then failure
	//Otherwise success
	
	//*newoffset = running thread FD 
	return VM_STATUS_SUCCESS;
}





void enqueueToMutex(TVMMutexID mutex, TVMThreadID thread) {
	TVMThreadPriority priority = allThreads[thread].t_priority;
	
	if(priority == VM_THREAD_PRIORITY_LOW) {
		(allMutexs[mutex].m_low_waiting_threads).push_back(thread);
	//	cout << "Pushed to low prio list. New size of waiting list = " << (allMutexs[mutex].m_low_waiting_threads).size() << endl;
	}
	else if (priority == VM_THREAD_PRIORITY_NORMAL) {
		(allMutexs[mutex].m_normal_waiting_threads).push_back(thread);
		//cout << "Pushed to norm prio list. New size of waiting list = " << (allMutexs[mutex].m_normal_waiting_threads).size() << endl;
	}
	else if (priority == VM_THREAD_PRIORITY_HIGH) {
		(allMutexs[mutex].m_high_waiting_threads).push_back(thread);
		//cout << "Pushed to high prio list. New size of waiting list = " << (allMutexs[mutex].m_high_waiting_threads).size() << endl;
	}
	else {
		//cout << "Error: Invalid thread priority in enqueueToMutex()." << endl;
	}	
	return;
}
	
TVMStatus VMMutexCreate(TVMMutexIDRef mutexref) {
	// Return error if mutexref is NULL
	if(!mutexref)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	
	// 1. Suspend signals
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	
	// 2. Initiate Mutex			
	Mutex newMutex;
	newMutex.m_id = allMutexs.size();
	newMutex.m_owner_id = VM_THREAD_ID_INVALID;
	
	// 3. Store thread object
	allMutexs.push_back(newMutex);
	
	// 4. Store thread id in param's id
	*mutexref = newMutex.m_id;
	
	// 6. Resume signals and return
	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout) {
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);

	int found = 0;
	vector<Mutex>::iterator i;
	
	// Verify it's a valid mutex.
	for(i = allMutexs.begin(); i != allMutexs.end(); ++i) {
		if(i->m_id == mutex) {
			found = 1;
			break;
		}
	}
	if(!found) {
		cout << "Aquire() Error: Invalid ID!" << endl;
		MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_ID;
	}

	if(allMutexs[mutex].m_owner_id == VM_THREAD_ID_INVALID) {				// If mutex is unlocked
		allMutexs[mutex].m_owner_id = runningThreadID;						// change the owner id aka lock
		(allThreads[runningThreadID].t_mutexs_held).push_back(mutex);		// add it to the list of mutexs held by thread
		MachineResumeSignals(&OldState);									// resume signals		
		return VM_STATUS_SUCCESS;											// return, no need to reschedule.
	}	

	else if(timeout == VM_TIMEOUT_IMMEDIATE) {								// If mutex is locked, and timeout is immediate
		MachineResumeSignals(&OldState);									// Resume signals
		return VM_STATUS_FAILURE;											// Return failure
	}

	else if(timeout == VM_TIMEOUT_INFINITE) {								// If mutex is locked, and timeout is infinite
		
		enqueueToMutex(mutex, runningThreadID);								// Add running thread to corresponding waitlist for mutex.
		allThreads[runningThreadID].t_state = VM_THREAD_STATE_WAITING;		// Mark running thread as WAITING

		MachineResumeSignals(&OldState);									// Resume signals.
		schedule();															// Schedule again
		
		if(allMutexs[mutex].m_id == runningThreadID) {
			MachineResumeSignals(&OldState);
			return VM_STATUS_SUCCESS;
		}
		else {
			MachineResumeSignals(&OldState);
			return VM_STATUS_FAILURE;
		}
	}
	
	else {																	// If timeout is a number	
		
		enqueueToMutex(mutex, runningThreadID);								// Add running thread to corresponding waitlist for mutex.
		allThreads[runningThreadID].t_ticks = timeout;
		allThreads[runningThreadID].t_state = VM_THREAD_STATE_WAITING;
		sleeping_threads.push_back(allThreads[runningThreadID].t_id);
		
		MachineResumeSignals(&OldState);
		schedule();
		return VM_STATUS_SUCCESS;
	}
}

TVMStatus VMMutexRelease(TVMMutexID mutex) {
	int found = 0;
	vector<Mutex>::iterator i;	
	vector<TVMThreadID>::iterator itr;
	
	TMachineSignalState OldState;
	MachineSuspendSignals(&OldState);
	
	// Verify it's a valid mutex.
	for(i = allMutexs.begin(); i != allMutexs.end(); ++i) {
		if(i->m_id == mutex) {
			found = 1;
			break;
		}
	}
	if(!found) {
		MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	
	// If Mutex is already unlocked, return error.
	if(allMutexs[mutex].m_owner_id == VM_THREAD_ID_INVALID) {
		MachineResumeSignals(&OldState);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	
	// Change ownership of mutex back to VM_THREAD_ID_INVALID
	allMutexs[mutex].m_owner_id = VM_THREAD_ID_INVALID;
	
	// remove from previous owner's "held" vector
	TVMThreadID previousOwner = allMutexs[mutex].m_owner_id;
	for(itr = allThreads[previousOwner].t_mutexs_held.begin(); itr != allThreads[previousOwner].t_mutexs_held.begin(); ++i) {
		if(*itr == allMutexs[mutex].m_owner_id) {
			allThreads[previousOwner].t_mutexs_held.erase(itr);
			break;
		}
	}
	
	if(!(allMutexs[mutex].m_high_waiting_threads).empty() ||		// If any of the waiting queues has threads
	!(allMutexs[mutex].m_normal_waiting_threads).empty() ||
	!(allMutexs[mutex].m_low_waiting_threads).empty()) {
		
		TVMThreadID nextOwner = getNextMutexOwner(mutex);			// Get the waiting thread w/ highest priority
		allMutexs[mutex].m_owner_id = nextOwner;					// Assign the mutex to it
		enqueue(nextOwner);											// Put it in the corresponding ready queue
		
		if(allThreads[nextOwner].t_priority > allThreads[runningThreadID].t_priority) {
			allThreads[runningThreadID].t_state = VM_THREAD_STATE_READY;
			enqueue(runningThreadID);
			MachineResumeSignals(&OldState);
			schedule();
			return VM_STATUS_SUCCESS;
		}

	}

	MachineResumeSignals(&OldState);
	return VM_STATUS_SUCCESS;		

}

TVMThreadID getNextMutexOwner(TVMMutexID arg) {
	if(!(allMutexs[arg].m_high_waiting_threads.empty())) {
	//	cout << "High Priority waiting size = " << allMutexs[arg].m_high_waiting_threads.size() << endl;
		//cout << "Returning this exact value: " << allMutexs[arg].m_high_waiting_threads.front() << endl;
		return allMutexs[arg].m_high_waiting_threads.front();
	}
	if(!(allMutexs[arg].m_normal_waiting_threads.empty())) {
	//	cout << "norm Priority waiting size = " << allMutexs[arg].m_normal_waiting_threads.size() << endl;
	//	cout << "Returning this exact value: " << allMutexs[arg].m_normal_waiting_threads.front() << endl;
		return allMutexs[arg].m_normal_waiting_threads.front();
	}
	if(!(allMutexs[arg].m_low_waiting_threads.empty())) {
		
	//	cout << "low Priority waiting size = " << allMutexs[arg].m_low_waiting_threads.size() << endl;
	//	cout << "Returning this exact value: " << allMutexs[arg].m_low_waiting_threads.front() << endl;
		return allMutexs[arg].m_low_waiting_threads.front();
	}
	cout << "getNextMutexOwner Error: All waitlists are empty." << endl;
	return VM_THREAD_ID_INVALID;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref) {
	if(!ownerref)
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	int found = 0;
	vector<Mutex>::const_iterator i;
	
	for(i = allMutexs.begin(); i != allMutexs.end(); ++i) {
		if(i->m_id == mutex) {
			found = 1;
			break;
		}	
	}
	if(!found)
		return VM_STATUS_ERROR_INVALID_ID;
	
	*ownerref = allMutexs[mutex].m_owner_id;
	return VM_STATUS_SUCCESS;
}

} // End of extern block
