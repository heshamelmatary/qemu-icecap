#ifndef ICECAP_TIMER_H
#define ICECAP_TIMER_H

#include "hw/sysbus.h"

#define TYPE_ICECAP_TIMER "icecap.timer"

#define ICECAP_TIMER(obj) OBJECT_CHECK(IceCapTimerState, (obj), TYPE_ICECAP_TIMER)

typedef struct IceCapTimerState {

    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;

    bool enabled;
    uint64_t compare;

} IceCapTimerState;

#endif /* ICECAP_TIMER_H */
