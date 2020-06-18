#ifndef ICECAP_RING_BUFFER_H
#define ICECAP_RING_BUFFER_H

#include "hw/sysbus.h"
#include "chardev/char-fe.h"

#define TYPE_ICECAP_RING_BUFFER "icecap.ring-buffer"

#define ICECAP_RING_BUFFER(obj) OBJECT_CHECK(IceCapRingBufferState, (obj), TYPE_ICECAP_RING_BUFFER)

#define ICECAP_RING_BUFFER_CTRL_SIZE 4096

typedef struct IceCapRingBufferSideLayout {
    hwaddr ctrl;
    hwaddr data;
    hwaddr size;
} IceCapRingBufferSideLayout;

typedef struct IceCapRingBufferLayout {
    IceCapRingBufferSideLayout read;
    IceCapRingBufferSideLayout write;
} IceCapRingBufferLayout;

typedef struct IceCapRingBuffer {
    IceCapRingBufferLayout layout;
    size_t private_offset_r;
    size_t private_offset_w;
} IceCapRingBuffer;

typedef struct IceCapRingBufferState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    CharBackend chr;
    IceCapRingBuffer rb;
    bool enabled;
} IceCapRingBufferState;

extern int icecap_ring_buffer_hack_chardev_ix;

#endif /* ICECAP_RING_BUFFER_H */
