// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#ifndef KBE_CLIENT_SDK_PLUGIN_H
#define KBE_CLIENT_SDK_PLUGIN_H

#include "client_sdk.h"
#include "common/common.h"
#include "helper/debug_helper.h"

namespace KBEngine{

class ClientSDKPlugin : public ClientSDK
{
public:
	ClientSDKPlugin();
	virtual ~ClientSDKPlugin();

	virtual std::string name() const {
		return "plugin";
	}


	virtual bool create(const std::string& path);

	
protected:
	std::string initBody_;
};

}
#endif
