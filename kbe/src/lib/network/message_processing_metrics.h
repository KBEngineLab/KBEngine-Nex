#ifndef KBE_NETWORK_MESSAGE_PROCESSING_METRICS_H
#define KBE_NETWORK_MESSAGE_PROCESSING_METRICS_H

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace KBEngine
{
namespace Network
{

enum MessageProcessingCategory
{
	MESSAGE_PROCESSING_CLIENT_MOVEMENT = 0,
	MESSAGE_PROCESSING_PYTHON_METHOD,
	MESSAGE_PROCESSING_CELL_MIGRATION,
	MESSAGE_PROCESSING_WATCHER_CONTROL,
	MESSAGE_PROCESSING_OTHER,
	MESSAGE_PROCESSING_CATEGORY_COUNT
};

class MessageProcessingCategoryStats
{
public:
	explicit MessageProcessingCategoryStats(std::uint32_t sampleRate = 1) :
		calls_(0),
		sampledCalls_(0),
		sampledTotalNanos_(0),
		sampledMaxNanos_(0),
		slowSamplesOver1ms_(0),
		sampleRate_(sampleRate),
		slowestHandlerID_(0),
		slowestHandlerName_()
	{
		assert(sampleRate_ > 0);
	}

	bool beginCall()
	{
		++calls_;
		// Always sample the first call so rare categories remain observable.
		// 首次调用始终采样，确保低频类别不会在整个测量窗口内保持空白。
		return calls_ == 1 || calls_ % sampleRate_ == 0;
	}

	void recordSample(std::uint64_t durationNanos, std::uint16_t handlerID,
		const std::string& handlerName)
	{
		++sampledCalls_;
		sampledTotalNanos_ += durationNanos;
		if (durationNanos > sampledMaxNanos_)
		{
			sampledMaxNanos_ = durationNanos;
			slowestHandlerID_ = handlerID;
			// Only a new maximum copies the name, keeping the sampled hot path allocation-free.
			// 仅刷新历史最大值时复制名称，确保采样热路径不会持续分配字符串。
			slowestHandlerName_ = handlerName;
		}
		if (durationNanos >= 1000000)
			++slowSamplesOver1ms_;
	}

	void recordSample(std::uint64_t durationNanos)
	{
		recordSample(durationNanos, 0, std::string());
	}

	std::uint64_t calls() const { return calls_; }
	std::uint64_t sampledCalls() const { return sampledCalls_; }
	std::uint64_t sampledTotalNanos() const { return sampledTotalNanos_; }
	std::uint64_t sampledMaxNanos() const { return sampledMaxNanos_; }
	std::uint64_t slowSamplesOver1ms() const { return slowSamplesOver1ms_; }
	std::uint32_t sampleRate() const { return sampleRate_; }
	std::uint16_t slowestHandlerID() const { return slowestHandlerID_; }
	const std::string& slowestHandlerName() const { return slowestHandlerName_; }
	std::uint64_t sampledAverageNanos() const
	{
		return sampledCalls_ == 0 ? 0 : sampledTotalNanos_ / sampledCalls_;
	}

private:
	std::uint64_t calls_;
	std::uint64_t sampledCalls_;
	std::uint64_t sampledTotalNanos_;
	std::uint64_t sampledMaxNanos_;
	std::uint64_t slowSamplesOver1ms_;
	std::uint32_t sampleRate_;
	std::uint16_t slowestHandlerID_;
	std::string slowestHandlerName_;
};

/**
 * Main-message handler timing with category-specific deterministic sampling.
 * 主消息处理分类计时：按类别使用确定性采样，限制高频移动和通用消息的时钟读取成本。
 */
class MessageProcessingMetrics
{
public:
	MessageProcessingMetrics() :
		stats_{{
			MessageProcessingCategoryStats(64),
			MessageProcessingCategoryStats(8),
			MessageProcessingCategoryStats(1),
			MessageProcessingCategoryStats(1),
			MessageProcessingCategoryStats(256)
		}}
	{
	}

	static MessageProcessingCategory classify(const std::string& name)
	{
		if (name.find("onUpdateDataFromClient") != std::string::npos)
			return MESSAGE_PROCESSING_CLIENT_MOVEMENT;
		if (name.find("queryWatcher") != std::string::npos)
			return MESSAGE_PROCESSING_WATCHER_CONTROL;
		if (name.find("Migration") != std::string::npos ||
			name.find("Teleport") != std::string::npos)
		{
			return MESSAGE_PROCESSING_CELL_MIGRATION;
		}
		if ((name.find("Remote") != std::string::npos &&
			name.find("Method") != std::string::npos) ||
			name.find("onEntityCall") != std::string::npos)
		{
			return MESSAGE_PROCESSING_PYTHON_METHOD;
		}
		return MESSAGE_PROCESSING_OTHER;
	}

	static const char* categoryName(MessageProcessingCategory category)
	{
		static const char* const names[MESSAGE_PROCESSING_CATEGORY_COUNT] = {
			"clientMovement", "pythonMethod", "cellMigration", "watcherControl", "other"
		};
		return names[static_cast<std::size_t>(category)];
	}

	bool beginCall(MessageProcessingCategory category)
	{
		return stats(category).beginCall();
	}

	void recordSample(MessageProcessingCategory category, std::uint64_t durationNanos)
	{
		stats(category).recordSample(durationNanos);
	}

	void recordSample(MessageProcessingCategory category, std::uint64_t durationNanos,
		std::uint16_t handlerID, const std::string& handlerName)
	{
		stats(category).recordSample(durationNanos, handlerID, handlerName);
	}

	MessageProcessingCategoryStats& stats(MessageProcessingCategory category)
	{
		return stats_[static_cast<std::size_t>(category)];
	}

	const MessageProcessingCategoryStats& stats(MessageProcessingCategory category) const
	{
		return stats_[static_cast<std::size_t>(category)];
	}

private:
	std::array<MessageProcessingCategoryStats, MESSAGE_PROCESSING_CATEGORY_COUNT> stats_;
};

}
}

#endif // KBE_NETWORK_MESSAGE_PROCESSING_METRICS_H
