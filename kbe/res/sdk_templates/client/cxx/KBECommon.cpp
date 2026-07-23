// Fill out your copyright notice in the Description page of Project Settings.

#include "KBECommon.h"
#include <chrono>

namespace KBEngine
{

}

double getTimeSeconds() {
    using namespace std::chrono;
    auto now = high_resolution_clock::now();
    auto ms  = time_point_cast<milliseconds>(now);
    auto epoch = ms.time_since_epoch();
    return epoch.count() * 0.001; // 转为秒
}

