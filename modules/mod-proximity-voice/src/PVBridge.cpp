/*
 * mod-proximity-voice - link to the companion voice server
 */

#include "PVBridge.h"
#include "Log.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <istream>

namespace ProximityVoice
{
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    Bridge::Bridge(std::string host, uint16 port, std::string secret, uint32 reconnectMs) :
        _host(std::move(host)), _port(port), _secret(std::move(secret)),
        _reconnectMs(reconnectMs ? reconnectMs : 5000),
        _strand(asio::make_strand(_io)), _socket(_strand), _resolver(_strand), _reconnectTimer(_strand)
    {
    }

    Bridge::~Bridge()
    {
        Stop();
    }

    void Bridge::Start()
    {
        if (_thread.joinable())
            return;

        _stopping.store(false, std::memory_order_relaxed);

        _thread = std::thread([this]()
        {
            auto guard = asio::make_work_guard(_io);
            asio::post(_strand, [this]() { DoResolve(); });

            try
            {
                _io.run();
            }
            catch (std::exception const& e)
            {
                LOG_ERROR("module.proximityvoice", "Voice bridge thread stopped: {}", e.what());
            }
        });
    }

    void Bridge::Stop()
    {
        if (!_thread.joinable())
            return;

        _stopping.store(true, std::memory_order_relaxed);

        asio::post(_strand, [this]()
        {
            boost::system::error_code ignored;
            _reconnectTimer.cancel();
            _resolver.cancel();
            _socket.close(ignored);
        });

        _io.stop();
        _thread.join();
        _connected.store(false, std::memory_order_relaxed);
    }

    void Bridge::Send(std::string line)
    {
        if (_stopping.load(std::memory_order_relaxed) || !_connected.load(std::memory_order_relaxed))
            return;

        asio::post(_strand, [this, line = std::move(line)]() mutable
        {
            if (!_socket.is_open())
                return;

            _writeQueue.push_back(std::move(line));

            if (!_writing)
                DoWrite();
        });
    }

    std::vector<WireRecord> Bridge::DrainInbound()
    {
        std::vector<WireRecord> out;
        std::lock_guard<std::mutex> lock(_inboundMutex);
        out.swap(_inbound);
        return out;
    }

    bool Bridge::ConsumeResyncFlag()
    {
        return _resyncPending.exchange(false, std::memory_order_relaxed);
    }

    void Bridge::DoResolve()
    {
        if (_stopping.load(std::memory_order_relaxed))
            return;

        _resolver.async_resolve(_host, std::to_string(_port),
            [this](boost::system::error_code const& ec, tcp::resolver::results_type endpoints)
            {
                if (ec)
                {
                    HandleFailure("resolve", ec);
                    ScheduleReconnect();
                    return;
                }

                DoConnect(endpoints);
            });
    }

    void Bridge::DoConnect(tcp::resolver::results_type const& endpoints)
    {
        asio::async_connect(_socket, endpoints,
            [this](boost::system::error_code const& ec, tcp::endpoint const&)
            {
                if (ec)
                {
                    HandleFailure("connect", ec);
                    ScheduleReconnect();
                    return;
                }

                boost::system::error_code option;
                _socket.set_option(tcp::no_delay(true), option);

                _connected.store(true, std::memory_order_relaxed);
                _outageLogged = false;
                LOG_INFO("module.proximityvoice", "Voice bridge connected to {}:{}", _host, _port);

                WireRecord hello("HELLO");
                hello.Set("ver", uint32(1));
                hello.Set("secret", _secret);
                _writeQueue.push_back(hello.Encode());

                // Everything the voice server knew is stale after a reconnect.
                _resyncPending.store(true, std::memory_order_relaxed);

                if (!_writing)
                    DoWrite();

                DoRead();
            });
    }

    void Bridge::DoRead()
    {
        asio::async_read_until(_socket, _readBuffer, '\n',
            [this](boost::system::error_code const& ec, std::size_t bytes)
            {
                if (ec)
                {
                    HandleFailure("read", ec);
                    ScheduleReconnect();
                    return;
                }

                std::string line(bytes, '\0');
                std::istream stream(&_readBuffer);
                stream.read(line.data(), std::streamsize(bytes));

                WireRecord record;
                if (WireRecord::Decode(line, record))
                {
                    std::lock_guard<std::mutex> lock(_inboundMutex);
                    // Guard against a runaway peer filling memory between world ticks.
                    if (_inbound.size() < 8192)
                        _inbound.push_back(std::move(record));
                }
                else
                {
                    LOG_DEBUG("module.proximityvoice", "Voice bridge dropped a malformed record");
                }

                DoRead();
            });
    }

    void Bridge::DoWrite()
    {
        if (_writeQueue.empty())
        {
            _writing = false;
            return;
        }

        _writing = true;

        asio::async_write(_socket, asio::buffer(_writeQueue.front()),
            [this](boost::system::error_code const& ec, std::size_t)
            {
                if (ec)
                {
                    _writing = false;
                    HandleFailure("write", ec);
                    ScheduleReconnect();
                    return;
                }

                _writeQueue.pop_front();
                DoWrite();
            });
    }

    void Bridge::ScheduleReconnect()
    {
        if (_stopping.load(std::memory_order_relaxed))
            return;

        _connected.store(false, std::memory_order_relaxed);

        boost::system::error_code ignored;
        _socket.close(ignored);
        _writeQueue.clear();
        _writing = false;
        _readBuffer.consume(_readBuffer.size());

        _reconnectTimer.expires_after(std::chrono::milliseconds(_reconnectMs));
        _reconnectTimer.async_wait([this](boost::system::error_code const& ec)
        {
            if (!ec)
                DoResolve();
        });
    }

    void Bridge::HandleFailure(std::string_view what, boost::system::error_code const& ec)
    {
        if (_stopping.load(std::memory_order_relaxed) || ec == asio::error::operation_aborted)
            return;

        // Only the first failure of an outage is worth a log line; retries are noise.
        if (!_outageLogged)
        {
            LOG_WARN("module.proximityvoice", "Voice bridge {} failed ({}), retrying every {} ms",
                what, ec.message(), _reconnectMs);
            _outageLogged = true;
        }
    }
}
