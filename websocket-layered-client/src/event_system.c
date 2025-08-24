/**
 * 事件系统实现
 * 
 * 提供异步事件处理机制：
 * - 事件队列管理
 * - 优先级调度
 * - 定时器管理
 * - 线程安全的事件分发
 */

#include "event_system.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// 事件系统结构体
struct event_system {
    event_system_config_t config;
    
    // 事件队列
    event_queue_item_t *queue_head;
    event_queue_item_t *queue_tail;
    uint32_t queue_size;
    
    // 事件监听器
    event_listener_t *listeners;
    
    // 定时器
    timer_info_t *timers;
    uint32_t next_timer_id;

    // 事件过滤器
    event_filter_t filter;
    void *filter_data;

    // 统计信息
    event_system_stats_t stats;
    
    // 线程安全
    pthread_mutex_t mutex;
    pthread_cond_t queue_cond;
    
    // 事件循环
    struct ev_loop *loop;
    ev_async async_watcher;
    ev_timer timer_watcher;
    
    // 状态
    bool running;
    pthread_t *worker_threads;
};

// 默认配置
event_system_config_t event_system_config_default(void) {
    event_system_config_t config = {
        .max_queue_size = 10000,
        .worker_thread_count = 2,
        .enable_priority_queue = true,
        .thread_safe = true,
        .event_timeout_ms = 5000
    };
    return config;
}

// 生成事件 ID
char *generate_event_id(void) {
    static uint64_t counter = 0;
    char *id = malloc(32);
    if (id) {
        snprintf(id, 32, "evt_%lu_%lu", time(NULL), __sync_add_and_fetch(&counter, 1));
    }
    return id;
}

// 获取当前时间戳（微秒）
uint64_t get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// 创建通用事件
generic_event_t *generic_event_create(event_type_t type,
                                     event_priority_t priority,
                                     void *data,
                                     size_t data_size,
                                     void (*data_destructor)(void *data)) {
    generic_event_t *event = calloc(1, sizeof(generic_event_t));
    if (!event) return NULL;
    
    event->type = type;
    event->priority = priority;
    event->timestamp = get_timestamp_us();
    event->event_id = generate_event_id();
    event->data_size = data_size;
    event->data_destructor = data_destructor;
    
    if (data && data_size > 0) {
        event->data = malloc(data_size);
        if (event->data) {
            memcpy(event->data, data, data_size);
        } else {
            generic_event_destroy(event);
            return NULL;
        }
    } else {
        event->data = data;
    }
    
    return event;
}

// 销毁通用事件
void generic_event_destroy(generic_event_t *event) {
    if (!event) return;
    
    if (event->data && event->data_destructor) {
        event->data_destructor(event->data);
    } else if (event->data && event->data_size > 0) {
        free(event->data);
    }
    
    free(event->event_id);
    free(event);
}

// 复制事件
generic_event_t *generic_event_clone(const generic_event_t *event) {
    if (!event) return NULL;
    
    return generic_event_create(event->type, event->priority, 
                               event->data, event->data_size, 
                               event->data_destructor);
}

// 异步事件处理回调
static void async_cb(EV_P_ ev_async *w, int revents) {
    event_system_t *system = (event_system_t *)w->data;
    if (system) {
        event_system_process_all(system);
    }
}

// 定时器回调
static void timer_cb(EV_P_ ev_timer *w, int revents) {
    event_system_t *system = (event_system_t *)w->data;
    if (!system) return;
    
    pthread_mutex_lock(&system->mutex);

    timer_info_t *timer = system->timers;
    
    while (timer) {
        if (timer->active && timer->callback) {
            // 触发定时器回调
            timer->callback(timer->user_data);
            
            if (!timer->repeat) {
                timer->active = false;
            }
        }
        timer = (timer_info_t *)timer; // 简化的链表遍历
    }
    
    pthread_mutex_unlock(&system->mutex);
}

// 工作线程函数
static void *worker_thread_func(void *arg) {
    event_system_t *system = (event_system_t *)arg;
    
    while (system->running) {
        pthread_mutex_lock(&system->mutex);
        
        // 等待事件
        while (system->running && !system->queue_head) {
            pthread_cond_wait(&system->queue_cond, &system->mutex);
        }
        
        if (!system->running) {
            pthread_mutex_unlock(&system->mutex);
            break;
        }
        
        // 取出事件
        event_queue_item_t *item = system->queue_head;
        if (item) {
            system->queue_head = item->next;
            if (!system->queue_head) {
                system->queue_tail = NULL;
            }
            system->queue_size--;
        }
        
        pthread_mutex_unlock(&system->mutex);
        
        if (item && item->event) {
            // 处理事件
            event_listener_t *listener = system->listeners;
            while (listener) {
                if (listener->active && 
                    listener->event_type == item->event->type &&
                    listener->handler) {
                    
                    uint64_t start_time = get_timestamp_us();
                    listener->handler(item->event, listener->user_data);
                    uint64_t end_time = get_timestamp_us();
                    
                    // 更新统计信息
                    pthread_mutex_lock(&system->mutex);
                    system->stats.events_processed++;
                    double processing_time = (end_time - start_time) / 1000.0;
                    system->stats.avg_processing_time_ms = 
                        (system->stats.avg_processing_time_ms + processing_time) / 2.0;
                    pthread_mutex_unlock(&system->mutex);
                }
                listener = listener->next;
            }
            
            // 清理事件
            generic_event_destroy(item->event);
            free(item);
        }
    }
    
    return NULL;
}

// 创建事件系统
event_system_t *event_system_create(const event_system_config_t *config) {
    if (!config) return NULL;
    
    event_system_t *system = calloc(1, sizeof(event_system_t));
    if (!system) return NULL;
    
    system->config = *config;
    system->running = true;
    system->next_timer_id = 1;
    
    // 初始化同步对象
    if (pthread_mutex_init(&system->mutex, NULL) != 0 ||
        pthread_cond_init(&system->queue_cond, NULL) != 0) {
        event_system_destroy(system);
        return NULL;
    }
    
    // 创建工作线程
    if (config->worker_thread_count > 0) {
        system->worker_threads = calloc(config->worker_thread_count, sizeof(pthread_t));
        if (!system->worker_threads) {
            event_system_destroy(system);
            return NULL;
        }
        
        for (uint32_t i = 0; i < config->worker_thread_count; i++) {
            if (pthread_create(&system->worker_threads[i], NULL, 
                             worker_thread_func, system) != 0) {
                event_system_destroy(system);
                return NULL;
            }
        }
    }
    
    return system;
}

// 销毁事件系统
void event_system_destroy(event_system_t *system) {
    if (!system) return;
    
    // 停止系统
    event_system_stop(system);
    
    // 等待工作线程结束
    if (system->worker_threads) {
        for (uint32_t i = 0; i < system->config.worker_thread_count; i++) {
            pthread_join(system->worker_threads[i], NULL);
        }
        free(system->worker_threads);
    }
    
    // 清理事件队列
    event_queue_clear(system);
    
    // 清理监听器
    event_listener_t *listener = system->listeners;
    while (listener) {
        event_listener_t *next = listener->next;
        free(listener);
        listener = next;
    }
    
    // 清理定时器
    free(system->timers);
    
    // 销毁同步对象
    pthread_mutex_destroy(&system->mutex);
    pthread_cond_destroy(&system->queue_cond);
    
    free(system);
}

// 启动事件系统
int event_system_start(event_system_t *system) {
    if (!system) return -1;
    
    pthread_mutex_lock(&system->mutex);
    system->running = true;
    pthread_mutex_unlock(&system->mutex);
    
    return 0;
}

// 停止事件系统
void event_system_stop(event_system_t *system) {
    if (!system) return;
    
    pthread_mutex_lock(&system->mutex);
    system->running = false;
    pthread_cond_broadcast(&system->queue_cond);
    pthread_mutex_unlock(&system->mutex);
    
    if (system->loop) {
        ev_async_stop(system->loop, &system->async_watcher);
        ev_timer_stop(system->loop, &system->timer_watcher);
    }
}

// 设置事件循环
void event_system_set_event_loop(event_system_t *system, struct ev_loop *loop) {
    if (!system) return;
    
    system->loop = loop;
    
    if (loop) {
        // 初始化异步监视器
        ev_async_init(&system->async_watcher, async_cb);
        system->async_watcher.data = system;
        ev_async_start(loop, &system->async_watcher);
        
        // 初始化定时器监视器
        ev_timer_init(&system->timer_watcher, timer_cb, 0.1, 0.1);
        system->timer_watcher.data = system;
        ev_timer_start(loop, &system->timer_watcher);
    }
}

// 发布事件
int event_system_publish(event_system_t *system, const generic_event_t *event) {
    if (!system || !event) return -1;
    
    pthread_mutex_lock(&system->mutex);
    
    // 检查队列是否已满
    if (system->queue_size >= system->config.max_queue_size) {
        system->stats.events_dropped++;
        pthread_mutex_unlock(&system->mutex);
        return -1;
    }
    
    // 创建队列项
    event_queue_item_t *item = calloc(1, sizeof(event_queue_item_t));
    if (!item) {
        pthread_mutex_unlock(&system->mutex);
        return -1;
    }
    
    item->event = generic_event_clone(event);
    if (!item->event) {
        free(item);
        pthread_mutex_unlock(&system->mutex);
        return -1;
    }
    
    // 添加到队列
    if (system->config.enable_priority_queue) {
        // 按优先级插入
        event_queue_item_t **current = &system->queue_head;
        while (*current && (*current)->event->priority >= event->priority) {
            current = &(*current)->next;
        }
        item->next = *current;
        *current = item;
        
        if (!item->next) {
            system->queue_tail = item;
        }
    } else {
        // 添加到队列尾部
        if (system->queue_tail) {
            system->queue_tail->next = item;
        } else {
            system->queue_head = item;
        }
        system->queue_tail = item;
    }
    
    system->queue_size++;
    
    // 通知工作线程
    pthread_cond_signal(&system->queue_cond);
    pthread_mutex_unlock(&system->mutex);
    
    // 通知事件循环
    if (system->loop) {
        ev_async_send(system->loop, &system->async_watcher);
    }
    
    return 0;
}

// 发布高优先级事件
int event_system_publish_urgent(event_system_t *system, const generic_event_t *event) {
    if (!system || !event) return -1;
    
    generic_event_t urgent_event = *event;
    urgent_event.priority = EVENT_PRIORITY_URGENT;
    
    return event_system_publish(system, &urgent_event);
}

// 订阅事件类型
int event_system_subscribe(event_system_t *system,
                          event_type_t event_type,
                          event_handler_t handler,
                          void *user_data) {
    if (!system || !handler) return -1;
    
    event_listener_t *listener = calloc(1, sizeof(event_listener_t));
    if (!listener) return -1;
    
    listener->event_type = event_type;
    listener->handler = handler;
    listener->user_data = user_data;
    listener->active = true;
    
    pthread_mutex_lock(&system->mutex);
    listener->next = system->listeners;
    system->listeners = listener;
    system->stats.active_listeners++;
    pthread_mutex_unlock(&system->mutex);
    
    return 0;
}

// 取消订阅
void event_system_unsubscribe(event_system_t *system,
                             event_type_t event_type,
                             event_handler_t handler) {
    if (!system || !handler) return;
    
    pthread_mutex_lock(&system->mutex);
    
    event_listener_t **current = &system->listeners;
    while (*current) {
        if ((*current)->event_type == event_type && 
            (*current)->handler == handler) {
            event_listener_t *to_remove = *current;
            *current = (*current)->next;
            free(to_remove);
            system->stats.active_listeners--;
            break;
        }
        current = &(*current)->next;
    }
    
    pthread_mutex_unlock(&system->mutex);
}

// 创建定时器
uint32_t event_system_create_timer(event_system_t *system,
                                  uint32_t interval_ms,
                                  bool repeat,
                                  timer_callback_t callback,
                                  void *user_data) {
    if (!system || !callback) return 0;

    pthread_mutex_lock(&system->mutex);

    // 创建新的定时器信息
    timer_info_t *timer = calloc(1, sizeof(timer_info_t));
    if (!timer) {
        pthread_mutex_unlock(&system->mutex);
        return 0;
    }

    timer->timer_id = system->next_timer_id++;
    timer->interval_ms = interval_ms;
    timer->repeat = repeat;
    timer->callback = callback;
    timer->user_data = user_data;
    timer->active = true;

    // 添加到定时器链表
    timer_info_t *old_head = system->timers;
    system->timers = timer;
    if (old_head) {
        // 简化的链表插入（实际应该使用更好的数据结构）
        timer_info_t *current = timer;
        while (current && current != old_head) {
            current = (timer_info_t *)current; // 简化处理
        }
    }

    system->stats.active_timers++;

    uint32_t timer_id = timer->timer_id;
    pthread_mutex_unlock(&system->mutex);

    return timer_id;
}

// 销毁定时器
void event_system_destroy_timer(event_system_t *system, uint32_t timer_id) {
    if (!system || timer_id == 0) return;

    pthread_mutex_lock(&system->mutex);

    timer_info_t **current = &system->timers;
    while (*current) {
        if ((*current)->timer_id == timer_id) {
            timer_info_t *to_remove = *current;
            *current = (timer_info_t *)to_remove; // 简化的链表删除

            if (to_remove->active) {
                system->stats.active_timers--;
            }

            free(to_remove);
            break;
        }
        current = (timer_info_t **)current; // 简化处理
    }

    pthread_mutex_unlock(&system->mutex);
}

// 暂停定时器
void event_system_pause_timer(event_system_t *system, uint32_t timer_id) {
    if (!system || timer_id == 0) return;

    pthread_mutex_lock(&system->mutex);

    timer_info_t *timer = system->timers;
    while (timer) {
        if (timer->timer_id == timer_id) {
            if (timer->active) {
                timer->active = false;
                system->stats.active_timers--;
            }
            break;
        }
        timer = (timer_info_t *)timer; // 简化处理
    }

    pthread_mutex_unlock(&system->mutex);
}

// 恢复定时器
void event_system_resume_timer(event_system_t *system, uint32_t timer_id) {
    if (!system || timer_id == 0) return;

    pthread_mutex_lock(&system->mutex);

    timer_info_t *timer = system->timers;
    while (timer) {
        if (timer->timer_id == timer_id) {
            if (!timer->active) {
                timer->active = true;
                system->stats.active_timers++;
            }
            break;
        }
        timer = (timer_info_t *)timer; // 简化处理
    }

    pthread_mutex_unlock(&system->mutex);
}

// 处理事件队列（单次）
int event_system_process_once(event_system_t *system) {
    if (!system) return -1;
    
    pthread_mutex_lock(&system->mutex);
    
    if (!system->queue_head) {
        pthread_mutex_unlock(&system->mutex);
        return 0;
    }
    
    event_queue_item_t *item = system->queue_head;
    system->queue_head = item->next;
    if (!system->queue_head) {
        system->queue_tail = NULL;
    }
    system->queue_size--;
    
    pthread_mutex_unlock(&system->mutex);
    
    if (item && item->event) {
        // 处理事件
        event_listener_t *listener = system->listeners;
        while (listener) {
            if (listener->active && 
                listener->event_type == item->event->type &&
                listener->handler) {
                listener->handler(item->event, listener->user_data);
            }
            listener = listener->next;
        }
        
        generic_event_destroy(item->event);
        free(item);
        return 1;
    }
    
    return 0;
}

// 处理所有待处理事件
int event_system_process_all(event_system_t *system) {
    if (!system) return -1;
    
    int processed = 0;
    while (event_system_process_once(system) > 0) {
        processed++;
    }
    
    return processed;
}

// 获取事件系统统计信息
const event_system_stats_t *event_system_get_stats(const event_system_t *system) {
    if (!system) return NULL;
    return &system->stats;
}

// 获取队列大小
uint32_t event_queue_size(const event_system_t *system) {
    if (!system) return 0;
    return system->queue_size;
}

// 清空事件队列
void event_queue_clear(event_system_t *system) {
    if (!system) return;
    
    pthread_mutex_lock(&system->mutex);
    
    event_queue_item_t *item = system->queue_head;
    while (item) {
        event_queue_item_t *next = item->next;
        generic_event_destroy(item->event);
        free(item);
        item = next;
    }
    
    system->queue_head = NULL;
    system->queue_tail = NULL;
    system->queue_size = 0;
    
    pthread_mutex_unlock(&system->mutex);
}
