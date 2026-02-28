#include "VmHandlerMatcher.hpp"
#include "triton/x8664Cpu.hpp"
#include <optional>
#include <iomanip>

bool IsVariable(const triton::ast::SharedAbstractNode& node, const std::string& name) {
	if (node->getType() == triton::ast::VARIABLE_NODE) {
		auto* variableNode = reinterpret_cast<triton::ast::VariableNode*>(node.get());
		return variableNode->getSymbolicVariable()->getAlias() == name;
	}
	// Если это ссылка, рекурсивно проверяем то, на что она указывает
	if (node->getType() == triton::ast::REFERENCE_NODE) {
		return IsVariable(reinterpret_cast<triton::ast::ReferenceNode*>(node.get())->getSymbolicExpression()->getAst(), name);
	}
	return false;
}
bool IsVariableContains(const triton::ast::SharedAbstractNode& node, const std::string& name) {
	if (node->getType() == triton::ast::VARIABLE_NODE) {
		auto* variableNode = reinterpret_cast<triton::ast::VariableNode*>(node.get());
		return variableNode->getSymbolicVariable()->getAlias().find(name) != std::string::npos;
	}
	// Если это ссылка, рекурсивно проверяем то, на что она указывает
	if (node->getType() == triton::ast::REFERENCE_NODE) {
		return IsVariable(reinterpret_cast<triton::ast::ReferenceNode*>(node.get())->getSymbolicExpression()->getAst(), name);
	}
	return false;
}
std::optional<std::string> GetVariableName(const triton::ast::SharedAbstractNode& node) {
	if (node->getType() == triton::ast::VARIABLE_NODE) {
		auto* variableNode = reinterpret_cast<triton::ast::VariableNode*>(node.get());
		return variableNode->getSymbolicVariable()->getAlias();
	}
	for (const auto& child : node->getChildren()) {
		auto res = GetVariableName(child);
		if (res) return *res;
	}
	return std::nullopt;
}
triton::ast::SharedAbstractNode GetUnderlyingVariable(const triton::ast::SharedAbstractNode& node) {
	if (!node) return nullptr;

	if (node->getType() == triton::ast::VARIABLE_NODE) {
		return node;
	}

	// Если это ZeroExtend или SignExtend, спускаемся к ребенку
	if (node->getType() == triton::ast::ZX_NODE ||
		node->getType() == triton::ast::EXTRACT_NODE) {
		for (auto& children : node->getChildren()) {
			auto result = GetUnderlyingVariable(children);
			if (result) return result;
		}
		
	}

	return nullptr;
}

bool IsStackArgument(const triton::ast::SharedAbstractNode& node) {
	if (node->getType() == triton::ast::VARIABLE_NODE) {
		return GetVariableName(node)->starts_with("StackArg_");
	}

	if (node->getChildren().size() > 0) {
		return IsStackArgument(node->getChildren()[0]);
	}
	return false;
}
bool DependsOnStack(const triton::ast::SharedAbstractNode& node) {
	if (node == nullptr) return false;
	if (IsStackArgument(node)) return true;
	for (const auto& child : node->getChildren()) {
		if (DependsOnStack(child)) return true;
	}
	return false;
}
std::optional<uint64_t> GetStackValueByName(const HandlerEmulationData& data, const std::string& varName) {
	if (!varName.starts_with("StackArg_")) return std::nullopt;

	try {
		uint64_t offset = std::stoull(varName.substr(9));

		uint64_t targetAddr = data.vspChange.startValue + offset;

		auto it = data.logicReads.find(targetAddr);
		if (it != data.logicReads.end()) {
			return it->second.concreteValue;
		}
	}
	catch (...) {}

	return std::nullopt;
}

bool IsAllOnes(const triton::ast::SharedAbstractNode& node) {
	if (node->getType() != triton::ast::BV_NODE) return false;
	uint64_t val = node->evaluate().convert_to<uint64_t>();
	uint32_t size = node->getBitvectorSize();
	uint64_t mask = (size == 64) ? -1ULL : ((1ULL << size) - 1);
	return (val & mask) == mask;
}

std::optional<int64_t> GetAddImmediate(const triton::ast::SharedAbstractNode& node, const std::string& symVarName) {
	if (node->getType() != triton::ast::BVADD_NODE) return std::nullopt;
	bool symVarMatches = false;
	bool constantValueFound = false;
	int64_t addedValue = 0;
	for (auto& children : node->getChildren()) {
		if (IsVariable(children, symVarName)) {
			symVarMatches = true;
		}
		if (children->getType() == triton::ast::BV_NODE) {
			uint64_t u64Value = static_cast<uint64_t>(children->evaluate());
			addedValue = static_cast<int64_t>(u64Value);
			constantValueFound = true;
		}
	}

	if (symVarMatches && constantValueFound) return addedValue;
	else return std::nullopt;
}

std::optional<HandlerMatch> TryMatchVmEntry(const HandlerEmulationData& data) {
	int64_t rspDelta = data.rspChange.endValue - data.rspChange.startValue;
	if (rspDelta >= -16) return std::nullopt;

	if (data.logicWrites.size() < 8) {
		return std::nullopt;
	}

	HandlerMatch match;
	match.type = Handler_VmEntry;
	match.addr = data.baseAddress;
	match.vipBefore = data.baseAddress;
	match.vspBefore = data.rspChange.startValue;
	//match.vspAfter = match.vspBefore;
	VmEntryHandlerData entryHandlerData;

	
	bool isFirst = true;
	bool hasRet = false;
	bool hasPrologue = false;
	for (auto& read : data.logicReads) {
		auto& instr = read.second.instruction->instruction;
		if (instr->getType() == triton::arch::x86::ID_INS_RET) {
			hasRet = true;
			break;
		}
	}

	std::vector<std::uint64_t> keys;
	for (auto& [addr, writeInfo] : data.logicWrites) {
		keys.push_back(addr);
	}
	std::reverse(keys.begin(), keys.end());

	{
		auto& firstRecord = data.logicWrites.at(keys[0]);
		auto& nextRecord = data.logicWrites.at(keys[1]);

		auto& firstInstruction = firstRecord.instruction->instruction;
		auto& nextInstruction = nextRecord.instruction->instruction;

		bool hasPushConst = firstInstruction->getType() == triton::arch::x86::ID_INS_PUSH &&
			firstInstruction->operands[0].getType() == triton::arch::OP_IMM;

		bool isCallImm = nextInstruction->getType() == triton::arch::x86::ID_INS_CALL &&
			nextInstruction->operands[0].getType() == triton::arch::OP_IMM;

		if (hasPushConst && isCallImm) {
			hasPrologue = true;
			entryHandlerData.pushedConst = firstRecord.concreteValue;
			entryHandlerData.returnAddr = nextRecord.concreteValue;
		}
	}

	int pushCount = 0;
	uint64_t endIndex = keys.size() - (hasRet ? 1 : 0);
	for (int i = hasPrologue ? 2 : 0;
		i < endIndex;
		++i) {

		auto& record = data.logicWrites.at(keys[i]);
		auto& instruction = record.instruction->instruction;
		auto instructionType = instruction->getType();

		if (instructionType != triton::arch::x86::ID_INS_PUSH &&
			instructionType != triton::arch::x86::ID_INS_PUSHFQ) continue;

		if (++pushCount == 17) {
			entryHandlerData.imageBaseDifference = record.concreteValue;
			continue;
		}
		
		NativeRegData pushedReg;
		if (instruction->getType() == triton::arch::x86::ID_INS_PUSHFQ) {
			pushedReg.id = triton::arch::register_e::ID_REG_X86_EFLAGS;
			pushedReg.name = "rflags";
			pushedReg.value = record.concreteValue;
		}
		else {
			auto& reg = instruction->operands[0].getRegister();
			if (instruction->operands[0].getType() != triton::arch::OP_REG) continue;
			pushedReg.id = reg.getId();
			pushedReg.name = reg.getName();
			pushedReg.value = record.concreteValue;
		}
		entryHandlerData.pushRegsOrder.push_back(std::move(pushedReg));
	}

	if (hasPrologue) pushCount += 2;

	for (const auto& [addr, readInfo] : data.logicReads) {
		if (readInfo.size != 4) continue;

		if (addr < defaultImageBase) continue;

		auto& instr = readInfo.instruction->instruction;
		if (instr->operands.size() < 2) continue;

		auto& vipReg = instr->operands[1].getMemory().getBaseRegister();
		entryHandlerData.vipReg = vipReg.getId();
		
		break;
	}

	for (auto& [regId, ast] : data.registerAstMap) {

		if (ast->getType() != triton::ast::BVADD_NODE) continue;

		if (regId == triton::arch::register_e::ID_REG_X86_RSP) continue;

		auto vspDeltaOpt = GetAddImmediate(ast, "VM_CONTEXT");

		if (vspDeltaOpt && *vspDeltaOpt == (-pushCount * 8)) {
			entryHandlerData.vspReg = regId;
			match.vspAfter = match.vspBefore + *vspDeltaOpt;
			break;
		}

	}

	if (entryHandlerData.vspReg == triton::arch::ID_REG_INVALID ||
		entryHandlerData.vipReg == triton::arch::ID_REG_INVALID) {
		return std::nullopt;
	}

	if (entryHandlerData.pushRegsOrder.size() < 8) {
		return std::nullopt;
	}

	match.matchData = std::move(entryHandlerData);

	return match;
}


std::optional<HandlerMatch> TryMatchVmPopReg(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	auto vspDeltaOpt = GetAddImmediate(data.vspAst.value(), "VSP");
	auto vipDeltaOpt = GetAddImmediate(data.vipAst.value(), "VIP");

	if (!vspDeltaOpt || !vipDeltaOpt) return std::nullopt;

	if (std::abs(*vipDeltaOpt) != 5) return std::nullopt;
	if (*vspDeltaOpt < 1 || *vspDeltaOpt > 8) return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmPop;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;
	VmContextAccessData pushRegData;
	bool is8BitHandler = false;

	for (auto& [addr, writeInfo] : data.logicWrites) {
		auto variableName = GetVariableName(writeInfo.ast);
		if (!variableName) continue;
		if (*variableName != "StackArg_0") continue;
		if (writeInfo.ast->getType() == triton::ast::EXTRACT_NODE) is8BitHandler = true;
		if (!is8BitHandler)
			match.bitDepth = static_cast<HandlerBitDepth>(writeInfo.size * 8);
		else
			match.bitDepth = BitDepth_8;
		pushRegData.offset = addr - data.rspChange.startValue;
		if (pushRegData.offset > 0x100) continue;
		pushRegData.regIndex = pushRegData.offset / writeInfo.size;
		pushRegData.value = writeInfo.concreteValue;
		
		pushRegData.contextAccess.instAddr = writeInfo.instruction->GetAddress();
		auto& operand = writeInfo.instruction->instruction->operands[0].getMemory();
		pushRegData.contextAccess.regId = operand.getIndexRegister().getId();

		match.matchData = pushRegData;

		return match;
	}

	return std::nullopt;
}

std::optional<HandlerMatch> TryMatchVmPushConst(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	auto vspDeltaOpt = GetAddImmediate(data.vspAst.value(), "VSP");
	auto vipDeltaOpt = GetAddImmediate(data.vipAst.value(), "VIP");

	if (!vspDeltaOpt || !vipDeltaOpt) return std::nullopt;

	if (std::abs(*vipDeltaOpt) < 5) return std::nullopt;
	if (*vspDeltaOpt > -1 || *vspDeltaOpt < -8) return std::nullopt;

	int64_t operandSize = std::abs(*vipDeltaOpt) - 4;
	if (operandSize < 1) return std::nullopt;

	bool isStandardHandler = false;
	bool is8BitHandler = false;

	if (operandSize == std::abs(*vspDeltaOpt))
		isStandardHandler = true;
	
	if (operandSize == 1 && vspDeltaOpt == -2)
		is8BitHandler = true;

	if (!isStandardHandler && !is8BitHandler)
		return std::nullopt;

	if (isStandardHandler && is8BitHandler)
		return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmPushConst;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;
	VmPushConstData pushConstData;

	for (auto& [addr, writeInfo] : data.logicWrites) {
		auto varNode = GetUnderlyingVariable(writeInfo.ast);
		if (varNode) continue;
		if (addr == data.vspChange.startValue + *vspDeltaOpt) {
			match.bitDepth = static_cast<HandlerBitDepth>(operandSize * 8);
			pushConstData.value = writeInfo.concreteValue;
			
			pushConstData.constData.instAddr = writeInfo.instruction->GetAddress();
			auto& operand = writeInfo.instruction->instruction->operands[1].getRegister();
			pushConstData.constData.regId = operand.getId();

			match.matchData = std::move(pushConstData);
			return match;
		}
	}

	return std::nullopt;
}

std::optional<HandlerMatch> TryMatchVmPushReg(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	auto vspDeltaOpt = GetAddImmediate(data.vspAst.value(), "VSP");
	auto vipDeltaOpt = GetAddImmediate(data.vipAst.value(), "VIP");

	if (!vspDeltaOpt || !vipDeltaOpt) return std::nullopt;

	if (std::abs(*vipDeltaOpt) != 5) return std::nullopt;
	if (*vspDeltaOpt > -1 || *vspDeltaOpt < -8) return std::nullopt;
	//if (data.rspChange.endValue - data.rspChange.startValue != 0) return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmPushReg;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;
	VmContextAccessData pushRegData;
	bool foundRead = false;

	for (auto& [addr, readInfo] : data.logicReads) {
		auto varNode = GetUnderlyingVariable(readInfo.ast);
		if (!varNode) continue;
		auto variableName = GetVariableName(varNode);
		if (!variableName) continue;
		if (!variableName->starts_with("VmReg_")) continue;

		pushRegData.contextAccess.instAddr = readInfo.instruction->GetAddress();
		auto& operand = readInfo.instruction->instruction->operands[1].getMemory();
		pushRegData.contextAccess.regId = operand.getIndexRegister().getId();
		foundRead = true;
		break;
	}

	if (!foundRead) return std::nullopt;

	for (auto& [addr, writeInfo] : data.logicWrites) {
		auto varNode = GetUnderlyingVariable(writeInfo.ast);
		if (!varNode) continue;
		auto variableName = GetVariableName(varNode);
		if (!variableName) continue;
		if (variableName->starts_with("VmReg_")) {
			bool foundRegRead = false;
			for (auto& read : data.logicReads) {
				if (IsVariable(read.second.ast, *variableName)) {
					match.bitDepth = static_cast<HandlerBitDepth>(read.second.size * 8);
					//if (match.bitDepth == BitDepth_8)
					//	__debugbreak();
					foundRegRead = true;
					break;
				}
			}
			if (!foundRegRead) continue;
			pushRegData.value = writeInfo.concreteValue;
			auto regOffsetStr = variableName->substr(6);
			pushRegData.offset = std::stoull(regOffsetStr, nullptr, 10);
			pushRegData.regIndex = pushRegData.offset / writeInfo.size;

			match.matchData = std::move(pushRegData);

			
			return match;
		}
	}

	return std::nullopt;
}

std::optional<HandlerMatch> TryMatchVmPushVsp(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	auto vspDeltaOpt = GetAddImmediate(data.vspAst.value(), "VSP");
	auto vipDeltaOpt = GetAddImmediate(data.vipAst.value(), "VIP");

	if (!vspDeltaOpt || !vipDeltaOpt) return std::nullopt;

	if (std::abs(*vipDeltaOpt) != 4) return std::nullopt;
	if (*vspDeltaOpt > -1 || *vspDeltaOpt < -8) return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmPushVsp;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;

	VmPushVspData pushVspData;

	for (auto& [addr, writeInfo] : data.logicWrites) {
		auto variable = GetUnderlyingVariable(writeInfo.ast);
		if (!variable) continue;
		if (IsVariable(variable, "VSP") &&
			addr == data.vspChange.startValue + *vspDeltaOpt) 
		{
			pushVspData.value = writeInfo.concreteValue;
			match.bitDepth = static_cast<HandlerBitDepth>(writeInfo.size * 8);
			match.matchData = pushVspData;
			return match;
		}
	}

	return std::nullopt;
}
std::optional<HandlerMatch> TryMatchVmPopVsp(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	if (!IsVariable(*data.vspAst, "StackArg_0")) return std::nullopt;

	bool hasWrites = true;
	if (data.systemReads.size() < 256 ||
		data.systemWrites.size() < 256) {
		hasWrites = false;
	}

	int64_t vspDelta = data.vspChange.endValue - data.vspChange.startValue;
	if (hasWrites && vspDelta > 0) return std::nullopt;
	if (!hasWrites && vspDelta < 0) return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmPopVsp;
	match.bitDepth = BitDepth_64;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;

	VmPopVspData popVspData;
	popVspData.offset = match.vspAfter - match.vspBefore;

	match.matchData = popVspData;

	return match;
}

std::optional<HandlerMatch> TryMatchVmReadMem(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	auto ptrReadIt = data.logicReads.find(data.vspChange.startValue);
	if (ptrReadIt == data.logicReads.end() || ptrReadIt->second.size != 8) {
		return std::nullopt;
	}

	uint64_t targetAddress = ptrReadIt->second.concreteValue;
	auto dataReadIt = data.logicReads.find(targetAddress);
	if (dataReadIt == data.logicReads.end()) {
		return std::nullopt;
	}
	uint32_t opSize = dataReadIt->second.size;
	bool is8BitHandler = false;
	bool isDefaultHandler = false;

	int64_t expectedDelta = 8 - opSize;
	int64_t actualDelta = data.vspChange.endValue - data.vspChange.startValue;

	if (actualDelta == expectedDelta) {
		isDefaultHandler = true;
	}

	if (opSize == 1 && actualDelta == 6) {
		is8BitHandler = true;
	}

	if (!isDefaultHandler && !is8BitHandler) return std::nullopt;
	if (isDefaultHandler && is8BitHandler) return std::nullopt;

	auto stackWriteIt = data.logicWrites.find(data.vspChange.endValue);
	if (stackWriteIt == data.logicWrites.end()) {
		return std::nullopt;
	}

	if (stackWriteIt->second.concreteValue != dataReadIt->second.concreteValue) {
		return std::nullopt;
	}

	HandlerMatch match;
	match.type = Handler_VmReadMem;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;
	if (!is8BitHandler)
		match.bitDepth = static_cast<HandlerBitDepth>(opSize * 8);
	else
		match.bitDepth = BitDepth_8;

	VmMemAccessData readMemData;
	readMemData.address = dataReadIt->first;
	readMemData.value = dataReadIt->second.concreteValue;
	match.matchData = readMemData;

	return match;
}
std::optional<HandlerMatch> TryMatchVmWriteMem(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;
	
	auto vspDeltaOpt = GetAddImmediate(data.vspAst.value(), "VSP");
	auto vipDeltaOpt = GetAddImmediate(data.vipAst.value(), "VIP");
	if (!vspDeltaOpt || !vipDeltaOpt) return std::nullopt;

	if (*vspDeltaOpt < 9) return std::nullopt;
	if (std::abs(*vipDeltaOpt) != 4) return std::nullopt;

	auto ptrReadIt = data.logicReads.find(data.vspChange.startValue);
	if (ptrReadIt == data.logicReads.end() || ptrReadIt->second.size != 8) {
		return std::nullopt;
	}
	uint64_t targetAddress = ptrReadIt->second.concreteValue;

	auto storeIt = data.logicWrites.find(targetAddress);

	// Если записи по этому адресу нет, это не VM_STORE
	if (storeIt == data.logicWrites.end()) {
		return std::nullopt;
	}

	const auto& writeInfo = storeIt->second;

	HandlerBitDepth bitDepth = static_cast<HandlerBitDepth>(writeInfo.size * 8);

	uint64_t valStackAddr = data.vspChange.startValue + 8;
	auto valReadIt = data.logicReads.find(valStackAddr);

	if (valReadIt != data.logicReads.end()) {
		// Проверяем совпадение значений (с учетом обрезания по маске размера)
		uint64_t mask = (writeInfo.size == 8) ? -1ULL : ((1ULL << (writeInfo.size * 8)) - 1);

		// Сравниваем то, что прочитали из стека, с тем, что записали в память
		if ((valReadIt->second.concreteValue & mask) != (writeInfo.concreteValue & mask)) {
			// Значения не совпадают, возможно это не Store, а какая-то сложная мутация
			return std::nullopt;
		}
	}
	else {
		// Если мы не нашли явного чтения по VSP+8, возможно стек был упакован плотно (например VSP+4).
		// Но при `add r9, 0x10` (как в логе) это точно +8.
		// Можно оставить проверку values опциональной или искать ближайшее чтение.
		return std::nullopt;
	}

	HandlerMatch match;
	match.type = Handler_VmWriteMem;
	match.bitDepth = bitDepth;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;

	VmMemAccessData memAccessData;
	memAccessData.address = storeIt->second.address;
	memAccessData.value = storeIt->second.concreteValue;
	match.matchData = memAccessData;

	return match;
}

VmHandlerType DetectAluOperation(const triton::ast::SharedAbstractNode& node) {
	auto type = node->getType();
	auto children = node->getChildren();
	if (type == triton::ast::ZX_NODE || type == triton::ast::SX_NODE ||
		type == triton::ast::EXTRACT_NODE) {
		for (auto child : children) {
			if (child->getType() == triton::ast::INTEGER_NODE) continue;
			return DetectAluOperation(child);
		}
	}

	// 1. Прямые операции
	if (type == triton::ast::BVADD_NODE) return Handler_VmAdd;
	if (type == triton::ast::BVSUB_NODE) return Handler_VmAdd; // SUB это ADD с отрицанием, но можно выделить отдельно
	if (type == triton::ast::BVLSHR_NODE) return Handler_VmShr;
	if (type == triton::ast::BVSHL_NODE) return Handler_VmShl;
	if (type == triton::ast::BVROL_NODE) return Handler_VmRol;
	if (type == triton::ast::BVROR_NODE) return Handler_VmRor;

	// 2. Сложная логика (VMProtect любит XOR -1 вместо NOT)
	// NOR = ~(A | B) -> XOR(OR(A, B), -1)
	if (type == triton::ast::BVXOR_NODE) {
		bool hasMinusOne = false;
		bool hasOr = false;
		bool hasAnd = false;

		for (const auto& child : node->getChildren()) {
			// Проверяем константу -1 (все единицы: 0xFF, 0xFFFF...)
			if (IsAllOnes(child)) hasMinusOne = true;
			// Проверяем операцию
			else if (child->getType() == triton::ast::BVOR_NODE) hasOr = true;
			else if (child->getType() == triton::ast::BVAND_NODE) hasAnd = true;
		}

		if (hasMinusOne && hasOr) return Handler_VmNor;
		if (hasMinusOne && hasAnd) return Handler_VmNand;
	}

	// Если просто NOT(OR(...))
	if (type == triton::ast::BVNOT_NODE) {
		auto child = node->getChildren()[0];
		if (child->getType() == triton::ast::BVOR_NODE) return Handler_VmNor;
		if (child->getType() == triton::ast::BVAND_NODE) return Handler_VmNand;
	}

	return Handler_Unknown;
}
std::optional<HandlerMatch> TryMatchGenericVmAlu(const HandlerEmulationData& data) {
	if (data.logicWrites.empty()) return std::nullopt;

	const MemoryAccessInfo* resultWrite = nullptr;
	VmHandlerType detectedType = Handler_Unknown;
	bool is8BitHandler = false;

	for (const auto& [addr, writeInfo] : data.logicWrites) {
		// Пропускаем простые константы (это могут быть флаги)
		if (writeInfo.ast->getType() == triton::ast::BV_NODE) continue;

		// Пытаемся определить операцию по AST
		auto type = DetectAluOperation(writeInfo.ast);
		if (type != Handler_Unknown) {
			// Проверяем зависимость от стека
			if (DependsOnStack(writeInfo.ast)) {
				detectedType = type;
				resultWrite = &writeInfo;
				is8BitHandler = writeInfo.ast->getType() == triton::ast::ZX_NODE;
				break; // Нашли результат
			}
		}
	}

	if (detectedType == Handler_Unknown || resultWrite == nullptr) {
		return std::nullopt;
	}

	HandlerMatch match;
	match.type = detectedType;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;
	if (!is8BitHandler)
		match.bitDepth = static_cast<HandlerBitDepth>(resultWrite->size * 8);
	else
		match.bitDepth = BitDepth_8;

	VmAluData aluData;
	aluData.result = resultWrite->concreteValue;
	for (const auto& [addr, writeInfo] : data.logicWrites) {
		if (&writeInfo == resultWrite) continue;

		if (std::abs(static_cast<int64_t>(addr - resultWrite->address)) <= 16) {
			aluData.flags = writeInfo.concreteValue;
			break;
		}
	}

	uint64_t arg1Addr = data.vspChange.startValue;
	uint32_t argSize = resultWrite->size; // Размер аргументов обычно равен размеру результата

	// --- Arg 1 ---
	auto itArg1 = data.logicReads.find(arg1Addr);
	if (itArg1 != data.logicReads.end()) {
		aluData.arg1 = itArg1->second.concreteValue;
	}

	bool isUnary = false; //(detectedType == Handler_VmNot || detectedType == Handler_VmNeg); // Дополнить по необходимости

	if (!isUnary) {
		uint64_t arg2Addr = arg1Addr + argSize;

		auto itArg2 = data.logicReads.find(arg2Addr);

		if (itArg2 != data.logicReads.end()) {
			aluData.arg2 = itArg2->second.concreteValue;
		}
		else {
			if (detectedType == Handler_VmShr || detectedType == Handler_VmShl ||
				detectedType == Handler_VmRol || detectedType == Handler_VmRor) {
				for (auto& read : data.logicReads) {
					if (read.first > arg1Addr && read.first <= arg1Addr + 8) {
						aluData.arg2 = read.second.concreteValue;
						break;
					}
				}
			}
		}
	}

	match.matchData = aluData;
	return match;
}

std::optional<HandlerMatch> TryMatchVmDoubleShift(const HandlerEmulationData& data) {
	const MemoryAccessInfo* resultWrite = nullptr;
	VmHandlerType detectedType = Handler_Unknown;

	// 1. Ищем запись результата (OR двух сдвигов)
	for (const auto& [addr, writeInfo] : data.logicWrites) {
		if (writeInfo.ast->getType() == triton::ast::BVOR_NODE) {
			auto children = writeInfo.ast->getChildren();
			if (children.size() != 2) continue;

			auto child1 = children[0];
			auto child2 = children[1];

			// Проверяем паттерн (SHL + LSHR)
			bool hasShl = (child1->getType() == triton::ast::BVSHL_NODE || child2->getType() == triton::ast::BVSHL_NODE);
			bool hasShr = (child1->getType() == triton::ast::BVLSHR_NODE || child2->getType() == triton::ast::BVLSHR_NODE);

			if (hasShl && hasShr) {
				// Определяем тип: SHLD или SHRD
				// SHLD: Dest сдвигается ВЛЕВО (SHL).
				// SHRD: Dest сдвигается ВПРАВО (LSHR).
				// Dest (Arg1) - это обычно StackArg с меньшим смещением (StackArg_0).

				auto varInShl = GetVariableName(child1->getType() == triton::ast::BVSHL_NODE ? child1 : child2);
				auto varInShr = GetVariableName(child1->getType() == triton::ast::BVLSHR_NODE ? child1 : child2);

				// Эвристика: StackArg_0 это Dest.
				// Если StackArg_0 внутри SHL -> SHLD.
				// Если StackArg_0 внутри SHR -> SHRD.

				if (!varInShl && !varInShr) continue;

				if (varInShl == "StackArg_0") detectedType = Handler_VmShld;
				else if (varInShr == "StackArg_0") detectedType = Handler_VmShrd;

				resultWrite = &writeInfo;
				break;
			}
		}
	}

	if (!resultWrite) return std::nullopt;

	HandlerMatch match;
	match.type = detectedType;
	match.bitDepth = static_cast<HandlerBitDepth>(resultWrite->size * 8);
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;

	VmShldData shldData;
	shldData.result = resultWrite->concreteValue;

	// 2. Собираем Флаги
	for (const auto& [addr, writeInfo] : data.logicWrites) {
		if (&writeInfo == resultWrite) continue;
		// Флаги обычно пишутся рядом (разница < 16 байт)
		if (std::abs(static_cast<int64_t>(addr - resultWrite->address)) <= 16) {
			shldData.flags = writeInfo.concreteValue;
			break;
		}
	}

	// 3. Собираем Аргументы (Src, Dst, Shift)
	// Используем смещения от VSP.
	// В SHLD 32-bit (как в логе):
	// Arg1 (Dst) @ VSP + 0 (или + смещение в хендлере) -> StackArg_0
	// Arg2 (Src) @ VSP + 4 -> StackArg_4
	// Arg3 (Shift) @ VSP + 8 -> StackArg_8

	// Мы можем попытаться найти их по именам StackArg_X, которые мы парсили для типа
	// Или просто взять по адресам относительно startValue VSP.

	// Адрес первого аргумента (Dst)
	uint64_t baseArgAddr = data.vspChange.startValue;

	// Коррекция на основе лога: в логе чтения идут по адресам 0x14f396 (Arg0), 0x14f39a (Arg4).
	// Старый VSP (startValue) был 0x14f396. Значит совпадает идеально.

	uint32_t opSize = resultWrite->size; // 4 байта

	// DST (Arg1)
	if (auto val = GetStackValueByName(data, "StackArg_0")) {
		shldData.dst = *val;
	}
	else {
		// Fallback по адресу
		if (data.logicReads.count(baseArgAddr))
			shldData.dst = data.logicReads.at(baseArgAddr).concreteValue;
	}

	// SRC (Arg2)
	if (auto val = GetStackValueByName(data, "StackArg_" + std::to_string(opSize))) { // StackArg_4
		shldData.src = *val;
	}
	else {
		if (data.logicReads.count(baseArgAddr + opSize))
			shldData.src = data.logicReads.at(baseArgAddr + opSize).concreteValue;
	}

	// SHIFT (Arg3 / Size)
	// Shift count часто лежит следом (Arg3) или читается как 1 байт.
	// В логе: StackArg_8 (offset 8), size 1 byte.
	// Пытаемся найти чтение по адресу Arg2 + opSize
	uint64_t shiftAddr = baseArgAddr + opSize + opSize; // +4+4 = +8

	// Ищем точное чтение
	if (data.logicReads.count(shiftAddr)) {
		shldData.shift = data.logicReads.at(shiftAddr).concreteValue;
	}
	else {
		// Если не нашли по адресу, пробуем вытащить константу из AST
		// (bvshl ... (_ bv5 32)) -> 5
		auto children = resultWrite->ast->getChildren();
		auto shlNode = (children[0]->getType() == triton::ast::BVSHL_NODE) ? children[0] : children[1];
		if (shlNode->getChildren().size() > 1) {
			auto shiftNode = shlNode->getChildren()[1];
			if (shiftNode->getType() == triton::ast::BV_NODE) {
				shldData.shift = shiftNode->evaluate().convert_to<uint64_t>();
			}
		}
	}

	match.matchData = shldData;

	return match;
}

std::optional<HandlerMatch> TryMatchVmJmpIndirectRemap(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmJmpIndirect;
	match.bitDepth = BitDepth_64;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;

	VmJmpData jmpData;

	std::vector<triton::arch::register_e> vspCandidats;
	std::vector<triton::arch::register_e> vipCandidats;
	for (auto& [reg, ast] : data.registerAstMap) {
		auto vspDeltaOpt = GetAddImmediate(ast, "VSP");
		auto vipDeltaOpt = GetAddImmediate(ast, "StackArg_0");
		if (vspDeltaOpt && *vspDeltaOpt == 8) {
			vspCandidats.push_back(reg);
		}
		if (vipDeltaOpt && std::abs(*vipDeltaOpt) == 4) {
			vipCandidats.push_back(reg);
		}
	}

	if (vspCandidats.empty() || vipCandidats.empty()) return std::nullopt;

	if (vspCandidats.size() == 1) {
		jmpData.newVspReg = vspCandidats[0];
	}
	else {
		for (auto& candidat : vspCandidats) {
			if (candidat == data.vspRegId) continue;
			jmpData.newVspReg = candidat;
			break;
		}
	}
	
	if (vipCandidats.size() == 1) {
		jmpData.newVipReg = vipCandidats[0];
	}
	else {
		for (auto& candidat : vipCandidats) {
			if (candidat == data.vipRegId) continue;
			jmpData.newVipReg = candidat;
			break;
		}
	}

	auto readIt = data.logicReads.find(data.vspChange.startValue);
	if (readIt == data.logicReads.end()) return std::nullopt;
	auto vipRegAst = data.registerAstMap.at(jmpData.newVipReg);
	auto shift = GetAddImmediate(vipRegAst, "StackArg_0");
	if (!shift) return std::nullopt;

	auto& instr = readIt->second.instruction;
	jmpData.newVip = readIt->second.concreteValue;
	jmpData.newVipShift = *shift;
	jmpData.jmpDestData.instAddr = instr->GetAddress();
	auto& operand = instr->instruction->operands[0].getRegister();
	jmpData.jmpDestData.regId = operand.getId();

	auto vspRegAst = data.registerAstMap.at(jmpData.newVspReg);
	if (!vspRegAst) return std::nullopt;
	auto vspShiftOpt = GetAddImmediate(vspRegAst, "VSP");
	if (!vspShiftOpt) return std::nullopt;
	match.vspAfter = data.vspChange.startValue + *vspShiftOpt;

	if (jmpData.newVipReg == triton::arch::register_e::ID_REG_INVALID ||
		jmpData.newVspReg == triton::arch::register_e::ID_REG_INVALID) {
		return std::nullopt;
	}

	match.matchData = jmpData;

	return match;
}

std::optional<HandlerMatch> TryMatchVmExit(const HandlerEmulationData& data) {
	if (data.logicReads.size() < 8 ||
		data.logicReads.size() > 17) {
		return std::nullopt;
	}

	HandlerMatch match;
	match.type = Handler_VmExit;
	match.bitDepth = BitDepth_64;
	match.addr = data.baseAddress;
	match.vipBefore = data.vipChange.startValue;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.rspChange.endValue;

	VmExitData exitData;
	int actualPopCount = 0;
	for (auto& [addr, readInfo] : data.logicReads) {
		auto& instr = readInfo.instruction->instruction;
		auto instrType = instr->getType();
		if (instrType == triton::arch::x86::ID_INS_POP) {
			auto& operand = instr->operands[0];
			if (operand.getType() != triton::arch::OP_REG) continue;
			auto& regOperand = operand.getRegister();
			++actualPopCount;
			NativeRegData pushedRegData;
			pushedRegData.id = regOperand.getId();
			pushedRegData.name = regOperand.getName();
			pushedRegData.value = readInfo.concreteValue;
			exitData.popedRegsOrder.push_back(pushedRegData);
		}
		else if (instrType == triton::arch::x86::ID_INS_POPFQ) {
			++actualPopCount;
			NativeRegData pushedRegData;
			pushedRegData.id = data.context->registers.x86_eflags.getId();
			pushedRegData.name = data.context->registers.x86_eflags.getName();
			pushedRegData.value = readInfo.concreteValue;
			exitData.popedRegsOrder.push_back(pushedRegData);
		}
	}

	if (actualPopCount != data.logicReads.size() - 1) return std::nullopt;
	match.matchData = std::move(exitData);
	return match;
}

std::optional<HandlerMatch> TryMatchVmDispatch(const HandlerEmulationData& data) {
	if (!data.vipAst || !data.vspAst) return std::nullopt;

	if (!IsVariable(*data.vspAst, "VSP")) return std::nullopt;

	auto vipDeltaOpt = GetAddImmediate(data.vipAst.value(), "VIP");
	if (!vipDeltaOpt) return std::nullopt;
	if (std::abs(*vipDeltaOpt) != 4) return std::nullopt;

	HandlerMatch match;
	match.type = Handler_VmDispatch;
	match.bitDepth = BitDepth_64;
	match.vipBefore = data.vipChange.startValue;
	match.addr = data.baseAddress;
	match.vspBefore = data.vspChange.startValue;
	match.vspAfter = data.vspChange.endValue;

	return match;
}

HandlerMatch MatchVmHandler(const HandlerEmulationData& data) {

	auto isVmEntryHandler = TryMatchVmEntry(data);
	if (isVmEntryHandler) return *isVmEntryHandler;

	auto isVmPopRegHandler = TryMatchVmPopReg(data);
	if (isVmPopRegHandler) return *isVmPopRegHandler;

	auto isVmPushConstHandler = TryMatchVmPushConst(data);
	if (isVmPushConstHandler) return *isVmPushConstHandler;

	auto isVmPushRegHandler = TryMatchVmPushReg(data);
	if (isVmPushRegHandler) return *isVmPushRegHandler;

	auto isVmPushVspHandler = TryMatchVmPushVsp(data);
	if (isVmPushVspHandler) return *isVmPushVspHandler;

	auto isVmPopVsp = TryMatchVmPopVsp(data);
	if (isVmPopVsp) return *isVmPopVsp;

	auto isVmReadMem = TryMatchVmReadMem(data);
	if (isVmReadMem) return *isVmReadMem;

	auto isVmWriteMem = TryMatchVmWriteMem(data);
	if (isVmWriteMem) return *isVmWriteMem;

	auto isVmAlu = TryMatchGenericVmAlu(data);
	if (isVmAlu) return *isVmAlu;

	auto isVmDoubleShft = TryMatchVmDoubleShift(data);
	if (isVmDoubleShft) return *isVmDoubleShft;

	auto isVmJmpIndirectRemap = TryMatchVmJmpIndirectRemap(data);
	if (isVmJmpIndirectRemap) return *isVmJmpIndirectRemap;

	auto isVmDispatch = TryMatchVmDispatch(data);
	if (isVmDispatch) return *isVmDispatch;

	auto isVmExit = TryMatchVmExit(data);
	if (isVmExit) return *isVmExit;

	return {};
}

std::vector<VirtualBasicBlock> SplitToVirtualBlocks(const std::vector<HandlerMatch>& trace) {
	std::vector<VirtualBasicBlock> blocks;

	VirtualBasicBlock currentBlock;
	int64_t currentStackOffset = 0;

	for (auto& instr : trace) {
		currentBlock.instructions.push_back(instr);
		if (currentBlock.instructions.size() == 1) {
			currentBlock.startAddr = instr.vipBefore;
			currentBlock.virtualStackOffset = currentStackOffset;
		}
		currentStackOffset += instr.vspAfter - instr.vspBefore;
		
		if (instr.type == Handler_VmJmpIndirect) {
			currentBlock.nextVipShift = std::get<VmJmpData>(instr.matchData).newVipShift;
		}

		if (instr.type == Handler_VmJmpIndirect ||
			instr.type == Handler_VmExit) {
			blocks.push_back(std::move(currentBlock));
		}
	}

	return blocks;
}

std::ostream& operator<<(std::ostream& os, const HandlerMatch& p) {
	os << std::hex << "0x" << p.addr << ": ";
	os << "0x" << p.vipBefore << ": " << std::dec;

	if (p.type == VmHandlerType::Handler_Unknown) {
		os << "UNKNOWN";
	}
	else if (p.type == VmHandlerType::Handler_VmEntry) {
		auto matchData = std::get<VmEntryHandlerData>(p.matchData);
		os << "VM_ENTRY { ";
		for (auto& reg : matchData.pushRegsOrder) {
			os << reg.name << ' ';
		}
		os << "} Image base dif: " << matchData.imageBaseDifference;
	}
	else if (p.type == VmHandlerType::Handler_VmPop) {
		auto matchData = std::get<VmContextAccessData>(p.matchData);
		os
			<< "VM_POP" << p.bitDepth << "\tR" << matchData.regIndex
			<< "\t\t Offset: 0x" << std::hex << matchData.offset <<
			" Value: 0x" << matchData.value << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmPushConst) {
		auto matchData = std::get<VmPushConstData>(p.matchData);
		os
			<< "VM_PUSH_CONST" << p.bitDepth 
			<< std::hex << " 0x" << matchData.value 
			<< std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmPushReg) {
		auto matchData = std::get<VmContextAccessData>(p.matchData);
		os
			<< "VM_PUSH_REG" << p.bitDepth << "   R" << matchData.regIndex
			<< "\t\t Offset: 0x" << std::hex << matchData.offset <<
			" Value: 0x" << matchData.value << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmPushVsp) {
		auto matchData = std::get<VmPushVspData>(p.matchData);
		os
			<< "VM_PUSH_VSP" << p.bitDepth 
			<< std::hex << "\t\t\t Pushed VSP: 0x" 
			<< matchData.value << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmPopVsp) {
		os << "VM_POP_VSP";
	}
	else if (p.type == VmHandlerType::Handler_VmReadMem) {
		auto matchData = std::get<VmMemAccessData>(p.matchData);
		os
			<< "VM_READ_MEM" << p.bitDepth << std::hex 
			<< "\t\t\t [0x" << matchData.address << "]->0x" 
			<< matchData.value << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmWriteMem) {
		auto matchData = std::get<VmMemAccessData>(p.matchData);
		os
			<< "VM_WRITE_MEM" << p.bitDepth << std::hex 
			<< "\t\t\t [0x" << matchData.address << "]=0x" 
			<< matchData.value << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmAdd) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_ADD" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmNand) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_NAND" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmNor) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_NOR" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmRol) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_ROL" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmRor) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_ROR" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmShl) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_SHL" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmShr) {
		auto matchData = std::get<VmAluData>(p.matchData);
		os
			<< "VM_SHR" << p.bitDepth << std::hex
			<< "\t\t\t Arg1: 0x" << matchData.arg1
			<< " Arg2: 0x" << matchData.arg2
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmShld) {
		auto matchData = std::get<VmShldData>(p.matchData);
		os
			<< "VM_SHLD" << p.bitDepth << std::hex
			<< "\t\t\t Dst: 0x" << matchData.dst
			<< " Src: 0x" << matchData.src
			<< " Shift: 0x" << matchData.shift
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmShrd) {
		auto matchData = std::get<VmShldData>(p.matchData);
		os
			<< "VM_SHRD" << p.bitDepth << std::hex
			<< "\t\t\t Dst: 0x" << matchData.dst
			<< " Src: 0x" << matchData.src
			<< " Shift: 0x" << matchData.shift
			<< " Result: 0x" << matchData.result
			<< " Flags(may not match the trace): 0x" << matchData.flags << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmJmpIndirect) {

		auto matchData = std::get<VmJmpData>(p.matchData);
		os
			<< "VM_JMP_INDIRECT" << std::hex 
			<< "\t\t Dst VIP: 0x" 
			<< matchData.newVip << std::dec;
	}
	else if (p.type == VmHandlerType::Handler_VmDispatch) {
		std::cout << "VM_DISPATCH";
	}
	else if (p.type == VmHandlerType::Handler_VmExit) {
		auto matchData = std::get<VmExitData>(p.matchData);
		std::cout << "VM_EXIT { ";
		for (auto& reg : matchData.popedRegsOrder) {
			std::cout << reg.name << ' ';
		}
		std::cout << "}";
	}

	return os;
}

std::ostream& operator<<(std::ostream& os, const VirtualBasicBlock& p) {
	os << "Block VIP: 0x" << std::hex << p.startAddr << std::dec << '\n';
	for (auto& instr : p.instructions) {
		os << instr << '\n';
	}
	return os;
}