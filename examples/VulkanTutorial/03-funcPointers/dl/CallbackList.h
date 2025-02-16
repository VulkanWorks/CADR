#pragma once

#include <algorithm>
#include <functional>
#include <tuple>
#include <vector>


template<typename T>
class CallbackList final {
protected:
	std::vector<std::tuple<std::function<T>,void*>> _callbacks;
public:
	void append(std::function<T> cb,void* obj=nullptr);
	template<typename ObjType> void append(void (ObjType::*cb)(),ObjType* obj);
	void remove(std::function<T> cb,void* obj=nullptr);
	template<typename ObjType> void remove(void (ObjType::*cb)(),ObjType* obj);
	template<typename ...Args> void invoke(Args&& ...args);
};


// inline and template methods
template<typename T> void CallbackList<T>::append(std::function<T> cb,void* obj)  { _callbacks.emplace_back(cb,obj); }
template<typename T>template<typename ObjType> void CallbackList<T>::append(void (ObjType::*cb)(),ObjType* obj)  { append(std::bind(cb,obj),obj); }
template<typename T> void CallbackList<T>::remove(std::function<T> cb,void* obj)
	{
		auto it=std::find_if(_callbacks.rbegin(),_callbacks.rend(),
				[cb,obj](std::tuple<std::function<T>,void*> item) {
					return obj==std::get<1>(item) && cb.template target<T*>()==std::get<0>(item).template target<T*>();
				});
		if(it!=_callbacks.rend())
			_callbacks.erase((++it).base());
	}
template<typename T>template<typename ObjType> void CallbackList<T>::remove(void (ObjType::*cb)(),ObjType* obj)  { remove(std::bind(cb,obj),obj); }
template<typename T>template<typename ...Args> void CallbackList<T>::invoke(Args&& ...args)
	{
		for(auto& cb:_callbacks)
			std::invoke(std::get<0>(cb),std::forward<Args>(args)...);
	}
