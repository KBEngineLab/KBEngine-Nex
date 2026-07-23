/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2018 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
	IDAllocate(������)
		��������һ������������������Ψһid�� ʹ����������������Լ���֤�� һ��Ӧ��ֻ��ʹ��
		ͬһ��id����������ȡid����Ψһ�ġ�
		
		�����һ�� unsigned int���ͣ� �����������һֱ���Ϸ��䣬 ���ﵽ���͵����ֵ֮���
		��תͷ�ִ�0��ʼ�����ۼӷ��䣬 �����list��Ѱ�ң� �����ǰҪ�����IDû����list���ҵ�
		��ô���id�������䡣

		�÷�:
		IDAllocate<ENTITY_ID>* m_IDAllocPtr = new IDAllocate<ENTITY_ID>;
		// ����һ��id 
		m_IDAllocPtr->alloc()
		// ����һ��id
		m_IDAllocPtr->reclaim()
		
	IDServer(������)
		�����Ҫ���ṩ������������֮���entityID�ķ��䣬 ����Ҫ��baseappmgrʹ�ã� ÿ��IDserver
		�����ȡID��ʱ�� ����������ͻ����һ��Ψһid�θ��ͻ��ˣ� ��ô�ͻ��˾Ϳ��Ը��������
		�������е�Ψһid���������ɵķ��ɡ�
		
		�÷�:
		IDServer<ENTITY_ID>* m_idServer = new IDServer<ENTITY_ID>(1, 400);
		// ��ȡһ��id�� �������IDClient
		std::pair< unsigned int, unsigned int > idRange = m_idServer->allocRange();
		g_socketStreamIDClient->send(idRange.first, idRange.second);
		
	IDClient(�ͻ���)
		���ģ�������IDServer����id����ͽ��յģ� ��
		
		�÷�:
		IDClient<ENTITY_ID>* m_idClient = new IDClient<ENTITY_ID>;
		// ����IDServer���͹�����id��
		m_idClient->onAddRange(id_begin, id_end);
		// ����һ��id 
		m_idClient->alloc()
*/
#ifndef KBE_IDALLOCATE_H
#define KBE_IDALLOCATE_H

#include "helper/debug_helper.h"
#include "common/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <iostream>	
#include <queue>	

	
namespace KBEngine{

class ServerApp;

// ֱ��ʹ��һ���������� �������������ʹ�С�͹���������Ҫʹ���޷�������
// ��������ʱ����id�� ���Һܿ�黹�� �����Ų���ID��ͻ
template<typename T>
class IDAllocate
{
public:
	IDAllocate(): last_id_(0)
	{
	}

	virtual ~IDAllocate()
	{
	}	
	
	/** 
		����һ��id 
	*/
	T alloc(void)
	{
		T t = ++last_id_;
		if(t == 0)
			t = ++last_id_;

		return t;
	}
	
	/** 
		����һ��id 
	*/
	virtual void reclaim(T id)
	{
	}

	T lastID() const{ return last_id_; }
	void lastID(T v){ last_id_ = v; }

protected:
	// ���һ�����뵽��ID
	T last_id_;
};

// �����id�����洢���б��У� �´�ʹ�û���л�ȡ
template< typename T >
class IDAllocateFromList : public IDAllocate<T>
{
public:
	IDAllocateFromList(): IDAllocate<T>()
	{
	}

	~IDAllocateFromList()
	{
	}	
	
	/** 
		����һ��id 
	*/
	T alloc(void)
	{
		if(id_list_.size() > 0)
		{
			T n = id_list_.front();
			id_list_.pop();
			return n;
		}
		
		T t = ++IDAllocate<T>::last_id_;
		if(t == 0)
			t = ++IDAllocate<T>::last_id_;

		return t;
	}
	
	/** 
		����һ��id 
	*/
	void reclaim(T id)
	{
		id_list_.push(id);
	}

protected:
	// id�б��� ����ID����������б���
	typename std::queue< T > id_list_;
};


template< typename T >
class IDServer
{
public:
	IDServer(T id_begin, T range_step): 
	last_id_range_begin_(id_begin), 
	range_step_(range_step)
	{
	}

	~IDServer()
	{
	}
	
	/** 
		����һ��id�� 
	*/
	std::pair< T, T > allocRange(void)
	{
		INFO_MSG(fmt::format("IDServer::allocRange: {}-{}.\n", 
			last_id_range_begin_, (last_id_range_begin_ + range_step_)));
		
		std::pair< T, T > p = std::make_pair(last_id_range_begin_, last_id_range_begin_ + range_step_);
		last_id_range_begin_ += range_step_;
		return p;
	}

	void set_range_step(T range_step) {
		range_step_ = range_step;
	}

protected:
	// ���һ�����뵽��ID�ε���ʼλ��
	T last_id_range_begin_;
	
	// id�ε�һ���γ���
	T range_step_;	
};

template< typename T >
class IDClient
{										
public:
	IDClient():
	  last_id_range_begin_(0), 
	last_id_range_end_(0),
	requested_idserver_alloc_(false)
	{
	}
	
	/** 
		����ʱ����֪ͨIDServer���л��գ� ��ʹ�����Լ������ⷽ���ά�� 
	*/
	virtual ~IDClient()
	{
	}	
	
	bool hasReqServerAlloc() const { 
		return requested_idserver_alloc_; 
	}

	void setReqServerAllocFlag(bool has){ 
		requested_idserver_alloc_ = has; 
	}

	size_t size()
	{ 
		size_t nCount = last_id_range_end_ - last_id_range_begin_; 
		if(nCount <= 0)
		{
			// �����Ƿ��л����ID�Σ�����id���þ�ʱ����������뻺�浽���
			if(id_list_.size() > 0)
			{
				std::pair< T, T > n = id_list_.front();
				last_id_range_begin_ = n.first;
				last_id_range_end_ = n.second;
				id_list_.pop();
				nCount = last_id_range_end_ - last_id_range_begin_; 
			}
		}

		return nCount;
	}
	
	/**
		���entityID�Ƿ��� 
		ע�⣺һ��tick��ʹ��ID������Ҫ����ID_ENOUGH_LIMIT
	*/
	virtual void onAlloc(void) {
	};
	
	/** 
		idserver ���������һ��id�� 
	*/
	void onAddRange(T id_begin, T id_end)
	{
		INFO_MSG(fmt::format("IDClient::onAddRange: number of ids increased from {} to {}.\n", id_begin, id_end));
		if(size() <= 0)
		{
			last_id_range_begin_ = id_begin;
			last_id_range_end_ = id_end;
		}
		else
		{
			id_list_.push(std::make_pair(id_begin, id_end));
		}
	}
	
	/** 
		����һ��id 
	*/
	T alloc(void)
	{
		KBE_ASSERT(size() > 0 && "IDClient:: alloc:no usable of the id.\n");

		T id = last_id_range_begin_++;

		if(last_id_range_begin_ > last_id_range_end_)
		{
			// �����Ƿ��л����ID�Σ�����id���þ�ʱ����������뻺�浽���
			if(id_list_.size() > 0)
			{
				std::pair< T, T > n = id_list_.front();
				last_id_range_begin_ = n.first;
				last_id_range_end_ = n.second;
				id_list_.pop();
			}
			else
			{
				last_id_range_begin_ = last_id_range_end_ = 0;
			}
		}
		
		onAlloc();
		return id;
	}
	
	/** 
		����һ��id
	*/
	void onReclaim(T id)
	{
	}
	
protected:
	// id�б��� ����ID�ζ���������б���
	typename std::queue< std::pair< T, T > > id_list_;

	// ���һ�����뵽��ID�ε���ʼλ��
	T last_id_range_begin_;
	T last_id_range_end_;

	// �Ƿ��Ѿ�����ID����˷���ID
	bool requested_idserver_alloc_;	
};

class EntityIDClient : public IDClient<ENTITY_ID>
{
public:
	EntityIDClient();
	
	virtual ~EntityIDClient()
	{
	}	

	virtual void onAlloc(void);
	
	void pApp(ServerApp* pApp){ 
		pApp_ = pApp; 
	}

protected:
	ServerApp* pApp_;
};

}

#endif // KBE_IDALLOCATE_H
