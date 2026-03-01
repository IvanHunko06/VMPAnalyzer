#include <map>
#include <optional>
#include <shared_mutex>
#include "VmHandlerMatcher.hpp"
#include "VmHandlerTrace.hpp"

class HandlerCachedStorage {
private:
	struct CachedHandlerInfo {
		VmHandlerType type;
		HandlerBitDepth bitDepth;

		// Äëÿ Jmp
		triton::arch::register_e newVipReg;
		triton::arch::register_e newVspReg;
		TraceInstructionLocation jumpDestData;
		int64_t newVipShift;


		// Äëÿ PushConst
		TraceInstructionLocation constData;

		//Äëÿ PushReg / PopReg
		TraceInstructionLocation contextAccessLocation;
	};
	std::map<uint64_t, CachedHandlerInfo> handlers_;
	std::shared_mutex mutex_;
	bool enabled_;

public:
	HandlerCachedStorage(bool enableStorage) {
		this->enabled_ = enableStorage;
	}

	void CacheInstruction(const HandlerMatch& match);

	std::optional<HandlerMatch> TryGetCachedInstruction(
		uint64_t addr, 
		const VmHandlerTrace& trace, 
		triton::arch::register_e vipReg, 
		triton::arch::register_e vspReg
	);
private:
	uint64_t GetDataFromTrace(const VmHandlerTrace& trace, const TraceInstructionLocation& location);
};