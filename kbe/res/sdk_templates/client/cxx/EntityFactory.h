//
// Created by KBEngineLab on 2025/12/1.
//

// EntityFactory.h
#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <utility>

#include "Entity.h"
class EntityFactory {
public:
    using Creator = std::function<KBEngine::Entity*()>;

    static EntityFactory& instance() {
        static EntityFactory inst;
        return inst;
    }

    void registerType(const std::string& name, Creator creator) {
        creators_[name] = std::move(creator);
    }

    KBEngine::Entity* create(const std::string& name) {
        auto it = creators_.find(name);
        if (it != creators_.end()) {
            return it->second();
        }

        ERROR_MSG("EntityFactory::create: unknown class name: %s", name.c_str());
        return new KBEngine::Entity();
    }

    void reset() {
        creators_.clear();
    }

private:
    std::unordered_map<std::string, Creator> creators_;
};