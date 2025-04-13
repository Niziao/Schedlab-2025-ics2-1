#include <vector>
#include <queue>
#include <map>
#include <unordered_set>
#include <algorithm>
#include "policy.h"

using namespace std;

struct TaskInfo {
    int id;
    int arrivalTime;
    int deadline;
    Event::Task::Priority priority;
    enum State {
        READY,
        RUNNING_CPU,
        WAITING_IO,
        RUNNING_IO,
        FINISHED
    };
    State state;
};

// Comparator for CPU tasks: prioritize by deadline, then by priority, then by arrival time
struct CompareCPUTask {
    bool operator()(const TaskInfo& a, const TaskInfo& b) {
        // First compare by deadline (earlier deadline has higher priority)
        if (a.deadline != b.deadline) return a.deadline > b.deadline;
        
        // Then compare by priority (higher priority has higher priority)
        if (a.priority != b.priority) return a.priority < b.priority;
        
        // Finally compare by arrival time (earlier arrival has higher priority)
        return a.arrivalTime > b.arrivalTime;
    }
};

// Comparator for IO tasks: prioritize by deadline, then by arrival time
struct CompareIOTask {
    bool operator()(const TaskInfo& a, const TaskInfo& b) {
        // First compare by deadline (earlier deadline has higher priority)
        if (a.deadline != b.deadline) return a.deadline > b.deadline;
        
        // Then compare by arrival time (earlier arrival has higher priority)
        return a.arrivalTime > b.arrivalTime;
    }
};

// Global state
static map<int, TaskInfo> all_tasks;
static unordered_set<int> valid_tasks;
static priority_queue<TaskInfo, vector<TaskInfo>, CompareCPUTask> cpu_ready_queue;
static priority_queue<TaskInfo, vector<TaskInfo>, CompareIOTask> io_waiting_queue;

// Clean up queues to remove invalid tasks
void clean_queues() {
    // Clean CPU queue
    priority_queue<TaskInfo, vector<TaskInfo>, CompareCPUTask> new_cpu;
    while (!cpu_ready_queue.empty()) {
        auto& t = cpu_ready_queue.top();
        if (valid_tasks.count(t.id) && all_tasks[t.id].state == TaskInfo::READY) {
            new_cpu.push(t);
        }
        cpu_ready_queue.pop();
    }
    cpu_ready_queue = move(new_cpu);

    // Clean IO queue
    priority_queue<TaskInfo, vector<TaskInfo>, CompareIOTask> new_io;
    while (!io_waiting_queue.empty()) {
        auto& t = io_waiting_queue.top();
        if (valid_tasks.count(t.id) && all_tasks[t.id].state == TaskInfo::WAITING_IO) {
            new_io.push(t);
        }
        io_waiting_queue.pop();
    }
    io_waiting_queue = move(new_io);
}

// Main scheduling policy function
Action policy(const vector<Event>& events, int current_cpu, int current_io) {
    // Process all events
    for (const auto& e : events) {
        switch (e.type) {
            case Event::Type::kTaskArrival: {
                const int task_id = e.task.taskId;
                if (!valid_tasks.count(task_id)) {
                    TaskInfo task{
                        task_id,
                        e.task.arrivalTime,
                        e.task.deadline,
                        e.task.priority,
                        TaskInfo::READY
                    };
                    all_tasks[task_id] = task;
                    valid_tasks.insert(task_id);
                    cpu_ready_queue.push(task);
                }
                break;
            }
            case Event::Type::kIoRequest: {
                const int task_id = e.task.taskId;
                if (valid_tasks.count(task_id)) {
                    auto& task = all_tasks[task_id];
                    task.state = TaskInfo::WAITING_IO;
                    io_waiting_queue.push(task);
                }
                break;
            }
            case Event::Type::kIoEnd: {
                const int task_id = e.task.taskId;
                if (valid_tasks.count(task_id)) {
                    auto& task = all_tasks[task_id];
                    task.state = TaskInfo::READY;
                    cpu_ready_queue.push(task);
                }
                break;
            }
            case Event::Type::kTaskFinish: {
                const int task_id = e.task.taskId;
                valid_tasks.erase(task_id);
                all_tasks.erase(task_id);
                break;
            }
            case Event::Type::kTimer:
            default:
                break;
        }
    }

    // Clean up queues to remove invalid tasks
    clean_queues();

    // Handle current CPU task
    if (current_cpu != 0 && valid_tasks.count(current_cpu)) {
        auto& task = all_tasks[current_cpu];
        if (task.state == TaskInfo::RUNNING_CPU) {
            task.state = TaskInfo::READY;
            cpu_ready_queue.push(task);
        }
    }

    // Select new CPU task
    int new_cpu = 0;
    
    // First, try to find a high priority task with a deadline approaching
    priority_queue<TaskInfo, vector<TaskInfo>, CompareCPUTask> temp_cpu = cpu_ready_queue;
    while (!temp_cpu.empty()) {
        const auto& task = temp_cpu.top();
        if (valid_tasks.count(task.id) && all_tasks[task.id].state == TaskInfo::READY) {
            // Check if this is a high priority task with a deadline approaching
            if (task.priority == Event::Task::Priority::kHigh && 
                task.deadline - events[0].time < 200) {  // Deadline is within 200 time units
                new_cpu = task.id;
                all_tasks[task.id].state = TaskInfo::RUNNING_CPU;
                break;
            }
        }
        temp_cpu.pop();
    }
    
    // If no high priority task with approaching deadline, use the highest priority task
    if (new_cpu == 0) {
        while (!cpu_ready_queue.empty()) {
            const auto& task = cpu_ready_queue.top();
            if (valid_tasks.count(task.id) && all_tasks[task.id].state == TaskInfo::READY) {
                new_cpu = task.id;
                all_tasks[task.id].state = TaskInfo::RUNNING_CPU;
                cpu_ready_queue.pop();
                break;
            }
            cpu_ready_queue.pop(); // Remove invalid task
        }
    } else {
        // Remove the selected task from the queue
        priority_queue<TaskInfo, vector<TaskInfo>, CompareCPUTask> new_queue;
        while (!cpu_ready_queue.empty()) {
            const auto& task = cpu_ready_queue.top();
            if (task.id != new_cpu) {
                new_queue.push(task);
            }
            cpu_ready_queue.pop();
        }
        cpu_ready_queue = move(new_queue);
    }

    // Handle IO scheduling
    int new_io = current_io;
    if (current_io == 0) {
        // IO is idle, select a new task if available
        // First, try to find a high priority task with a deadline approaching
        priority_queue<TaskInfo, vector<TaskInfo>, CompareIOTask> temp_io = io_waiting_queue;
        while (!temp_io.empty()) {
            const auto& task = temp_io.top();
            if (valid_tasks.count(task.id) && all_tasks[task.id].state == TaskInfo::WAITING_IO) {
                // Check if this is a high priority task with a deadline approaching
                if (task.priority == Event::Task::Priority::kHigh && 
                    task.deadline - events[0].time < 200) {  // Deadline is within 200 time units
                    new_io = task.id;
                    all_tasks[task.id].state = TaskInfo::RUNNING_IO;
                    break;
                }
            }
            temp_io.pop();
        }
        
        // If no high priority task with approaching deadline, use the highest priority task
        if (new_io == 0) {
            while (!io_waiting_queue.empty()) {
                const auto& task = io_waiting_queue.top();
                if (valid_tasks.count(task.id) && all_tasks[task.id].state == TaskInfo::WAITING_IO) {
                    new_io = task.id;
                    all_tasks[task.id].state = TaskInfo::RUNNING_IO;
                    io_waiting_queue.pop();
                    break;
                }
                io_waiting_queue.pop(); // Remove invalid task
            }
        } else {
            // Remove the selected task from the queue
            priority_queue<TaskInfo, vector<TaskInfo>, CompareIOTask> new_queue;
            while (!io_waiting_queue.empty()) {
                const auto& task = io_waiting_queue.top();
                if (task.id != new_io) {
                    new_queue.push(task);
                }
                io_waiting_queue.pop();
            }
            io_waiting_queue = move(new_queue);
        }
    } else if (!valid_tasks.count(current_io)) {
        // Current IO task is no longer valid
        new_io = 0;
    }

    return {new_cpu, new_io};
}
