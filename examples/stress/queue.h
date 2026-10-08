#ifndef STRESS_QUEUE_H
#define STRESS_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>

#define QUEUE_CAPACITY 32U

typedef struct {
	uint32_t id, seed, checksum, core;
} Message;

typedef struct {
	atomic_uint sequence;
	Message message;
} Slot;

typedef struct {
	atomic_uint enqueue, dequeue;
	Slot slots[QUEUE_CAPACITY];
} Queue;

static inline void queueInit(Queue *q)
{
	atomic_init(&q->enqueue, 0);
	atomic_init(&q->dequeue, 0);
	for (unsigned i = 0; i < QUEUE_CAPACITY; i++)
		atomic_init(&q->slots[i].sequence, i);
}

static inline int queuePush(Queue *q, Message m)
{
	unsigned pos = atomic_load_explicit(&q->enqueue, memory_order_relaxed);
	Slot *s;
	for (;;) {
		s = &q->slots[pos & (QUEUE_CAPACITY - 1)];
		unsigned seq = atomic_load_explicit(&s->sequence, memory_order_acquire);
		int32_t difference = (int32_t)(seq - pos);
		if (!difference) {
			if (atomic_compare_exchange_weak_explicit(&q->enqueue, &pos, pos + 1,
								  memory_order_relaxed,
								  memory_order_relaxed))
				break;
		} else if (difference < 0)
			return 0;
		else
			pos = atomic_load_explicit(&q->enqueue, memory_order_relaxed);
	}
	s->message = m;
	atomic_store_explicit(&s->sequence, pos + 1, memory_order_release);
	return 1;
}

static inline int queuePop(Queue *q, Message *m)
{
	unsigned pos = atomic_load_explicit(&q->dequeue, memory_order_relaxed);
	Slot *s;
	for (;;) {
		s = &q->slots[pos & (QUEUE_CAPACITY - 1)];
		unsigned seq = atomic_load_explicit(&s->sequence, memory_order_acquire);
		int32_t difference = (int32_t)(seq - (pos + 1));
		if (!difference) {
			if (atomic_compare_exchange_weak_explicit(&q->dequeue, &pos, pos + 1,
								  memory_order_relaxed,
								  memory_order_relaxed))
				break;
		} else if (difference < 0)
			return 0;
		else
			pos = atomic_load_explicit(&q->dequeue, memory_order_relaxed);
	}
	*m = s->message;
	atomic_store_explicit(&s->sequence, pos + QUEUE_CAPACITY, memory_order_release);
	return 1;
}

#endif
