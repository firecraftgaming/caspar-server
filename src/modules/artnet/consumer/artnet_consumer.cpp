/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Eliyah Sundström eliyah@sundstroem.com
 */

#include "artnet_consumer.h"

#undef NOMINMAX
// ^^ This is needed to avoid a conflict between boost asio and other header files defining NOMINMAX

#include <common/array.h>
#include <common/future.h>
#include <common/log.h>
#include <common/ptree.h>
#include <common/utf.h>

#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/locale/encoding_utf.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cmath>
#include <thread>
#include <utility>
#include <vector>

using namespace boost::asio;
using namespace boost::asio::ip;

namespace caspar { namespace artnet {

struct configuration
{
    int refreshRate = 10;
    std::vector<sender> senders;
};

struct artnet_consumer : public core::frame_consumer
{
    const configuration           config;
    std::vector<computed_sender> computed_senders;

  public:
    // frame_consumer

    explicit artnet_consumer(configuration config)
        : config(std::move(config))
        , io_service()
        , socket(io_service, udp::v4())
    {
        compute_senders();
    }

    void initialize(const core::video_format_desc& /*format_desc*/, int /*channel_index*/) override
    {
        thread_ = std::thread([this] {
            long long time      = 1000 / config.refreshRate;
            auto      last_send = std::chrono::system_clock::now();

            while (!abort_request_) {
                try {
                    auto                          now             = std::chrono::system_clock::now();
                    std::chrono::duration<double> elapsed_seconds = now - last_send;
                    long long                     elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed_seconds).count();

                    long long sleep_time = time - elapsed_ms;
                    if (sleep_time > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));

                    last_send = now;

                    frame_mutex_.lock();
                    auto frame = last_frame_;

                    frame_mutex_.unlock();
                    if (!frame)
                        continue; // No frame available

                    send_computed_senders(frame);
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                }
            }
        });
    }

    ~artnet_consumer()
    {
        abort_request_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        last_frame_ = frame;

        return make_ready_future(true);
    }

    std::wstring print() const override { return L"artnet[]"; }

    std::wstring name() const override { return L"artnet"; }

    int index() const override { return 1337; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["artnet/computed-senders"]  = computed_senders.size();
        state["artnet/senders"]           = config.senders.size();
        state["artnet/refresh-rate"]      = config.refreshRate;

        return state;
    }

  private:
    core::const_frame last_frame_;
    std::mutex        frame_mutex_;

    std::thread       thread_;
    std::atomic<bool> abort_request_{false};

    boost::asio::io_service io_service;
    udp::socket   socket;
    udp::endpoint remote_endpoint;

    void send_computed_senders(core::const_frame frame)
    {
        for (auto computed_sender : computed_senders)
            send_computed_sender(computed_sender, frame);
    }

    void send_computed_sender(computed_sender sender, core::const_frame frame) {
        uint8_t dmx_data[512];
        memset(dmx_data, 0, 512);

        for (auto computed_fixture : sender.fixtures) {
            auto     color = average_color(frame, computed_fixture.rectangle);
            uint8_t* ptr   = dmx_data + computed_fixture.address;

            switch (computed_fixture.type) {
                case FixtureType::DIMMER:
                    ptr[0] = (uint8_t)(0.279 * color.r + 0.547 * color.g + 0.106 * color.b);
                    break;
                case FixtureType::RGB:
                    ptr[0] = color.r;
                    ptr[1] = color.g;
                    ptr[2] = color.b;
                    break;
                case FixtureType::RGBW:
                    uint8_t w = std::min(std::min(color.r, color.g), color.b);
                    ptr[0]    = color.r - w;
                    ptr[1]    = color.g - w;
                    ptr[2]    = color.b - w;
                    ptr[3]    = w;
                    break;
            }
        }

        send_dmx_data(sender, dmx_data, 512);
    }

    vector<computed_fixture> compute_fixtures(sender sender)
    {
        vector<computed_fixture> computed_fixtures;
        for (auto fixture : sender.fixtures) {
            for (int i = 0; i < fixture.fixtureCount; i++) {
                computed_fixture computed_fixture{};
                computed_fixture.type    = fixture.type;
                computed_fixture.address = fixture.startAddress + i * fixture.fixtureChannels;

                computed_fixture.rectangle = compute_rect(fixture.fixtureBox, i, fixture.fixtureCount);
                computed_fixtures.push_back(computed_fixture);
            }
        }

        return compute_fixtures;
    }

    void compute_senders()
    {
        computed_senders.clear();
        for (auto sender : config.senders) {
            computed_sender computed_sender = {};

            std::string host_ = u8(sender.host);
            computed_sender.endpoint =
                boost::asio::ip::udp::endpoint(boost::asio::ip::address::from_string(host_), sender.port);

            computed_sender.universe = sender.universe;
            computed_sender.fixtures = compute_fixtures(sender);
            computed_senders.push_back(computed_sender);
        }
    }

    void send_dmx_data(computed_sender sender, const std::uint8_t* data, std::size_t length)
    {
        int universe = sender.universe;

        std::uint8_t hUni = (universe >> 8) & 0xff;
        std::uint8_t lUni = universe & 0xff;

        std::uint8_t hLen = (length >> 8) & 0xff;
        std::uint8_t lLen = (length & 0xff);

        std::uint8_t header[] = {65, 114, 116, 45, 78, 101, 116, 0, 0, 80, 0, 14, 0, 0, lUni, hUni, hLen, lLen};
        std::uint8_t buffer[18 + 512];

        for (int i = 0; i < 18 + 512; i++) {
            if (i < 18) {
                buffer[i] = header[i];
                continue;
            }

            if (i - 18 < length) {
                buffer[i] = data[i - 18];
                continue;
            }

            buffer[i] = 0;
        }

        boost::system::error_code err;
        socket.send_to(boost::asio::buffer(buffer), sender.endpoint, 0, err);
        if (err)
            CASPAR_THROW_EXCEPTION(io_error() << msg_info(err.message()));
    }
};

std::vector<fixture> get_fixtures_ptree(const boost::property_tree::wptree& ptree)
{
    std::vector<fixture> fixtures;

    using boost::property_tree::wptree;

    for (auto& xml_fixture : ptree | witerate_children(L"fixtures") | welement_context_iteration) {
        ptree_verify_element_name(xml_fixture, L"fixture");
        fixture f{};

        int startAddress = xml_fixture.second.get(L"start-address", 0);
        if (startAddress < 1)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture start address must be specified"));

        f.startAddress = startAddress - 1;

        int fixtureCount = xml_fixture.second.get(L"fixture-count", -1);
        if (fixtureCount < 1)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture count must be specified"));

        f.fixtureCount = fixtureCount;

        std::wstring type = xml_fixture.second.get(L"type", L"");
        if (type.empty())
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Fixture type must be specified"));

        if (boost::iequals(type, L"DIMMER")) {
            f.type = FixtureType::DIMMER;
        } else if (boost::iequals(type, L"RGB")) {
            f.type = FixtureType::RGB;
        } else if (boost::iequals(type, L"RGBW")) {
            f.type = FixtureType::RGBW;
        } else {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Unknown fixture type"));
        }

        int fixtureChannels = xml_fixture.second.get(L"fixture-channels", -1);
        if (fixtureChannels < 0)
            fixtureChannels = f.type;
        if (fixtureChannels < f.type)
            CASPAR_THROW_EXCEPTION(
                user_error() << msg_info(
                    L"Fixture channel count must be at least enough channels for current color mode"));

        f.fixtureChannels = fixtureChannels;

        box b{};

        auto x = xml_fixture.second.get(L"x", 0.0f);
        auto y = xml_fixture.second.get(L"y", 0.0f);

        b.x = x;
        b.y = y;

        auto width  = xml_fixture.second.get(L"width", 0.0f);
        auto height = xml_fixture.second.get(L"height", 0.0f);

        b.width  = width;
        b.height = height;

        auto rotation = xml_fixture.second.get(L"rotation", 0.0f);

        b.rotation   = rotation;
        f.fixtureBox = b;

        fixtures.push_back(f);
    }

    return fixtures;
}

std::vector<sender> get_senders_ptree(const boost::property_tree::wptree& ptree)
{
    std::vector<sender> senders;

    using boost::property_tree::wptree;
    for (auto& xml_sender : ptree | witerate_children(L"senders") | welement_context_iteration) {
        ptree_verify_element_name(xml_sender, L"sender");
        sender s{};

        s.universe    = xml_sender.second.get(L"universe", s.universe);
        s.host        = xml_sender.second.get(L"host", s.host);
        s.port        = xml_sender.second.get(L"port", s.port);

        s.fixtures = get_fixtures_ptree(xml_sender.second);
        senders.push_back(s);
    }

    return senders;
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels)
{
    configuration config;
    config.refreshRate = ptree.get(L"refresh-rate", config.refreshRate);

    if (config.refreshRate < 1)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Refresh rate must be at least 1"));

    config.senders = get_senders_ptree(ptree);
    return spl::make_shared<artnet_consumer>(config);
}
}} // namespace caspar::artnet
