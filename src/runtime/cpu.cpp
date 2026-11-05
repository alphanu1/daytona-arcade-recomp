// license:BSD-3-Clause
// copyright-holders:Farfetch'd, R. Belmont
//
// The i960 context and the services recompiled code calls: memory access,
// call/return with MAME's 4-frame register cache, interrupt entry, IAC.
// Transplanted from MAME's src/devices/cpu/i960/i960.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above kept
// as the licence requires). Changes: the device framework and cycle
// accounting are removed, memory goes through Cpu::bus, fatalerror/logerror
// map to the runtime. There is no instruction interpreter here: game code
// is statically recompiled. See THIRD_PARTY.md.

#include "runtime/cpu.h"

#include <cstdarg>
#include <cstring>

namespace rt {

uint32_t Cpu::i960_read_dword_unaligned(uint32_t address)
{
	if (!DWORD_ALIGNED(address))
		return bus->read_byte(address) | bus->read_byte(address+1)<<8 | bus->read_byte(address+2)<<16 | bus->read_byte(address+3)<<24;
	else
		return bus->read_dword(address);
}

std::pair<uint32_t, uint16_t> Cpu::i960_read_dword_unaligned_flags(uint32_t address)
{
	if (!DWORD_ALIGNED(address)) {
		auto v = bus->read_byte_flags(address);
		return std::pair<uint32_t, uint16_t>(v.first | bus->read_byte(address+1)<<8 | bus->read_byte(address+2)<<16 | bus->read_byte(address+3)<<24, v.second);
	} else
		return bus->read_dword_flags(address);
}

uint16_t Cpu::i960_read_word_unaligned(uint32_t address)
{
	if (!WORD_ALIGNED(address))
		return bus->read_byte(address) | bus->read_byte(address+1)<<8;
	else
		return bus->read_word(address);
}

void Cpu::i960_write_dword_unaligned(uint32_t address, uint32_t data)
{
	if (!DWORD_ALIGNED(address))
	{
		bus->write_byte(address, data & 0xff);
		bus->write_byte(address+1, (data>>8)&0xff);
		bus->write_byte(address+2, (data>>16)&0xff);
		bus->write_byte(address+3, (data>>24)&0xff);
	}
	else
	{
		bus->write_dword(address, data);
	}
}

uint16_t Cpu::i960_write_dword_unaligned_flags(uint32_t address, uint32_t data)
{
	if (!DWORD_ALIGNED(address))
	{
		uint16_t flags = bus->write_byte_flags(address, data & 0xff);
		bus->write_byte(address+1, (data>>8)&0xff);
		bus->write_byte(address+2, (data>>16)&0xff);
		bus->write_byte(address+3, (data>>24)&0xff);
		return flags;
	}
	else
	{
		return bus->write_dword_flags(address, data);
	}
}

void Cpu::i960_write_word_unaligned(uint32_t address, uint16_t data)
{
	if (!WORD_ALIGNED(address))
	{
		bus->write_byte(address, data & 0xff);
		bus->write_byte(address+1, (data>>8)&0xff);
	}
	else
	{
		bus->write_word(address, data);
	}
}

void Cpu::send_iac(uint32_t adr)
{
	uint32_t iac[4];
	iac[0] = bus->read_dword(adr);
	iac[1] = bus->read_dword(adr+4);
	iac[2] = bus->read_dword(adr+8);
	iac[3] = bus->read_dword(adr+12);

	switch(iac[0]>>24) {
	case 0x40:  // generate irq
		logerror("I960: %x: IAC %08x %08x %08x %08x (generate IRQ)\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
		break;
	case 0x41:  // test for pending interrupts
		logerror("I960: %x: IAC %08x %08x %08x %08x (test for pending interrupts)\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
		check_pending_irqs();
		break;
	case 0x80:  // store SAT & PRCB in memory
		bus->write_dword(iac[1], m_SAT);
		bus->write_dword(iac[1]+4, m_PRCB);
		break;
	case 0x89:  // invalidate internal instruction cache
		logerror("I960: %x: IAC %08x %08x %08x %08x (invalidate internal instruction cache)\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
		// we do not emulate the instruction cache, so this is safe to ignore
		break;
	case 0x8f:  // enable/disable breakpoints
		logerror("I960: %x: IAC %08x %08x %08x %08x (enable/disable breakpoints)\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
		// processor breakpoints are not emulated, safe to ignore
		break;
	case 0x91:  // stop processor
		logerror("I960: %x: IAC %08x %08x %08x %08x (stop processor)\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
		break;
	case 0x92:  // continue initialization
		logerror("I960: %x: IAC %08x %08x %08x %08x (continue initialization)\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
		break;
	case 0x93: // reinit
		m_SAT  = iac[1];
		m_PRCB = iac[2];
		m_IP   = iac[3];
		break;
	default:
		fatalerror("I960: %x: IAC %08x %08x %08x %08x\n", m_PIP, iac[0], iac[1], iac[2], iac[3]);
	}
}

void Cpu::take_interrupt(int vector, int lvl)
{
	int int_tab =  bus->read_dword(m_PRCB+20);    // interrupt table
	int int_SP  =  bus->read_dword(m_PRCB+24);    // interrupt stack
	int SP;
	uint32_t IRQV;

	IRQV = bus->read_dword(int_tab + 36 + (vector-8)*4);

	// start the process
	if(!(m_PC & 0x2000))    // if this is a nested interrupt, don't re-get int_SP
	{
		SP = int_SP;
	}
	else
	{
		SP = m_r[I960_SP];
	}

	SP = (SP + 63) & ~63;
	SP += 64;   // add padding to prevent buffer underflow when saving processor state

	do_call(IRQV, 7, SP);

	// save the processor state
	bus->write_dword(m_r[I960_FP]-16, m_PC);
	bus->write_dword(m_r[I960_FP]-12, m_AC);
	// store the vector
	bus->write_dword(m_r[I960_FP]-8, vector-8);

	m_PC &= ~0x001f0401;    // clear priority (bits 16-20), trace-fault pending (bit 10) and trace enable (bit 0)
	m_PC |= (lvl<<16);      // set CPU level to current IRQ level
	m_PC |= 0x2002;         // set supervisor mode & interrupt flag
}

void Cpu::check_immediate_irqs()
{
	int cpu_pri = (m_PC >> 16) & 0x1f;

	if ((m_immediate_irq) && ((cpu_pri < m_immediate_pri) || (m_immediate_pri == 31)))
	{
		if (on_take) on_take(on_take_ctx, m_immediate_vector, m_IP, false); // runtime hook
		take_interrupt(m_immediate_vector, m_immediate_pri);
		m_immediate_irq = 0;
	}
}

void Cpu::check_pending_irqs()
{
	int int_tab = bus->read_dword(m_PRCB + 20);    // interrupt table
	int cpu_pri = (m_PC >> 16) & 0x1f;
	int pending_pri = bus->read_dword(int_tab);    // read pending priorities
	int take = -1;
	static const uint32_t lvlmask[4] = { 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 };

	for (int lvl = 31; lvl >= 0; lvl--) {
		if ((pending_pri & (1 << lvl)) && ((cpu_pri < lvl) || (lvl == 31))) {
			int word, wordl, wordh;

			// figure out which word contains this level's priorities
			word = ((lvl / 4) * 4) + 4; // (lvl/4) = word address, *4 for byte address, +4 to skip pending priorities
			wordl = (lvl % 4) * 8;
			wordh = (wordl + 8) - 1;

			int vword = bus->read_dword(int_tab + word);

			// take the first vector we find for this level
			for (int irq = wordh; irq >= wordl; irq--) {
				if (vword & (1 << irq)) {
					// clear pending bit
					vword &= ~(1 << irq);
					bus->write_dword(int_tab + word, vword);
					take = irq;
					break;
				}
			}

			// if no vectors were found at our level, it's an error
			if (take == -1) {
				logerror("i960: ERROR! no vector found for pending level %d\n", lvl);

				// try to recover...
				pending_pri &= ~(1 << lvl);
				bus->write_dword(int_tab, pending_pri);
				return;
			}

			// if no vectors are waiting for this level, clear the level bit
			if (!(vword & lvlmask[lvl % 4])) {
				pending_pri &= ~(1 << lvl);
				bus->write_dword(int_tab, pending_pri);
			}

			take += ((lvl / 4) * 32);

			if (on_take) on_take(on_take_ctx, take, m_IP, true); // runtime hook
			take_interrupt(take, lvl);
			return;
		}
	}
}

#ifdef M2_FAST_CPU
// A register frame (16 words, 4-aligned): straight-line loads and stores.
// GCC makes a 64-byte memcpy (or a copy loop) a library call on the SH-4,
// several per call and return in the profile.
static inline void copy_frame(uint32_t *d, const uint32_t *s)
{
	const uint32_t a0 = s[0], a1 = s[1], a2 = s[2], a3 = s[3], a4 = s[4], a5 = s[5], a6 = s[6], a7 = s[7];
	const uint32_t a8 = s[8], a9 = s[9], a10 = s[10], a11 = s[11], a12 = s[12], a13 = s[13], a14 = s[14], a15 = s[15];
	d[0] = a0, d[1] = a1, d[2] = a2, d[3] = a3, d[4] = a4, d[5] = a5, d[6] = a6, d[7] = a7;
	d[8] = a8, d[9] = a9, d[10] = a10, d[11] = a11, d[12] = a12, d[13] = a13, d[14] = a14, d[15] = a15;
}
#endif

void Cpu::do_call(uint32_t adr, int type, uint32_t stack)
{
#ifdef M2_DC_NATIVE_HOST
	if (on_call) on_call(on_call_ctx, *this, adr, type);
#endif
	int i;
	uint32_t FP;

	// call and callx take 9 cycles base

	// set the new RIP
	m_r[I960_RIP] = m_IP;
//  osd_printf_debug("CALL (type %d): FP %x, %x => %x, stack %x, rcache_pos %d\n", type, m_r[I960_FP], m_r[I960_RIP], adr, stack, m_rcache_pos);

	// are we out of cache entries?
	if (m_rcache_pos >= I960_RCACHE_SIZE) {
		// flush the current register set to the current frame
		FP = m_r[I960_FP] & ~0x3f;
#ifdef M2_FAST_CPU
		if (work_ram && (FP >> 20) == 0x005) // work RAM (nothing watches its writes): one copy, not 16 bus calls
			copy_frame(reinterpret_cast<uint32_t *>(M2_AL(work_ram + (FP & 0xfffff), 64)), m_r);
		else
#endif
		for (i = 0; i < 16; i++) {
			bus->write_dword(FP + (i*4), m_r[i]);
		}
	}
	else    // a cache entry is available, use it
	{
#ifdef M2_FAST_CPU
		copy_frame(&m_rcache[m_rcache_pos][0], m_r);
#else
		memcpy(&m_rcache[m_rcache_pos][0], m_r, 0x10 * sizeof(uint32_t));
#endif
		m_rcache_frame_addr[m_rcache_pos] = m_r[I960_FP] & ~0x3f;
	}
	m_rcache_pos++;

	m_IP = adr;
	m_r[I960_PFP] = m_r[I960_FP] & ~7;
	m_r[I960_PFP] |= type;

	if(type == 7) { // interrupts need special handling
		// set the stack to the passed-in value to properly handle nested interrupts
		// (can't set it externally or the original program's SP will be lost)
		m_r[I960_SP] = stack;
	}

	m_r[I960_FP]  = (m_r[I960_SP] + 63) & ~63;
	m_r[I960_SP]  = m_r[I960_FP] + 64;
}

void Cpu::do_ret_0()
{
//  int type = m_r[I960_PFP] & 7;

	m_r[I960_FP] = m_r[I960_PFP] & ~0x3f;

	m_rcache_pos--;

	// normal situation: if we're still above rcache size, we're not in cache.
	// abnormal situation (after the app does a FLUSHREG): rcache_pos will be 0
	// coming in, but we must still treat it as a not-in-cache situation.
	if ((m_rcache_pos >= I960_RCACHE_SIZE) || (m_rcache_pos < 0))
	{
		int i;
#ifdef M2_FAST_CPU
		if (work_ram && (m_r[I960_FP] >> 20) == 0x005) // (as do_call's spill: one copy)
			copy_frame(m_r, reinterpret_cast<const uint32_t *>(M2_AL(work_ram + (m_r[I960_FP] & 0xfffff), 64)));
		else
#endif
		for(i=0; i<0x10; i++)
			m_r[i] = bus->read_dword(m_r[I960_FP]+4*i);

		if (m_rcache_pos < 0)
		{
			m_rcache_pos = 0;
		}
	}
	else
	{
#ifdef M2_FAST_CPU
		copy_frame(m_r, m_rcache[m_rcache_pos]);
#else
		memcpy(m_r, m_rcache[m_rcache_pos], 0x10*sizeof(uint32_t));
#endif
	}

//  osd_printf_debug("RET (type %d): FP %x, %x => %x, rcache_pos %d\n", type, m_r[I960_FP], m_IP, m_r[I960_RIP], m_rcache_pos);
	m_IP = m_r[I960_RIP];
}

void Cpu::do_ret()
{
	uint32_t x, y;
#ifdef M2_DC_NATIVE_HOST
	const uint32_t source = m_IP;
#endif
	switch(m_r[I960_PFP] & 7) {
	case 0:
		do_ret_0();
#ifdef M2_DC_NATIVE_HOST
		if (on_return) on_return(on_return_ctx, *this, source);
#endif
		break;

	case 7:
		x = bus->read_dword(m_r[I960_FP]-16);
		y = bus->read_dword(m_r[I960_FP]-12);
		do_ret_0();
		m_AC = y;
		// #### test supervisor
		m_PC = x;

		// check for another IRQ now that we're back
		check_pending_irqs();
		break;

	default:
		fatalerror("I960: %x: Unsupported return mode %d\n", m_PIP, m_r[I960_PFP] & 7);
	}
}

void Cpu::execute_set_input(int irqline, int state)
{
	if (m_irq_line_state[irqline] == state)
		return;

	m_irq_line_state[irqline] = state;

	int int_tab =  bus->read_dword(m_PRCB+20);    // interrupt table
	int cpu_pri = (m_PC>>16)&0x1f;
	int vector =0;
	int priority;
	uint32_t pend, word, wordofs;

	// We support the 4 external IRQ lines in "normal" mode only.
	// The i960's interrupt support is a bit more complete than that,
	// but Namco and Sega both went for the cheapest solution.

	switch (irqline)
	{
		case I960_IRQ0:
			vector = m_ICR & 0xff;
			break;

		case I960_IRQ1:
			vector = (m_ICR>>8)&0xff;
			break;

		case I960_IRQ2:
			vector = (m_ICR>>16)&0xff;
			break;

		case I960_IRQ3:
			vector = (m_ICR>>24)&0xff;
			break;
	}

	if(!vector)
	{
		logerror("i960: interrupt line %d in IAC mode, unsupported!\n", irqline);
		return;
	}


	priority = vector / 8;

	if(state) {
		// check if we can take this "right now"
		if (((cpu_pri < priority) || (priority == 31)) && (m_immediate_irq == 0))
		{
			m_immediate_irq = 1;
			m_immediate_vector = vector;
			m_immediate_pri = priority;
		}
		else
		{
			// store the interrupt in the "pending" table
			pend = bus->read_dword(int_tab);
			pend |= (1 << priority);
			bus->write_dword(int_tab, pend);

			// now bitfield-ize the vector
			word = ((vector / 32) * 4) + 4;
			wordofs = vector % 32;
			pend = bus->read_dword(int_tab + word);
			pend |= (1 << wordofs);
			bus->write_dword(int_tab + word, pend);
		}

		// and ack it to the core now that it's queued
		standard_irq_callback(irqline, m_IP);
	}
}

void Cpu::reset()
{
	m_SAT        = bus->read_dword(0);
	m_PRCB       = bus->read_dword(4);
	m_IP         = bus->read_dword(12);
	m_PC         = 0x001f2002;
	m_AC         = 0;
	m_ICR       = 0xff000000;
	m_immediate_irq = 0;

	memset(m_r, 0, sizeof(m_r));
	memset(m_rcache, 0, sizeof(m_rcache));

	m_r[I960_FP] = bus->read_dword(m_PRCB+24);
	m_r[I960_SP] = m_r[I960_FP] + 64;
	m_rcache_pos = 0;

}


void fatalerror(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	throw Fatal(buf);
}

} // namespace rt
