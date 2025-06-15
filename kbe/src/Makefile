ifndef KBE_ROOT
export KBE_ROOT := $(subst /kbe/src,,$(CURDIR))
endif



all clean:
	@start_time=$$(date +%s); \
	start_fmt=$$(date -d "@$${start_time}" '+%Y-%m-%d %H:%M:%S'); \
	echo "编译开始：$$(date '+%Y-%m-%d %H:%M:%S')"; \
	$(MAKE) -C lib $@; \
	$(MAKE) -C server $@; \
	end_time=$$(date +%s); \
	echo "编译开始：$${start_fmt}"; \
	echo "编译完成：$$(date '+%Y-%m-%d %H:%M:%S')"; \
	elapsed=$$((end_time - start_time)); \
	hours=$$((elapsed / 3600)); \
	minutes=$$(((elapsed % 3600) / 60)); \
	seconds=$$((elapsed % 60)); \
	echo "共计用时：$${hours} 小时 $${minutes} 分钟 $${seconds} 秒";

	
cleanall:
	$(MAKE) clean

