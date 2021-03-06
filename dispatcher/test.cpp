// cplusplusatomic.cpp: 定义控制台应用程序的入口点。
//

#include "Dispatcher.h"
#include <iostream>

int main()
{
	TaskPtrArray depend;

	auto task1 = Dispatcher::GetInstance()->ConstructTask([]() {
		std::cout << "task1 run" << std::endl;
	});
	
	auto task2 = Dispatcher::GetInstance()->ConstructTask([]() {
		std::cout << "task2 run" << std::endl;
	});
	//depend.push_back(task1.get());
	auto task3 = Dispatcher::GetInstance()->ConstructTask([]() {
		std::cout << "task3 run" << std::endl;
	});
	depend.clear();
	depend.push_back(task2.get());
	depend.push_back(task3.get());
	Dispatcher::GetInstance()->Dispatch(task1.get(), MainQueue, depend);
	depend.clear();
	Dispatcher::GetInstance()->Dispatch(task2.get(), ResLoadQueue, depend);
	Dispatcher::GetInstance()->Dispatch(task3.get(), ResLoadQueue, depend);
	Dispatcher::GetInstance()->WaitUntilComplete(task1.get());
    return 0;
}

