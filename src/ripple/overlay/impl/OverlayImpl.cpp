//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/basics/Log.h>
#include <ripple/basics/make_SSLContext.h>
#include <ripple/server/JsonWriter.h>
#include <ripple/overlay/impl/OverlayImpl.h>
#include <ripple/overlay/impl/PeerImp.h>
#include <ripple/overlay/impl/TMHello.h>
#include <ripple/peerfinder/make_Manager.h>
#include <beast/ByteOrder.h>
#include <beast/http/rfc2616.h>
#include <beast/utility/ci_char_traits.h>

namespace ripple {

/** A functor to visit all active peers and retrieve their JSON data */
struct get_peer_json
{
    typedef Json::Value return_type;

    Json::Value json;

    get_peer_json ()
    { }

    void operator() (Peer::ptr const& peer)
    {
        json.append (peer->json ());
    }

    Json::Value operator() ()
    {
        return json;
    }
};

//------------------------------------------------------------------------------

OverlayImpl::Child::Child (OverlayImpl& overlay)
    : overlay_(overlay)
{
}

OverlayImpl::Child::~Child()
{
    overlay_.remove(*this);
}

//------------------------------------------------------------------------------

OverlayImpl::Timer::Timer (OverlayImpl& overlay)
    : Child(overlay)
    , timer_(overlay_.io_service_)
{
}

void
OverlayImpl::Timer::stop()
{
    error_code ec;
    timer_.cancel(ec);
}

void
OverlayImpl::Timer::run()
{
    error_code ec;
    timer_.expires_from_now (std::chrono::seconds(1));
    timer_.async_wait(overlay_.strand_.wrap(
        std::bind(&Timer::on_timer, shared_from_this(),
            beast::asio::placeholders::error)));
}

void
OverlayImpl::Timer::on_timer (error_code ec)
{
    if (ec || overlay_.isStopping())
    {
        if (ec && ec != boost::asio::error::operation_aborted)
            if (overlay_.journal_.error) overlay_.journal_.error <<
                "on_timer: " << ec.message();
        return;
    }

    overlay_.m_peerFinder->once_per_second();
    overlay_.sendEndpoints();
    overlay_.autoConnect();

    timer_.expires_from_now (std::chrono::seconds(1));
    timer_.async_wait(overlay_.strand_.wrap(std::bind(
        &Timer::on_timer, shared_from_this(),
            beast::asio::placeholders::error)));
}

//------------------------------------------------------------------------------

OverlayImpl::OverlayImpl (
    Setup const& setup,
    Stoppable& parent,
    ServerHandler& serverHandler,
    Resource::Manager& resourceManager,
    beast::File const& pathToDbFileOrDirectory,
    Resolver& resolver,
    boost::asio::io_service& io_service)
    : Overlay (parent)
    , io_service_ (io_service)
    , work_ (boost::in_place(std::ref(io_service_)))
    , strand_ (io_service_)
    , setup_(setup)
    , journal_ (deprecatedLogs().journal("Overlay"))
    , serverHandler_(serverHandler)
    , m_resourceManager (resourceManager)
    , m_peerFinder (PeerFinder::make_Manager (*this, io_service,
        pathToDbFileOrDirectory, get_seconds_clock(),
            deprecatedLogs().journal("PeerFinder")))
    , m_resolver (resolver)
    , next_id_(1)
{
    beast::PropertyStream::Source::add (m_peerFinder.get());
}

OverlayImpl::~OverlayImpl ()
{
    stop();

    // Block until dependent objects have been destroyed.
    // This is just to catch improper use of the Stoppable API.
    //
    std::unique_lock <decltype(mutex_)> lock (mutex_);
    cond_.wait (lock, [this] { return list_.empty(); });
}

//------------------------------------------------------------------------------

void
OverlayImpl::onLegacyPeerHello (
    std::unique_ptr<beast::asio::ssl_bundle>&& ssl_bundle,
        boost::asio::const_buffer buffer, endpoint_type remote_endpoint)
{
    error_code ec;
    auto const local_endpoint (ssl_bundle->socket.local_endpoint(ec));
    if (ec)
        return;

    auto const slot = m_peerFinder->new_inbound_slot (
        beast::IPAddressConversion::from_asio(local_endpoint),
            beast::IPAddressConversion::from_asio(remote_endpoint));

    if (slot == nullptr)
        // self connect, close
        return;

    auto const peer = std::make_shared<PeerImp>(std::move(ssl_bundle),
        boost::asio::const_buffers_1(buffer), remote_endpoint, *this,
            m_resourceManager, *m_peerFinder, slot, next_id_++);
    {
        // As we are not on the strand, run() must be called
        // while holding the lock, otherwise new I/O can be
        // queued after a call to stop().
        std::lock_guard <decltype(mutex_)> lock (mutex_);
        add(peer);    
        peer->run();
    }
}

Handoff
OverlayImpl::onHandoff (std::unique_ptr <beast::asio::ssl_bundle>&& ssl_bundle,
    beast::http::message&& request,
        endpoint_type remote_endpoint)
{
    Handoff handoff;
    if (! isPeerUpgrade(request))
        return handoff;

    handoff.moved = true;

    if (journal_.trace) journal_.trace <<
        "Peer connection upgrade from " << remote_endpoint;

    error_code ec;
    auto const local_endpoint (ssl_bundle->socket.local_endpoint(ec));
    if (ec)
    {
        if (journal_.trace) journal_.trace <<
            "Peer " << remote_endpoint << " failed: " << ec.message();
        return handoff;
    }

    auto consumer = m_resourceManager.newInboundEndpoint(
        beast::IPAddressConversion::from_asio(remote_endpoint));
    if (consumer.disconnect())
        return handoff;

    auto const slot = m_peerFinder->new_inbound_slot (
        beast::IPAddressConversion::from_asio(local_endpoint),
            beast::IPAddressConversion::from_asio(remote_endpoint));

    if (slot == nullptr)
    {
        // self-connect, close
        handoff.moved = false;
        return handoff;
    }

    // TODO Validate HTTP request

    {
        auto const types = beast::rfc2616::split_commas(
            request.headers["Connect-As"]);
        if (std::find_if(types.begin(), types.end(),
                [](std::string const& s)
                {
                    return beast::ci_equal(s,
                        std::string("peer"));
                }) == types.end())
        {
            handoff.moved = false;
            handoff.response = makeRedirectResponse(slot, request,
                remote_endpoint.address());
            handoff.keep_alive = request.keep_alive();
            return handoff;
        }
    }

    handoff.moved = true;
    bool success = true;
    protocol::TMHello hello;
    std::tie(hello, success) = parseHello (request, journal_);
    if(! success)
        return handoff;
    uint256 sharedValue;
    std::tie(sharedValue, success) = makeSharedValue(
        ssl_bundle->stream.native_handle(), journal_);
    if(! success)
        return handoff;
    RippleAddress publicKey;
    std::tie(publicKey, success) = verifyHello (hello,
        sharedValue, journal_, getApp());
    if(! success)
        return handoff;

    std::string name;
    bool clusterNode = getApp().getUNL().nodeInCluster(
        publicKey, name);
    auto const result = m_peerFinder->activate (slot,
        RipplePublicKey(publicKey), clusterNode);

    if (result != PeerFinder::Result::success)
    {
        if (journal_.trace) journal_.trace <<
            "Peer " << remote_endpoint << " redirected, slots full";
        handoff.moved = false;
        handoff.response = makeRedirectResponse(slot, request,
            remote_endpoint.address());
        handoff.keep_alive = request.keep_alive();
        return handoff;
    }

    auto const peer = std::make_shared<PeerImp>(std::move(ssl_bundle),
        std::move(request), hello, remote_endpoint, publicKey, consumer,
            slot,  *this, m_resourceManager, *m_peerFinder, next_id_++);
    {
        // As we are not on the strand, run() must be called
        // while holding the lock, otherwise new I/O can be
        // queued after a call to stop().
        std::lock_guard <decltype(mutex_)> lock (mutex_);
        add(peer);
        peer->run();
    }
    handoff.moved = true;
    return handoff;
}

//------------------------------------------------------------------------------

bool
OverlayImpl::isPeerUpgrade(beast::http::message const& request)
{
    if (! request.upgrade())
        return false;
    auto const versions = parse_ProtocolVersions(
        request.headers["Upgrade"]);
    if (versions.size() == 0)
        return false;
    if (! request.request() && request.status() != 101)
        return false;
    return true;
}

std::shared_ptr<HTTP::Writer>
OverlayImpl::makeRedirectResponse (PeerFinder::Slot::ptr const& slot,
    beast::http::message const& request, address_type remote_address)
{
    Json::Value json(Json::objectValue);
    {
        auto const result = m_peerFinder->redirect(slot);
        Json::Value& ips = (json["peer-ips"] = Json::arrayValue);
        for (auto const& _ : m_peerFinder->redirect(slot))
            ips.append(_.address.to_string());
    }

    beast::http::message m;
    m.request(false);
    m.status(503);
    m.reason("Service Unavailable");
    m.headers.append("Remote-Address", remote_address.to_string());
    m.version(request.version());
    if (request.version() == std::make_pair(1, 0))
    {
        //?
    }
    auto const response = HTTP::make_JsonWriter (m, json);
    return response;
}

//------------------------------------------------------------------------------

void
OverlayImpl::connect (beast::IP::Endpoint const& remote_endpoint)
{
    assert(work_);

    PeerFinder::Slot::ptr const slot =
        m_peerFinder->new_outbound_slot (remote_endpoint);
    if (slot == nullptr)
        return;
    auto const peer = std::make_shared <PeerImp> (remote_endpoint,
        io_service_, *this, m_resourceManager, *m_peerFinder, slot,
            setup_.context, next_id_++);
    {
        // We're on the strand but lets make this code
        // the same as the others to avoid confusion.
        std::lock_guard <decltype(mutex_)> lock (mutex_);
        add(peer);
        peer->run();
    }
}

//--------------------------------------------------------------------------

void
OverlayImpl::remove (PeerFinder::Slot::ptr const& slot)
{
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    auto const iter = m_peers.find (slot);
    assert(iter != m_peers.end ());
    m_peers.erase (iter);
}

//--------------------------------------------------------------------------
//
// Stoppable
//
//--------------------------------------------------------------------------

// Caller must hold the mutex
void
OverlayImpl::checkStopped ()
{
    if (isStopping() && areChildrenStopped () && list_.empty())
        stopped();
}

void
OverlayImpl::onPrepare()
{
    PeerFinder::Config config;

    if (getConfig ().PEERS_MAX != 0)
        config.maxPeers = getConfig ().PEERS_MAX;

    config.outPeers = config.calcOutPeers();

    auto const port = serverHandler_.setup().overlay.port;

    config.wantIncoming =
        (! getConfig ().PEER_PRIVATE) && (port != 0);
    // if it's a private peer or we are running as standalone
    // automatic connections would defeat the purpose.
    config.autoConnect =
        !getConfig().RUN_STANDALONE &&
        !getConfig().PEER_PRIVATE;
    config.listeningPort = port;
    config.features = "";

    // Enforce business rules
    config.applyTuning();

    m_peerFinder->setConfig (config);

    auto bootstrapIps (getConfig ().IPS);

    // If no IPs are specified, use the Ripple Labs round robin
    // pool to get some servers to insert into the boot cache.
    if (bootstrapIps.empty ())
        bootstrapIps.push_back ("r.ripple.com 51235");

    if (!bootstrapIps.empty ())
    {
        m_resolver.resolve (bootstrapIps,
            [this](std::string const& name,
                std::vector <beast::IP::Endpoint> const& addresses)
            {
                std::vector <std::string> ips;
                ips.reserve(addresses.size());
                for (auto const& addr : addresses)
                    ips.push_back (to_string (addr));
                std::string const base ("config: ");
                if (!ips.empty ())
                    m_peerFinder->addFallbackStrings (base + name, ips);
            });
    }

    // Add the ips_fixed from the rippled.cfg file
    if (! getConfig ().RUN_STANDALONE && !getConfig ().IPS_FIXED.empty ())
    {
        m_resolver.resolve (getConfig ().IPS_FIXED,
            [this](
                std::string const& name,
                std::vector <beast::IP::Endpoint> const& addresses)
            {
                if (!addresses.empty ())
                    m_peerFinder->addFixedPeer (name, addresses);
            });
    }
}

void
OverlayImpl::onStart ()
{
    auto const timer = std::make_shared<Timer>(*this);
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    list_.emplace(timer.get(), timer);
    timer_ = timer;
    timer->run();
}

void
OverlayImpl::onStop ()
{
    strand_.dispatch(std::bind(&OverlayImpl::stop, this));
}

void
OverlayImpl::onChildrenStopped ()
{
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    checkStopped ();
}

//--------------------------------------------------------------------------
//
// PropertyStream
//
//--------------------------------------------------------------------------

void
OverlayImpl::onWrite (beast::PropertyStream::Map& stream)
{
}

//--------------------------------------------------------------------------
/** A peer has connected successfully
    This is called after the peer handshake has been completed and during
    peer activation. At this point, the peer address and the public key
    are known.
*/
void
OverlayImpl::activate (std::shared_ptr<PeerImp> const& peer)
{
    std::lock_guard <decltype(mutex_)> lock (mutex_);

    // Now track this peer
    {
        auto const result (m_shortIdMap.emplace (
            std::piecewise_construct,
                std::make_tuple (peer->id()),
                    std::make_tuple (peer)));
        assert(result.second);
        (void) result.second;
    }

    {
        auto const result (m_publicKeyMap.emplace(
            peer->getNodePublic(), peer));
        assert(result.second);
        (void) result.second;
    }

    journal_.debug <<
        "activated " << peer->getRemoteAddress() <<
        " (" << peer->id() <<
        ":" << RipplePublicKey(peer->getNodePublic()) << ")";

    // We just accepted this peer so we have non-zero active peers
    assert(size() != 0);
}

void
OverlayImpl::onPeerDeactivate (Peer::id_t id,
    RippleAddress const& publicKey)
{
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    m_shortIdMap.erase(id);
    m_publicKeyMap.erase(publicKey);
}

/** The number of active peers on the network
    Active peers are only those peers that have completed the handshake
    and are running the Ripple protocol.
*/
std::size_t
OverlayImpl::size()
{
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    return m_publicKeyMap.size ();
}

// Returns information on verified peers.
Json::Value
OverlayImpl::json ()
{
    return foreach (get_peer_json());
}

Overlay::PeerSequence
OverlayImpl::getActivePeers()
{
    Overlay::PeerSequence ret;
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    ret.reserve (m_publicKeyMap.size ());
    for (auto const& e : m_publicKeyMap)
    {
        auto const sp = e.second.lock();
        if (sp)
            ret.push_back(sp);
    }

    return ret;
}

Peer::ptr
OverlayImpl::findPeerByShortID (Peer::id_t const& id)
{
    std::lock_guard <decltype(mutex_)> lock (mutex_);
    auto const iter = m_shortIdMap.find (id);
    if (iter != m_shortIdMap.end ())
        return iter->second.lock();
    return Peer::ptr();
}

//------------------------------------------------------------------------------

void
OverlayImpl::add (std::shared_ptr<PeerImp> const& peer)
{
    {
        auto const result =
            m_peers.emplace (peer->slot(), peer);
        assert (result.second);
        (void) result.second;
    }
    list_.emplace(peer.get(), peer);
}

void
OverlayImpl::remove (Child& child)
{
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    list_.erase(&child);
    if (list_.empty())
        checkStopped();
}

void
OverlayImpl::stop()
{
    std::lock_guard<decltype(mutex_)> lock(mutex_);
    if (work_)
    {
        work_ = boost::none;
        for (auto& _ : list_)
        {
            auto const child = _.second.lock();
            // Happens when the child is about to be destroyed
            if (child != nullptr)
                child->stop();
        }
    }
}

void
OverlayImpl::autoConnect()
{
    auto const result = m_peerFinder->autoconnect();
    for (auto addr : result)
        connect (addr);
}

void
OverlayImpl::sendEndpoints()
{
    auto const result = m_peerFinder->buildEndpointsForPeers();
    for (auto const& e : result)
    {
        std::shared_ptr<PeerImp> peer;
        {
            std::lock_guard <decltype(mutex_)> lock (mutex_);
            auto const iter = m_peers.find (e.first);
            if (iter != m_peers.end())
                peer = iter->second.lock();
        }
        if (peer)
            peer->sendEndpoints (e.second.begin(), e.second.end());
    }
}


//------------------------------------------------------------------------------

Overlay::Setup
setup_Overlay (BasicConfig const& config)
{
    Overlay::Setup setup;
    auto const& section = config.section("overlay");
    set (setup.http_handshake, "http_handshake", section);
    set (setup.auto_connect, "auto_connect", section);
    std::string promote;
    set (promote, "become_superpeer", section);
    if (promote == "never")
        setup.promote = Overlay::Promote::never;
    else if (promote == "always")
        setup.promote = Overlay::Promote::always;
    else
        setup.promote = Overlay::Promote::automatic;
    setup.context = make_SSLContext();
    return setup;
}

std::unique_ptr <Overlay>
make_Overlay (
    Overlay::Setup const& setup,
    beast::Stoppable& parent,
    ServerHandler& serverHandler,
    Resource::Manager& resourceManager,
    beast::File const& pathToDbFileOrDirectory,
    Resolver& resolver,
    boost::asio::io_service& io_service)
{
    return std::make_unique <OverlayImpl> (setup, parent, serverHandler,
        resourceManager, pathToDbFileOrDirectory, resolver, io_service);
}

}
