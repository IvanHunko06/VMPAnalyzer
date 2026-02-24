#include "triton/context.hpp"
#include <fstream>
#include "VmBasicBlock.hpp"
#include "VmHandlerEmulator.hpp"
#include "VmHandlerMatcher.hpp"
//#include "LLVM_Lifting/VmEmulator.hpp"
#include "LLVM_Lifting/LLVMTraceLifter.hpp"
#include "LLVM_Lifting/CFGRepatcher.hpp"
#include <set>

int main() {

 	std::fstream file("E:/Programs/pin-external-3.31-98869-gfa6f126a8-msvc-windows/trace6.log", std::ios::in);
	
	auto vmBasicBlocks = ParseVmBasicBlocks(file);

	//triton::arch::register_e vipReg = triton::arch::ID_REG_X86_RBX;
	//triton::arch::register_e vspReg = triton::arch::ID_REG_X86_R9;
	triton::arch::register_e vipReg = triton::arch::ID_REG_INVALID;
	triton::arch::register_e vspReg = triton::arch::ID_REG_INVALID;

	std::set<uint64_t> breakpoints{ };

	std::vector<HandlerMatch> vmHandlers;
	int64_t vspAccumulatedChange = 0;
	for (auto& block : vmBasicBlocks) {
		auto data = EmulateVmHandler(block, vipReg, vspReg);
		if (breakpoints.contains(data.baseAddress)) {
			PrintBasicBlock(block);
			std::cout << '\n';
			PrintEmulationData(data);
			__debugbreak();
		}
		auto match = MatchVmHandler(data);
		std::cout << std::hex << "0x" << block.instructions[0].instruction->getAddress() << std::dec << ": ";
		bool formatAdditionalCarry = false;
		if (match.type == VmHandlerType::Handler_Unknown) {
			PrintBasicBlock(block);
			std::cout << '\n';
			PrintEmulationData(data);
			continue;
		}
		else if (match.type == VmHandlerType::Handler_VmEntry) {
			auto matchData = std::get<VmEntryHandlerData>(match.matchData);
			std::cout << "\n#Registers are written in the order in which they are placed on the native stack.\n"
				<< "#At the top of the virtual stack is the image base dif, the written registers in right-to-left order, and 2 return addresses.\n";
			std::cout << "VM_ENTRY { ";
			for (auto& reg : matchData.popedRegsOrder) {
				std::cout << reg.name << ' ';
			}
			std::cout << "} Image base dif: " << matchData.imageBaseDifference;
			vipReg = matchData.vipReg;
			vspReg = matchData.vspReg;
		}
		else if (match.type == VmHandlerType::Handler_VmPop) {
			auto matchData = std::get<VmPopRegData>(match.matchData);
			std::cout
				<< "VM_POP" << match.bitDepth << "\tR" << matchData.regIndex
				<< "\t\t Offset: 0x" << std::hex << matchData.offset <<
				" Value: 0x" << matchData.value << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmPushConst) {
			auto matchData = std::get<VmPushConstData>(match.matchData);
			std::cout
				<< "VM_PUSH_CONST" << match.bitDepth << std::hex << " 0x" << matchData.value << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmPushReg) {
			auto matchData = std::get<VmPushRegData>(match.matchData);
			std::cout
				<< "VM_PUSH_REG" << match.bitDepth << "   R" << matchData.regIndex
				<< "\t\t RegOffset: 0x" << std::hex << matchData.regOffset <<
				" Value: 0x" << matchData.value << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmPushVsp) {
			auto matchData = std::get<VmPushVspData>(match.matchData);
			std::cout
				<< "VM_PUSH_VSP" << match.bitDepth << std::hex << "\t\t\t Pushed VSP: 0x" << matchData.value << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmPopVsp) {
			std::cout << "VM_POP_VSP";
		}
		else if (match.type == VmHandlerType::Handler_VmReadMem) {
			auto matchData = std::get<VmMemAccessData>(match.matchData);
			std::cout
				<< "VM_READ_MEM" << match.bitDepth << std::hex << "\t\t\t [0x" << matchData.address << "]->0x" << matchData.value << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmWriteMem) {
			auto matchData = std::get<VmMemAccessData>(match.matchData);
			std::cout
				<< "VM_WRITE_MEM" << match.bitDepth << std::hex << "\t\t\t [0x" << matchData.address << "]=0x" << matchData.value << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmAdd) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_ADD" << match.bitDepth << std::hex 
				<< "\t\t\t Arg1: 0x" << matchData.arg1 
				<< " Arg2: 0x" << matchData.arg2 
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmNand) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_NAND" << match.bitDepth << std::hex
				<< "\t\t\t Arg1: 0x" << matchData.arg1
				<< " Arg2: 0x" << matchData.arg2
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmNor) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_NOR" << match.bitDepth << std::hex
					<< "\t\t\t Arg1: 0x" << matchData.arg1
					<< " Arg2: 0x" << matchData.arg2
					<< " Result: 0x" << matchData.result
					<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmRol) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_ROL" << match.bitDepth << std::hex
				<< "\t\t\t Arg1: 0x" << matchData.arg1
				<< " Arg2: 0x" << matchData.arg2
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmRor) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_ROR" << match.bitDepth << std::hex
				<< "\t\t\t Arg1: 0x" << matchData.arg1
				<< " Arg2: 0x" << matchData.arg2
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmShl) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_SHL" << match.bitDepth << std::hex
				<< "\t\t\t Arg1: 0x" << matchData.arg1
				<< " Arg2: 0x" << matchData.arg2
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmShr) {
			auto matchData = std::get<VmAluData>(match.matchData);
			std::cout
				<< "VM_SHR" << match.bitDepth << std::hex
				<< "\t\t\t Arg1: 0x" << matchData.arg1
				<< " Arg2: 0x" << matchData.arg2
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmShld) {
			auto matchData = std::get<VmShldData>(match.matchData);
			std::cout
				<< "VM_SHLD" << match.bitDepth << std::hex
				<< "\t\t\t Dst: 0x" << matchData.dst
				<< " Src: 0x" << matchData.src
				<< " Shift: 0x" << matchData.shift
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmShrd) {
			auto matchData = std::get<VmShldData>(match.matchData);
			std::cout
				<< "VM_SHRD" << match.bitDepth << std::hex
				<< "\t\t\t Dst: 0x" << matchData.dst
				<< " Src: 0x" << matchData.src
				<< " Shift: 0x" << matchData.shift
				<< " Result: 0x" << matchData.result
				<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
		}
		else if (match.type == VmHandlerType::Handler_VmJmpIndirect) {
			auto matchData = std::get<VmJmpData>(match.matchData);
			std::cout
				<< "VM_JMP_INDIRECT" << std::hex << "\t\t\t Dst VIP: 0x" << matchData.newVip << std::dec;
			formatAdditionalCarry = true;
		}
		else if (match.type == VmHandlerType::Handler_VmJmpIndirectRemap) {
			auto matchData = std::get<VmJmpData>(match.matchData);
			std::cout
				<< "VM_JMP_INDIRECT" << std::hex << "\t\t Dst VIP: 0x" << matchData.newVip << std::dec;
			vipReg = matchData.newVipReg;
			vspReg = matchData.newVspReg;
			formatAdditionalCarry = true;
		}
		else if (match.type == VmHandlerType::Handler_VmDispatch) {
			std::cout << "VM_DISPATCH";
		}
		else if (match.type == VmHandlerType::Handler_VmExit) {
			auto matchData = std::get<VmExitData>(match.matchData);
			std::cout << "VM_EXIT { ";
			for (auto& reg : matchData.popedRegsOrder) {
				std::cout << reg.name << ' ';
			}
			std::cout << "}";
		}

		vmHandlers.push_back(std::move(match));

		if (data.vipRegId && data.vspRegId) {
			std::cout
				<< std::hex << "\t\t\tVSP: 0x" << data.vspChange.startValue
				<< " VIP: 0x" << data.vipChange.startValue << std::dec;
		}

		int64_t vspDelta = match.vspAfter - match.vspBefore;
		vspAccumulatedChange += vspDelta;
		std::cout << "\tVSP DELTA: " << vspDelta << "\tACCUMULATED VSP: " << vspAccumulatedChange;

		std::cout << '\n';
		if (formatAdditionalCarry) {
			std::cout << '\n';
		}

	}

	auto virtualBlocks = SplitToVirtualBlocks(vmHandlers);
	LLVMTraceLifter lifter(virtualBlocks);
	//lifter.OptimizeModule(true);
	//lifter.PrintModule();
	//CFGRepatcher::Patch(lifter);
	lifter.OptimizeModule(true);
	lifter.PrintModule();
	
	//lifter.OptimizeModule(true);
	//lifter.PrintModule();
	lifter.DumpModuleToFile("vm_lifted_module.ll");
	//EmulateVmCode(vmHandlers);
}