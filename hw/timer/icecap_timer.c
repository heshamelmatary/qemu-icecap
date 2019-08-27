#include "qemu/osdep.h"

#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/timer/icecap_timer.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"

#define REG_FREQ     0x00
#define REG_ENABLE   0x04
#define REG_COUNT    0x08
#define REG_COMPARE  0x10

#define ICECAP_TIMER_FREQUENCY NANOSECONDS_PER_SECOND

#define LO_32(x) ((x) & (~0UL >> 32))
#define HI_32(x) ((x) & (~0UL << 32))

static uint64_t icecap_timer_get_count(IceCapTimerState *s)
{
    return (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void icecap_timer_update(IceCapTimerState *s)
{
    if (!s->enabled) {
        qemu_irq_lower(s->irq);
        timer_del(s->timer);
    } else if (icecap_timer_get_count(s) < s->compare) {
        qemu_irq_lower(s->irq);
        timer_mod(s->timer, s->compare);
    } else {
        qemu_irq_raise(s->irq);
        timer_del(s->timer);
    }
}

static void icecap_timer_cb(void *opaque)
{
    IceCapTimerState *s = (IceCapTimerState *)opaque;
    assert(s->enabled); // This would indicate the presence of a bug
    icecap_timer_update(s);
}

static uint64_t icecap_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    IceCapTimerState *s = (IceCapTimerState *)opaque;
    uint64_t r;

    switch (offset) {
    case REG_FREQ:
        if (size != 4)
            goto no;
        r = ICECAP_TIMER_FREQUENCY;
        break;
    case REG_COUNT:
        switch (size) {
        case 4:
            r = LO_32(icecap_timer_get_count(s));
            break;
        case 8:
            r = icecap_timer_get_count(s);
            break;
        default:
            goto no;
        }
        break;
    case REG_COUNT + 4:
        if (size != 4)
            goto no;
        r = icecap_timer_get_count(s) >> 32;
        break;
    default:
        goto no;
    }

    return r;
no:
    qemu_log_mask(LOG_GUEST_ERROR, "icecap_timer_read: Bad offset 0x%x and size %u\n", (int)offset, size);
    return 0;
}

static void icecap_timer_write(void * opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IceCapTimerState *s = (IceCapTimerState *)opaque;

    switch (offset) {
    case REG_ENABLE:
        if (size != 4)
            goto no;
        s->enabled = value;
        break;
    case REG_COMPARE:
        switch (size) {
        case 4:
            s->compare = HI_32(s->compare) | LO_32(value);
            break;
        case 8:
            s->compare = value;
            break;
        default:
            goto no;
        }
        break;
    case REG_COMPARE + 4:
        if (size != 4)
            goto no;
        s->compare = (value << 32) | LO_32(s->compare);
        break;
    default:
        goto no;
    }

    icecap_timer_update(s);
    return;
no:
    qemu_log_mask(LOG_GUEST_ERROR, "icecap_timer_write: Bad offset 0x%x and size %u\n", (int)offset, size);
    return;
}

static const MemoryRegionOps icecap_timer_ops = {
    .read = icecap_timer_read,
    .write = icecap_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void icecap_timer_init(Object *obj)
{
    IceCapTimerState *s = ICECAP_TIMER(obj);
    SysBusDevice *dev = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &icecap_timer_ops, s, "icecap_timer", 0x1000);
    sysbus_init_mmio(dev, &s->iomem);
    sysbus_init_irq(dev, &s->irq);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, icecap_timer_cb, s);
    s->enabled = false;
    s->compare = 0;
}

static Property icecap_timer_properties[] = {
    DEFINE_PROP_END_OF_LIST()
};

static void icecap_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->props = icecap_timer_properties;
}

static const TypeInfo icecap_timer_info = {
    .name          = TYPE_ICECAP_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IceCapTimerState),
    .instance_init = icecap_timer_init,
    .class_init    = icecap_timer_class_init,
};

static void icecap_timer_register_types(void)
{
    type_register_static(&icecap_timer_info);
}

type_init(icecap_timer_register_types)
