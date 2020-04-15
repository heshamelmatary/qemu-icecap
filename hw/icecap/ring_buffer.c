#include "qemu/osdep.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/icecap/ring_buffer.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "exec/cpu-common.h"

#define ICECAP_RING_BUFFER_S_NOTIFY_READ (1 << 0)
#define ICECAP_RING_BUFFER_S_NOTIFY_WRITE (1 << 1)

typedef struct icecap_ring_buffer_ctrl {
    size_t offset_r;
    size_t offset_w;
    uint64_t status;
} icecap_ring_buffer_ctrl_t;

static inline void rb_ctrl_read_offset_r(IceCapRingBuffer *rb, size_t *r)
{
    cpu_physical_memory_read(rb->layout.read.ctrl + offsetof(icecap_ring_buffer_ctrl_t, offset_r), (void *)r, sizeof(*r));
}

static inline void rb_ctrl_read_offset_w(IceCapRingBuffer *rb, size_t *r)
{
    cpu_physical_memory_read(rb->layout.read.ctrl + offsetof(icecap_ring_buffer_ctrl_t, offset_w), (void *)r, sizeof(*r));
}

static inline void rb_ctrl_read_status(IceCapRingBuffer *rb, uint64_t *r)
{
    cpu_physical_memory_read(rb->layout.read.ctrl + offsetof(icecap_ring_buffer_ctrl_t, status), (void *)r, sizeof(*r));
}

static inline void rb_ctrl_write_offset_r(IceCapRingBuffer *rb, size_t *r)
{
    cpu_physical_memory_write(rb->layout.write.ctrl + offsetof(icecap_ring_buffer_ctrl_t, offset_r), (void *)r, sizeof(*r));
}

static inline void rb_ctrl_write_offset_w(IceCapRingBuffer *rb, size_t *r)
{
    cpu_physical_memory_write(rb->layout.write.ctrl + offsetof(icecap_ring_buffer_ctrl_t, offset_w), (void *)r, sizeof(*r));
}

static inline void rb_ctrl_write_status(IceCapRingBuffer *rb, uint64_t *r)
{
    cpu_physical_memory_write(rb->layout.write.ctrl + offsetof(icecap_ring_buffer_ctrl_t, status), (void *)r, sizeof(*r));
}

static inline void rb_data_read(IceCapRingBuffer *rb, hwaddr offset, uint8_t *buf, int size)
{
    cpu_physical_memory_read(rb->layout.read.data + offset, (void *)buf, size);
}

static inline void rb_data_write(IceCapRingBuffer *rb, hwaddr offset, const uint8_t *buf, int size)
{
    cpu_physical_memory_write(rb->layout.write.data + offset, (const void *)buf, size);
}

static size_t rb_poll_read(IceCapRingBuffer *rb)
{
    size_t offset_r;
    size_t offset_w;
    offset_r = rb->private_offset_r;
    rb_ctrl_read_offset_w(rb, &offset_w);
    assert(offset_r <= offset_w);
    assert(offset_w - offset_r <= rb->layout.read.size);
    return offset_w - offset_r;
}

static size_t rb_poll_write(IceCapRingBuffer *rb)
{
    size_t offset_r;
    size_t offset_w;
    rb_ctrl_read_offset_r(rb, &offset_r);
    offset_w = rb->private_offset_w;
    assert(offset_r <= offset_w);
    assert(offset_w - offset_r <= rb->layout.write.size);
    return rb->layout.write.size - (offset_w - offset_r);
}

static void rb_skip(IceCapRingBuffer *rb, size_t n)
{
    assert(n <= rb_poll_read(rb));
    rb->private_offset_r += n;
}

static void rb_peek(IceCapRingBuffer *rb, size_t n, uint8_t *buf)
{
    size_t offset = rb->private_offset_r % rb->layout.read.size;
    size_t n1 = rb->layout.read.size - offset;
    assert(n <= rb_poll_read(rb));
    if (n <= n1)
        rb_data_read(rb, offset, buf, n);
    else {
        rb_data_read(rb, offset, buf, n1);
        rb_data_read(rb, 0, buf + n1, n - n1);
    }
}

static void rb_read(IceCapRingBuffer *rb, size_t n, uint8_t *buf)
{
    rb_peek(rb, n, buf);
    rb_skip(rb, n);
}

static void rb_write(IceCapRingBuffer *rb, size_t n, const uint8_t *buf)
{
    size_t offset = rb->private_offset_w % rb->layout.write.size;
    size_t n1 = rb->layout.write.size - offset;
    assert(n <= rb_poll_write(rb));
    if (n <= n1)
        rb_data_write(rb, offset, buf, n);
    else {
        rb_data_write(rb, offset, buf, n1);
        rb_data_write(rb, 0, buf + n1, n - n1);
    }
    rb->private_offset_w += n;
}

static void rb_notify_read(IceCapRingBufferState *s)
{
    uint64_t status;
    rb_ctrl_write_offset_r(&s->rb, &s->rb.private_offset_r);
    rb_ctrl_read_status(&s->rb, &status);
    if (status & ICECAP_RING_BUFFER_S_NOTIFY_READ) {
        /* TODO should be edge triggered */
        /* qemu_irq_pulse(s->irq); */
        qemu_irq_raise(s->irq);
    }
}

static void rb_notify_write(IceCapRingBufferState *s)
{
    uint64_t status;
    rb_ctrl_write_offset_w(&s->rb, &s->rb.private_offset_w);
    rb_ctrl_read_status(&s->rb, &status);
    if (status & ICECAP_RING_BUFFER_S_NOTIFY_WRITE) {
        /* TODO should be edge triggered */
        /* qemu_irq_pulse(s->irq); */
        qemu_irq_raise(s->irq);
    }
}

static void rb_flush_rx(IceCapRingBufferState *s)
{
    assert(qemu_chr_fe_backend_connected(&s->chr));
    size_t n = rb_poll_read(&s->rb);
    if (n == 0) {
        return;
    }
    uint8_t *buf = g_malloc(n);
    assert(buf);
    rb_read(&s->rb, n, buf);
    /* XXX this blocks entire thread. Rewrite to use
     * qemu_chr_fe_write and background I/O callbacks */
    qemu_chr_fe_write_all(&s->chr, buf, n);
    rb_notify_read(s);
}

static void rb_flush_tx(IceCapRingBufferState *s)
{
    IceCapRingBuffer *rb = &s->rb;
    size_t min = MIN((s->rx_fifo_tail - s->rx_fifo_head) % ICECAP_RX_FIFO_SIZE, rb_poll_write(rb));
    /* if (!min) { */
    /*     return; */
    /* } */
    for (size_t i = 0; i < min; i++) {
        rb_write(rb, 1, s->rx_fifo + s->rx_fifo_head + i);
    };
    s->rx_fifo_head += min;
    rb_notify_write(s);
}

static void rb_callback(IceCapRingBufferState *s)
{
    rb_flush_tx(s);
    rb_flush_rx(s);
}

static int icecap_ring_buffer_can_receive(void *opaque)
{
    IceCapRingBufferState *s = opaque;
    return s->enabled;
}

static void icecap_ring_buffer_receive(void *opaque, const uint8_t *buf, int size)
{
    IceCapRingBufferState *s = opaque;

    assert((s->rx_fifo_head - s->rx_fifo_tail - 1) % ICECAP_RX_FIFO_SIZE + 1> size);
    size_t n1 = ICECAP_RX_FIFO_SIZE - s->rx_fifo_tail;
    if (size <= n1)
        memcpy(s->rx_fifo + s->rx_fifo_tail, buf, size);
    else {
        memcpy(s->rx_fifo + s->rx_fifo_tail, buf, n1);
        memcpy(s->rx_fifo, buf + n1, size - n1);
    }
    s->rx_fifo_tail = (s->rx_fifo_tail + size) % ICECAP_RX_FIFO_SIZE;

    rb_callback(s);
}

static void icecap_ring_buffer_event(void *opaque, int event)
{
    IceCapRingBufferState *s = opaque;
    rb_callback(s);
}

static void icecap_ring_buffer_enable(IceCapRingBufferState *s)
{
    size_t offset = 0;
    uint64_t status = ICECAP_RING_BUFFER_S_NOTIFY_READ | ICECAP_RING_BUFFER_S_NOTIFY_WRITE;
    rb_ctrl_write_offset_r(&s->rb, &offset);
    rb_ctrl_write_offset_w(&s->rb, &offset);
    rb_ctrl_write_status(&s->rb, &status);
    s->rb.private_offset_r = 0;
    s->rb.private_offset_w = 0;
    s->enabled = true;
}

static uint64_t icecap_ring_buffer_read(void *opaque, hwaddr offset, unsigned size)
{
    printf("guest attempted to read icecap_ring_buffer_mmio\n");
    assert(false);
}

#define LAYOUT_REG_SIZE (sizeof(IceCapRingBufferLayout))

#define VAL_NOTIFY   1
#define VAL_ACK      2
#define VAL_ENABLE   3

static void icecap_ring_buffer_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IceCapRingBufferState *s = (IceCapRingBufferState *)opaque;
    /* printf("write: 0x%lx, 0x%lx, 0x%x\n", offset, value, size); */
    assert(size == sizeof(uint32_t));
    switch (offset) {
        case LAYOUT_REG_SIZE:
            switch(value) {
                case VAL_NOTIFY:
                    assert(s->enabled);
                    rb_callback(s);
                    break;
                case VAL_ACK:
                    /* TODO should be edge-triggered */
                    qemu_irq_lower(s->irq);
                    break;
                case VAL_ENABLE:
                    icecap_ring_buffer_enable(s);
                    break;
                default:
                    assert(false);
            }
            break;
        default:
            assert(offset < LAYOUT_REG_SIZE);
            *((uint32_t *)((uint8_t *)(&s->rb.layout) + offset)) = (uint32_t)value;
    }
}

static const MemoryRegionOps icecap_ring_buffer_ops = {
    .read = icecap_ring_buffer_read,
    .write = icecap_ring_buffer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void icecap_ring_buffer_init(Object *obj)
{
    IceCapRingBufferState *s = ICECAP_RING_BUFFER(obj);
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &icecap_ring_buffer_ops, s, "icecap_ring_buffer", 0x1000);
    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);

    Chardev *chr = serial_hd(1);
    qemu_chr_fe_init(&s->chr, chr, &error_abort);
    qemu_chr_fe_set_handlers(&s->chr, icecap_ring_buffer_can_receive, icecap_ring_buffer_receive, icecap_ring_buffer_event, NULL, s, NULL, true);

    s->rx_fifo_head = 0;
    s->rx_fifo_tail = 0;

    s->enabled = false;
}

static Property icecap_ring_buffer_properties[] = {
    DEFINE_PROP_END_OF_LIST()
};

static void icecap_ring_buffer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = icecap_ring_buffer_properties;
}

static const TypeInfo icecap_ring_buffer_info = {
    .name          = TYPE_ICECAP_RING_BUFFER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IceCapRingBufferState),
    .instance_init = icecap_ring_buffer_init,
    .class_init    = icecap_ring_buffer_class_init,
};

static void icecap_ring_buffer_register_types(void)
{
    type_register_static(&icecap_ring_buffer_info);
}

type_init(icecap_ring_buffer_register_types)
