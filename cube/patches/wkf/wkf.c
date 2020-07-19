/* 
 * Copyright (c) 2019-2020, Extrems <extrems@extremscorner.org>
 * 
 * This file is part of Swiss.
 * 
 * Swiss is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * Swiss is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * with Swiss.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../alt/emulator.h"
#include "../base/common.h"
#include "../base/exi.h"
#include "../base/os.h"

static struct {
	void *buffer;
	uint32_t length;
	uint32_t offset;
	bool frag;
} dvd = {0};

static struct {
	uint32_t base_sector;
	int items;
	struct {
		void *address;
		uint32_t length;
		uint32_t offset;
		uint32_t sector;
		read_frag_cb callback;
	} queue[2];
} wkf = {0};

OSAlarm read_alarm = {0};

static void wkf_read(void *address, uint32_t length, uint32_t offset, uint32_t sector)
{
	if (wkf.base_sector != sector) {
		DI[2] = 0xDE000000;
		DI[3] = sector;
		DI[4] = 0x5A000000;
		DI[7] = 0b001;
		while (DI[7] & 0b001);
		wkf.base_sector = sector;
	}

	DI[0] = 0b0011000;
	DI[1] = 0;
	DI[2] = 0xA8000000;
	DI[3] = offset >> 2;
	DI[4] = length;
	DI[5] = (uint32_t)address;
	DI[6] = length;
	DI[7] = 0b011;

	OSUnmaskInterrupts(OS_INTERRUPTMASK_PI_DI);
}

void do_read_disc(void *address, uint32_t length, uint32_t offset, uint32_t sector, read_frag_cb callback)
{
	int i;

	for (i = 0; i < wkf.items; i++)
		if (wkf.queue[i].callback == callback)
			return;

	wkf.queue[i].address = address;
	wkf.queue[i].length = length;
	wkf.queue[i].offset = offset;
	wkf.queue[i].sector = sector;
	wkf.queue[i].callback = callback;
	if (wkf.items++) return;

	wkf_read(address, length, offset, sector);
}

void di_interrupt_handler(OSInterrupt interrupt, OSContext *context)
{
	OSMaskInterrupts(OS_INTERRUPTMASK_PI_DI);

	void *address = wkf.queue[0].address;
	uint32_t length = wkf.queue[0].length;
	uint32_t offset = wkf.queue[0].offset;
	uint32_t sector = wkf.queue[0].sector;
	read_frag_cb callback = wkf.queue[0].callback;

	if (--wkf.items) {
		memcpy(wkf.queue, wkf.queue + 1, wkf.items * sizeof(*wkf.queue));
		wkf_read(wkf.queue[0].address, wkf.queue[0].length, wkf.queue[0].offset, wkf.queue[0].sector);
	}

	callback(address, length);
}

bool exi_probe(int32_t chan)
{
	if (chan == EXI_CHANNEL_2)
		return false;
	if (chan == *VAR_EXI_SLOT)
		return false;

	return true;
}

bool exi_try_lock(int32_t chan, uint32_t dev, EXIControl *exi)
{
	if (!(exi->state & EXI_STATE_LOCKED) || exi->dev != dev)
		return false;
	if (chan == *VAR_EXI_SLOT && dev == EXI_DEVICE_0)
		return false;

	return true;
}

void schedule_read(OSTick ticks)
{
	void read_callback(void *address, uint32_t length)
	{
		dvd.buffer += length;
		dvd.length -= length;
		dvd.offset += length;

		schedule_read(OSMicrosecondsToTicks(300));
	}

	OSCancelAlarm(&read_alarm);

	if (!dvd.length) {
		di_complete_transfer();
		return;
	}

	dtk_fill_buffer();
	dvd.frag = is_frag_read(dvd.offset, dvd.length);

	if (!dvd.frag)
		read_disc_frag(dvd.buffer, dvd.length, dvd.offset, read_callback);
	else
		OSSetAlarm(&read_alarm, ticks, (OSAlarmHandler)trickle_read);
}

void perform_read(uint32_t address, uint32_t length, uint32_t offset)
{
	dvd.buffer = OSPhysicalToUncached(address);
	dvd.length = length;
	dvd.offset = offset;

	schedule_read(OSMicrosecondsToTicks(300));
}

void trickle_read(void)
{
	if (dvd.length && dvd.frag) {
		OSTick start = OSGetTick();
		int size = read_frag(dvd.buffer, dvd.length, dvd.offset);
		OSTick end = OSGetTick();

		dvd.buffer += size;
		dvd.length -= size;
		dvd.offset += size;

		schedule_read(OSDiffTick(end, start));
	}
}

void device_reset(void)
{
	end_read();
}

void change_disc(void)
{
	*(uint32_t *)VAR_CURRENT_DISC = !*(uint32_t *)VAR_CURRENT_DISC;

	(*DI_EMU)[1] &= ~0b001;
	(*DI_EMU)[1] |=  0b100;
	di_update_interrupts();
}
