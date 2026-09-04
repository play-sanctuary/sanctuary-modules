/*
 * mod-proximity-voice - link to the companion voice server
 *
 * Runs its own io_context on a dedicated thread so world updates never block on
 * the socket. Outbound records are queued and written asynchronously; inbound
 * records are parked in a queue and drained on the world thread, so nothing here
 * ever touches a Player.
 */

#ifndef MOD_PROXIMITY_VOICE_BRIDGE_H
#define MOD_PROXIMITY_VOICE_BRIDGE_H

#include "PVWire.h"
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/streambuf.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ProximityVoice
{
    class Bridge
    {
    public:
        Bridge(std::string host, uint16 port, std::string secret, uint32 reconnectMs);
        ~Bridge();

        Bridge(Bridge const&) = delete;
        Bridge& operator=(Bridge const&) = delete;

        void Start();
        void Stop();

        bool IsConnected() const { return _connected.load(std::memory_order_relaxed); }

        /// Thread-safe. Records sent while disconnected are dropped - the manager
        /// re-sends full state on reconnect, so a stale queue would only cause harm.
        void Send(std::string line);

        /// Moves everything received since the last call. Call from the world thread.
        std::vector<WireRecord> DrainInbound();

        /// True exactly once after each successful handshake, so the manager knows
        /// it must re-publish every live session.
        bool ConsumeResyncFlag();

    private:
        void DoResolve();
        void DoConnect(boost::asio::ip::tcp::resolver::results_type const& endpoints);
        void DoRead();
        void DoWrite();
        void ScheduleReconnect();
        void HandleFailure(std::string_view what, boost::system::error_code const& ec);

        std::string const _host;
        uint16 const _port;
        std::string const _secret;
        uint32 const _reconnectMs;

        boost::asio::io_context _io;
        boost::asio::strand<boost::asio::io_context::executor_type> _strand;
        boost::asio::ip::tcp::socket _socket;
        boost::asio::ip::tcp::resolver _resolver;
        boost::asio::steady_timer _reconnectTimer;
        boost::asio::streambuf _readBuffer;

        std::deque<std::string> _writeQueue;
        bool _writing = false;

        std::thread _thread;
        std::atomic<bool> _connected{false};
        std::atomic<bool> _stopping{false};
        std::atomic<bool> _resyncPending{false};
        /// Suppresses a log line per retry once we have already reported the outage.
        bool _outageLogged = false;

        std::mutex _inboundMutex;
        std::vector<WireRecord> _inbound;
    };
}

#endif // MOD_PROXIMITY_VOICE_BRIDGE_H
