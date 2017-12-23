

#ifndef WorkQueue_hpp 
#define WorkQueue_hpp

// TODO: Open it later
#include <cassert>
//#define Assert(e) ((void)0)
#undef Assert
#define Assert assert

#include "../queue/lfringqueue.h"

#include <mutex>
#include <vector>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <memory>

#define DEBUG_DISPATCHER 0

#if DEBUG_DISPATCHER
#include <cstdio>
#include <thread>
#endif

/* Dispacher only provides a default main queue associated with some workers   
 * To add a new queue, for example add a resources load queue (if dispatch resource load task to main queue, it will block the main queue):
 * 1) add a new queue type : ResLoadQueue
 * 2) implements a new worker, ResLoadWorkerThread : public WorkerThread
 * 3) attach the worker to NXDispacher
 * we can do above steps when initializing resource load module 
 */
enum TaskQueueType {
	MainQueue = 0,
	
	ResLoadQueue,
	
	TaskQueueNum
};

bool inline IsValidQueue(TaskQueueType queue) 
{
	return (queue >= 0 && queue < TaskQueueNum);
}

class Task;
class TaskGroup;
class TaskQueue;
class Dispatcher;

class WorkerThread
{
public:
    WorkerThread();
    void SetTaskQueue(TaskQueue* queue);
	void Start();
    void DoWork();
    void ShutDown();

	virtual void OnStart() {};
	virtual void OnShutDown() {};

private:
    TaskQueue* mTaskQueue;
    bool mShutDown;
};

class Task;

/* std::shared_ptr is not thread safe, use this carefully
 * a typical usage scenario, only allow main thread access shared_ptr, work thread only access the real ptr
 */
typedef std::shared_ptr<Task>       TaskShrPtr;
typedef std::vector<TaskShrPtr>     TaskShrPtrArray;

typedef Task* TaskPtr;
typedef std::vector<TaskPtr>    TaskPtrArray;


/*		↓←---------------------(release)-----------------------------------+
 *	Invalid → Contructed → Suspend → Dispatched → Executing → Notifying → Competed →-↑
 *							↑-----------(dispatch)-------------------------↓
 */
enum TaskStatus
{
	// this task is in a invalid status,
	// any method call to this task is undefined
	// only dispatch system can acess a invalid status
	Invalid = -1,
	
	// a new task, and has not been dispached
	Constructed = 1,
	
	Suspend,
	
	// task is in task queue and waitting to be executed
	Dispatched,
	
	// worker thread is executing the task
	Executing,
	
	Notifying,
	
	// this task is complete, you can dispatch this task again
	Completed,
};

class Task
{
public:

	Task(): mStatus(TaskStatus::Invalid), mPrerequisiteNum(0), mWaitting(false), mTaskQueueToRun(MainQueue)
    {
    }
    
    Task(const std::function<void()>& func): mFunc(std::move(func)), mStatus(TaskStatus::Invalid), mPrerequisiteNum(0), mWaitting(false), mTaskQueueToRun(MainQueue)
    {
    }
    
    Task(std::function<void()>* func): mFunc(*func), mStatus(TaskStatus::Invalid), mPrerequisiteNum(0), mWaitting(false), mTaskQueueToRun(MainQueue)
    {
    }
    
    ~Task();
    
    inline void Execute()
    {
		mStatus.store(TaskStatus::Executing);
        mFunc();
        OnCompleted();
    }
    
    inline bool IsComplete()
    {
		return mStatus.load() == TaskStatus::Completed;
    }
	
    inline void SetFunc(std::function<void()> func)
    {
        mFunc = func;
    }
	
	inline void Wait()
	{
		Assert(mStatus.load() >= TaskStatus::Invalid);
		
		// if this task is never dispached, it has nothing to do just return
        if (mStatus.load() == TaskStatus::Constructed || mStatus.load() == TaskStatus::Invalid)
		{
			return;
		}

		{
			std::unique_lock<std::mutex> lock(mLock);
			if (!IsComplete()) {
				mWaitting.store(true);
				mCondition.wait(lock);
			}
		}
		
		return;
	}
	
	inline bool AddSubsequent(Task* task)
	{
		Assert(task->mStatus.load() != TaskStatus::Invalid);
		Assert(mStatus.load() != TaskStatus::Invalid);
		Assert(task != this && task != NULL);
		
		bool ret;
		
		// prerequisite task is self, dead lock
		if (task != this && task != NULL)
		{
			std::lock_guard<std::mutex> lock(mLock);
			
			int status = mStatus.load();
			if (status != TaskStatus::Notifying && status != TaskStatus::Invalid && status != TaskStatus::Completed)
			{
				mSubsequent.push_back(task);
				ret = true;
			}
			else
			{
				ret = false;
			}
		}
		else
		{
			ret = true;
		}
		
		return ret;
	}
	
protected:
	inline void OnDispached()
	{
		std::lock_guard<std::mutex> lock(mLock);
		
		// dispatch system disallow dispatching the same task many times at the same time
		int status = mStatus.load();
		if (!(status == TaskStatus::Constructed || status == TaskStatus::Suspend || status == TaskStatus::Completed))
		{
			Assert(0);
		}
		
		mStatus.store(TaskStatus::Dispatched);
	}
	
	inline void OnCompleted()
	{
		Assert(mStatus.load() == TaskStatus::Executing);
		
		std::lock_guard<std::mutex> lock(mLock);
		
		mStatus.store(TaskStatus::Notifying);
		// notify all tasks depends on this task
		{
			for (auto& task : mSubsequent)
			{
				task->OnPrerequisiteComplete();
			}
			mSubsequent.clear();
		}
		
		//std::atomic_thread_fence(std::memory_order_seq_cst);
		mStatus.store(TaskStatus::Completed);
		
		// wake up all threads waitting for this task
		// at this point, dispach system should nerver change this task anymore, because we are going to wake up waitting thread
		// And we do not know what will happen (may be this task will released)
		{
			//std::lock_guard<std::mutex> lock(mLock);
			if (mWaitting) {
				mWaitting.store(false);
				mCondition.notify_all();
			}
		}
	}
	
	void OnPrerequisiteComplete();
	
protected:
	std::vector<Task*>		mSubsequent;
	std::mutex				mLock;
	std::condition_variable mCondition;
    std::function<void()>   mFunc;
	std::atomic<int>		mPrerequisiteNum;
    std::atomic<int>		mStatus;
	std::atomic<bool>		mWaitting;
	
	TaskQueueType			mTaskQueueToRun;
	
	friend TaskGroup;
	friend TaskQueue;
	friend Dispatcher;
};

class TaskGroup : public Task
{
public:
	TaskGroup();
	
	void AddTask(Task*);
    int  TaskNum() { return (int)mTasks.size(); }
    
private:
	void PrepareGroup();
	void ExtractTask_Functor();
	void Complete_Functor();

private:
	std::vector<Task*>	mTasks;
	TaskShrPtr			mExtractTask;
	
	friend Dispatcher;
};

class TaskEvent
{
public:
    TaskEvent(): mWaitThreadNum(0), mSignal(false)
    {
    }

    void    Wait();
    void    Send();
    int     GetWaitNum();

private:
    std::mutex              mLock;
    std::condition_variable mCondition;
    volatile int            mWaitThreadNum;
    volatile bool           mSignal;
};

class TaskQueue
{
public:
	TaskQueue() : m_queue_impl(2048)
	{
	}

    bool Enqueue(Task* task);
    bool Dequeue(Task*& task);
    
    TaskEvent& GetEvent();
	bool IsEmpty();

	bool AttachWorker(WorkerThread* thread);
    
private:
    TaskEvent mEvent;
   
	lfringqueue<Task> m_queue_impl;

	std::vector<WorkerThread*> mWorkers;

#if 1
    std::atomic<int>    mEnqueueNum;
    std::atomic<int>    mDequeueNum;
#endif
};

class Dispatcher
{
public:
    static Dispatcher* GetInstance();
	static TaskShrPtr ConstructTask(const std::function<void()>& func);
	static TaskGroup* ConstructTaskGroup();

	bool		AttachWorker(WorkerThread* thread, TaskQueueType queue);
    
    bool        Dispatch(Task* task, TaskQueueType queue);
    bool        Dispatch(Task* task, TaskQueueType queue, TaskPtrArray& prerequisite);
	bool		Dispatch(TaskGroup* taskGroup, TaskQueueType queue);
    
    // sync API for game thread
    // so this APIs apply shared ptr of tasks
    // and asume that task will not release when waiting
	void        WaitUntilComplete(Task* task);
	void		WaitUntilComplete(TaskGroup* taskGroup);
    void        WaitUntilComplete(TaskShrPtrArray& task);
    
private:
    Dispatcher();
    
	TaskQueue       mTaskQueue[TaskQueueNum];
};
#endif /* WorkQueue_hpp */
