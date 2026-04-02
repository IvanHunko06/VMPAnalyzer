#include "HandlerCachedStorage.hpp"

void HandlerCachedStorage::CacheInstruction(const HandlerMatch& match) {
	if (!enabled_) return;

	std::unique_lock<std::shared_mutex> lock(mutex_);
	if (handlers_.contains(match.addr)) return;

	CachedHandlerInfo info;
	info.type = match.type;
	info.bitDepth = match.bitDepth;
	if (match.type == Handler_VmPushConst) {
		auto& data = std::get<VmPushConstData>(match.matchData);
		info.constData = data.constData;
	}
	else if (match.type == Handler_VmJmpIndirect) {
		auto& data = std::get<VmJmpData>(match.matchData);
		info.jumpDestData = data.jmpDestData;
		info.newVipReg = data.newVipReg;
		info.newVspReg = data.newVspReg;
		info.newVipShift = data.newVipShift;
	}
	else if (match.type == Handler_VmPushReg ||
		match.type == Handler_VmPop) {
		auto& data = std::get<VmContextAccessData>(match.matchData);
		info.contextAccessLocation = data.contextAccess;
	}

	handlers_[match.addr] = info;
}

std::optional<HandlerMatch> HandlerCachedStorage::TryGetCachedInstruction(
	uint64_t addr, 
	const VmHandlerTrace& trace, 
	triton::arch::register_e vipReg,
	triton::arch::register_e vspReg) 
{
	if (!enabled_) return std::nullopt;

	std::shared_lock<std::shared_mutex> lock(mutex_);

	auto cachedDataIt = handlers_.find(addr);
	if (cachedDataIt == handlers_.end()) return std::nullopt;

	auto& cachedData = cachedDataIt->second;
	HandlerMatch match;
	match.addr = addr;
	match.type = cachedData.type;
	match.bitDepth = cachedData.bitDepth;
	match.vipBefore = trace.instructions[0].GetRegisterValue(vipReg);
	match.vspBefore = trace.instructions[0].GetRegisterValue(vspReg);
	match.vspAfter = trace.instructions[trace.instructions.size() - 1].GetRegisterValue(vspReg);


	if (cachedData.type == Handler_VmPushVsp) {
		VmPushVspData data;
		match.matchData = data;
	}
	else if (cachedData.type == Handler_VmPopVsp) {
		VmPopVspData data;
		match.matchData = data;
	}
	else if (cachedData.type == Handler_VmAdd ||
		cachedData.type == Handler_VmNor ||
		cachedData.type == Handler_VmNand ||
		cachedData.type == Handler_VmShl ||
		cachedData.type == Handler_VmShr) {
		VmAluData data;
		match.matchData = data;

	}
	else if (cachedData.type == Handler_VmShld ||
		cachedData.type == Handler_VmShrd) {
		VmShldData data;
		match.matchData = data;
	}
	else if (cachedData.type == Handler_VmReadMem ||
		cachedData.type == Handler_VmWriteMem) {
		VmMemAccessData data;
		match.matchData = data;
	}
	else if (cachedData.type == Handler_VmPushReg ||
		cachedData.type == Handler_VmPop) {
		VmContextAccessData data;
		data.offset = GetDataFromTrace(trace, cachedData.contextAccessLocation);
		data.regIndex = data.offset / (match.bitDepth / 8);
		match.matchData = data;
	}
	else if (cachedData.type == Handler_VmPushConst) {
		VmPushConstData data;
		uint64_t rawValue = GetDataFromTrace(trace, cachedData.constData);
		uint64_t mask = (cachedData.bitDepth == 64) ? ~0ULL : ((1ULL << cachedData.bitDepth) - 1);
		data.value = rawValue & mask;
		match.matchData = data;
	}
	else if (cachedData.type == Handler_VmJmpIndirect) {
		VmJmpData data;
		data.newVip = GetDataFromTrace(trace, cachedData.jumpDestData);
		data.newVipShift = cachedData.newVipShift;
		data.newVipReg = cachedData.newVipReg;
		data.newVspReg = cachedData.newVspReg;

		match.vspAfter = trace.instructions[trace.instructions.size() - 1].GetRegisterValue(data.newVspReg);

		match.matchData = data;

	}
	else if (cachedData.type == Handler_VmDispatch) {

	}
	else {
		__debugbreak();
	}
	
	return match;
}


uint64_t HandlerCachedStorage::GetDataFromTrace(const VmHandlerTrace& trace, const TraceInstructionLocation& location) {
	for (auto& instr : trace.instructions) {
		if (instr.GetAddress() == location.instAddr) {
			auto value = instr.GetRegisterValue(location.regId);
			return value;
		}
	}
	__debugbreak();
	return 0;
}