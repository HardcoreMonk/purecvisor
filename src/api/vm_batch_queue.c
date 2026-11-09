   
                         
                                  
  
                           
                                                                      
                                                                            
                                                               
                                                      
                                                                                                
  
                                                                    
   
#include "vm_batch_queue.h"

struct PcvVmBatchSlots {
    guint    capacity;
    guint    in_use;
    GQueue   waiting;                              
    gboolean waking;                                  
};

struct PcvVmBatchQueue {
    gchar             **vms;                                
    guint               n;
    guint               next;                      
    guint               in_flight;                           
    guint               max_in_flight;
    gboolean            pumping;                                                     
    gboolean            waiting;                                   
    gboolean            granted;                                                
    gchar              *actor;                                                                          
    PcvVmBatchQueueOps  ops;
};

static void _slots_wake(PcvVmBatchSlots *s);

                               
static void _slots_enqueue(PcvVmBatchQueue *q)
{
    if (q->waiting)
        return;
    g_queue_push_tail(&q->ops.slots->waiting, q);
    q->waiting = TRUE;
}

                                                              
static gboolean _may_take_slot(const PcvVmBatchQueue *q)
{
    const PcvVmBatchSlots *s = q->ops.slots;
    if (!s)
        return TRUE;
    if (s->in_use >= s->capacity)
        return FALSE;
    return q->granted || g_queue_is_empty((GQueue *)&s->waiting);
}

static void _queue_free(PcvVmBatchQueue *q)
{
    if (q->waiting)
        g_queue_remove(&q->ops.slots->waiting, q);
    g_strfreev(q->vms);
    g_free(q->actor);
    g_free(q);
}

                                                            
                                           
static void _queue_pump(PcvVmBatchQueue *q)
{
    PcvVmBatchSlots *s = q->ops.slots;
    q->pumping = TRUE;
    while (q->next < q->n && q->in_flight < q->max_in_flight) {
        const gchar *vm = q->vms[q->next];
        if (q->ops.terminating && q->ops.terminating()) {
            q->next++;
            q->ops.report(vm, q->actor, "daemon shutting down", q->ops.user_data);
            continue;
        }
        if (!_may_take_slot(q)) {                                     
            _slots_enqueue(q);
            break;
        }
        q->next++;
        q->granted = FALSE;
        if (s)
            s->in_use++;
        q->in_flight++;
        gchar *reason = NULL;
        if (q->ops.submit(vm, q->actor, q, q->ops.user_data, &reason)) {
            g_free(reason);
            continue;                                
        }
        q->in_flight--;
        if (s)
            s->in_use--;
        q->ops.report(vm, q->actor, reason ? reason : "submit rejected", q->ops.user_data);
        g_free(reason);
    }
    q->pumping = FALSE;
    q->granted = FALSE;
    if (q->next >= q->n && q->in_flight == 0)
        _queue_free(q);
    if (s)
        _slots_wake(s);
}

                                                                           
static void _slots_wake(PcvVmBatchSlots *s)
{
    if (s->waking)
        return;
    s->waking = TRUE;
    while (s->in_use < s->capacity && !g_queue_is_empty(&s->waiting)) {
        PcvVmBatchQueue *h = g_queue_pop_head(&s->waiting);
        h->waiting = FALSE;
        h->granted = TRUE;
        _queue_pump(h);
    }
    s->waking = FALSE;
}

void pcv_vm_batch_queue_run(const gchar *const *vms, guint n, const PcvVmBatchQueueOps *ops)
{
    g_return_if_fail(ops && ops->submit && ops->report);
    if (!vms || n == 0)
        return;
    PcvVmBatchQueue *q = g_new0(PcvVmBatchQueue, 1);
    q->vms = g_new0(gchar *, n + 1);
    for (guint i = 0; i < n; i++)
        q->vms[i] = g_strdup(vms[i]);
    q->n = n;
    q->ops = *ops;
    q->actor = g_strdup(ops->actor);
    q->ops.actor = q->actor;                       
    q->max_in_flight = ops->max_in_flight ? ops->max_in_flight : 1;
    _queue_pump(q);
}

void pcv_vm_batch_queue_item_done(PcvVmBatchQueue *queue)
{
    g_return_if_fail(queue && queue->in_flight > 0);
    queue->in_flight--;
    PcvVmBatchSlots *s = queue->ops.slots;
    if (s)
        s->in_use--;
    if (queue->pumping)
        return;                                                   
    if (!s) {
        _queue_pump(queue);
        return;
    }
                                                            
    if (queue->next < queue->n)
        _slots_enqueue(queue);
    else if (queue->in_flight == 0)
        _queue_free(queue);
    _slots_wake(s);
}

PcvVmBatchSlots *pcv_vm_batch_slots_new(guint capacity)
{
    PcvVmBatchSlots *s = g_new0(PcvVmBatchSlots, 1);
    s->capacity = capacity ? capacity : 1;
    g_queue_init(&s->waiting);
    return s;
}

                                                                 
void pcv_vm_batch_slots_free(PcvVmBatchSlots *slots)
{
    if (!slots)
        return;
    g_return_if_fail(slots->in_use == 0 && g_queue_is_empty(&slots->waiting));
    g_free(slots);
}

                                                         
guint pcv_vm_batch_slots_in_use(const PcvVmBatchSlots *slots)
{
    return slots ? slots->in_use : 0;
}

PcvVmBatchSlots *pcv_vm_batch_stop_slots(guint capacity)
{
    static PcvVmBatchSlots *stop_slots = NULL;                          
    if (!stop_slots)
        stop_slots = pcv_vm_batch_slots_new(capacity);
    return stop_slots;
}
