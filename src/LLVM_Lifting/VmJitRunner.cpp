#include "VmJitRunner.hpp"
#include "VmEmulator.hpp"

extern "C" {
	uint64_t LLVM_JIT_ReadMem(uint64_t address, uint32_t size) {	
        uint64_t value = GetVirtualMemoryValue(address, size);
        //printf("[READ]  Addr: 0x%llx, Val: 0x%llx, Size: %d bytes\n", address, value, size);
        return value;
	}
	void LLVM_JIT_WriteMem(uint64_t address, uint64_t value, uint32_t size) {
		//printf("[WRITE] Addr: 0x%llx, Val: 0x%llx, Size: %d bytes\n", address, value, size);
        SetVirtualMemoryValue(address, value, size);
	}

    void LLVM_JIT_LogPop(uint32_t bitDepth, uint64_t offset, uint64_t value) {
        std::cout << "JIT_POP" << bitDepth << "\tR" << offset / (bitDepth / 8) << '\t'
            << std::hex << "Offset: 0x" << offset
            << " Value: 0x" << value << std::dec << '\n';
    }
    void LLVM_JIT_LogPushReg(uint32_t bitDepth, uint64_t offset, uint64_t value) {
        std::cout << "JIT_PUSH_REG" << bitDepth << "\tR" << offset / (bitDepth / 8) << '\t'
            << std::hex << "Offset: 0x" << offset
            << " Value: 0x" << value << std::dec << '\n';
    }
    void LLVM_JIT_LogPushConst(uint32_t bitDepth, uint64_t value) {
        std::cout << "JIT_PUSH_CONST" << bitDepth << std::hex 
            << " 0x" << value << std::dec << '\n';
    }

    void LLVM_JIT_LogPushVsp(uint32_t bitDepth, uint64_t value) {
        std::cout << "JIT_PUSH_VSP" << bitDepth << std::hex
            << " 0x" << value << std::dec << '\n';
    }
    void LLVM_JIT_LogPopVsp(uint32_t bitDepth, uint64_t value) {
        std::cout << "JIT_POP_VSP" << bitDepth << std::hex
            << "\t\tValue: 0x" << value << std::dec << '\n';
    }

    void LLVM_JIT_LogReadMem(uint32_t bitDepth, uint64_t addr, uint64_t value) {
        std::cout << "JIT_READ_MEM" << bitDepth << std::hex 
            << "\t\t[0x" << addr << "]->0x" 
            << value << std::dec << '\n';
    }
    void LLVM_JIT_LogWriteMem(uint32_t bitDepth, uint64_t addr, uint64_t value) {
        std::cout << "JIT_WRITE_MEM" << bitDepth << std::hex
            << "\t\t[0x" << addr << "]=0x"
            << value << std::dec << '\n';
    }

    void LLVM_JIT_LogAdd(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags) {
        std::cout << "JIT_ADD" << bitDepth << std::hex
            << "\t\tArg1=0x" << arg1
            << " Arg2=0x" << arg2
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }
    void LLVM_JIT_LogNor(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags) {
        std::cout << "JIT_NOR" << bitDepth << std::hex
            << "\t\tArg1=0x" << arg1
            << " Arg2=0x" << arg2
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }
    void LLVM_JIT_LogNand(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags) {
        std::cout << "JIT_NAND" << bitDepth << std::hex
            << "\t\tArg1=0x" << arg1
            << " Arg2=0x" << arg2
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }
    void LLVM_JIT_LogShl(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags) {
        std::cout << "JIT_SHL" << bitDepth << std::hex
            << "\t\tArg1=0x" << arg1
            << " Arg2=0x" << arg2
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }
    void LLVM_JIT_LogShr(uint32_t bitDepth, uint64_t arg1, uint64_t arg2, uint64_t result, uint64_t flags) {
        std::cout << "JIT_SHR" << bitDepth << std::hex
            << "\t\tArg1=0x" << arg1
            << " Arg2=0x" << arg2
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }

    void LLVM_JIT_LogShld(uint32_t bitDepth, uint64_t dst, uint64_t src, uint8_t shift, uint64_t result, uint64_t flags) {
        std::cout << "JIT_SHLD" << bitDepth << std::hex
            << "\t\tDst=0x" << dst
            << " Src=0x" << src
            << " Shift=0x" << (uint64_t)shift
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }
    void LLVM_JIT_LogShrd(uint32_t bitDepth, uint64_t dst, uint64_t src, uint8_t shift, uint64_t result, uint64_t flags) {
        std::cout << "JIT_SHRD" << bitDepth << std::hex
            << "\t\tDst=0x" << dst
            << " Src=0x" << src
            << " Shift=0x" << (uint64_t)shift
            << " Result=0x" << result
            << " Flags=0x" << flags
            << std::dec << '\n';
    }

    void LLVM_JIT_LogJmpIndirect(uint64_t dst) {
        std::cout << "JIT_JMP 0x" << std::hex << dst << std::dec << "\n\n";
    }
}

void VmJitRunner::RegisterHooks() {
    using namespace llvm;
    using namespace llvm::orc;
    // Создаем маппинг Имя -> Адрес
    SymbolMap symbols;

    // Mangle нужен, чтобы JIT правильно понял имена (например, добавил _ для Windows)
    MangleAndInterner Mangle(jit->getExecutionSession(), jit->getDataLayout());

    // JIT_ReadMem
    symbols[Mangle("LLVM_JIT_ReadMem")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_ReadMem),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );

    // JIT_WriteMem
    symbols[Mangle("LLVM_JIT_WriteMem")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_WriteMem),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );


    symbols[Mangle("LLVM_JIT_LogPop")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogPop),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogPushReg")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogPushReg),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogPushConst")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogPushConst),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );


    symbols[Mangle("LLVM_JIT_LogPushVsp")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogPushVsp),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogPopVsp")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogPopVsp),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );


    symbols[Mangle("LLVM_JIT_LogReadMem")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogReadMem),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogWriteMem")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogWriteMem),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );


    symbols[Mangle("LLVM_JIT_LogAdd")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogAdd),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogNor")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogNor),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogNand")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogNand),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogShl")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogShl),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogShr")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogShr),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );


    symbols[Mangle("LLVM_JIT_LogShld")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogShld),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );
    symbols[Mangle("LLVM_JIT_LogShrd")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogShrd),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );


    symbols[Mangle("LLVM_JIT_LogJmpIndirect")] = JITEvaluatedSymbol(
        pointerToJITTargetAddress(&LLVM_JIT_LogJmpIndirect),
        JITSymbolFlags::Exported | JITSymbolFlags::Callable
    );

    // Внедряем в процесс JIT
    cantFail(jit->getMainJITDylib().define(absoluteSymbols(symbols)));
}