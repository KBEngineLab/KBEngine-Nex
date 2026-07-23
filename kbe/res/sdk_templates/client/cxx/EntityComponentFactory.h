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

#include "EntityComponent.h"

class EntityComponentFactory {
public:
    using Creator = std::function<KBEngine::EntityComponent*()>;

    static EntityComponentFactory& instance() {
        static EntityComponentFactory inst;
        return inst;
    }

    void registerType(const std::string& name, Creator creator) {
        creators_[name] = std::move(creator);
    }

    KBEngine::EntityComponent* create(const std::string& name) {
        auto it = creators_.find(name);
        if (it != creators_.end()) {
            return it->second();
        }
        ERROR_MSG("EntityComponentFactory::create: unknown class name: %s" , name.c_str());
        return new KBEngine::EntityComponent();
    }

    void reset() {
        creators_.clear();
    }

private:
    std::unordered_map<std::string, Creator> creators_;
};
