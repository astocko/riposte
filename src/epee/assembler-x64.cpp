// Copyright 2011 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "../exceptions.h"
#include "assembler-x64.h"
#include "assembler-x64-inl.h"
#include <string.h>

namespace v8 {
namespace internal {

// ----------------------------------------------------------------------------
// General helper functions

#define IS_POWER_OF_TWO(x) (((x) & ((x) - 1)) == 0)

// Returns true iff x is a power of 2 (or zero). Cannot be used with the
// maximally negative value of the type T (the -1 overflows).
template<typename T>
static inline bool IsPowerOf2(T x) {
	return IS_POWER_OF_TWO(x);
}

// The type-based aliasing rule allows the compiler to assume that pointers of
// different types (for some definition of different) never alias each other.
// Thus the following code does not work:
//
// float f = foo();
// int fbits = *(int*)(&f);
//
// The compiler 'knows' that the int pointer can't refer to f since the types
// don't match, so the compiler may cache f in a register, leaving random data
// in fbits.  Using C++ style casts makes no difference, however a pointer to
// char data is assumed to alias any other pointer.  This is the 'memcpy
// exception'.
//
// Bit_cast uses the memcpy exception to move the bits from a variable of one
// type of a variable of another type.  Of course the end result is likely to
// be implementation dependent.  Most compilers (gcc-4.2 and MSVC 2005)
// will completely optimize BitCast away.
//
// There is an additional use for BitCast.
// Recent gccs will warn when they see casts that may result in breakage due to
// the type-based aliasing rule.  If you have checked that there is no breakage
// you can use BitCast to cast one pointer type to another.  This confuses gcc
// enough that it can no longer see that you have cast one pointer type to
// another thus avoiding the warning.

// We need different implementations of BitCast for pointer and non-pointer
// values. We use partial specialization of auxiliary struct to work around
// issues with template functions overloading.
template<class Dest, class Source>
struct BitCastHelper {

	INLINE(static Dest cast(const Source& source)) {
		Dest dest;
		memcpy(&dest, &source, sizeof(dest));
		return dest;
	}
};

template<class Dest, class Source>
struct BitCastHelper<Dest, Source*> {
	INLINE(static Dest cast(Source* source)) {
		return BitCastHelper<Dest, uintptr_t>::cast(
				reinterpret_cast<uintptr_t> (source));
	}
};

template<class Dest, class Source>
INLINE(Dest BitCast(const Source& source));

template<class Dest, class Source>
inline Dest BitCast(const Source& source) {
	return BitCastHelper<Dest, Source>::cast(source);
}

// -----------------------------------------------------------------------------
// Register constants.

const int Register::kRegisterCodeByAllocationIndex[kNumAllocatableRegisters] =
		{
		// rax, rbx, rdx, rcx, rdi, r8, r9, r11, r14, r15
				0, 3, 2, 1, 7, 8, 9, 11, 14, 15 };

const int Register::kAllocationIndexByRegisterCode[kNumRegisters] = { 0, 3, 2,
		1, -1, -1, -1, 4, 5, 6, -1, 7, -1, -1, 8, 9 };

// -----------------------------------------------------------------------------
// Implementation of Operand

Operand::Operand(Register base, int32_t disp) :
	rex_(0) {
	len_ = 1;
	if (base.is(rsp) || base.is(r12)) {
		// SIB byte is needed to encode (rsp + offset) or (r12 + offset).
		set_sib(times_1, rsp, base);
	}

	if (disp == 0 && !base.is(rbp) && !base.is(r13)) {
		set_modrm(0, base);
	} else if (is_int8(disp)) {
		set_modrm(1, base);
		set_disp8(disp);
	} else {
		set_modrm(2, base);
		set_disp32(disp);
	}
}

Operand::Operand(Register base, Register index, ScaleFactor scale, int32_t disp) :
	rex_(0) {
	assert(!index.is(rsp));
	len_ = 1;
	set_sib(scale, index, base);
	if (disp == 0 && !base.is(rbp) && !base.is(r13)) {
		// This call to set_modrm doesn't overwrite the REX.B (or REX.X) bits
		// possibly set by set_sib.
		set_modrm(0, rsp);
	} else if (is_int8(disp)) {
		set_modrm(1, rsp);
		set_disp8(disp);
	} else {
		set_modrm(2, rsp);
		set_disp32(disp);
	}
}

Operand::Operand(Register index, ScaleFactor scale, int32_t disp) :
	rex_(0) {
	assert(!index.is(rsp));
	len_ = 1;
	set_modrm(0, rsp);
	set_sib(scale, index, rbp);
	set_disp32(disp);
}

Operand::Operand(const Operand& operand, int32_t offset) {
	assert(operand.len_ >= 1);
	// Operand encodes REX ModR/M [SIB] [Disp].
	byte modrm = operand.buf_[0];
	assert(modrm < 0xC0); // Disallow mode 3 (register target).
	bool has_sib = ((modrm & 0x07) == 0x04);
	byte mode = modrm & 0xC0;
	int disp_offset = has_sib ? 2 : 1;
	int base_reg = (has_sib ? operand.buf_[1] : modrm) & 0x07;
	// Mode 0 with rbp/r13 as ModR/M or SIB base register always has a 32-bit
	// displacement.
	bool is_baseless = (mode == 0) && (base_reg == 0x05); // No base or RIP base.
	int32_t disp_value = 0;
	if (mode == 0x80 || is_baseless) {
		// Mode 2 or mode 0 with rbp/r13 as base: Word displacement.
		disp_value = *BitCast<const int32_t*> (&operand.buf_[disp_offset]);
	} else if (mode == 0x40) {
		// Mode 1: Byte displacement.
		disp_value = static_cast<signed char> (operand.buf_[disp_offset]);
	}

	// Write new operand with same registers, but with modified displacement.
	assert(
			offset >= 0 ? disp_value + offset > disp_value : disp_value
					+ offset < disp_value); // No overflow.
	disp_value += offset;
	rex_ = operand.rex_;
	if (!is_int8(disp_value) || is_baseless) {
		// Need 32 bits of displacement, mode 2 or mode 1 with register rbp/r13.
		buf_[0] = (modrm & 0x3f) | (is_baseless ? 0x00 : 0x80);
		len_ = disp_offset + 4;
		*reinterpret_cast<int32_t*> (&buf_[disp_offset]) = disp_value;
	} else if (disp_value != 0 || (base_reg == 0x05)) {
		// Need 8 bits of displacement.
		buf_[0] = (modrm & 0x3f) | 0x40; // Mode 1.
		len_ = disp_offset + 1;
		buf_[disp_offset] = static_cast<byte> (disp_value);
	} else {
		// Need no displacement.
		buf_[0] = (modrm & 0x3f); // Mode 0.
		len_ = disp_offset;
	}
	if (has_sib) {
		buf_[1] = operand.buf_[1];
	}
}

bool Operand::AddressUsesRegister(Register reg) const {
	int code = reg.code();
	assert((buf_[0] & 0xC0) != 0xC0); // Always a memory operand.
	// Start with only low three bits of base register. Initial decoding doesn't
	// distinguish on the REX.B bit.
	int base_code = buf_[0] & 0x07;
	if (base_code == rsp.code()) {
		// SIB byte present in buf_[1].
		// Check the index register from the SIB byte + REX.X prefix.
		int index_code = ((buf_[1] >> 3) & 0x07) | ((rex_ & 0x02) << 2);
		// Index code (including REX.X) of 0x04 (rsp) means no index register.
		if (index_code != rsp.code() && index_code == code)
			return true;
		// Add REX.B to get the full base register code.
		base_code = (buf_[1] & 0x07) | ((rex_ & 0x01) << 3);
		// A base register of 0x05 (rbp) with mod = 0 means no base register.
		if (base_code == rbp.code() && ((buf_[0] & 0xC0) == 0))
			return false;
		return code == base_code;
	} else {
		// A base register with low bits of 0x05 (rbp or r13) and mod = 0 means
		// no base register.
		if (base_code == rbp.code() && ((buf_[0] & 0xC0) == 0))
			return false;
		base_code |= ((rex_ & 0x01) << 3);
		return code == base_code;
	}
}

// -----------------------------------------------------------------------------
// Implementation of Assembler.

Assembler::Assembler(void* buffer, int buffer_size) {
	if (buffer == NULL) {
		// Do our own buffer management.
		if (buffer_size <= kMinimalBufferSize) {
			buffer_size = kMinimalBufferSize;
		}
		if (buffer == NULL) {
			buffer_ = new byte[buffer_size];
		} else {
			buffer_ = static_cast<byte*> (buffer);
		}
		buffer_size_ = buffer_size;
		own_buffer_ = true;
	} else {
		// Use externally provided buffer instead.
		assert(buffer_size > 0);
		buffer_ = static_cast<byte*> (buffer);
		buffer_size_ = buffer_size;
		own_buffer_ = false;
	}

	// Clear the buffer in debug mode unless it was provided by the
	// caller in which case we can't be sure it's okay to overwrite
	// existing code in it.
#ifdef DEBUG
	if (own_buffer_) {
		memset(buffer_, 0xCC, buffer_size); // int3
	}
#endif

	// Setup buffer pointers.
	assert(buffer_ != NULL);
	pc_ = buffer_;
}

Assembler::~Assembler() {
	if (own_buffer_) {
		delete[] buffer_;
	}
}

void Assembler::Align(int m) {
	assert(IsPowerOf2(m));
	int delta = (m - (pc_offset() & (m - 1))) & (m - 1);
	while (delta >= 9) {
		nop(9);
		delta -= 9;
	}
	if (delta > 0) {
		nop(delta);
	}
}

void Assembler::CodeTargetAlign() {
	Align(16); // Preferred alignment of jump targets on x64.
}

void Assembler::bind_to(Label* L, int pos) {
	assert(!L->is_bound()); // Label may only be bound once.
	assert(0 <= pos && pos <= pc_offset()); // Position must be valid.
	if (L->is_linked()) {
		int current = L->pos();
		int next = long_at(current);
		while (next != current) {
			// Relative address, relative to point after address.
			int imm32 = pos - (current + sizeof(int32_t));
			long_at_put(current, imm32);
			current = next;
			next = long_at(next);
		}
		// Fix up last fixup on linked list.
		int last_imm32 = pos - (current + sizeof(int32_t));
		long_at_put(current, last_imm32);
	}
	while (L->is_near_linked()) {
		int fixup_pos = L->near_link_pos();
		int
				offset_to_next =
						static_cast<int> (*reinterpret_cast<int8_t*> (addr_at(
								fixup_pos)));
		assert(offset_to_next <= 0);
		int disp = pos - (fixup_pos + sizeof(int8_t));
		assert(is_int8(disp));
		set_byte_at(fixup_pos, disp);
		if (offset_to_next < 0) {
			L->link_to(fixup_pos + offset_to_next, Label::kNear);
		} else {
			L->UnuseNear();
		}
	}
	L->bind_to(pos);
}

void Assembler::bind(Label* L) {
	bind_to(L, pc_offset());
}

void Assembler::GrowBuffer() {
	assert(buffer_overflow());
	if (!own_buffer_)
		_error("external code buffer is too small");

	// Compute new buffer size.
	byte* buffer;
	int buffer_size;
	int instr_size;

	if (buffer_size_ < 4 * 1024) {
		buffer_size = 4 * 1024;
	} else {
		buffer_size = 2 * buffer_size_;
	}
	// Some internal data structures overflow for very large buffers,
	// they must ensure that kMaximalBufferSize is not too large.
	if ((buffer_size > kMaximalBufferSize)) {
		_error("Assembler::GrowBuffer");
	}

	// Setup new buffer.
	buffer = new byte[buffer_size];
	instr_size = pc_offset();

	// Clear the buffer in debug mode. Use 'int3' instructions to make
	// sure to get into problems if we ever run uninitialized code.
	// Copy the data.
	intptr_t pc_delta = buffer - buffer_;
	memmove(buffer, buffer_, instr_size);

	// Switch buffers.
	delete[] buffer_;

	buffer_ = buffer;
	buffer_size_ = buffer_size;
	pc_ += pc_delta;

	assert(!buffer_overflow());
}

void Assembler::emit_operand(int code, const Operand& adr) {
	assert(is_uint3(code));
	const unsigned length = adr.len_;
	assert(length > 0);

	// Emit updated ModR/M byte containing the given register.
	assert((adr.buf_[0] & 0x38) == 0);
	pc_[0] = adr.buf_[0] | code << 3;

	// Emit the rest of the encoded operand.
	for (unsigned i = 1; i < length; i++)
		pc_[i] = adr.buf_[i];
	pc_ += length;
}

// Assembler Instruction implementations.

void Assembler::arithmetic_op(byte opcode, Register reg, const Operand& op) {
	EnsureSpace ensure_space(this);
	emit_rex_64(reg, op);
	emit(opcode);
	emit_operand(reg, op);
}

void Assembler::arithmetic_op(byte opcode, Register reg, Register rm_reg) {
	EnsureSpace ensure_space(this);
	assert((opcode & 0xC6) == 2);
	if (rm_reg.low_bits() == 4) { // Forces SIB byte.
		// Swap reg and rm_reg and change opcode operand order.
		emit_rex_64(rm_reg, reg);
		emit(opcode ^ 0x02);
		emit_modrm(rm_reg, reg);
	} else {
		emit_rex_64(reg, rm_reg);
		emit(opcode);
		emit_modrm(reg, rm_reg);
	}
}

void Assembler::arithmetic_op_16(byte opcode, Register reg, Register rm_reg) {
	EnsureSpace ensure_space(this);
	assert((opcode & 0xC6) == 2);
	if (rm_reg.low_bits() == 4) { // Forces SIB byte.
		// Swap reg and rm_reg and change opcode operand order.
		emit(0x66);
		emit_optional_rex_32(rm_reg, reg);
		emit(opcode ^ 0x02);
		emit_modrm(rm_reg, reg);
	} else {
		emit(0x66);
		emit_optional_rex_32(reg, rm_reg);
		emit(opcode);
		emit_modrm(reg, rm_reg);
	}
}

void Assembler::arithmetic_op_16(byte opcode, Register reg,
		const Operand& rm_reg) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(reg, rm_reg);
	emit(opcode);
	emit_operand(reg, rm_reg);
}

void Assembler::arithmetic_op_32(byte opcode, Register reg, Register rm_reg) {
	EnsureSpace ensure_space(this);
	assert((opcode & 0xC6) == 2);
	if (rm_reg.low_bits() == 4) { // Forces SIB byte.
		// Swap reg and rm_reg and change opcode operand order.
		emit_optional_rex_32(rm_reg, reg);
		emit(opcode ^ 0x02); // E.g. 0x03 -> 0x01 for ADD.
		emit_modrm(rm_reg, reg);
	} else {
		emit_optional_rex_32(reg, rm_reg);
		emit(opcode);
		emit_modrm(reg, rm_reg);
	}
}

void Assembler::arithmetic_op_32(byte opcode, Register reg,
		const Operand& rm_reg) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(reg, rm_reg);
	emit(opcode);
	emit_operand(reg, rm_reg);
}

void Assembler::immediate_arithmetic_op(byte subcode, Register dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	if (is_int8(src.value_)) {
		emit(0x83);
		emit_modrm(subcode, dst);
		emit(src.value_);
	} else if (dst.is(rax)) {
		emit(0x05 | (subcode << 3));
		emitl(src.value_);
	} else {
		emit(0x81);
		emit_modrm(subcode, dst);
		emitl(src.value_);
	}
}

void Assembler::immediate_arithmetic_op(byte subcode, const Operand& dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	if (is_int8(src.value_)) {
		emit(0x83);
		emit_operand(subcode, dst);
		emit(src.value_);
	} else {
		emit(0x81);
		emit_operand(subcode, dst);
		emitl(src.value_);
	}
}

void Assembler::immediate_arithmetic_op_16(byte subcode, Register dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // Operand size override prefix.
	emit_optional_rex_32(dst);
	if (is_int8(src.value_)) {
		emit(0x83);
		emit_modrm(subcode, dst);
		emit(src.value_);
	} else if (dst.is(rax)) {
		emit(0x05 | (subcode << 3));
		emitw(src.value_);
	} else {
		emit(0x81);
		emit_modrm(subcode, dst);
		emitw(src.value_);
	}
}

void Assembler::immediate_arithmetic_op_16(byte subcode, const Operand& dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // Operand size override prefix.
	emit_optional_rex_32(dst);
	if (is_int8(src.value_)) {
		emit(0x83);
		emit_operand(subcode, dst);
		emit(src.value_);
	} else {
		emit(0x81);
		emit_operand(subcode, dst);
		emitw(src.value_);
	}
}

void Assembler::immediate_arithmetic_op_32(byte subcode, Register dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	if (is_int8(src.value_)) {
		emit(0x83);
		emit_modrm(subcode, dst);
		emit(src.value_);
	} else if (dst.is(rax)) {
		emit(0x05 | (subcode << 3));
		emitl(src.value_);
	} else {
		emit(0x81);
		emit_modrm(subcode, dst);
		emitl(src.value_);
	}
}

void Assembler::immediate_arithmetic_op_32(byte subcode, const Operand& dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	if (is_int8(src.value_)) {
		emit(0x83);
		emit_operand(subcode, dst);
		emit(src.value_);
	} else {
		emit(0x81);
		emit_operand(subcode, dst);
		emitl(src.value_);
	}
}

void Assembler::immediate_arithmetic_op_8(byte subcode, const Operand& dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	assert(is_int8(src.value_) || is_uint8(src.value_));
	emit(0x80);
	emit_operand(subcode, dst);
	emit(src.value_);
}

void Assembler::immediate_arithmetic_op_8(byte subcode, Register dst,
		Immediate src) {
	EnsureSpace ensure_space(this);
	if (dst.code() > 3) {
		// Use 64-bit mode byte registers.
		emit_rex_64(dst);
	}
	assert(is_int8(src.value_) || is_uint8(src.value_));
	emit(0x80);
	emit_modrm(subcode, dst);
	emit(src.value_);
}

void Assembler::shift(Register dst, Immediate shift_amount, int subcode) {
	EnsureSpace ensure_space(this);
	assert(is_uint6(shift_amount.value_)); // illegal shift count
	if (shift_amount.value_ == 1) {
		emit_rex_64(dst);
		emit(0xD1);
		emit_modrm(subcode, dst);
	} else {
		emit_rex_64(dst);
		emit(0xC1);
		emit_modrm(subcode, dst);
		emit(shift_amount.value_);
	}
}

void Assembler::shift(Register dst, int subcode) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xD3);
	emit_modrm(subcode, dst);
}

void Assembler::shift_32(Register dst, int subcode) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xD3);
	emit_modrm(subcode, dst);
}

void Assembler::shift_32(Register dst, Immediate shift_amount, int subcode) {
	EnsureSpace ensure_space(this);
	assert(is_uint5(shift_amount.value_)); // illegal shift count
	if (shift_amount.value_ == 1) {
		emit_optional_rex_32(dst);
		emit(0xD1);
		emit_modrm(subcode, dst);
	} else {
		emit_optional_rex_32(dst);
		emit(0xC1);
		emit_modrm(subcode, dst);
		emit(shift_amount.value_);
	}
}

void Assembler::bt(const Operand& dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0xA3);
	emit_operand(src, dst);
}

void Assembler::bts(const Operand& dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0xAB);
	emit_operand(src, dst);
}

void Assembler::call(Label* L) {
	EnsureSpace ensure_space(this);
	// 1110 1000 #32-bit disp.
	emit(0xE8);
	if (L->is_bound()) {
		int offset = L->pos() - pc_offset() - sizeof(int32_t);
		assert(offset <= 0);
		emitl(offset);
	} else if (L->is_linked()) {
		emitl(L->pos());
		L->link_to(pc_offset() - sizeof(int32_t));
	} else {
		assert(L->is_unused());
		int32_t current = pc_offset();
		emitl(current);
		L->link_to(current);
	}
}

void Assembler::call(Register adr) {
	EnsureSpace ensure_space(this);
	// Opcode: FF /2 r64.
	emit_optional_rex_32(adr);
	emit(0xFF);
	emit_modrm(0x2, adr);
}

void Assembler::call(const Operand& op) {
	EnsureSpace ensure_space(this);
	// Opcode: FF /2 m64.
	emit_optional_rex_32(op);
	emit(0xFF);
	emit_operand(0x2, op);
}

// Calls directly to the given address using a relative offset.
// Should only ever be used in Code objects for calls within the
// same Code object. Should not be used when generating new code (use labels),
// but only when patching existing code.
void Assembler::call(Address target) {
	EnsureSpace ensure_space(this);
	// 1110 1000 #32-bit disp.
	emit(0xE8);
	Address source = pc_ + 4;
	intptr_t displacement = target - source;
	assert(is_int32(displacement));
	emitl(static_cast<int32_t> (displacement));
}

void Assembler::clc() {
	EnsureSpace ensure_space(this);
	emit(0xF8);
}

void Assembler::cld() {
	EnsureSpace ensure_space(this);
	emit(0xFC);
}

void Assembler::cdq() {
	EnsureSpace ensure_space(this);
	emit(0x99);
}

void Assembler::cmovq(Condition cc, Register dst, Register src) {
	if (cc == always) {
		movq(dst, src);
	} else if (cc == never) {
		return;
	}
	// No need to check CpuInfo for CMOV support, it's a required part of the
	// 64-bit architecture.
	assert(cc >= 0); // Use mov for unconditional moves.
	EnsureSpace ensure_space(this);
	// Opcode: REX.W 0f 40 + cc /r.
	emit_rex_64(dst, src);
	emit(0x0f);
	emit(0x40 + cc);
	emit_modrm(dst, src);
}

void Assembler::cmovq(Condition cc, Register dst, const Operand& src) {
	if (cc == always) {
		movq(dst, src);
	} else if (cc == never) {
		return;
	}
	assert(cc >= 0);
	EnsureSpace ensure_space(this);
	// Opcode: REX.W 0f 40 + cc /r.
	emit_rex_64(dst, src);
	emit(0x0f);
	emit(0x40 + cc);
	emit_operand(dst, src);
}

void Assembler::cmovl(Condition cc, Register dst, Register src) {
	if (cc == always) {
		movl(dst, src);
	} else if (cc == never) {
		return;
	}
	assert(cc >= 0);
	EnsureSpace ensure_space(this);
	// Opcode: 0f 40 + cc /r.
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x40 + cc);
	emit_modrm(dst, src);
}

void Assembler::cmovl(Condition cc, Register dst, const Operand& src) {
	if (cc == always) {
		movl(dst, src);
	} else if (cc == never) {
		return;
	}
	assert(cc >= 0);
	EnsureSpace ensure_space(this);
	// Opcode: 0f 40 + cc /r.
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x40 + cc);
	emit_operand(dst, src);
}

void Assembler::cmpb_al(Immediate imm8) {
	assert(is_int8(imm8.value_) || is_uint8(imm8.value_));
	EnsureSpace ensure_space(this);
	emit(0x3c);
	emit(imm8.value_);
}

void Assembler::cpuid() {
	EnsureSpace ensure_space(this);
	emit(0x0F);
	emit(0xA2);
}

void Assembler::cqo() {
	EnsureSpace ensure_space(this);
	emit_rex_64();
	emit(0x99);
}

void Assembler::decq(Register dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xFF);
	emit_modrm(0x1, dst);
}

void Assembler::decq(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xFF);
	emit_operand(1, dst);
}

void Assembler::decl(Register dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xFF);
	emit_modrm(0x1, dst);
}

void Assembler::decl(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xFF);
	emit_operand(1, dst);
}

void Assembler::decb(Register dst) {
	EnsureSpace ensure_space(this);
	if (dst.code() > 3) {
		// Register is not one of al, bl, cl, dl.  Its encoding needs REX.
		emit_rex_32(dst);
	}
	emit(0xFE);
	emit_modrm(0x1, dst);
}

void Assembler::decb(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xFE);
	emit_operand(1, dst);
}

void Assembler::enter(Immediate size) {
	EnsureSpace ensure_space(this);
	emit(0xC8);
	emitw(size.value_); // 16 bit operand, always.
	emit(0);
}

void Assembler::hlt() {
	EnsureSpace ensure_space(this);
	emit(0xF4);
}

void Assembler::idivq(Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src);
	emit(0xF7);
	emit_modrm(0x7, src);
}

void Assembler::idivl(Register src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(src);
	emit(0xF7);
	emit_modrm(0x7, src);
}

void Assembler::imul(Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src);
	emit(0xF7);
	emit_modrm(0x5, src);
}

void Assembler::imul(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0xAF);
	emit_modrm(dst, src);
}

void Assembler::imul(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0xAF);
	emit_operand(dst, src);
}

void Assembler::imul(Register dst, Register src, Immediate imm) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	if (is_int8(imm.value_)) {
		emit(0x6B);
		emit_modrm(dst, src);
		emit(imm.value_);
	} else {
		emit(0x69);
		emit_modrm(dst, src);
		emitl(imm.value_);
	}
}

void Assembler::imull(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xAF);
	emit_modrm(dst, src);
}

void Assembler::imull(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xAF);
	emit_operand(dst, src);
}

void Assembler::imull(Register dst, Register src, Immediate imm) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	if (is_int8(imm.value_)) {
		emit(0x6B);
		emit_modrm(dst, src);
		emit(imm.value_);
	} else {
		emit(0x69);
		emit_modrm(dst, src);
		emitl(imm.value_);
	}
}

void Assembler::imulq(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0xAF);
	emit_modrm(dst, src);
}

void Assembler::imulq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0xAF);
	emit_operand(dst, src);
}

void Assembler::imulq(Register dst, Register src, Immediate imm) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	if (is_int8(imm.value_)) {
		emit(0x6B);
		emit_modrm(dst, src);
		emit(imm.value_);
	} else {
		emit(0x69);
		emit_modrm(dst, src);
		emitl(imm.value_);
	}
}

void Assembler::incq(Register dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xFF);
	emit_modrm(0x0, dst);
}

void Assembler::incq(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xFF);
	emit_operand(0, dst);
}

void Assembler::incl(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xFF);
	emit_operand(0, dst);
}

void Assembler::incl(Register dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xFF);
	emit_modrm(0, dst);
}

void Assembler::int3() {
	EnsureSpace ensure_space(this);
	emit(0xCC);
}

void Assembler::j(Condition cc, Label* L, Label::Distance distance) {
	if (cc == always) {
		jmp(L);
		return;
	} else if (cc == never) {
		return;
	}
	EnsureSpace ensure_space(this);
	assert(is_uint4(cc));
	if (L->is_bound()) {
		const int short_size = 2;
		const int long_size = 6;
		int offs = L->pos() - pc_offset();
		assert(offs <= 0);
		if (is_int8(offs - short_size)) {
			// 0111 tttn #8-bit disp.
			emit(0x70 | cc);
			emit((offs - short_size) & 0xFF);
		} else {
			// 0000 1111 1000 tttn #32-bit disp.
			emit(0x0F);
			emit(0x80 | cc);
			emitl(offs - long_size);
		}
	} else if (distance == Label::kNear) {
		// 0111 tttn #8-bit disp
		emit(0x70 | cc);
		byte disp = 0x00;
		if (L->is_near_linked()) {
			int offset = L->near_link_pos() - pc_offset();
			assert(is_int8(offset));
			disp = static_cast<byte> (offset & 0xFF);
		}
		L->link_to(pc_offset(), Label::kNear);
		emit(disp);
	} else if (L->is_linked()) {
		// 0000 1111 1000 tttn #32-bit disp.
		emit(0x0F);
		emit(0x80 | cc);
		emitl(L->pos());
		L->link_to(pc_offset() - sizeof(int32_t));
	} else {
		assert(L->is_unused());
		emit(0x0F);
		emit(0x80 | cc);
		int32_t current = pc_offset();
		emitl(current);
		L->link_to(current);
	}
}

void Assembler::jmp(Label* L, Label::Distance distance) {
	EnsureSpace ensure_space(this);
	const int short_size = sizeof(int8_t);
	const int long_size = sizeof(int32_t);
	if (L->is_bound()) {
		int offs = L->pos() - pc_offset() - 1;
		assert(offs <= 0);
		if (is_int8(offs - short_size)) {
			// 1110 1011 #8-bit disp.
			emit(0xEB);
			emit((offs - short_size) & 0xFF);
		} else {
			// 1110 1001 #32-bit disp.
			emit(0xE9);
			emitl(offs - long_size);
		}
	} else if (distance == Label::kNear) {
		emit(0xEB);
		byte disp = 0x00;
		if (L->is_near_linked()) {
			int offset = L->near_link_pos() - pc_offset();
			assert(is_int8(offset));
			disp = static_cast<byte> (offset & 0xFF);
		}
		L->link_to(pc_offset(), Label::kNear);
		emit(disp);
	} else if (L->is_linked()) {
		// 1110 1001 #32-bit disp.
		emit(0xE9);
		emitl(L->pos());
		L->link_to(pc_offset() - long_size);
	} else {
		// 1110 1001 #32-bit disp.
		assert(L->is_unused());
		emit(0xE9);
		int32_t current = pc_offset();
		emitl(current);
		L->link_to(current);
	}
}

void Assembler::jmp(Register target) {
	EnsureSpace ensure_space(this);
	// Opcode FF/4 r64.
	emit_optional_rex_32(target);
	emit(0xFF);
	emit_modrm(0x4, target);
}

void Assembler::jmp(const Operand& src) {
	EnsureSpace ensure_space(this);
	// Opcode FF/4 m64.
	emit_optional_rex_32(src);
	emit(0xFF);
	emit_operand(0x4, src);
}

void Assembler::lea(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x8D);
	emit_operand(dst, src);
}

void Assembler::leal(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x8D);
	emit_operand(dst, src);
}

void Assembler::load_rax(void* value) {
	EnsureSpace ensure_space(this);
	emit(0x48); // REX.W
	emit(0xA1);
	emitq(reinterpret_cast<uintptr_t> (value));
}

void Assembler::leave() {
	EnsureSpace ensure_space(this);
	emit(0xC9);
}

void Assembler::movb(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	if (dst.code() > 3) {
		// Register is not one of al, bl, cl, dl.  Its encoding needs REX.
		emit_rex_32(dst, src);
	} else {
		emit_optional_rex_32(dst, src);
	}
	emit(0x8A);
	emit_operand(dst, src);
}

void Assembler::movb(Register dst, Immediate imm) {
	EnsureSpace ensure_space(this);
	if (dst.code() > 3) {
		emit_rex_32(dst);
	}
	emit(0xB0 + dst.low_bits());
	emit(imm.value_);
}

void Assembler::movb(const Operand& dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.code() > 3) {
		emit_rex_32(src, dst);
	} else {
		emit_optional_rex_32(src, dst);
	}
	emit(0x88);
	emit_operand(src, dst);
}

void Assembler::movw(const Operand& dst, Register src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(src, dst);
	emit(0x89);
	emit_operand(src, dst);
}

void Assembler::movl(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x8B);
	emit_operand(dst, src);
}

void Assembler::movl(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		emit_optional_rex_32(src, dst);
		emit(0x89);
		emit_modrm(src, dst);
	} else {
		emit_optional_rex_32(dst, src);
		emit(0x8B);
		emit_modrm(dst, src);
	}
}

void Assembler::movl(const Operand& dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(src, dst);
	emit(0x89);
	emit_operand(src, dst);
}

void Assembler::movl(const Operand& dst, Immediate value) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xC7);
	emit_operand(0x0, dst);
	emit(value);
}

void Assembler::movl(Register dst, Immediate value) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xB8 + dst.low_bits());
	emit(value);
}

void Assembler::movq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x8B);
	emit_operand(dst, src);
}

void Assembler::movq(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		emit_rex_64(src, dst);
		emit(0x89);
		emit_modrm(src, dst);
	} else {
		emit_rex_64(dst, src);
		emit(0x8B);
		emit_modrm(dst, src);
	}
}

void Assembler::movq(Register dst, Immediate value) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xC7);
	emit_modrm(0x0, dst);
	emit(value); // Only 32-bit immediates are possible, not 8-bit immediates.
}

void Assembler::movq(const Operand& dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src, dst);
	emit(0x89);
	emit_operand(src, dst);
}

void Assembler::movq(Register dst, void* value) {
	// This method must not be used with heap object references. The stored
	// address is not GC safe. Use the handle version instead.
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xB8 | dst.low_bits());
	emitq(reinterpret_cast<uintptr_t> (value));
}

void Assembler::movq(Register dst, int64_t value) {
	// Non-relocatable values might not need a 64-bit representation.

	// Sadly, there is no zero or sign extending move for 8-bit immediates.
	if (is_int32(value)) {
		movq(dst, Immediate(static_cast<int32_t> (value)));
		return;
	} else if (is_uint32(value)) {
		movl(dst, Immediate(static_cast<int32_t> (value)));
		return;
	}
	// Value cannot be represented by 32 bits, so do a full 64 bit immediate
	// value.
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xB8 | dst.low_bits());
	emitq(value);
}

void Assembler::movq(const Operand& dst, Immediate value) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xC7);
	emit_operand(0, dst);
	emit(value);
}

// Loads the ip-relative location of the src label into the target location
// (as a 32-bit offset sign extended to 64-bit).
void Assembler::movl(const Operand& dst, Label* src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xC7);
	emit_operand(0, dst);
	if (src->is_bound()) {
		int offset = src->pos() - pc_offset() - sizeof(int32_t);
		assert(offset <= 0);
		emitl(offset);
	} else if (src->is_linked()) {
		emitl(src->pos());
		src->link_to(pc_offset() - sizeof(int32_t));
	} else {
		assert(src->is_unused());
		int32_t current = pc_offset();
		emitl(current);
		src->link_to(current);
	}
}

void Assembler::pmovsxbq(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x38);
	emit(0x22);
	emit_sse_operand(dst, src);
}

void Assembler::movsxbq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0xBE);
	emit_operand(dst, src);
}

void Assembler::movsxwq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0xBF);
	emit_operand(dst, src);
}

void Assembler::movsxlq(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x63);
	emit_modrm(dst, src);
}

void Assembler::movsxlq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst, src);
	emit(0x63);
	emit_operand(dst, src);
}

void Assembler::movzxbq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xB6);
	emit_operand(dst, src);
}

void Assembler::movzxbl(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xB6);
	emit_operand(dst, src);
}

void Assembler::movzxwq(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xB7);
	emit_operand(dst, src);
}

void Assembler::movzxwl(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xB7);
	emit_operand(dst, src);
}

void Assembler::repmovsb() {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit(0xA4);
}

void Assembler::repmovsw() {
	EnsureSpace ensure_space(this);
	emit(0x66); // Operand size override.
	emit(0xF3);
	emit(0xA4);
}

void Assembler::repmovsl() {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit(0xA5);
}

void Assembler::repmovsq() {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_rex_64();
	emit(0xA5);
}

void Assembler::mul(Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src);
	emit(0xF7);
	emit_modrm(0x4, src);
}

void Assembler::neg(Register dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xF7);
	emit_modrm(0x3, dst);
}

void Assembler::negl(Register dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xF7);
	emit_modrm(0x3, dst);
}

void Assembler::neg(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xF7);
	emit_operand(3, dst);
}

void Assembler::nop() {
	EnsureSpace ensure_space(this);
	emit(0x90);
}

void Assembler::not_(Register dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xF7);
	emit_modrm(0x2, dst);
}

void Assembler::not_(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_rex_64(dst);
	emit(0xF7);
	emit_operand(2, dst);
}

void Assembler::notl(Register dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0xF7);
	emit_modrm(0x2, dst);
}

void Assembler::nop(int n) {
	// The recommended muti-byte sequences of NOP instructions from the Intel 64
	// and IA-32 Architectures Software Developer's Manual.
	//
	// Length   Assembly                                Byte Sequence
	// 2 bytes  66 NOP                                  66 90H
	// 3 bytes  NOP DWORD ptr [EAX]                     0F 1F 00H
	// 4 bytes  NOP DWORD ptr [EAX + 00H]               0F 1F 40 00H
	// 5 bytes  NOP DWORD ptr [EAX + EAX*1 + 00H]       0F 1F 44 00 00H
	// 6 bytes  66 NOP DWORD ptr [EAX + EAX*1 + 00H]    66 0F 1F 44 00 00H
	// 7 bytes  NOP DWORD ptr [EAX + 00000000H]         0F 1F 80 00 00 00 00H
	// 8 bytes  NOP DWORD ptr [EAX + EAX*1 + 00000000H] 0F 1F 84 00 00 00 00 00H
	// 9 bytes  66 NOP DWORD ptr [EAX + EAX*1 +         66 0F 1F 84 00 00 00 00
	//          00000000H]                              00H

	assert(1 <= n);
	assert(n <= 9);
	EnsureSpace ensure_space(this);
	switch (n) {
	case 1:
		emit(0x90);
		return;
	case 2:
		emit(0x66);
		emit(0x90);
		return;
	case 3:
		emit(0x0f);
		emit(0x1f);
		emit(0x00);
		return;
	case 4:
		emit(0x0f);
		emit(0x1f);
		emit(0x40);
		emit(0x00);
		return;
	case 5:
		emit(0x0f);
		emit(0x1f);
		emit(0x44);
		emit(0x00);
		emit(0x00);
		return;
	case 6:
		emit(0x66);
		emit(0x0f);
		emit(0x1f);
		emit(0x44);
		emit(0x00);
		emit(0x00);
		return;
	case 7:
		emit(0x0f);
		emit(0x1f);
		emit(0x80);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		return;
	case 8:
		emit(0x0f);
		emit(0x1f);
		emit(0x84);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		return;
	case 9:
		emit(0x66);
		emit(0x0f);
		emit(0x1f);
		emit(0x84);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		emit(0x00);
		return;
	}
}

void Assembler::pop(Register dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0x58 | dst.low_bits());
}

void Assembler::pop(const Operand& dst) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst);
	emit(0x8F);
	emit_operand(0, dst);
}

void Assembler::popfq() {
	EnsureSpace ensure_space(this);
	emit(0x9D);
}

void Assembler::push(Register src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(src);
	emit(0x50 | src.low_bits());
}

void Assembler::push(const Operand& src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(src);
	emit(0xFF);
	emit_operand(6, src);
}

void Assembler::push(Immediate value) {
	EnsureSpace ensure_space(this);
	if (is_int8(value.value_)) {
		emit(0x6A);
		emit(value.value_); // Emit low byte of value.
	} else {
		emit(0x68);
		emitl(value.value_);
	}
}

void Assembler::push_imm32(int32_t imm32) {
	EnsureSpace ensure_space(this);
	emit(0x68);
	emitl(imm32);
}

void Assembler::pushfq() {
	EnsureSpace ensure_space(this);
	emit(0x9C);
}

void Assembler::rdtsc() {
	EnsureSpace ensure_space(this);
	emit(0x0F);
	emit(0x31);
}

void Assembler::ret(int imm16) {
	EnsureSpace ensure_space(this);
	assert(is_uint16(imm16));
	if (imm16 == 0) {
		emit(0xC3);
	} else {
		emit(0xC2);
		emit(imm16 & 0xFF);
		emit((imm16 >> 8) & 0xFF);
	}
}

void Assembler::setcc(Condition cc, Register reg) {
	if (cc > last_condition) {
		movb(reg, Immediate(cc == always ? 1 : 0));
		return;
	}
	EnsureSpace ensure_space(this);
	assert(is_uint4(cc));
	if (reg.code() > 3) { // Use x64 byte registers, where different.
		emit_rex_32(reg);
	}
	emit(0x0F);
	emit(0x90 | cc);
	emit_modrm(0x0, reg);
}

void Assembler::shld(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0xA5);
	emit_modrm(src, dst);
}

void Assembler::shrd(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0xAD);
	emit_modrm(src, dst);
}

void Assembler::xchg(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.is(rax) || dst.is(rax)) { // Single-byte encoding
		Register other = src.is(rax) ? dst : src;
		emit_rex_64(other);
		emit(0x90 | other.low_bits());
	} else if (dst.low_bits() == 4) {
		emit_rex_64(dst, src);
		emit(0x87);
		emit_modrm(dst, src);
	} else {
		emit_rex_64(src, dst);
		emit(0x87);
		emit_modrm(src, dst);
	}
}

void Assembler::store_rax(void* dst) {
	EnsureSpace ensure_space(this);
	emit(0x48); // REX.W
	emit(0xA3);
	emitq(reinterpret_cast<uintptr_t> (dst));
}

void Assembler::testb(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		emit_rex_32(src, dst);
		emit(0x84);
		emit_modrm(src, dst);
	} else {
		if (dst.code() > 3 || src.code() > 3) {
			// Register is not one of al, bl, cl, dl.  Its encoding needs REX.
			emit_rex_32(dst, src);
		}
		emit(0x84);
		emit_modrm(dst, src);
	}
}

void Assembler::testb(Register reg, Immediate mask) {
	assert(is_int8(mask.value_) || is_uint8(mask.value_));
	EnsureSpace ensure_space(this);
	if (reg.is(rax)) {
		emit(0xA8);
		emit(mask.value_); // Low byte emitted.
	} else {
		if (reg.code() > 3) {
			// Register is not one of al, bl, cl, dl.  Its encoding needs REX.
			emit_rex_32(reg);
		}
		emit(0xF6);
		emit_modrm(0x0, reg);
		emit(mask.value_); // Low byte emitted.
	}
}

void Assembler::testb(const Operand& op, Immediate mask) {
	assert(is_int8(mask.value_) || is_uint8(mask.value_));
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(rax, op);
	emit(0xF6);
	emit_operand(rax, op); // Operation code 0
	emit(mask.value_); // Low byte emitted.
}

void Assembler::testb(const Operand& op, Register reg) {
	EnsureSpace ensure_space(this);
	if (reg.code() > 3) {
		// Register is not one of al, bl, cl, dl.  Its encoding needs REX.
		emit_rex_32(reg, op);
	} else {
		emit_optional_rex_32(reg, op);
	}
	emit(0x84);
	emit_operand(reg, op);
}

void Assembler::testl(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		emit_optional_rex_32(src, dst);
		emit(0x85);
		emit_modrm(src, dst);
	} else {
		emit_optional_rex_32(dst, src);
		emit(0x85);
		emit_modrm(dst, src);
	}
}

void Assembler::testl(Register reg, Immediate mask) {
	// testl with a mask that fits in the low byte is exactly testb.
	if (is_uint8(mask.value_)) {
		testb(reg, mask);
		return;
	}
	EnsureSpace ensure_space(this);
	if (reg.is(rax)) {
		emit(0xA9);
		emit(mask);
	} else {
		emit_optional_rex_32(rax, reg);
		emit(0xF7);
		emit_modrm(0x0, reg);
		emit(mask);
	}
}

void Assembler::testl(const Operand& op, Immediate mask) {
	// testl with a mask that fits in the low byte is exactly testb.
	if (is_uint8(mask.value_)) {
		testb(op, mask);
		return;
	}
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(rax, op);
	emit(0xF7);
	emit_operand(rax, op); // Operation code 0
	emit(mask);
}

void Assembler::testq(const Operand& op, Register reg) {
	EnsureSpace ensure_space(this);
	emit_rex_64(reg, op);
	emit(0x85);
	emit_operand(reg, op);
}

void Assembler::testq(Register dst, Register src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		emit_rex_64(src, dst);
		emit(0x85);
		emit_modrm(src, dst);
	} else {
		emit_rex_64(dst, src);
		emit(0x85);
		emit_modrm(dst, src);
	}
}

void Assembler::testq(Register dst, Immediate mask) {
	EnsureSpace ensure_space(this);
	if (dst.is(rax)) {
		emit_rex_64();
		emit(0xA9);
		emit(mask);
	} else {
		emit_rex_64(dst);
		emit(0xF7);
		emit_modrm(0, dst);
		emit(mask);
	}
}

// FPU instructions.


void Assembler::fld(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xD9, 0xC0, i);
}

void Assembler::fld1() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xE8);
}

void Assembler::fldz() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xEE);
}

void Assembler::fldpi() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xEB);
}

void Assembler::fldln2() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xED);
}

void Assembler::fld_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xD9);
	emit_operand(0, adr);
}

void Assembler::fld_d(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDD);
	emit_operand(0, adr);
}

void Assembler::fstp_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xD9);
	emit_operand(3, adr);
}

void Assembler::fstp_d(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDD);
	emit_operand(3, adr);
}

void Assembler::fstp(int index) {
	assert(is_uint3(index));
	EnsureSpace ensure_space(this);
	emit_farith(0xDD, 0xD8, index);
}

void Assembler::fild_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDB);
	emit_operand(0, adr);
}

void Assembler::fild_d(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDF);
	emit_operand(5, adr);
}

void Assembler::fistp_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDB);
	emit_operand(3, adr);
}

void Assembler::fisttp_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDB);
	emit_operand(1, adr);
}

void Assembler::fisttp_d(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDD);
	emit_operand(1, adr);
}

void Assembler::fist_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDB);
	emit_operand(2, adr);
}

void Assembler::fistp_d(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDF);
	emit_operand(7, adr);
}

void Assembler::fabs() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xE1);
}

void Assembler::fchs() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xE0);
}

void Assembler::fcos() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xFF);
}

void Assembler::fsin() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xFE);
}

void Assembler::fyl2x() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xF1);
}

void Assembler::fadd(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDC, 0xC0, i);
}

void Assembler::fsub(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDC, 0xE8, i);
}

void Assembler::fisub_s(const Operand& adr) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(adr);
	emit(0xDA);
	emit_operand(4, adr);
}

void Assembler::fmul(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDC, 0xC8, i);
}

void Assembler::fdiv(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDC, 0xF8, i);
}

void Assembler::faddp(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDE, 0xC0, i);
}

void Assembler::fsubp(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDE, 0xE8, i);
}

void Assembler::fsubrp(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDE, 0xE0, i);
}

void Assembler::fmulp(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDE, 0xC8, i);
}

void Assembler::fdivp(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDE, 0xF8, i);
}

void Assembler::fprem() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xF8);
}

void Assembler::fprem1() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xF5);
}

void Assembler::fxch(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xD9, 0xC8, i);
}

void Assembler::fincstp() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xF7);
}

void Assembler::ffree(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDD, 0xC0, i);
}

void Assembler::ftst() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xE4);
}

void Assembler::fucomp(int i) {
	EnsureSpace ensure_space(this);
	emit_farith(0xDD, 0xE8, i);
}

void Assembler::fucompp() {
	EnsureSpace ensure_space(this);
	emit(0xDA);
	emit(0xE9);
}

void Assembler::fucomi(int i) {
	EnsureSpace ensure_space(this);
	emit(0xDB);
	emit(0xE8 + i);
}

void Assembler::fucomip() {
	EnsureSpace ensure_space(this);
	emit(0xDF);
	emit(0xE9);
}

void Assembler::fcompp() {
	EnsureSpace ensure_space(this);
	emit(0xDE);
	emit(0xD9);
}

void Assembler::fnstsw_ax() {
	EnsureSpace ensure_space(this);
	emit(0xDF);
	emit(0xE0);
}

void Assembler::fwait() {
	EnsureSpace ensure_space(this);
	emit(0x9B);
}

void Assembler::frndint() {
	EnsureSpace ensure_space(this);
	emit(0xD9);
	emit(0xFC);
}

void Assembler::fnclex() {
	EnsureSpace ensure_space(this);
	emit(0xDB);
	emit(0xE2);
}

void Assembler::sahf() {
	// TODO(X64): Test for presence. Not all 64-bit intel CPU's have sahf
	// in 64-bit mode. Test CpuID.
	EnsureSpace ensure_space(this);
	emit(0x9E);
}

void Assembler::emit_farith(int b1, int b2, int i) {
	assert(is_uint8(b1) && is_uint8(b2)); // wrong opcode
	assert(is_uint3(i)); // illegal stack offset
	emit(b1);
	emit(b2 + i);
}

// SSE 2 operations.

void Assembler::movd(XMMRegister dst, Register src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x6E);
	emit_sse_operand(dst, src);
}

void Assembler::movd(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(src, dst);
	emit(0x0F);
	emit(0x7E);
	emit_sse_operand(src, dst);
}

void Assembler::movq(XMMRegister dst, Register src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x6E);
	emit_sse_operand(dst, src);
}

void Assembler::movq(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0x7E);
	emit_sse_operand(src, dst);
}

void Assembler::movq(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	if (dst.low_bits() == 4) {
		// Avoid unnecessary SIB byte.
		emit(0xf3);
		emit_optional_rex_32(dst, src);
		emit(0x0F);
		emit(0x7e);
		emit_sse_operand(dst, src);
	} else {
		emit(0x66);
		emit_optional_rex_32(src, dst);
		emit(0x0F);
		emit(0xD6);
		emit_sse_operand(src, dst);
	}
}

void Assembler::movdqa(const Operand& dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0x7F);
	emit_sse_operand(src, dst);
}

void Assembler::movdqa(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x6F);
	emit_sse_operand(dst, src);
}

void Assembler::movdqu(const Operand& dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0x7F);
	emit_sse_operand(src, dst);
}

void Assembler::movdqu(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x6F);
	emit_sse_operand(dst, src);
}

void Assembler::extractps(Register dst, XMMRegister src, byte imm8) {
	assert(is_uint2(imm8));
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x3A);
	emit(0x17);
	emit_sse_operand(dst, src);
	emit(imm8);
}

void Assembler::movsd(const Operand& dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2); // double
	emit_optional_rex_32(src, dst);
	emit(0x0F);
	emit(0x11); // store
	emit_sse_operand(src, dst);
}

void Assembler::movsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2); // double
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x10); // load
	emit_sse_operand(dst, src);
}

void Assembler::movsd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2); // double
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x10); // load
	emit_sse_operand(dst, src);
}

void Assembler::movq(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF3); // double
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x7E); // load
	emit_sse_operand(dst, src);
}

void Assembler::movq(const Operand& dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // double
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0xD6); // load
	emit_sse_operand(src, dst);
}

void Assembler::movlpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // double
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x12); // load
	emit_sse_operand(dst, src);
}

void Assembler::movlpd(const Operand& dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // double
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0x13); // load
	emit_sse_operand(src, dst);
}

void Assembler::movhpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // double
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x16); // load
	emit_sse_operand(dst, src);
}

void Assembler::movhpd(const Operand& dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66); // double
	emit_rex_64(src, dst);
	emit(0x0F);
	emit(0x17); // load
	emit_sse_operand(src, dst);
}

void Assembler::movlhps(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x16);
	emit_sse_operand(dst, src);
}

void Assembler::movhlps(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x12);
	emit_sse_operand(dst, src);
}

void Assembler::movaps(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		// Try to avoid an unnecessary SIB byte.
		emit_optional_rex_32(src, dst);
		emit(0x0F);
		emit(0x29);
		emit_sse_operand(src, dst);
	} else {
		emit_optional_rex_32(dst, src);
		emit(0x0F);
		emit(0x28);
		emit_sse_operand(dst, src);
	}
}

void Assembler::movapd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	if (src.low_bits() == 4) {
		// Try to avoid an unnecessary SIB byte.
		emit(0x66);
		emit_optional_rex_32(src, dst);
		emit(0x0F);
		emit(0x29);
		emit_sse_operand(src, dst);
	} else {
		emit(0x66);
		emit_optional_rex_32(dst, src);
		emit(0x0F);
		emit(0x28);
		emit_sse_operand(dst, src);
	}
}

void Assembler::movss(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF3); // single
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x10); // load
	emit_sse_operand(dst, src);
}

void Assembler::movss(const Operand& src, XMMRegister dst) {
	EnsureSpace ensure_space(this);
	emit(0xF3); // single
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x11); // store
	emit_sse_operand(dst, src);
}

void Assembler::cvttss2si(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2C);
	emit_operand(dst, src);
}

void Assembler::cvttss2si(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2C);
	emit_sse_operand(dst, src);
}

void Assembler::cvttsd2si(Register dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2C);
	emit_operand(dst, src);
}

void Assembler::cvttsd2si(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2C);
	emit_sse_operand(dst, src);
}

void Assembler::cvttsd2siq(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x2C);
	emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2sd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2sd(XMMRegister dst, Register src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtlsi2ss(XMMRegister dst, Register src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtqsi2sd(XMMRegister dst, Register src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x2A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtss2sd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtss2sd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF3);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtsd2ss(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5A);
	emit_sse_operand(dst, src);
}

void Assembler::cvtsd2si(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x2D);
	emit_sse_operand(dst, src);
}

void Assembler::cvtsd2siq(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_rex_64(dst, src);
	emit(0x0F);
	emit(0x2D);
	emit_sse_operand(dst, src);
}

void Assembler::addsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x58);
	emit_sse_operand(dst, src);
}
void Assembler::addsd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x58);
	emit_sse_operand(dst, src);
}

void Assembler::addpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x58);
	emit_sse_operand(dst, src);
}
void Assembler::addpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x58);
	emit_sse_operand(dst, src);
}

void Assembler::minsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5D);
	emit_sse_operand(dst, src);
}
void Assembler::minsd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5D);
	emit_sse_operand(dst, src);
}
void Assembler::minpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5D);
	emit_sse_operand(dst, src);
}
void Assembler::minpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5D);
	emit_sse_operand(dst, src);
}

void Assembler::maxpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5F);
	emit_sse_operand(dst, src);
}
void Assembler::maxpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5F);
	emit_sse_operand(dst, src);
}

void Assembler::maxsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5F);
	emit_sse_operand(dst, src);
}
void Assembler::maxsd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5F);
	emit_sse_operand(dst, src);
}

void Assembler::pshufb(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x38);
	emit(0x00);
	emit_sse_operand(dst, src);
}
void Assembler::pand(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xDB);
	emit_sse_operand(dst, src);
}
void Assembler::pand(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xDB);
	emit_sse_operand(dst, src);
}
void Assembler::por(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xEB);
	emit_sse_operand(dst, src);
}
void Assembler::por(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xEB);
	emit_sse_operand(dst, src);
}
void Assembler::pxor(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xEF);
	emit_sse_operand(dst, src);
}
void Assembler::pxor(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xEF);
	emit_sse_operand(dst, src);
}
void Assembler::blendvpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x38);
	emit(0x15);
	emit_sse_operand(dst, src);
}
void Assembler::blendvpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x38);
	emit(0x15);
	emit_sse_operand(dst, src);
}
void Assembler::paddq(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xD4);
	emit_sse_operand(dst, src);
}
void Assembler::paddq(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xD4);
	emit_sse_operand(dst, src);
}
void Assembler::psubq(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xFB);
	emit_sse_operand(dst, src);
}
void Assembler::psubq(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0xFB);
	emit_sse_operand(dst, src);
}
void Assembler::pcmpeqq(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x38);
	emit(0x29);
	emit_sse_operand(dst, src);
}
void Assembler::pcmpeqq(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x38);
	emit(0x29);
	emit_sse_operand(dst, src);
}
void Assembler::haddpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x7C);
	emit_sse_operand(dst, src);
}
void Assembler::hsubpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x7D);
	emit_sse_operand(dst, src);
}

void Assembler::unpcklpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x14);
	emit_sse_operand(dst, src);
}
void Assembler::unpckhpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x15);
	emit_sse_operand(dst, src);
}
void Assembler::mulsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x59);
	emit_sse_operand(dst, src);
}
void Assembler::mulsd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x59);
	emit_sse_operand(dst, src);
}
void Assembler::mulpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x59);
	emit_sse_operand(dst, src);
}
void Assembler::mulpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x59);
	emit_sse_operand(dst, src);
}

void Assembler::subsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5C);
	emit_sse_operand(dst, src);
}
void Assembler::subsd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5C);
	emit_sse_operand(dst, src);
}

void Assembler::subpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5C);
	emit_sse_operand(dst, src);
}
void Assembler::subpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5C);
	emit_sse_operand(dst, src);
}

void Assembler::divsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5E);
	emit_sse_operand(dst, src);
}
void Assembler::divpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x5E);
	emit_sse_operand(dst, src);
}

void Assembler::andpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x54);
	emit_sse_operand(dst, src);
}
void Assembler::andpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x54);
	emit_sse_operand(dst, src);
}

void Assembler::orpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x56);
	emit_sse_operand(dst, src);
}
void Assembler::orpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x56);
	emit_sse_operand(dst, src);
}

void Assembler::xorpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x57);
	emit_sse_operand(dst, src);
}
void Assembler::xorpd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x57);
	emit_sse_operand(dst, src);
}

void Assembler::xorps(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x57);
	emit_sse_operand(dst, src);
}

void Assembler::sqrtsd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0xF2);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x51);
	emit_sse_operand(dst, src);
}
void Assembler::sqrtpd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0F);
	emit(0x51);
	emit_sse_operand(dst, src);
}

void Assembler::ucomisd(XMMRegister dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x2e);
	emit_sse_operand(dst, src);
}

void Assembler::ucomisd(XMMRegister dst, const Operand& src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x2e);
	emit_sse_operand(dst, src);
}

void Assembler::roundsd(XMMRegister dst, XMMRegister src,
		Assembler::RoundingMode mode) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x3a);
	emit(0x0b);
	emit_sse_operand(dst, src);
	// Mask precision exeption.
	emit(static_cast<byte> (mode) | 0x8);
}
void Assembler::roundpd(XMMRegister dst, XMMRegister src,
		Assembler::RoundingMode mode) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x3a);
	emit(0x09);
	emit_sse_operand(dst, src);
	// Mask precision exeption.
	emit(static_cast<byte> (mode) | 0x8);
}

void Assembler::cmppd(XMMRegister dst, XMMRegister src,
		Assembler::ComparisonType mode) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0xc2);
	emit_sse_operand(dst, src);
	// Mask precision exeption.
	emit(static_cast<byte> (mode));
}
void Assembler::cmppd(XMMRegister dst, const Operand& src,
		Assembler::ComparisonType mode) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0xc2);
	emit_sse_operand(dst, src);
	// Mask precision exeption.
	emit(static_cast<byte> (mode));
}

void Assembler::shufpd(XMMRegister dst, XMMRegister src,
		Assembler::ShuffleModeD mode) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0xc6);
	emit_sse_operand(dst, src);
	// Mask precision exeption.
	emit(static_cast<byte> (mode) | 0x8);
}

void Assembler::movmskpd(Register dst, XMMRegister src) {
	EnsureSpace ensure_space(this);
	emit(0x66);
	emit_optional_rex_32(dst, src);
	emit(0x0f);
	emit(0x50);
	emit_sse_operand(dst, src);
}

void Assembler::emit_sse_operand(XMMRegister reg, const Operand& adr) {
	Register ireg = { reg.code() };
	emit_operand(ireg, adr);
}

void Assembler::emit_sse_operand(XMMRegister dst, XMMRegister src) {
	emit(0xC0 | (dst.low_bits() << 3) | src.low_bits());
}

void Assembler::emit_sse_operand(XMMRegister dst, Register src) {
	emit(0xC0 | (dst.low_bits() << 3) | src.low_bits());
}

void Assembler::emit_sse_operand(Register dst, XMMRegister src) {
	emit(0xC0 | (dst.low_bits() << 3) | src.low_bits());
}

void Assembler::db(uint8_t data) {
	EnsureSpace ensure_space(this);
	emit(data);
}

void Assembler::dd(uint32_t data) {
	EnsureSpace ensure_space(this);
	emitl(data);
}

}
} // namespace v8::internal

