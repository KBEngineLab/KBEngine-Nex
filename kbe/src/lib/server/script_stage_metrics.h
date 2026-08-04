#ifndef KBE_SERVER_SCRIPT_STAGE_METRICS_H
#define KBE_SERVER_SCRIPT_STAGE_METRICS_H

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace KBEngine
{

enum ScriptStage
{
	SCRIPT_STAGE_RPC_LOOKUP = 0,
	SCRIPT_STAGE_PYTHON_LOOKUP,
	SCRIPT_STAGE_ARGUMENT_DECODE,
	SCRIPT_STAGE_PYTHON_CALL,
	SCRIPT_STAGE_CLEANUP,
	SCRIPT_STAGE_MIGRATION_SERIALIZE,
	SCRIPT_STAGE_MIGRATION_FORWARD,
	SCRIPT_STAGE_MIGRATION_DESERIALIZE_CREATE,
	SCRIPT_STAGE_MIGRATION_CALLBACK,
	SCRIPT_STAGE_MIGRATION_ENTITY_CONSTRUCT,
	SCRIPT_STAGE_MIGRATION_STREAM_RESTORE,
	SCRIPT_STAGE_MIGRATION_SPACE_AOI,
	SCRIPT_STAGE_MIGRATION_SPACE_REGISTER,
	SCRIPT_STAGE_MIGRATION_ENTER_SPACE_CALLBACK,
	SCRIPT_STAGE_MIGRATION_COORDINATE_INSTALL,
	SCRIPT_STAGE_MIGRATION_WITNESS_ENTER_WORLD,
	SCRIPT_STAGE_MIGRATION_WITNESS_SPACE_DATA,
	SCRIPT_STAGE_MIGRATION_WITNESS_NETWORK_NOTIFY,
	SCRIPT_STAGE_MIGRATION_WITNESS_VIEW_TRIGGER_INSTALL,
	SCRIPT_STAGE_MIGRATION_CALLBACK_SETUP,
	SCRIPT_STAGE_MIGRATION_BASE_NOTIFY,
	SCRIPT_STAGE_MIGRATION_WITNESS_TRIGGER_RESTORE,
	SCRIPT_STAGE_MIGRATION_PROXIMITY_RESTORE,
	SCRIPT_STAGE_MIGRATION_OWNER_CALLBACK,
	SCRIPT_STAGE_MIGRATION_COMPONENT_CALLBACK,
	SCRIPT_STAGE_COUNT
};

class ScriptStageStats
{
public:
	ScriptStageStats() :
		calls_(0),
		sampledCalls_(0),
		totalNanos_(0),
		maxNanos_(0),
		slowOver1ms_(0)
	{
	}

	void record(std::uint64_t durationNanos, bool sampled)
	{
		++calls_;
		if (!sampled)
			return;

		++sampledCalls_;
		totalNanos_ += durationNanos;
		if (durationNanos > maxNanos_)
			maxNanos_ = durationNanos;
		if (durationNanos >= 1000000)
			++slowOver1ms_;
	}

	std::uint64_t calls() const { return calls_; }
	std::uint64_t sampledCalls() const { return sampledCalls_; }
	std::uint64_t totalNanos() const { return totalNanos_; }
	std::uint64_t maxNanos() const { return maxNanos_; }
	std::uint64_t slowOver1ms() const { return slowOver1ms_; }
	std::uint64_t averageNanos() const
	{
		return sampledCalls_ == 0 ? 0 : totalNanos_ / sampledCalls_;
	}

private:
	std::uint64_t calls_;
	std::uint64_t sampledCalls_;
	std::uint64_t totalNanos_;
	std::uint64_t maxNanos_;
	std::uint64_t slowOver1ms_;
};

struct SlowScriptStage
{
	SlowScriptStage() : durationNanos(0), stage(SCRIPT_STAGE_RPC_LOOKUP) {}

	std::string name;
	std::string stageName;
	std::uint64_t durationNanos;
	ScriptStage stage;
};

/**
 * Process-local script and migration stage metrics for the component main thread.
 * 组件主线程专用的脚本与迁移阶段指标；固定容量避免慢样本集合随运行时间增长。
 */
class ScriptStageMetrics
{
public:
	static const std::size_t SLOW_TOP_CAPACITY = 8;
	static const std::uint32_t RPC_SAMPLE_RATE = 8;

	ScriptStageMetrics() : rpcCalls_(0) {}

	bool beginRpcCall()
	{
		++rpcCalls_;
		// 首次调用必须可观测；之后确定性采样，避免随机数和每次时钟读取进入 RPC 热路径。
		// The first call remains observable; deterministic sampling avoids RNG and per-call clocks on the RPC hot path.
		return rpcCalls_ == 1 || rpcCalls_ % RPC_SAMPLE_RATE == 0;
	}

	void record(ScriptStage stage, std::uint64_t durationNanos, bool sampled,
		const char* handlerName = NULL)
	{
		assert(stage >= 0 && stage < SCRIPT_STAGE_COUNT);
		stats(stage).record(durationNanos, sampled);
		if (sampled && durationNanos >= 1000000 && handlerName != NULL)
			recordSlow(stage, durationNanos, handlerName);
	}

	ScriptStageStats& stats(ScriptStage stage)
	{
		return stages_[static_cast<std::size_t>(stage)];
	}

	const ScriptStageStats& stats(ScriptStage stage) const
	{
		return stages_[static_cast<std::size_t>(stage)];
	}

	const SlowScriptStage& slow(std::size_t index) const
	{
		assert(index < SLOW_TOP_CAPACITY);
		return slowTop_[index];
	}

	std::string slowName(std::size_t index) const { return slow(index).name; }
	std::string slowStageName(std::size_t index) const { return slow(index).stageName; }
	std::uint64_t slowDurationNanos(std::size_t index) const { return slow(index).durationNanos; }
	std::uint64_t rpcCalls() const { return rpcCalls_; }
	std::uint32_t rpcSampleRate() const { return RPC_SAMPLE_RATE; }

	static const char* stageName(ScriptStage stage)
	{
		static const char* const names[SCRIPT_STAGE_COUNT] = {
			"rpcLookup", "pythonLookup", "argumentDecode", "pythonCall", "cleanup",
			"migrationSerialize", "migrationForward", "migrationDeserializeCreate", "migrationCallback",
			"migrationEntityConstruct", "migrationStreamRestore", "migrationSpaceAoi",
			"migrationSpaceRegister", "migrationEnterSpaceCallback", "migrationCoordinateInstall",
			"migrationWitnessEnterWorld", "migrationWitnessSpaceData", "migrationWitnessNetworkNotify",
			"migrationWitnessViewTriggerInstall", "migrationCallbackSetup", "migrationBaseNotify",
			"migrationWitnessTriggerRestore", "migrationProximityRestore",
			"migrationOwnerCallback", "migrationComponentCallback"
		};
		return names[static_cast<std::size_t>(stage)];
	}

private:
	void recordSlow(ScriptStage stage, std::uint64_t durationNanos, const char* handlerName)
	{
		// 同一 handler/阶段仅保留最慢观测，避免单个热点占满所有固定槽。
		// Retain only the maximum for each handler/stage so one hotspot cannot occupy every fixed slot.
		for (std::size_t i = 0; i < SLOW_TOP_CAPACITY; ++i)
		{
			if (slowTop_[i].stage != stage || slowTop_[i].name != handlerName)
				continue;
			if (durationNanos <= slowTop_[i].durationNanos)
				return;
			for (std::size_t j = i; j + 1 < SLOW_TOP_CAPACITY; ++j)
				slowTop_[j] = slowTop_[j + 1];
			slowTop_[SLOW_TOP_CAPACITY - 1] = SlowScriptStage();
			break;
		}

		std::size_t insertAt = SLOW_TOP_CAPACITY;
		for (std::size_t i = 0; i < SLOW_TOP_CAPACITY; ++i)
		{
			if (durationNanos > slowTop_[i].durationNanos)
			{
				insertAt = i;
				break;
			}
		}

		if (insertAt == SLOW_TOP_CAPACITY)
			return;

		for (std::size_t i = SLOW_TOP_CAPACITY - 1; i > insertAt; --i)
			slowTop_[i] = slowTop_[i - 1];

		slowTop_[insertAt].name.assign(handlerName);
		slowTop_[insertAt].stageName.assign(stageName(stage));
		slowTop_[insertAt].durationNanos = durationNanos;
		slowTop_[insertAt].stage = stage;
	}

	std::array<ScriptStageStats, SCRIPT_STAGE_COUNT> stages_;
	std::array<SlowScriptStage, SLOW_TOP_CAPACITY> slowTop_;
	std::uint64_t rpcCalls_;
};

inline ScriptStageMetrics& scriptStageMetrics()
{
	static ScriptStageMetrics metrics;
	return metrics;
}

}

#endif // KBE_SERVER_SCRIPT_STAGE_METRICS_H
