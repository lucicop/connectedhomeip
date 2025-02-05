/*
 *
 *    Copyright (c) 2020-2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *  @file
 *    This file contains definitions for Device class. The objects of this
 *    class will be used by Controller applications to interact with CHIP
 *    devices. The class provides mechanism to construct, send and receive
 *    messages to and from the corresponding CHIP devices.
 */

#pragma once

#include <app/CASEClient.h>
#include <app/CASEClientPool.h>
#include <app/DeviceProxy.h>
#include <app/util/basic-types.h>
#include <credentials/GroupDataProvider.h>
#include <lib/address_resolve/AddressResolve.h>
#include <messaging/ExchangeContext.h>
#include <messaging/ExchangeDelegate.h>
#include <messaging/ExchangeMgr.h>
#include <messaging/Flags.h>
#include <protocols/secure_channel/CASESession.h>
#include <system/SystemLayer.h>
#include <transport/SessionManager.h>
#include <transport/TransportMgr.h>
#include <transport/raw/MessageHeader.h>
#include <transport/raw/UDP.h>

namespace chip {

struct DeviceProxyInitParams
{
    SessionManager * sessionManager                                    = nullptr;
    SessionResumptionStorage * sessionResumptionStorage                = nullptr;
    Credentials::CertificateValidityPolicy * certificateValidityPolicy = nullptr;
    Messaging::ExchangeManager * exchangeMgr                           = nullptr;
    FabricTable * fabricTable                                          = nullptr;
    CASEClientPoolDelegate * clientPool                                = nullptr;
    Credentials::GroupDataProvider * groupDataProvider                 = nullptr;

    Optional<ReliableMessageProtocolConfig> mrpLocalConfig = Optional<ReliableMessageProtocolConfig>::Missing();

    CHIP_ERROR Validate() const
    {
        ReturnErrorCodeIf(sessionManager == nullptr, CHIP_ERROR_INCORRECT_STATE);
        // sessionResumptionStorage can be nullptr when resumption is disabled
        ReturnErrorCodeIf(exchangeMgr == nullptr, CHIP_ERROR_INCORRECT_STATE);
        ReturnErrorCodeIf(fabricTable == nullptr, CHIP_ERROR_INCORRECT_STATE);
        ReturnErrorCodeIf(groupDataProvider == nullptr, CHIP_ERROR_INCORRECT_STATE);
        ReturnErrorCodeIf(clientPool == nullptr, CHIP_ERROR_INCORRECT_STATE);

        return CHIP_NO_ERROR;
    }
};

/**
 * @brief Delegate provided when creating OperationalSessionSetup.
 *
 * Once OperationalSessionSetup establishes a connection (or errors out) and has notified all
 * registered application callbacks via OnDeviceConnected/OnDeviceConnectionFailure, this delegate
 * is used to deallocate the OperationalSessionSetup.
 */
class OperationalSessionReleaseDelegate
{
public:
    virtual ~OperationalSessionReleaseDelegate() = default;
    // TODO Issue #20452: Once cleanup from #20452 takes place we can provide OperationalSessionSetup *
    // instead of ScopedNodeId here.
    virtual void ReleaseSession(const ScopedNodeId & peerId) = 0;
};

/**
 * @brief Minimal implementation of DeviceProxy that encapsulates a SessionHolder to track a CASE session.
 *
 * Deprecated - Avoid using this object.
 *
 * OperationalDeviceProxy is a minimal implementation of DeviceProxy. It is meant to provide a transition
 * for existing consumers of OperationalDeviceProxy that were delivered a reference to that object in
 * their respective OnDeviceConnected callback, but were incorrectly holding onto that object past
 * the function call. OperationalDeviceProxy can be held on for as long as is desired, while still
 * minimizing the code changes needed to transition to a more final solution by virtue of
 * implementing DeviceProxy.
 */
class OperationalDeviceProxy : public DeviceProxy
{
public:
    OperationalDeviceProxy(Messaging::ExchangeManager * exchangeMgr, const SessionHandle & sessionHandle) :
        mExchangeMgr(exchangeMgr), mSecureSession(sessionHandle), mPeerScopedNodeId(sessionHandle->GetPeer())
    {}
    OperationalDeviceProxy() {}

    // Recommended to use InteractionModelEngine::ShutdownSubscriptions directly.
    void ShutdownSubscriptions() override { VerifyOrDie(false); } // Currently not implemented.
    void Disconnect() override
    {
        if (IsSecureConnected())
        {
            GetSecureSession().Value()->AsSecureSession()->MarkAsDefunct();
        }
        mSecureSession.Release();
        mExchangeMgr      = nullptr;
        mPeerScopedNodeId = ScopedNodeId();
    }
    Messaging::ExchangeManager * GetExchangeManager() const override { return mExchangeMgr; }
    chip::Optional<SessionHandle> GetSecureSession() const override { return mSecureSession.Get(); }
    NodeId GetDeviceId() const override { return mPeerScopedNodeId.GetNodeId(); }
    ScopedNodeId GetPeerScopedNodeId() const { return mPeerScopedNodeId; }

    bool ConnectionReady() const { return (mExchangeMgr != nullptr && IsSecureConnected()); }

private:
    bool IsSecureConnected() const override { return static_cast<bool>(mSecureSession); }

    Messaging::ExchangeManager * mExchangeMgr = nullptr;
    SessionHolder mSecureSession;
    ScopedNodeId mPeerScopedNodeId;
};

/**
 * @brief Callback prototype when secure session is established.
 *
 * Callback implementations are not supposed to store the exchangeMgr or the sessionHandle. Older
 * application code does incorrectly hold onto this information so do not follow those incorrect
 * implementations as an example.
 */
// TODO: OnDeviceConnected should not return ExchangeManager. Application should have this already. This
// was provided initially to keep code churn down during a large refactor of OnDeviceConnected.
typedef void (*OnDeviceConnected)(void * context, Messaging::ExchangeManager & exchangeMgr, SessionHandle & sessionHandle);
typedef void (*OnDeviceConnectionFailure)(void * context, const ScopedNodeId & peerId, CHIP_ERROR error);

/**
 * Represents a connection path to a device that is in an operational state.
 *
 * Handles the lifetime of communicating with such a device:
 *    - Discover the device using DNSSD (find out what IP address to use and what
 *      communication parameters are appropriate for it)
 *    - Establish a secure channel to it via CASE
 *    - Expose to consumers the secure session for talking to the device.
 */
class DLL_EXPORT OperationalSessionSetup : public SessionDelegate,
                                           public SessionEstablishmentDelegate,
                                           public AddressResolve::NodeListener
{
public:
    ~OperationalSessionSetup() override;

    OperationalSessionSetup(DeviceProxyInitParams & params, ScopedNodeId peerId,
                            OperationalSessionReleaseDelegate * releaseDelegate) :
        mSecureSession(*this)
    {
        mInitParams = params;
        if (params.Validate() != CHIP_NO_ERROR || releaseDelegate == nullptr)
        {
            mState = State::Uninitialized;
            return;
        }

        mSystemLayer     = params.exchangeMgr->GetSessionManager()->SystemLayer();
        mPeerId          = peerId;
        mFabricTable     = params.fabricTable;
        mReleaseDelegate = releaseDelegate;
        mState           = State::NeedsAddress;
        mAddressLookupHandle.SetListener(this);
    }

    /*
     * This function can be called to establish a secure session with the device.
     *
     * The device is expected to have been commissioned, A CASE session
     * setup will be triggered.
     *
     * On establishing the session, the callback function `onConnection` will be called. If the
     * session setup fails, `onFailure` will be called.
     *
     * If the session already exists, `onConnection` will be called immediately,
     * before the Connect call returns.
     *
     * `onFailure` may be called before the Connect call returns, for error
     * cases that are detected synchronously (e.g. inability to start an address
     * lookup).
     */
    void Connect(Callback::Callback<OnDeviceConnected> * onConnection, Callback::Callback<OnDeviceConnectionFailure> * onFailure);

    bool IsConnected() const { return mState == State::SecureConnected; }

    bool IsConnecting() const { return mState == State::Connecting; }

    /**
     * IsResolvingAddress returns true if we are doing an address resolution
     * that needs to happen before we can establish CASE.  We can be in the
     * middle of doing address updates at other times too (e.g. when we are
     * IsConnected()), but those will not cause a true return from
     * IsResolvingAddress().
     */
    bool IsResolvingAddress() const { return mState == State::ResolvingAddress; }

    //////////// SessionEstablishmentDelegate Implementation ///////////////
    void OnSessionEstablished(const SessionHandle & session) override;
    void OnSessionEstablishmentError(CHIP_ERROR error) override;

    //////////// SessionDelegate Implementation ///////////////

    // Called when a connection is closing. The object releases all resources associated with the connection.
    void OnSessionReleased() override;
    // Called when a message is not acked within first retrans timer, try to refresh the peer address
    void OnFirstMessageDeliveryFailed() override;
    // Called when a connection is hanging. Try to re-establish another session, and shift to the new session when done, the
    // original session won't be touched during the period.
    void OnSessionHang() override;

    /**
     *  Mark any open session with the device as expired.
     */
    void Disconnect();

    ScopedNodeId GetPeerId() const { return mPeerId; }

    Transport::PeerAddress GetPeerAddress() const { return mDeviceAddress; }

    static Transport::PeerAddress ToPeerAddress(const Dnssd::ResolvedNodeData & nodeData)
    {
        Inet::InterfaceId interfaceId = Inet::InterfaceId::Null();

        // TODO - Revisit usage of InterfaceID only for addresses that are IPv6 LLA
        // Only use the DNS-SD resolution's InterfaceID for addresses that are IPv6 LLA.
        // For all other addresses, we should rely on the device's routing table to route messages sent.
        // Forcing messages down an InterfaceId might fail. For example, in bridged networks like Thread,
        // mDNS advertisements are not usually received on the same interface the peer is reachable on.
        if (nodeData.resolutionData.ipAddress[0].IsIPv6LinkLocal())
        {
            interfaceId = nodeData.resolutionData.interfaceId;
        }

        return Transport::PeerAddress::UDP(nodeData.resolutionData.ipAddress[0], nodeData.resolutionData.port, interfaceId);
    }

    /**
     * @brief Get the fabricIndex
     */
    FabricIndex GetFabricIndex() const { return mPeerId.GetFabricIndex(); }

    /**
     * Triggers a DNSSD lookup to find a usable peer address for this operational device.
     */
    CHIP_ERROR LookupPeerAddress();

    // AddressResolve::NodeListener - notifications when dnssd finds a node IP address
    void OnNodeAddressResolved(const PeerId & peerId, const AddressResolve::ResolveResult & result) override;
    void OnNodeAddressResolutionFailed(const PeerId & peerId, CHIP_ERROR reason) override;

private:
    enum class State
    {
        Uninitialized,    // Error state: OperationalSessionSetup is useless
        NeedsAddress,     // No address known, lookup not started yet.
        ResolvingAddress, // Address lookup in progress.
        HasAddress,       // Have an address, CASE handshake not started yet.
        Connecting,       // CASE handshake in progress.
        SecureConnected,  // CASE session established.
    };

    DeviceProxyInitParams mInitParams;
    FabricTable * mFabricTable = nullptr;
    System::Layer * mSystemLayer;

    // mCASEClient is only non-null if we are in State::Connecting or just
    // allocated it as part of an attempt to enter State::Connecting.
    CASEClient * mCASEClient = nullptr;

    ScopedNodeId mPeerId;

    Transport::PeerAddress mDeviceAddress = Transport::PeerAddress::UDP(Inet::IPAddress::Any);

    void MoveToState(State aTargetState);

    State mState = State::Uninitialized;

    SessionHolderWithDelegate mSecureSession;

    Callback::CallbackDeque mConnectionSuccess;
    Callback::CallbackDeque mConnectionFailure;

    OperationalSessionReleaseDelegate * mReleaseDelegate;

    /// This is used when a node address is required.
    chip::AddressResolve::NodeLookupHandle mAddressLookupHandle;

    ReliableMessageProtocolConfig mRemoteMRPConfig = GetDefaultMRPConfig();

    CHIP_ERROR EstablishConnection();

    /*
     * This checks to see if an existing CASE session exists to the peer within the SessionManager
     * and if one exists, to load that into mSecureSession.
     *
     * Returns true if a valid session was found, false otherwise.
     *
     */
    bool AttachToExistingSecureSession();

    void CleanupCASEClient();

    void EnqueueConnectionCallbacks(Callback::Callback<OnDeviceConnected> * onConnection,
                                    Callback::Callback<OnDeviceConnectionFailure> * onFailure);

    /*
     * This dequeues all failure and success callbacks and appropriately
     * invokes either set depending on the value of error.
     *
     * If error == CHIP_NO_ERROR, only success callbacks are invoked.
     * Otherwise, only failure callbacks are invoked.
     *
     */
    void DequeueConnectionCallbacks(CHIP_ERROR error);

    /**
     * This function will set new IP address, port and MRP retransmission intervals of the device.
     */
    void UpdateDeviceData(const Transport::PeerAddress & addr, const ReliableMessageProtocolConfig & config);
};

} // namespace chip
