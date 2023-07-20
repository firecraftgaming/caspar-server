/*
* Copyright (c) 2023 Eliyah Sundström
*
* This file is part of an extension of the CasparCG project
*
* Author: Eliyah Sundström eliyah@sundstroem.com
 */


#pragma once

#include "../util/fixture_calculation.h"

#include <common/memory.h>

#include <core/consumer/frame_consumer.h>

#include <string>
#include <vector>

namespace caspar {
    namespace artnet {

        spl::shared_ptr<core::frame_consumer>
        create_preconfigured_consumer(const boost::property_tree::wptree&               ptree,
                                                                            const core::video_format_repository&              format_repository,
                                                                            std::vector<spl::shared_ptr<core::video_channel>> channels);
    }
} // namespace caspar::artnet
