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

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "common.hpp"
#include "instruction.hpp"

//#define ENABLE_EXPERIMENTAL 1

namespace randomx {

	class Program;
	struct ProgramConfiguration;
	class SuperscalarProgram;
	class JitCompilerX86;
	class Instruction;

	typedef void(JitCompilerX86::*InstructionGeneratorX86)(const Instruction&, int);

	class JitCompilerX86 {
	public:
		JitCompilerX86();
		~JitCompilerX86();

		void generateProgram(const Program&, const ProgramConfiguration&);
		void generateProgramLight(const Program&, const ProgramConfiguration&, uint32_t);
		template<size_t N>
		void generateSuperscalarHash(SuperscalarProgram (&programs)[N], const std::vector<uint64_t> &);
		void generateDatasetInitCode();
		ProgramFunc* getProgramFunc() const {
			return (ProgramFunc*)code;
		}
		DatasetInitFunc* getDatasetInitFunc() const {
			return (DatasetInitFunc*)code;
		}
		const uint8_t* getCode() const {
			return code;
		}
		size_t getCodeSize() const;
		void enableWriting();
		void enableExecution();
		void enableAll();

#ifdef ENABLE_EXPERIMENTAL
		// Instructions elided due to misc. optimizations.  Elided either means either avoided
		// completely or later converted to no-ops.
		int instructionsElided;

		// set to true when testing/benchmarking experimental optimizations & features
		bool experimental;
#endif

	private:
		static const InstructionGeneratorX86 engine[256];
		uint8_t* instructionOffsets[RANDOMX_PROGRAM_SIZE];
		int registerModifiedAt[RegistersCount];

#ifdef ENABLE_EXPERIMENTAL
		int prevRoundModeAt;
		uint8_t prevRoundReg;
		int prevFloatOpAt;
#endif

		uint8_t* const code;
		uint8_t* codePos;

		void generateProgramPrologue(const Program&, const ProgramConfiguration&);
		void generateProgramEpilogue(const Program&, const ProgramConfiguration&);
		void genAddressRegRax(const Instruction&, uint8_t reg);

		inline void genAddressImm(const Instruction& instr) {
		  emit32(instr.getImm32() & ScratchpadL3Mask);
		}

		inline void genSIB(int scale, int index, int base) {
		  emitByte((scale << 6) | (index << 3) | base);
		}

		inline void generateCode(const Instruction& instr, int i) {
			instructionOffsets[i] = codePos;
			auto generator = engine[instr.opcode];
			(this->*generator)(instr, i);
		}

		void generateSuperscalarCode(const Instruction&, const std::vector<uint64_t> &);

		inline void emitByte(uint8_t val) {
			*codePos++ = val;
		}

		inline void emit32(uint32_t val) {
			memcpy(codePos, &val, sizeof val);
			codePos += sizeof val;
		}

		inline void emit64(uint64_t val) {
			memcpy(codePos, &val, sizeof val);
			codePos += sizeof val;
		}

		template<size_t N>
		inline void emit(const uint8_t (&src)[N]) {
			emit(src, N);
		}

		inline void emit(const uint8_t* src, size_t count) {
			memcpy(codePos, src, count);
			codePos += count;
		}

		void h_IADD_RS(const Instruction&, int);
		void h_IADD_M(const Instruction&, int);
		void h_ISUB_R(const Instruction&, int);
		void h_ISUB_M(const Instruction&, int);
		void h_IMUL_R(const Instruction&, int);
		void h_IMUL_M(const Instruction&, int);
		void h_IMULH_R(const Instruction&, int);
		void h_IMULH_M(const Instruction&, int);
		void h_ISMULH_R(const Instruction&, int);
		void h_ISMULH_M(const Instruction&, int);
		void h_IMUL_RCP(const Instruction&, int);
		void h_INEG_R(const Instruction&, int);
		void h_IXOR_R(const Instruction&, int);
		void h_IXOR_M(const Instruction&, int);
		void h_IROR_R(const Instruction&, int);
		void h_IROL_R(const Instruction&, int);
		void h_ISWAP_R(const Instruction&, int);
		void h_FSWAP_R(const Instruction&, int);
		void h_FADD_R(const Instruction&, int);
		void h_FADD_M(const Instruction&, int);
		void h_FSUB_R(const Instruction&, int);
		void h_FSUB_M(const Instruction&, int);
		void h_FSCAL_R(const Instruction&, int);
		void h_FMUL_R(const Instruction&, int);
		void h_FDIV_M(const Instruction&, int);
		void h_FSQRT_R(const Instruction&, int);
		void h_CBRANCH(const Instruction&, int);
		void h_CFROUND(const Instruction&, int);
		void h_ISTORE(const Instruction&, int);
		void h_NOP(const Instruction&, int);
	};

}
