/*
 * Instructions of ARMv8.3 in a program, on a processor that has none of them: done here when they trap.
 *
 * Darwin userland 17 and later is arm64e, built for ARMv8.3: pointer authentication everywhere, and FJCVTZS wherever
 * JavaScript's numbers become integers. The cores this port runs on are ARMv8.2. A program's own code is rewritten
 * before it runs (tools/lower/pac_lower: each PAuth instruction replaced by the one that does the same without a
 * signature), so that nothing of it traps. What the rewriting cannot reach or cannot express comes here, as an
 * undefined instruction from user space (osfmk/arm64/sleh.c, handle_uncategorized):
 *
 *   - code that did not exist when the files were rewritten: a JIT compiler's, a trampoline written at run time;
 *   - a load with a signed pointer whose offset the plain load of four bytes cannot hold (LDRAA, LDRAB);
 *   - FJCVTZS, which no single ARMv8.2 instruction does.
 *
 * Each is done the way pac_lower would have written it: no pointer is ever signed, so there is nothing to check and
 * nothing to strip. Slow (a trap for one instruction), and right.
 *
 * boot-arg hakios_emulate=0 turns it off (the trap is then the program's, as before); =2 logs every one.
 * hakios_emulated[] counts them by kind (hakios_emulated_name[]), for whoever reports.
 */

#include <arm/thread.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/machine/thread_status.h>
#include <os/atomic_private.h>

extern char *proc_name_address(struct proc *p);
extern int proc_pid(struct proc *p);

int hakios_emulate_user_insn(arm_saved_state_t *state, uint32_t instr);

static TUNABLE(uint32_t, hakios_emulate, "hakios_emulate", 1);

enum { EMU_PAC, EMU_BRANCH, EMU_LOAD, EMU_PACGA, EMU_JSCVT, EMU_KINDS };
uint64_t hakios_emulated[EMU_KINDS];
const char *const hakios_emulated_name[EMU_KINDS] = { "pac/aut/xpac", "braa/blraa/retaa", "ldraa", "pacga", "fjcvtzs" };

/* register `reg` as a general one: 31 is the zero register */
static uint64_t
reg_or_zero(arm_saved_state_t *state, unsigned reg)
{
	return reg == 31 ? 0 : (uint64_t)get_saved_state_reg(state, reg);
}

/* register `reg` as a base: 31 is the stack pointer */
static uint64_t
reg_or_sp(arm_saved_state_t *state, unsigned reg)
{
	return reg == 31 ? (uint64_t)get_saved_state_sp(state) : (uint64_t)get_saved_state_reg(state, reg);
}

static void
set_reg(arm_saved_state_t *state, unsigned reg, uint64_t value)
{
	if (reg != 31) {
		set_saved_state_reg(state, reg, (register_t)value);
	}
}

/*
 * FJCVTZS: a double to a 32-bit integer the way JavaScript does it -- toward zero, and what does not fit, modulo 2^32;
 * infinities and NaN give 0. *exact: the conversion lost nothing (Z of the flags).
 */
static uint32_t
double_to_int32_js(uint64_t bits, int *exact)
{
	int negative = (int)(bits >> 63);
	int exponent = (int)((bits >> 52) & 0x7FF);
	uint64_t fraction = bits & 0x000FFFFFFFFFFFFFull, mantissa, magnitude;

	*exact = 0;
	if (exponent == 0x7FF) {
		return 0;
	}
	if (exponent == 0) {
		*exact = fraction == 0 && !negative;    /* +0 is exact; -0 and the denormals are not */
		return 0;
	}
	exponent -= 1023;
	mantissa = fraction | 0x0010000000000000ull;    /* the value is mantissa * 2^(exponent - 52) */
	if (exponent < 0) {
		return 0;
	}
	if (exponent <= 52) {
		magnitude = mantissa >> (52 - exponent);
		*exact = (mantissa & ((1ull << (52 - exponent)) - 1)) == 0 &&
		    magnitude <= (negative ? 0x80000000ull : 0x7FFFFFFFull);
	} else {
		magnitude = exponent - 52 >= 32 ? 0 : mantissa << (exponent - 52);
	}
	return negative ? (uint32_t)(0 - magnitude) : (uint32_t)magnitude;
}

/* 1: the instruction was done and the program goes on after it; 0: not one of these. */
int
hakios_emulate_user_insn(arm_saved_state_t *state, uint32_t instr)
{
	unsigned rn = (instr >> 5) & 31, rd = instr & 31;
	int kind;

	if (hakios_emulate == 0 || !is_saved_state64(state) || !PSR64_IS_USER64(get_saved_state_cpsr(state))) {
		return 0;
	}
	if ((instr & 0xFFFF0000u) == 0xDAC10000u) {
		/* PACIA .. AUTDB Xd, Xn; PACIZA .. AUTDZB Xd; XPACI, XPACD Xd: the pointer stays as it is */
		unsigned opcode = (instr >> 10) & 0x3F;

		if (!(opcode <= 7 || (opcode <= 17 && rn == 31))) {
			return 0;
		}
		kind = EMU_PAC;
		add_saved_state_pc(state, 4);
	} else if ((instr & 0xFFE0FC00u) == 0x9AC03000u) {
		/* PACGA Xd, Xn, Xm: any function of the two, the same as pac_lower's (EOR Xd, Xn, Xm, ROR #13) */
		uint64_t n = reg_or_zero(state, rn), m = reg_or_zero(state, (instr >> 16) & 31);

		set_reg(state, rd, n ^ ((m >> 13) | (m << 51)));
		kind = EMU_PACGA;
		add_saved_state_pc(state, 4);
	} else if ((instr & 0xFEFFF800u) == 0xD61F0800u || (instr & 0xFEFFF800u) == 0xD63F0800u) {
		/* BRAA BRAB BRAAZ BRABZ, BLRAA BLRAB BLRAAZ BLRABZ */
		uint64_t target = reg_or_zero(state, rn);

		if (!(instr & 0x01000000u) && rd != 31) {
			return 0;
		}
		if (instr & 0x00200000u) {
			set_saved_state_lr(state, (register_t)(get_saved_state_pc(state) + 4));
		}
		set_saved_state_pc(state, (register_t)target);
		kind = EMU_BRANCH;
	} else if ((instr & 0xFFFFFBFFu) == 0xD65F0BFFu) {
		/* RETAA, RETAB */
		set_saved_state_pc(state, (register_t)get_saved_state_lr(state));
		kind = EMU_BRANCH;
	} else if ((instr & 0xFF200400u) == 0xF8200400u) {
		/* LDRAA, LDRAB Xt, [Xn, #off] and [Xn, #off]!: the offset is S:imm9, signed, times 8 */
		int64_t offset = (int64_t)((instr >> 12) & 0x1FF);
		uint64_t address, value;

		if (instr & 0x00400000u) {
			offset -= 512;
		}
		address = reg_or_sp(state, rn) + (uint64_t)(offset * 8);
		if (copyin((user_addr_t)address, &value, sizeof(value)) != 0) {
			return 0;                       /* the fault is then reported as this instruction's */
		}
		if ((instr & 0x00000800u) && rn != 31) {
			set_saved_state_reg(state, rn, (register_t)address);
		}
		set_reg(state, rd, value);
		kind = EMU_LOAD;
		add_saved_state_pc(state, 4);
	} else if ((instr & 0xFFFFFC00u) == 0x1E7E0000u) {
		/* FJCVTZS Wd, Dn */
		arm_neon_saved_state_t *neon = current_thread()->machine.uNeon;
		int exact;
		uint32_t result;

		if (neon == NULL) {
			return 0;
		}
		result = double_to_int32_js((uint64_t)neon->ns_64.v.q[rn], &exact);
		set_reg(state, rd, result);
		set_saved_state_cpsr(state, (get_saved_state_cpsr(state) & ~0xF0000000u) | (exact ? 0x40000000u : 0));
		kind = EMU_JSCVT;
		add_saved_state_pc(state, 4);
	} else {
		return 0;
	}
	os_atomic_inc(&hakios_emulated[kind], relaxed);
	if (hakios_emulate >= 2) {
		struct proc *p = get_bsdtask_info(current_task());

		printf("hakios_emulate: %s[%d]: %s (0x%08x), now at 0x%llx\n", p ? proc_name_address(p) : "?", p ? proc_pid(p) : 0,
		    hakios_emulated_name[kind], instr, (unsigned long long)get_saved_state_pc(state));
	}
	return 1;
}
