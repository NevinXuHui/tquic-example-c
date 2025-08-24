#ifndef EVENT_SYSTEM_H
#define EVENT_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <ev.h>

#ifdef __cplusplus
extern "C" {
#endif

// 事件类型
typedef enum {
    EVENT_TYPE_WEBSOCKET,
    EVENT_TYPE_MESSAGE,
    EVENT_TYPE_BUSINESS,
    EVENT_TYPE_TIMER,
    EVENT_TYPE_CUSTOM
} event_type_t;

// 事件优先级
typedef enum {
    EVENT_PRIORITY_LOW = 0,
    EVENT_PRIORITY_NORMAL = 1,
    EVENT_PRIORITY_HIGH = 2,
    EVENT_PRIORITY_URGENT = 3
} event_priority_t;

// 通用事件结构
typedef struct {
    event_type_t type;
    event_priority_t priority;
    uint64_t timestamp;
    char *event_id;
    void *data;
    size_t data_size;
    void (*data_destructor)(void *data);
} generic_event_t;

// 事件队列项
typedef struct event_queue_item {
    generic_event_t *event;
    struct event_queue_item *next;
} event_queue_item_t;

// 事件处理器函数类型
typedef void (*event_handler_t)(const generic_event_t *event, void *user_data);

// 事件监听器
typedef struct event_listener {
    event_type_t event_type;
    event_handler_t handler;
    void *user_data;
    bool active;
    struct event_listener *next;
} event_listener_t;

// 事件系统
typedef struct event_system event_system_t;

// 定时器回调函数类型
typedef void (*timer_callback_t)(void *user_data);

// 定时器信息
typedef struct {
    uint32_t timer_id;
    uint32_t interval_ms;
    bool repeat;
    timer_callback_t callback;
    void *user_data;
    bool active;
} timer_info_t;

// 事件系统配置
typedef struct {
    uint32_t max_queue_size;        // 最大队列大小
    uint32_t worker_thread_count;   // 工作线程数
    bool enable_priority_queue;     // 启用优先级队列
    bool thread_safe;               // 线程安全模式
    uint32_t event_timeout_ms;      // 事件处理超时
} event_system_config_t;

// 事件系统统计信息
typedef struct {
    uint64_t events_processed;
    uint64_t events_dropped;
    uint64_t events_timeout;
    uint64_t queue_size;
    uint64_t max_queue_size_reached;
    uint32_t active_listeners;
    uint32_t active_timers;
    double avg_processing_time_ms;
} event_system_stats_t;

// 事件系统 API

/**
 * 创建事件系统
 */
event_system_t *event_system_create(const event_system_config_t *config);

/**
 * 销毁事件系统
 */
void event_system_destroy(event_system_t *system);

/**
 * 启动事件系统
 */
int event_system_start(event_system_t *system);

/**
 * 停止事件系统
 */
void event_system_stop(event_system_t *system);

/**
 * 设置事件循环
 */
void event_system_set_event_loop(event_system_t *system, struct ev_loop *loop);

/**
 * 发布事件
 */
int event_system_publish(event_system_t *system, const generic_event_t *event);

/**
 * 发布高优先级事件
 */
int event_system_publish_urgent(event_system_t *system, const generic_event_t *event);

/**
 * 订阅事件类型
 */
int event_system_subscribe(event_system_t *system,
                          event_type_t event_type,
                          event_handler_t handler,
                          void *user_data);

/**
 * 取消订阅
 */
void event_system_unsubscribe(event_system_t *system,
                             event_type_t event_type,
                             event_handler_t handler);

/**
 * 创建定时器
 */
uint32_t event_system_create_timer(event_system_t *system,
                                  uint32_t interval_ms,
                                  bool repeat,
                                  timer_callback_t callback,
                                  void *user_data);

/**
 * 销毁定时器
 */
void event_system_destroy_timer(event_system_t *system, uint32_t timer_id);

/**
 * 暂停定时器
 */
void event_system_pause_timer(event_system_t *system, uint32_t timer_id);

/**
 * 恢复定时器
 */
void event_system_resume_timer(event_system_t *system, uint32_t timer_id);

/**
 * 处理事件队列（单次）
 */
int event_system_process_once(event_system_t *system);

/**
 * 处理所有待处理事件
 */
int event_system_process_all(event_system_t *system);

/**
 * 获取事件系统统计信息
 */
const event_system_stats_t *event_system_get_stats(const event_system_t *system);

/**
 * 获取默认配置
 */
event_system_config_t event_system_config_default(void);

// 事件工具函数

/**
 * 创建通用事件
 */
generic_event_t *generic_event_create(event_type_t type,
                                     event_priority_t priority,
                                     void *data,
                                     size_t data_size,
                                     void (*data_destructor)(void *data));

/**
 * 销毁通用事件
 */
void generic_event_destroy(generic_event_t *event);

/**
 * 复制事件
 */
generic_event_t *generic_event_clone(const generic_event_t *event);

/**
 * 生成事件 ID
 */
char *generate_event_id(void);

/**
 * 获取当前时间戳（微秒）
 */
uint64_t get_timestamp_us(void);

// 线程安全的事件队列操作

/**
 * 线程安全地入队事件
 */
int event_queue_enqueue_safe(event_system_t *system, generic_event_t *event);

/**
 * 线程安全地出队事件
 */
generic_event_t *event_queue_dequeue_safe(event_system_t *system);

/**
 * 获取队列大小
 */
uint32_t event_queue_size(const event_system_t *system);

/**
 * 清空事件队列
 */
void event_queue_clear(event_system_t *system);

// 事件过滤器

/**
 * 事件过滤器函数类型
 */
typedef bool (*event_filter_t)(const generic_event_t *event, void *filter_data);

/**
 * 设置事件过滤器
 */
void event_system_set_filter(event_system_t *system,
                            event_filter_t filter,
                            void *filter_data);

/**
 * 移除事件过滤器
 */
void event_system_remove_filter(event_system_t *system);

#ifdef __cplusplus
}
#endif

#endif // EVENT_SYSTEM_H
