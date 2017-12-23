
#include "Dispatcher.h"
#include <thread>

#include <windows.h>
#define sleep(n) Sleep(n*1000)

void TaskEvent::Wait()
{
    std::unique_lock<std::mutex> lck(mLock);
    while (!mSignal) {
        mWaitThreadNum++;
        mCondition.wait(lck);
        mWaitThreadNum--;
    }
    mSignal = false;
}

// TODO: 根据任务数和当前睡眠线程来决定唤醒多少个线程
void TaskEvent::Send()
{
    std::unique_lock<std::mutex> lck(mLock);
//     有线程睡着了吗？
    if (mWaitThreadNum) {
        mSignal = true;
        mCondition.notify_one();
    }
}

int TaskEvent::GetWaitNum()
{
    std::unique_lock<std::mutex> lck(mLock);
    return mWaitThreadNum;
}

Task::~Task()
{
	std::lock_guard<std::mutex> lock(mLock);
}

inline void Task::OnPrerequisiteComplete()
{
	//  if the prerequisite task is all compelete, this task is ready to run, dispatch it
	if (mPrerequisiteNum.fetch_sub(1) == 1)
	{
		//bool ok = 
		Dispatcher::GetInstance()->Dispatch(this, mTaskQueueToRun);
		//Assert(ok);
	}
}

TaskGroup::TaskGroup()
{
	SetFunc(std::bind(&TaskGroup::Complete_Functor, this));
	mExtractTask	= Dispatcher::ConstructTask(std::bind(&TaskGroup::ExtractTask_Functor, this));
}

void TaskGroup::AddTask(Task * task)
{
	if (task)
	{
		task->mStatus.store(TaskStatus::Suspend);
		mTasks.push_back(task);
	}
	else
	{
		Assert(0);
	}
}

void TaskGroup::PrepareGroup()
{
    if (mTasks.size() == 0)
        return;
    
	mStatus.store(TaskStatus::Suspend);
	mPrerequisiteNum = (int)mTasks.size();
	for (auto& task : mTasks)
	{
		if (!task->AddSubsequent(this))
		{
			Assert(0);
		}
	}
}

void TaskGroup::ExtractTask_Functor()
{
	for (auto& task : mTasks)
	{
		//bool ok = 
		Dispatcher::GetInstance()->Dispatch(task, mTaskQueueToRun);
		//Assert(ok);
	}
	
	mTasks.clear();
}

void TaskGroup::Complete_Functor()
{
}

bool TaskQueue::IsEmpty()
{
	return m_queue_impl.empty();
}

bool TaskQueue::Enqueue(Task* task)
{
	bool ret_val = false;

	task->OnDispached();
	if (m_queue_impl.enqueue(task)) {
        mEvent.Send();
        mEnqueueNum.fetch_add(1);
		ret_val = true;
    }

	return ret_val;
}

bool TaskQueue::Dequeue(Task*& task)
{
	bool hasTask = m_queue_impl.dequeue(&task);

    if (hasTask) {
        mDequeueNum.fetch_add(1);
    }
    
    return hasTask;
}

TaskEvent& TaskQueue::GetEvent()
{
    return mEvent;
}

bool TaskQueue::AttachWorker(WorkerThread* thread) {
	mWorkers.push_back(thread);
	return true;
}

WorkerThread::WorkerThread() : mShutDown(false)
{
}

void WorkerThread::SetTaskQueue(TaskQueue *queue)
{
    mTaskQueue = queue;
}

void WorkerThread::Start()
{
	std::function<void()> f = std::bind(&WorkerThread::DoWork, this);
    std::thread worker(f);
    worker.detach();
}

void WorkerThread::DoWork()
{
	OnStart();

    do {
        if (mShutDown || !mTaskQueue) {
			break;
        }
        
        Task* task = nullptr;
        if (!mTaskQueue->Dequeue(task)) {
            mTaskQueue->GetEvent().Wait();
            continue;
        }
        
        if (!task) {
            continue;
        }
        
        task->Execute();
    } while (true);

	OnShutDown();
}

void WorkerThread::ShutDown()
{
    mShutDown = true;
}

#define DEFAULT_MAIN_QUEUE_WORKER_NUM 4

#define DEFAULT_RES_LOAD_QUEUE_WORKER_NUM 2

Dispatcher::Dispatcher()
{
	unsigned int mainQueueWorkNum = std::thread::hardware_concurrency();
	mainQueueWorkNum = mainQueueWorkNum ? mainQueueWorkNum : DEFAULT_MAIN_QUEUE_WORKER_NUM;
	
	for (unsigned int i = 0; i <mainQueueWorkNum; i++) {
		WorkerThread* thread = new WorkerThread();
		AttachWorker(thread, MainQueue);
	}

	for (unsigned int i = 0; i < DEFAULT_RES_LOAD_QUEUE_WORKER_NUM; i++) {
		WorkerThread* thread = new WorkerThread();
		AttachWorker(thread, ResLoadQueue);
	}
}

Dispatcher* Dispatcher::GetInstance()
{
    static Dispatcher taskDispatcher;
    return &taskDispatcher;
}

TaskShrPtr Dispatcher::ConstructTask(const std::function<void ()>& func)
{
    Task* task = new Task(func);
	task->mStatus.store(TaskStatus::Constructed);
    return TaskShrPtr(task);
}

TaskGroup* Dispatcher::ConstructTaskGroup()
{
	TaskGroup* group = new TaskGroup;
	group->mStatus.store(TaskStatus::Constructed);
	return group;
}

bool Dispatcher::AttachWorker(WorkerThread* thread, TaskQueueType queue)
{
	if (!IsValidQueue(queue) || !thread) {
		return false;
	}

	TaskQueue* task_queue = &mTaskQueue[queue];
	task_queue->AttachWorker(thread);
	thread->SetTaskQueue(task_queue);
	thread->Start();

	return true;
}

bool Dispatcher::Dispatch(Task *task, TaskQueueType queue)
{
	Assert(task->mPrerequisiteNum.load() == 0);
	
    if (!task)
		return true;
            
	if (!IsValidQueue(queue)) {
		return false;
	}
	
	return mTaskQueue[queue].Enqueue(task);
}

bool Dispatcher::Dispatch(Task* task, TaskQueueType queue, TaskPtrArray& prerequisite)
{
	bool ret = false;
	bool dispachRightNow = false;
	
	if (!task || !IsValidQueue(queue))
		return false;
	
	int num = (int)prerequisite.size();
	
	task->mPrerequisiteNum	=	num;
	task->mTaskQueueToRun	=	queue;
	task->mStatus.store(TaskStatus::Suspend);
	
	if (!num)
	{
		// no prerequisite task, dispatch now
		dispachRightNow = true;
	}
	else
	{
		int completeNum = 0;
		for (auto& preTask : prerequisite)
		{
			if (!preTask->AddSubsequent(task))
				completeNum++;
		}
		
		// all prerequisite is complete, dispatch it
		if (completeNum)
		{
			dispachRightNow = task->mPrerequisiteNum.fetch_sub(completeNum) == completeNum;
		}
	}

	if (dispachRightNow)
	{
		ret = Dispatch(task, queue);
	}
	
	return ret;
}

bool Dispatcher::Dispatch(TaskGroup* taskGroup, TaskQueueType queue)
{
	if (!taskGroup || !IsValidQueue(queue))
	{
		Assert(0);
		return false;
	}
	
    if (taskGroup->TaskNum() == 0)
        return true;
    
	taskGroup->mTaskQueueToRun = queue;
	taskGroup->PrepareGroup();
	return Dispatch(taskGroup->mExtractTask.get(), MainQueue);
}

void Dispatcher::WaitUntilComplete(Task* task)
{
	if (task == nullptr || task->mStatus == TaskStatus::Constructed) {
        return;
    }

	Assert(task->mStatus.load() != TaskStatus::Invalid);
	
    do {
        if (task->IsComplete()) {
            break;
        }

		task->Wait();
    } while (false);
	
	return;
}

void Dispatcher::WaitUntilComplete(TaskGroup* taskGroup)
{
	WaitUntilComplete(taskGroup->mExtractTask.get());
	taskGroup->Wait();
}

void Dispatcher::WaitUntilComplete(TaskShrPtrArray& tasks)
{
	// check task status
	for (size_t i = 0; i < tasks.size(); i++) {
		if(!tasks[i]->IsComplete()) {
			tasks[i]->Wait();
		}
	}
}
