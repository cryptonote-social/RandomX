/*
Copyright (c) 2018-2019, tevador <tevador@gmail.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdexcept>
#include <cstring>
#include <climits>
#include <stdio.h>
#include "jit_compiler_x86.hpp"
#include "jit_compiler_x86_static.hpp"
#include "superscalar.hpp"
#include "program.hpp"
#include "reciprocal.h"
#include "virtual_memory.hpp"

namespace randomx {
	/*

	REGISTER ALLOCATION:

	; rax -> temporary
	; rbx -> iteration counter "ic"
	; rcx -> temporary
	; rdx -> temporary
	; rsi -> scratchpad pointer
	; rdi -> dataset pointer
	; rbp -> memory registers "ma" (high 32 bits), "mx" (low 32 bits)
	; rsp -> stack pointer
	; r8  -> "r0"
	; r9  -> "r1"
	; r10 -> "r2"
	; r11 -> "r3"
	; r12 -> "r4"
	; r13 -> "r5"
	; r14 -> "r6"
	; r15 -> "r7"
	; xmm0 -> "f0"
	; xmm1 -> "f1"
	; xmm2 -> "f2"
	; xmm3 -> "f3"
	; xmm4 -> "e0"
	; xmm5 -> "e1"
	; xmm6 -> "e2"
	; xmm7 -> "e3"
	; xmm8 -> "a0"
	; xmm9 -> "a1"
	; xmm10 -> "a2"
	; xmm11 -> "a3"
	; xmm12 -> temporary
	; xmm13 -> E 'and' mask = 0x00ffffffffffffff00ffffffffffffff
	; xmm14 -> E 'or' mask  = 0x3*00000000******3*00000000******
	; xmm15 -> scale mask   = 0x81f000000000000081f0000000000000

	*/

	//Calculate the required code buffer size that is sufficient for the largest possible program:

	constexpr size_t MaxRandomXInstrCodeSize = 32;   //FDIV_M requires up to 32 bytes of x86 code
	constexpr size_t MaxSuperscalarInstrSize = 14;   //IMUL_RCP requires 14 bytes of x86 code
	constexpr size_t SuperscalarProgramHeader = 128; //overhead per superscalar program
	constexpr size_t CodeAlign = 4096;               //align code size to a multiple of 4 KiB
	constexpr size_t ReserveCodeSize = CodeAlign;    //function prologue/epilogue + reserve

	constexpr size_t RandomXCodeSize = alignSize(ReserveCodeSize + MaxRandomXInstrCodeSize * RANDOMX_PROGRAM_SIZE, CodeAlign);
	constexpr size_t SuperscalarSize = alignSize(ReserveCodeSize + (SuperscalarProgramHeader + MaxSuperscalarInstrSize * SuperscalarMaxSize) * RANDOMX_CACHE_ACCESSES, CodeAlign);

	static_assert(RandomXCodeSize < INT32_MAX / 2, "RandomXCodeSize is too large");
	static_assert(SuperscalarSize < INT32_MAX / 2, "SuperscalarSize is too large");

	constexpr uint32_t CodeSize = RandomXCodeSize + SuperscalarSize;

	constexpr int32_t superScalarHashOffset = RandomXCodeSize;

	const uint8_t* codePrologue = (uint8_t*)&randomx_program_prologue;
	const uint8_t* codeLoopBegin = (uint8_t*)&randomx_program_loop_load;
	const uint8_t* codeLoopLoad = (uint8_t*)&randomx_program_loop_load;
	const uint8_t* codeProgramStart = (uint8_t*)&randomx_program_read_dataset;
	const uint8_t* codeReadDataset = (uint8_t*)&randomx_program_read_dataset;
	const uint8_t* codeReadDatasetLightSshInit = (uint8_t*)&randomx_program_read_dataset_sshash_init;
	const uint8_t* codeReadDatasetLightSshFin = (uint8_t*)&randomx_program_read_dataset_sshash_fin;
	const uint8_t* codeDatasetInit = (uint8_t*)&randomx_dataset_init;
	const uint8_t* codeLoopStore = (uint8_t*)&randomx_program_loop_store;
	const uint8_t* codeLoopEnd = (uint8_t*)&randomx_program_loop_end;
	const uint8_t* codeEpilogue = (uint8_t*)&randomx_program_epilogue;
	const uint8_t* codeProgramEnd = (uint8_t*)&randomx_program_end;
	const uint8_t* codeShhLoad = (uint8_t*)&randomx_sshash_load;
	const uint8_t* codeShhPrefetch = (uint8_t*)&randomx_sshash_prefetch;
	const uint8_t* codeShhEnd = (uint8_t*)&randomx_sshash_end;
	const uint8_t* codeShhInit = (uint8_t*)&randomx_sshash_init;

	const int32_t prologueSize = codeLoopBegin - codePrologue;
	const int32_t loopLoadSize = codeProgramStart - codeLoopLoad;
	const int32_t readDatasetSize = codeReadDatasetLightSshInit - codeReadDataset;
	const int32_t readDatasetLightInitSize = codeReadDatasetLightSshFin - codeReadDatasetLightSshInit;
	const int32_t readDatasetLightFinSize = codeLoopStore - codeReadDatasetLightSshFin;
	const int32_t loopStoreSize = codeLoopEnd - codeLoopStore;
	const int32_t datasetInitSize = codeEpilogue - codeDatasetInit;
	const int32_t epilogueSize = codeShhLoad - codeEpilogue;
	const int32_t codeSshLoadSize = codeShhPrefetch - codeShhLoad;
	const int32_t codeSshPrefetchSize = codeShhEnd - codeShhPrefetch;
	const int32_t codeSshInitSize = codeProgramEnd - codeShhInit;

	const int32_t xmmConstantsOffset = (uint8_t*)&randomx_program_xmm_constants - codePrologue;
	const int32_t epilogueOffset = CodeSize - epilogueSize;

	static const uint8_t REX_ADD_RM[] = { 0x4c, 0x03 };
	static const uint8_t REX_SUB_RR[] = { 0x4d, 0x2b };
	static const uint8_t REX_SUB_RM[] = { 0x4c, 0x2b };
	static const uint8_t REX_MOV_RR[] = { 0x41, 0x8b };
	static const uint8_t REX_MOV_RR64[] = { 0x49, 0x8b };
	static const uint8_t REX_MOV_R64R[] = { 0x4c, 0x8b };
	static const uint8_t REX_IMUL_RR[] = { 0x4d, 0x0f, 0xaf };
	static const uint8_t REX_IMUL_RRI[] = { 0x4d, 0x69 };
	static const uint8_t REX_IMUL_RM[] = { 0x4c, 0x0f, 0xaf };
	static const uint8_t REX_MUL_R[] = { 0x49, 0xf7 };
	static const uint8_t REX_MUL_M[] = { 0x48, 0xf7 };
	static const uint8_t REX_81[] = { 0x49, 0x81 };
	static const uint8_t AND_EAX_I = 0x25;
	static const uint8_t MOV_EAX_I = 0xb8;
	static const uint8_t MOV_RAX_I[] = { 0x48, 0xb8 };
	static const uint8_t MOV_RCX_I[] = { 0x48, 0xb9 };
	static const uint8_t REX_LEA[] = { 0x4f, 0x8d };
	static const uint8_t REX_MUL_MEM[] = { 0x48, 0xf7, 0x24, 0x0e };
	static const uint8_t REX_IMUL_MEM[] = { 0x48, 0xf7, 0x2c, 0x0e };
	static const uint8_t REX_SHR_RAX[] = { 0x48, 0xc1, 0xe8 };
	static const uint8_t MUL_RCX[] = { 0x48, 0xf7, 0xe1 };
	static const uint8_t REX_SHR_RDX[] = { 0x48, 0xc1, 0xea };
	static const uint8_t REX_SH[] = { 0x49, 0xc1 };
	static const uint8_t AND_ECX_I[] = { 0x81, 0xe1 };
	static const uint8_t ADD_RAX_RCX[] = { 0x48, 0x01, 0xC8 };
	static const uint8_t SAR_RAX_I8[] = { 0x48, 0xC1, 0xF8 };
	static const uint8_t ADD_R_RAX[] = { 0x4C, 0x03 };
	static const uint8_t XOR_EAX_EAX[] = { 0x33, 0xC0 };
	static const uint8_t ADD_RDX_R[] = { 0x4c, 0x01 };
	static const uint8_t SUB_RDX_R[] = { 0x4c, 0x29 };
	static const uint8_t SAR_RDX_I8[] = { 0x48, 0xC1, 0xFA };
	static const uint8_t REX_NEG[] = { 0x49, 0xF7 };
	static const uint8_t REX_XOR_RR[] = { 0x4D, 0x33 };
	static const uint8_t REX_XOR_RI[] = { 0x49, 0x81 };
	static const uint8_t REX_XOR_RM[] = { 0x4c, 0x33 };
	static const uint8_t REX_ROT_CL[] = { 0x49, 0xd3 };
	static const uint8_t REX_ROT_I8[] = { 0x49, 0xc1 };
	static const uint8_t SHUFPD[] = { 0x66, 0x0f, 0xc6 };
	static const uint8_t REX_ADDPD[] = { 0x66, 0x41, 0x0f, 0x58 };
	static const uint8_t REX_SUBPD[] = { 0x66, 0x41, 0x0f, 0x5c };
	static const uint8_t REX_XORPS[] = { 0x41, 0x0f, 0x57 };
	static const uint8_t REX_MULPD[] = { 0x66, 0x41, 0x0f, 0x59 };
	static const uint8_t REX_MAXPD[] = { 0x66, 0x41, 0x0f, 0x5f };
	static const uint8_t SQRTPD[] = { 0x66, 0x0f, 0x51 };
	static const uint8_t ROL_RAX[] = { 0x48, 0xc1, 0xc0 };
	static const uint8_t XOR_ECX_ECX[] = { 0x33, 0xC9 };
	static const uint8_t REX_CMP_R32I[] = { 0x41, 0x81 };
	static const uint8_t REX_CMP_M32I[] = { 0x81, 0x3c, 0x06 };
	static const uint8_t MOVAPD[] = { 0x66, 0x0f, 0x29 };
	static const uint8_t REX_XOR_EAX[] = { 0x41, 0x33 };
	static const uint8_t SUB_EBX_JNZ[] = { 0x83, 0xEB, 0x01, 0x0f, 0x85 };
	static const uint8_t JMP = 0xe9;
	static const uint8_t REX_XOR_RAX_R64[] = { 0x49, 0x33 };
	static const uint8_t REX_XCHG[] = { 0x4d, 0x87 };
	static const uint8_t REX_PADD[] = { 0x66, 0x44, 0x0f };
	static const uint8_t CALL = 0xe8;
	static const uint8_t REX_ADD_I[] = { 0x49, 0x81 };
	static const uint8_t REX_TEST[] = { 0x49, 0xF7 };
	static const uint8_t JZ[] = { 0x0f, 0x84 };
	static const uint8_t SHORT_JZ = 0x74;
	static const uint8_t RET = 0xc3;
	static const uint8_t LEA_32[] = { 0x41, 0x8d };
	static const uint8_t MOVNTI[] = { 0x4c, 0x0f, 0xc3 };
	static const uint8_t ADD_EBX_I[] = { 0x81, 0xc3 };

	static const uint8_t NOP1[] = { 0x90 };
	static const uint8_t NOP2[] = { 0x66, 0x90 };
	static const uint8_t NOP3[] = { 0x66, 0x66, 0x90 };
	static const uint8_t NOP4[] = { 0x0F, 0x1F, 0x40, 0x00 };
	static const uint8_t NOP5[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
	static const uint8_t NOP6[] = { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 };
	static const uint8_t NOP7[] = { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t NOP8[] = { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const uint8_t NOP9[] = { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };

	static const uint8_t* NOPX[] = { NOP1, NOP2, NOP3, NOP4, NOP5, NOP6, NOP7, NOP8, NOP9 };

	// maps ModMem values to the appropriate scratchpad mask to emit
	static uint32_t ScratchpadMask[] = { ScratchpadL2Mask, ScratchpadL1Mask, ScratchpadL1Mask, ScratchpadL1Mask };

	inline size_t JitCompilerX86::getCodeSize() const {
		return CodeSize;
	}

	JitCompilerX86::JitCompilerX86() : code((uint8_t*)allocMemoryPages(CodeSize)) {
#ifdef ENABLE_EXPERIMENTAL
		experimental = false;
#endif
		memcpy(code, codePrologue, prologueSize);
		memcpy(code + prologueSize, codeLoopLoad, loopLoadSize);
		memcpy(code + epilogueOffset, codeEpilogue, epilogueSize);
	}

	JitCompilerX86::~JitCompilerX86() {
		freePagedMemory(code, CodeSize);
	}

	void JitCompilerX86::enableAll() {
		setPagesRWX(code, CodeSize);
	}

	void JitCompilerX86::enableWriting() {
		setPagesRW(code, CodeSize);
	}

	void JitCompilerX86::enableExecution() {
		setPagesRX(code, CodeSize);
	}

	void JitCompilerX86::generateProgram(const Program& prog, const ProgramConfiguration& pcfg) {
#ifdef ENABLE_EXPERIMENTAL
		instructionsElided = 0;
#endif
		generateProgramPrologue(prog, pcfg);
		memcpy(codePos, codeReadDataset, readDatasetSize);
		codePos += readDatasetSize;
		generateProgramEpilogue(prog, pcfg);
	}

	void JitCompilerX86::generateProgramLight(
		const Program& prog,
		const ProgramConfiguration& pcfg,
		uint32_t datasetOffset) {
		generateProgramPrologue(prog, pcfg);
		emit(codeReadDatasetLightSshInit, readDatasetLightInitSize);
		emit(ADD_EBX_I);
		emit32(datasetOffset / CacheLineSize);
		emitByte(CALL);
		emit32(superScalarHashOffset - ((codePos - code) + 4));
		emit(codeReadDatasetLightSshFin, readDatasetLightFinSize);
		generateProgramEpilogue(prog, pcfg);
	}

	template<size_t N>
	void JitCompilerX86::generateSuperscalarHash(
		SuperscalarProgram(&programs)[N],
		const std::vector<uint64_t> &reciprocalCache) {
		memcpy(code + superScalarHashOffset, codeShhInit, codeSshInitSize);
		codePos = code + superScalarHashOffset + codeSshInitSize;
		for (int j = 0; j < N; ++j) {
			SuperscalarProgram& prog = programs[j];
			for (int i = 0; i < prog.getSize(); ++i) {
				generateSuperscalarCode(prog(i), reciprocalCache);
			}
			emit(codeShhLoad, codeSshLoadSize);
			if (j < N - 1) {
				emit(REX_MOV_RR64);
				emitByte(0xd8 + prog.getAddressRegister());
				emit(codeShhPrefetch, codeSshPrefetchSize);
#ifdef RANDOMX_ALIGN
				int align = (codePos % 16);
				while (align != 0) {
					const int nopSize = 16 - align;
					if (nopSize > 9) nopSize = 9;
					emit(NOPX[nopSize - 1], nopSize);
					align = (codePos % 16);
				}
#endif
			}
		}
		emitByte(RET);
	}

	template
	void JitCompilerX86::generateSuperscalarHash(
			SuperscalarProgram(&programs)[RANDOMX_CACHE_ACCESSES],
			const std::vector<uint64_t> &reciprocalCache);

	void JitCompilerX86::generateDatasetInitCode() {
		memcpy(code, codeDatasetInit, datasetInitSize);
	}

	void JitCompilerX86::generateProgramPrologue(const Program& prog, const ProgramConfiguration& pcfg) {
		std::fill(registerModifiedAt, registerModifiedAt + RegistersCount, -1);
		lastBranchAt = -1;
#ifdef ENABLE_EXPERIMENTAL
		prevRoundModeAt = -1;
		prevFloatOpAt = -1;
#endif
		// initialize Group E register masks in xmm_constants with quadwords 14 & 15
		memcpy(code + xmmConstantsOffset + 16, &pcfg.eMask, sizeof(pcfg.eMask));

		codePos = code + prologueSize + loopLoadSize;

		for (int i = 0; i < prog.getSize(); ++i) {
			generateCode(prog(i), i);
		}
		emit(REX_MOV_RR);
		emitByte(0xc0 + pcfg.readReg2);
		emit(REX_XOR_EAX);
		emitByte(0xc0 + pcfg.readReg3);
	}

	void JitCompilerX86::generateProgramEpilogue(const Program& prog, const ProgramConfiguration& pcfg) {
		// XOR of registers readReg0 and readReg1 (step 1 of sec. 4.6.2)
		emit(REX_MOV_RR64);
		emitByte(0xc0 + pcfg.readReg0);
		emit(REX_XOR_RAX_R64);
		emitByte(0xc0 + pcfg.readReg1);
		memcpy(codePos, codeLoopStore, loopStoreSize);
		codePos += loopStoreSize;
		emit(SUB_EBX_JNZ);
		emit32(prologueSize - (codePos - code) - 4);
		emitByte(JMP);
		emit32(epilogueOffset - (codePos - code) - 4);
	}

	void JitCompilerX86::generateSuperscalarCode(const Instruction& instr, const std::vector<uint64_t> &reciprocalCache) {
		switch ((SuperscalarInstructionType)instr.opcode)
		{
		case randomx::SuperscalarInstructionType::ISUB_R:
			emit(REX_SUB_RR);
			emitByte(0xc0 + 8 * instr.dst + instr.src);
			break;
		case randomx::SuperscalarInstructionType::IXOR_R:
			emit(REX_XOR_RR);
			emitByte(0xc0 + 8 * instr.dst + instr.src);
			break;
		case randomx::SuperscalarInstructionType::IADD_RS:
			emit(REX_LEA);
			emitByte(0x04 + 8 * instr.dst);
			genSIB(instr.getModShift(), instr.src, instr.dst);
			break;
		case randomx::SuperscalarInstructionType::IMUL_R:
			emit(REX_IMUL_RR);
			emitByte(0xc0 + 8 * instr.dst + instr.src);
			break;
		case randomx::SuperscalarInstructionType::IROR_C:
			emit(REX_ROT_I8);
			emitByte(0xc8 + instr.dst);
			emitByte(instr.getImm32() & 63);
			break;
		case randomx::SuperscalarInstructionType::IADD_C7:
			emit(REX_81);
			emitByte(0xc0 + instr.dst);
			emit32(instr.getImm32());
			break;
		case randomx::SuperscalarInstructionType::IXOR_C7:
			emit(REX_XOR_RI);
			emitByte(0xf0 + instr.dst);
			emit32(instr.getImm32());
			break;
		case randomx::SuperscalarInstructionType::IADD_C8:
			emit(REX_81);
			emitByte(0xc0 + instr.dst);
			emit32(instr.getImm32());
#ifdef RANDOMX_ALIGN
			emit(NOP1);
#endif
			break;
		case randomx::SuperscalarInstructionType::IXOR_C8:
			emit(REX_XOR_RI);
			emitByte(0xf0 + instr.dst);
			emit32(instr.getImm32());
#ifdef RANDOMX_ALIGN
			emit(NOP1);
#endif
			break;
		case randomx::SuperscalarInstructionType::IADD_C9:
			emit(REX_81);
			emitByte(0xc0 + instr.dst);
			emit32(instr.getImm32());
#ifdef RANDOMX_ALIGN
			emit(NOP2);
#endif
			break;
		case randomx::SuperscalarInstructionType::IXOR_C9:
			emit(REX_XOR_RI);
			emitByte(0xf0 + instr.dst);
			emit32(instr.getImm32());
#ifdef RANDOMX_ALIGN
			emit(NOP2);
#endif
			break;
		case randomx::SuperscalarInstructionType::IMULH_R:
			emit(REX_MOV_RR64);
			emitByte(0xc0 + instr.dst);
			emit(REX_MUL_R);
			emitByte(0xe0 + instr.src);
			emit(REX_MOV_R64R);
			emitByte(0xc2 + 8 * instr.dst);
			break;
		case randomx::SuperscalarInstructionType::ISMULH_R:
			emit(REX_MOV_RR64);
			emitByte(0xc0 + instr.dst);
			emit(REX_MUL_R);
			emitByte(0xe8 + instr.src);
			emit(REX_MOV_R64R);
			emitByte(0xc2 + 8 * instr.dst);
			break;
		case randomx::SuperscalarInstructionType::IMUL_RCP:
			emit(MOV_RAX_I);
			emit64(reciprocalCache[instr.getImm32()]);
			emit(REX_IMUL_RM);
			emitByte(0xc0 + 8 * instr.dst);
			break;
		default:
			UNREACHABLE;
		}
	}

	inline void JitCompilerX86::genAddressRegRax(const Instruction& instr, uint8_t src) {
		emit(LEA_32);
		emitByte(0x80 + src);
		if (src == RegisterNeedsSib) {
			emitByte(0x24);
		}
		emit32(instr.getImm32());
		emitByte(AND_EAX_I);
		emit32(ScratchpadMask[instr.getModMem()]);
	}

	void JitCompilerX86::h_IADD_RS(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		emit(REX_LEA);
		if (dst == RegisterNeedsDisplacement) {
			emitByte(0xac);
			genSIB(instr.getModShift(), instr.src % RegistersCount, dst);
			emit32(instr.getImm32());
		}
		else {
			emitByte(0x04 + 8 * dst);
			genSIB(instr.getModShift(), instr.src % RegistersCount, dst);
		}
	}

	void JitCompilerX86::h_IADD_M(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			genAddressRegRax(instr, src);
			emit(REX_ADD_RM);
			emitByte(0x04 + 8 * dst);
			emitByte(0x06);
		}
		else {
			emit(REX_ADD_RM);
			emitByte(0x86 + 8 * dst);
			genAddressImm(instr);
		}
	}

	void JitCompilerX86::h_ISUB_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(REX_SUB_RR);
			emitByte(0xc0 + 8 * dst + src);
		}
		else {
			emit(REX_81);
			emitByte(0xe8 + dst);
			emit32(instr.getImm32());
		}
	}

	void JitCompilerX86::h_ISUB_M(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			genAddressRegRax(instr, src);
			emit(REX_SUB_RM);
			emitByte(0x04 + 8 * dst);
			emitByte(0x06);
		}
		else {
			emit(REX_SUB_RM);
			emitByte(0x86 + 8 * dst);
			genAddressImm(instr);
		}
	}

	void JitCompilerX86::h_IMUL_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(REX_IMUL_RR);
			emitByte(0xc0 + 8 * dst + src);
		}
		else {
			emit(REX_IMUL_RRI);
			emitByte(0xc0 + 9 * dst);
			emit32(instr.getImm32());
		}
	}

	void JitCompilerX86::h_IMUL_M(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			genAddressRegRax(instr, src);
			emit(REX_IMUL_RM);
			emitByte(0x04 + 8 * dst);
			emitByte(0x06);
		}
		else {
			emit(REX_IMUL_RM);
			emitByte(0x86 + 8 * dst);
			genAddressImm(instr);
		}
	}

	void JitCompilerX86::h_IMULH_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		emit(REX_MOV_RR64);
		emitByte(0xc0 + dst);
		emit(REX_MUL_R);
		emitByte(0xe0 + (instr.src % RegistersCount));
		emit(REX_MOV_R64R);
		emitByte(0xc2 + 8 * dst);
	}

	void JitCompilerX86::h_IMULH_M(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(LEA_32);
			emitByte(0x80 + src + 8);
			if (src == RegisterNeedsSib) {
				emitByte(0x24);
			}
			emit32(instr.getImm32());
			emit(AND_ECX_I);
			emit32(ScratchpadMask[instr.getModMem()]);
			emit(REX_MOV_RR64);
			emitByte(0xc0 + dst);
			emit(REX_IMUL_MEM);
			emit(REX_MOV_RR64);
			emitByte(0xc0 + dst);
			emit(REX_MUL_MEM);
		}
		else {
			emit(REX_MOV_RR64);
			emitByte(0xc0 + dst);
			emit(REX_MUL_M);
			emitByte(0xa6);
			genAddressImm(instr);
		}
		emit(REX_MOV_R64R);
		emitByte(0xc2 + 8 * dst);
	}

	void JitCompilerX86::h_ISMULH_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		emit(REX_MOV_RR64);
		emitByte(0xc0 + dst);
		emit(REX_MUL_R);
		emitByte(0xe8 + (instr.src % RegistersCount));
		emit(REX_MOV_R64R);
		emitByte(0xc2 + 8 * dst);
	}

	void JitCompilerX86::h_ISMULH_M(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(LEA_32);
			emitByte(0x80 + src + 8);
			if (src == RegisterNeedsSib) {
				emitByte(0x24);
			}
			emit32(instr.getImm32());
			emit(AND_ECX_I);
			emit32(ScratchpadMask[instr.getModMem()]);
			emit(REX_MOV_RR64);
			emitByte(0xc0 + dst);
			emit(REX_IMUL_MEM);
		}
		else {
			emit(REX_MOV_RR64);
			emitByte(0xc0 + dst);
			emit(REX_MUL_M);
			emitByte(0xae);
			genAddressImm(instr);
		}
		emit(REX_MOV_R64R);
		emitByte(0xc2 + 8 * dst);
	}

	void JitCompilerX86::h_IMUL_RCP(const Instruction& instr, int i) {
		uint64_t divisor = instr.getImm32();
		if (isZeroOrPowerOf2(divisor)) return;
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		emit(MOV_RAX_I);
		emit64(randomx_reciprocal_fast(divisor));
		emit(REX_IMUL_RM);
		emitByte(0xc0 + 8 * dst);
	}

	void JitCompilerX86::h_INEG_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		emit(REX_NEG);
		emitByte(0xd8 + dst);
	}

	void JitCompilerX86::h_IXOR_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(REX_XOR_RR);
			emitByte(0xc0 + 8 * dst + src);
		}
		else {
			emit(REX_XOR_RI);
			emitByte(0xf0 + dst);
			emit32(instr.getImm32());
		}
	}

	void JitCompilerX86::h_IXOR_M(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			genAddressRegRax(instr, src);
			emit(REX_XOR_RM);
			emitByte(0x04 + 8 * dst);
			emitByte(0x06);
		}
		else {
			emit(REX_XOR_RM);
			emitByte(0x86 + 8 * dst);
			genAddressImm(instr);
		}
	}

	void JitCompilerX86::h_IROR_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		// we must mark the registerModified bit even if we elide, or branching offsets will be
		// computed incorrectly.
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(REX_MOV_RR);
			emitByte(0xc8 + src);
			emit(REX_ROT_CL);
			emitByte(0xc8 + dst);
			return;
		}
		const int amt = instr.getImm32() & 63;
		if (amt == 0) {
#ifdef ENABLE_EXPERIMENTAL
			instructionsElided++;
#endif
			return;
		}
		emit(REX_ROT_I8);
		emitByte(0xc8 + dst);
		emitByte(amt);
	}

	void JitCompilerX86::h_IROL_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		// we must mark the registerModified bit even if we elide, or branching offsets will be
		// computed incorrectly.
		registerModifiedAt[dst] = i;
		const auto src = instr.src % RegistersCount;
		if (src != dst) {
			emit(REX_MOV_RR);
			emitByte(0xc8 + src);
			emit(REX_ROT_CL);
			emitByte(0xc0 + dst);
			return;
		}
		const int amt = instr.getImm32() & 63;
		if (amt == 0) {
#ifdef ENABLE_EXPERIMENTAL
			instructionsElided++;
#endif
			return;
		}
		emit(REX_ROT_I8);
		emitByte(0xc0 + dst);
		emitByte(amt);
	}

	void JitCompilerX86::h_ISWAP_R(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		const auto src = instr.src % RegistersCount;
		if (src == dst) return;
		registerModifiedAt[dst] = i;
		registerModifiedAt[src] = i;
		emit(REX_XCHG);
		emitByte(0xc0 + src + 8 * dst);

	}

	void JitCompilerX86::h_FSWAP_R(const Instruction& instr, int i) {
		emit(SHUFPD);
		emitByte(0xc0 + 9 * (instr.dst % RegistersCount));
		emitByte(1);
	}

	void JitCompilerX86::h_FADD_R(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		emit(REX_ADDPD);
		emitByte(0xc0 + (instr.src % RegisterCountFlt) + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FADD_M(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		genAddressRegRax(instr, instr.src % RegistersCount);
		static const uint8_t REX_CVTDQ2PD_XMM12_ADDPD[] = {
			0xf3, 0x44, 0x0f, 0xe6, 0x24, 0x06,
			0x66, 0x41, 0x0f, 0x58
		};
		emit(REX_CVTDQ2PD_XMM12_ADDPD);
		emitByte(0xc4 + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FSUB_R(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		emit(REX_SUBPD);
		emitByte(0xc0 + (instr.src % RegisterCountFlt) + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FSUB_M(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		genAddressRegRax(instr, instr.src % RegistersCount);
		static const uint8_t REX_CVTDQ2PD_XMM12_SUBPD[] = {
			0xf3, 0x44, 0x0f, 0xe6, 0x24, 0x06,
			0x66, 0x41, 0x0f, 0x5c
		};
		emit(REX_CVTDQ2PD_XMM12_SUBPD);
		emitByte(0xc4 + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FSCAL_R(const Instruction& instr, int i) {
		emit(REX_XORPS);
		emitByte(0xc7 + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FMUL_R(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		emit(REX_MULPD);
		emitByte(0xe0 + (instr.src % RegisterCountFlt) + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FDIV_M(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		genAddressRegRax(instr, instr.src % RegistersCount);
		static const uint8_t REX_CVTDQ2PD_XMM12_ANDPS_XMM12_DIVPD[] = {
			0xf3, 0x44, 0x0f, 0xe6, 0x24, 0x06,
			0x45, 0x0F, 0x54, 0xE5, 0x45, 0x0F, 0x56, 0xE6,
			0x66, 0x41, 0x0f, 0x5e
		};
		emit(REX_CVTDQ2PD_XMM12_ANDPS_XMM12_DIVPD);
		emitByte(0xe4 + 8 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_FSQRT_R(const Instruction& instr, int i) {
#ifdef ENABLE_EXPERIMENTAL
		prevFloatOpAt = i;
#endif
		emit(SQRTPD);
		emitByte(0xe4 + 9 * (instr.dst % RegisterCountFlt));
	}

	void JitCompilerX86::h_CFROUND(const Instruction& instr, int i) {
		auto src = instr.src % RegistersCount;
#ifdef ENABLE_EXPERIMENTAL
		if (prevRoundModeAt > prevFloatOpAt) {
			// The previous rounding mode change will have no effect because we are just changing it
			// again before it was used, so we can turn it into a no-op.
			uint8_t* code_loc = instructionOffsets[prevRoundModeAt];
			memcpy(code_loc, NOP8, 8);
			memcpy(code_loc + 8, NOP8, 8);
			memcpy(code_loc + 16, NOP7, 7);
			instructionsElided++;
		}
		prevRoundModeAt = i;
		prevRoundReg = src;
#endif

		emit(REX_MOV_RR64);
		emitByte(0xc0 + src);
		const int rotate = (static_cast<int>(instr.getImm32() & 63) - 2) & 63;
		if (rotate != 0) {
			static const uint8_t ROR_RAX[] = { 0x48, 0xc1, 0xc8 };
			emit(ROR_RAX);
			emitByte(rotate);
		}
		static const uint8_t AND_LDMXCSR[] = {
			0x83, 0xe0, 0x0c, 0x0f, 0xae, 0x14, 0x04
		};
		emit(AND_LDMXCSR);
	}

	void JitCompilerX86::h_CBRANCH(const Instruction& instr, int i) {
		const auto dst = instr.dst % RegistersCount;
		int branchDestinationAt = registerModifiedAt[dst];
		if (branchDestinationAt < lastBranchAt) {
			branchDestinationAt = lastBranchAt + 1;
		} else {
			branchDestinationAt++;
		}
		lastBranchAt = i;
#ifdef ENABLE_EXPERIMENTAL
		// If the branch destination is the last rounding operation, and the rounding source
		// register hasn't been modified, then we can bump up the branch point because the
		// rounding operation will be a no-op.
		if (branchDestinationAt == prevRoundModeAt &&
			prevRoundReg != dst &&
			registerModifiedAt[prevRoundReg] < prevRoundModeAt) {
			branchDestinationAt++;
			// more like "possibly elided" since it's elided only if branch happens?
			instructionsElided++;
		}
		if (branchDestinationAt <= prevFloatOpAt) {
			prevRoundModeAt = -1;
		}
#endif
		emit(REX_ADD_I);
		emitByte(0xc0 + dst);
		const int shift = instr.getModCond() + ConditionOffset;
		uint32_t imm = instr.getImm32() | (1UL << shift);
		if (ConditionOffset > 0 || shift > 0)
			imm &= ~(1UL << (shift - 1));
		emit32(imm);
		emit(REX_TEST);
		emitByte(0xc0 + dst);
		emit32(ConditionMask << shift);
		const auto offset = instructionOffsets[branchDestinationAt] - codePos - 2;
		if (offset >= -128) {
			emitByte(SHORT_JZ);
			emitByte(offset);
		} else {
			emit(JZ);
			emit32(offset - 4);
		}
	}

	void JitCompilerX86::h_ISTORE(const Instruction& instr, int i) {
		emit(LEA_32);
		const auto dst = instr.dst % RegistersCount;
		emitByte(0x80 + dst);
		if (dst == RegisterNeedsSib) {
			emitByte(0x24);
		}
		emit32(instr.getImm32());
		emitByte(AND_EAX_I);
		if (instr.getModCond() < StoreL3Condition) {
			emit32(ScratchpadMask[instr.getModMem()]);
		}
		else {
			emit32(ScratchpadL3Mask);
		}
		static const uint8_t REX_MOV_MR[] = { 0x4c, 0x89 };
		emit(REX_MOV_MR);
		emitByte(0x04 + 8 * (instr.src % RegistersCount));
		emitByte(0x06);
	}

	void JitCompilerX86::h_NOP(const Instruction& instr, int i) {
		emit(NOP1);
	}

#include "instruction_weights.hpp"
#define INST_HANDLE(x) REPN(&JitCompilerX86::h_##x, WT(x))

	const InstructionGeneratorX86 JitCompilerX86::engine[256] = {
		INST_HANDLE(IADD_RS)
		INST_HANDLE(IADD_M)
		INST_HANDLE(ISUB_R)
		INST_HANDLE(ISUB_M)
		INST_HANDLE(IMUL_R)
		INST_HANDLE(IMUL_M)
		INST_HANDLE(IMULH_R)
		INST_HANDLE(IMULH_M)
		INST_HANDLE(ISMULH_R)
		INST_HANDLE(ISMULH_M)
		INST_HANDLE(IMUL_RCP)
		INST_HANDLE(INEG_R)
		INST_HANDLE(IXOR_R)
		INST_HANDLE(IXOR_M)
		INST_HANDLE(IROR_R)
		INST_HANDLE(IROL_R)
		INST_HANDLE(ISWAP_R)
		INST_HANDLE(FSWAP_R)
		INST_HANDLE(FADD_R)
		INST_HANDLE(FADD_M)
		INST_HANDLE(FSUB_R)
		INST_HANDLE(FSUB_M)
		INST_HANDLE(FSCAL_R)
		INST_HANDLE(FMUL_R)
		INST_HANDLE(FDIV_M)
		INST_HANDLE(FSQRT_R)
		INST_HANDLE(CBRANCH)
		INST_HANDLE(CFROUND)
		INST_HANDLE(ISTORE)
		INST_HANDLE(NOP)
	};

}
