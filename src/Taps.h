/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include "AbstractManager.h"
#include "AbstractModule.h"
#include "Configurable.h"
#include "InputModulePlugin.h"
#include <yaml-cpp/yaml.h>

namespace visor {

class Tap : public AbstractModule
{

public:
    Tap(const std::string &name)
        : AbstractModule(name)
    {
    }

    void info_json(json &j) const override {
        AbstractModule::_common_info_json(j);
    }
};

class TapManager : public AbstractManager<Tap>
{

    const InputPluginRegistry *_input_plugin_registry;

public:
    TapManager(const InputPluginRegistry *inputManager)
        : _input_plugin_registry(inputManager)
    {
    }

    virtual ~TapManager()
    {
    }

    void load(const YAML::Node &tap_yaml, bool strict);
};

}