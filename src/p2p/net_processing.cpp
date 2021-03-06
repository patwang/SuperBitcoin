// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Super Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "net_processing.h"

#include "p2p/addrman.h"
#include "utils/arith_uint256.h"
#include "block/blockencodings.h"
#include "config/argmanager.h"
#include "config/chainparams.h"
#include "chaincontrol/validation.h"
#include "hash.h"
#include "block/validation.h"
#include "merkleblock.h"
#include "net.h"
#include "netmessagemaker.h"
#include "netbase.h"
#include "wallet/fees.h"
#include "sbtccore/transaction/policy.h"
#include "block/block.h"
#include "transaction/transaction.h"
#include "random.h"
#include "reverse_iterator.h"
#include "framework/scheduler.h"
#include "tinyformat.h"
#include "framework/ui_interface.h"
#include "utils/util.h"
#include "utils/utilmoneystr.h"
#include "utils/utilstrencodings.h"
#include "framework/validationinterface.h"
#include "mempool/txmempool.h"
#include "interface/imempoolcomponent.h"
#include "interface/exchangeformat.h"
#include "interface/ichaincomponent.h"
#include "chaincontrol/checkpoints.h"
#include "chaincontrol/blockfilemanager.h"

#if defined(NDEBUG)
# error "Super Bitcoin cannot be compiled without assertions."
#endif

#define UNUSED(x) ((void)(x))

SET_CPP_SCOPED_LOG_CATEGORY(CID_P2P_NET);

std::atomic<int64_t> nTimeBestReceived(0); // Used only to inform the wallet of when we last received a block

static const uint64_t RANDOMIZER_ID_ADDRESS_RELAY = 0x3cac0035b5866b90ULL; // SHA256("main address relay")[0:8]

// Internal stuff
namespace
{
    /** Number of nodes with fSyncStarted. */
    int nSyncStarted = 0;

    /**
     * Sources of received blocks, saved to be able to send them reject
     * messages or ban them when processing happens afterwards. Protected by
     * cs_main.
     * Set mapBlockSource[hash].second to false if the node should not be
     * punished if the block is invalid.
     */
    std::map<uint256, std::pair<NodeId, bool>> mapBlockSource;

    /** Blocks that are in flight, and that are in the queue to be downloaded. Protected by cs_main. */
    struct QueuedBlock
    {
        uint256 hash;
        const CBlockIndex *pindex;                               //!< Optional.
        bool fValidatedHeaders;                                  //!< Whether this block has validated headers at the time of request.
        std::unique_ptr<PartiallyDownloadedBlock> partialBlock;  //!< Optional, used for CMPCTBLOCK downloads
    };
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> > mapBlocksInFlight;

    /** Stack of nodes which we have set to announce using compact blocks */
    std::list<NodeId> lNodesAnnouncingHeaderAndIDs;

    /** Number of preferable block download peers. */
    int nPreferredDownload = 0;

    /** Number of peers from which we're downloading blocks. */
    int nPeersWithValidatedDownloads = 0;

    /** Number of outbound peers with m_chain_sync.m_protect. */
    int g_outbound_peers_with_protect_from_disconnect = 0;

    /** When our tip was last updated. */
    int64_t g_last_tip_update = 0;

    //    /** Relay map, protected by cs_main. */
    //    typedef std::map<uint256, CTransactionRef> MapRelay;
    //    MapRelay mapRelay;
    //    /** Expiration-time ordered list of (expire time, relay map entry) pairs, protected by cs_main). */
    //    std::deque<std::pair<int64_t, MapRelay::iterator>> vRelayExpiration;
} // namespace

namespace
{

    struct CBlockReject
    {
        unsigned char chRejectCode;
        std::string strRejectReason;
        uint256 hashBlock;
    };

    /**
     * Maintain validation-specific state about nodes, protected by cs_main, instead
     * by CNode's own locks. This simplifies asynchronous operation, where
     * processing of incoming data is done after the ProcessMessage call returns,
     * and we're no longer holding the node's locks.
     */
    struct CNodeState
    {
        //! The peer's address
        const CService address;
        //! Whether we have a fully established connection.
        bool fCurrentlyConnected;
        //! Accumulated misbehaviour score for this peer.
        int nMisbehavior;
        //! Whether this peer should be disconnected and banned (unless whitelisted).
        bool fShouldBan;
        //! String name of this peer (debugging/logging purposes).
        const std::string name;
        //! List of asynchronously-determined block rejections to notify this peer about.
        std::vector<CBlockReject> rejects;
        //! The best known block we know this peer has announced.
        const CBlockIndex *pindexBestKnownBlock;
        //! The hash of the last unknown block this peer has announced.
        uint256 hashLastUnknownBlock;
        //! The last full block we both have.
        const CBlockIndex *pindexLastCommonBlock;
        //! The best header we have sent our peer.
        const CBlockIndex *pindexBestHeaderSent;
        //! Length of current-streak of unconnecting headers announcements
        int nUnconnectingHeaders;
        //! Whether we've started headers synchronization with this peer.
        bool fSyncStarted;
        //! When to potentially disconnect peer for stalling headers download
        int64_t nHeadersSyncTimeout;
        //! Since when we're stalling block download progress (in microseconds), or 0.
        int64_t nStallingSince;
        std::list<QueuedBlock> vBlocksInFlight;
        //! When the first entry in vBlocksInFlight started downloading. Don't care when vBlocksInFlight is empty.
        int64_t nDownloadingSince;
        int nBlocksInFlight;
        int nBlocksInFlightValidHeaders;
        //! Whether we consider this a preferred download peer.
        bool fPreferredDownload;
        //! Whether this peer wants invs or headers (when possible) for block announcements.
        bool fPreferHeaders;
        //! Whether this peer wants invs or cmpctblocks (when possible) for block announcements.
        bool fPreferHeaderAndIDs;
        /**
          * Whether this peer will send us cmpctblocks if we request them.
          * This is not used to gate request logic, as we really only care about fSupportsDesiredCmpctVersion,
          * but is used as a flag to "lock in" the version of compact blocks (fWantsCmpctWitness) we send.
          */
        bool fProvidesHeaderAndIDs;
        //! Whether this peer can give us witnesses
        bool fHaveWitness;
        //! Whether this peer wants witnesses in cmpctblocks/blocktxns
        bool fWantsCmpctWitness;
        /**
         * If we've announced NODE_WITNESS to this peer: whether the peer sends witnesses in cmpctblocks/blocktxns,
         * otherwise: whether this peer sends non-witnesses in cmpctblocks/blocktxns.
         */
        bool fSupportsDesiredCmpctVersion;

        /** State used to enforce CHAIN_SYNC_TIMEOUT
          * Only in effect for outbound, non-manual connections, with
          * m_protect == false
          * Algorithm: if a peer's best known block has less work than our tip,
          * set a timeout CHAIN_SYNC_TIMEOUT seconds in the future:
          *   - If at timeout their best known block now has more work than our tip
          *     when the timeout was set, then either reset the timeout or clear it
          *     (after comparing against our current tip's work)
          *   - If at timeout their best known block still has less work than our
          *     tip did when the timeout was set, then send a getheaders message,
          *     and set a shorter timeout, HEADERS_RESPONSE_TIME seconds in future.
          *     If their best known block is still behind when that new timeout is
          *     reached, disconnect.
          */
        struct ChainSyncTimeoutState
        {
            //! A timeout used for checking whether our peer has sufficiently synced
            int64_t m_timeout;
            //! A header with the work we require on our peer's chain
            const CBlockIndex *m_work_header;
            //! After timeout is reached, set to true after sending getheaders
            bool m_sent_getheaders;
            //! Whether this peer is protected from disconnection due to a bad/slow chain
            bool m_protect;
        };

        ChainSyncTimeoutState m_chain_sync;

        //! Time of last new block announcement
        int64_t m_last_block_announcement;

        CNodeState(CAddress addrIn, std::string addrNameIn) : address(addrIn), name(addrNameIn)
        {
            fCurrentlyConnected = false;
            nMisbehavior = 0;
            fShouldBan = false;
            pindexBestKnownBlock = nullptr;
            hashLastUnknownBlock.SetNull();
            pindexLastCommonBlock = nullptr;
            pindexBestHeaderSent = nullptr;
            nUnconnectingHeaders = 0;
            fSyncStarted = false;
            nHeadersSyncTimeout = 0;
            nStallingSince = 0;
            nDownloadingSince = 0;
            nBlocksInFlight = 0;
            nBlocksInFlightValidHeaders = 0;
            fPreferredDownload = false;
            fPreferHeaders = false;
            fPreferHeaderAndIDs = false;
            fProvidesHeaderAndIDs = false;
            fHaveWitness = false;
            fWantsCmpctWitness = false;
            fSupportsDesiredCmpctVersion = false;
            m_chain_sync = {0, nullptr, false, false};
            m_last_block_announcement = 0;
        }
    };

    /** Map maintaining per-node state. Requires cs_main. */
    std::map<NodeId, CNodeState> mapNodeState;

    // Requires cs_main.
    CNodeState *State(NodeId pnode)
    {
        std::map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
        if (it == mapNodeState.end())
            return nullptr;
        return &it->second;
    }

    void UpdatePreferredDownload(CNode *node, CNodeState *state)
    {
        nPreferredDownload -= state->fPreferredDownload;

        // Whether this node should be marked as a preferred download node.
        state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

        nPreferredDownload += state->fPreferredDownload;
    }

    void PushNodeVersion(CNode *pnode, CConnman *connman, int64_t nTime)
    {
        ServiceFlags nLocalNodeServices = pnode->GetLocalServices();
        uint64_t nonce = pnode->GetLocalNonce();
        int nNodeStartingHeight = pnode->GetMyStartingHeight();
        NodeId nodeid = pnode->GetId();
        CAddress addr = pnode->addr;

        CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService(), addr.nServices));
        CAddress addrMe = CAddress(CService(), nLocalNodeServices);

        connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::VERSION, PROTOCOL_VERSION,
                                                                          (uint64_t)nLocalNodeServices, nTime, addrYou,
                                                                          addrMe,
                                                                          nonce, strSubVersion, nNodeStartingHeight,
                                                                          ::fRelayTxes));

        NLogFormat("send version message: version %d, blocks=%d, us=%s, them=%s, peer=%d",
                   PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(), addrYou.ToString(), nodeid);
    }

    // Requires cs_main.
    // Returns a bool indicating whether we requested this block.
    // Also used if a block was /not/ received and timed out or started with another peer
    bool MarkBlockAsReceived(const uint256 &hash)
    {
        std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(
                hash);
        if (itInFlight != mapBlocksInFlight.end())
        {
            CNodeState *state = State(itInFlight->second.first);
            state->nBlocksInFlightValidHeaders -= itInFlight->second.second->fValidatedHeaders;
            if (state->nBlocksInFlightValidHeaders == 0 && itInFlight->second.second->fValidatedHeaders)
            {
                // Last validated block on the queue was received.
                nPeersWithValidatedDownloads--;
            }
            if (state->vBlocksInFlight.begin() == itInFlight->second.second)
            {
                // First block on the queue was received, update the start download time for the next one
                state->nDownloadingSince = std::max(state->nDownloadingSince, GetTimeMicros());
            }
            state->vBlocksInFlight.erase(itInFlight->second.second);
            state->nBlocksInFlight--;
            state->nStallingSince = 0;
            mapBlocksInFlight.erase(itInFlight);
            return true;
        }
        return false;
    }

    // Requires cs_main.
    // returns false, still setting pit, if the block was already in flight from the same peer
    // pit will only be valid as long as the same cs_main lock is being held
    bool MarkBlockAsInFlight(NodeId nodeid, const uint256 &hash, const CBlockIndex *pindex = nullptr,
                             std::list<QueuedBlock>::iterator **pit = nullptr)
    {
        CNodeState *state = State(nodeid);
        assert(state != nullptr);

        // Short-circuit most stuff in case its from the same node
        std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(
                hash);
        if (itInFlight != mapBlocksInFlight.end() && itInFlight->second.first == nodeid)
        {
            if (pit)
            {
                *pit = &itInFlight->second.second;
            }
            return false;
        }

        // Make sure it's not listed somewhere already.
        MarkBlockAsReceived(hash);

        GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
        CTxMemPool &mempool = ifTxMempoolObj->GetMemPool();
        std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(),
                                                                            {hash, pindex, pindex != nullptr,
                                                                             std::unique_ptr<PartiallyDownloadedBlock>(
                                                                                     pit ? new PartiallyDownloadedBlock(
                                                                                             &mempool) : nullptr)});
        state->nBlocksInFlight++;
        state->nBlocksInFlightValidHeaders += it->fValidatedHeaders;
        if (state->nBlocksInFlight == 1)
        {
            // We're starting a block download (batch) from this peer.
            state->nDownloadingSince = GetTimeMicros();
        }
        if (state->nBlocksInFlightValidHeaders == 1 && pindex != nullptr)
        {
            nPeersWithValidatedDownloads++;
        }
        itInFlight = mapBlocksInFlight.insert(std::make_pair(hash, std::make_pair(nodeid, it))).first;
        if (pit)
            *pit = &itInFlight->second.second;
        return true;
    }

    /** Check whether the last unknown block a peer advertised is not yet known. */
    void ProcessBlockAvailability(NodeId nodeid)
    {
        CNodeState *state = State(nodeid);
        assert(state != nullptr);

        if (!state->hashLastUnknownBlock.IsNull())
        {
            GET_CHAIN_INTERFACE(ifChainObj);
            CBlockIndex *pIndexOld = ifChainObj->GetBlockIndex(state->hashLastUnknownBlock);
            if ((pIndexOld != nullptr) && (pIndexOld->nChainWork > 0))
            {
                if (state->pindexBestKnownBlock == nullptr ||
                    pIndexOld->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                    state->pindexBestKnownBlock = pIndexOld;
                state->hashLastUnknownBlock.SetNull();
            }
        }
    }

    /** Update tracking information about which blocks a peer is assumed to have. */
    void UpdateBlockAvailability(NodeId nodeid, const uint256 &hash)
    {
        CNodeState *state = State(nodeid);
        assert(state != nullptr);

        ProcessBlockAvailability(nodeid);

        GET_CHAIN_INTERFACE(ifChainObj);
        CBlockIndex *pIndex = ifChainObj->GetBlockIndex(hash);
        if ((pIndex != nullptr) && (pIndex->nChainWork > 0))
        {
            // An actually better block was announced.
            if (state->pindexBestKnownBlock == nullptr ||
                pIndex->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = pIndex;
        } else
        {
            // An unknown block was announced; just assume that the latest one is the best one.
            state->hashLastUnknownBlock = hash;
        }
    }

    void MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid, CConnman *connman)
    {
        AssertLockHeld(cs_main);
        CNodeState *nodestate = State(nodeid);
        if (!nodestate || !nodestate->fSupportsDesiredCmpctVersion)
        {
            // Never ask from peers who can't provide witnesses.
            return;
        }
        if (nodestate->fProvidesHeaderAndIDs)
        {
            for (std::list<NodeId>::iterator it = lNodesAnnouncingHeaderAndIDs.begin();
                 it != lNodesAnnouncingHeaderAndIDs.end(); it++)
            {
                if (*it == nodeid)
                {
                    lNodesAnnouncingHeaderAndIDs.erase(it);
                    lNodesAnnouncingHeaderAndIDs.push_back(nodeid);
                    return;
                }
            }
            connman->ForNode(nodeid, [connman](CNode *pfrom)
            {
                bool fAnnounceUsingCMPCTBLOCK = false;
                uint64_t nCMPCTBLOCKVersion = (pfrom->GetLocalServices() & NODE_WITNESS) ? 2 : 1;
                if (lNodesAnnouncingHeaderAndIDs.size() >= 3)
                {
                    // As per BIP152, we only get 3 of our peers to announce
                    // blocks using compact encodings.
                    connman->ForNode(lNodesAnnouncingHeaderAndIDs.front(),
                                     [connman, fAnnounceUsingCMPCTBLOCK, nCMPCTBLOCKVersion](CNode *pnodeStop)
                                     {
                                         connman->PushMessage(pnodeStop, CNetMsgMaker(pnodeStop->GetSendVersion()).Make(
                                                 NetMsgType::SENDCMPCT, fAnnounceUsingCMPCTBLOCK, nCMPCTBLOCKVersion));
                                         return true;
                                     });
                    lNodesAnnouncingHeaderAndIDs.pop_front();
                }
                fAnnounceUsingCMPCTBLOCK = true;
                connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SENDCMPCT,
                                                                                       fAnnounceUsingCMPCTBLOCK,
                                                                                       nCMPCTBLOCKVersion));
                lNodesAnnouncingHeaderAndIDs.push_back(pfrom->GetId());
                return true;
            });
        }
    }

    bool TipMayBeStale(const Consensus::Params &consensusParams)
    {
        AssertLockHeld(cs_main);
        if (g_last_tip_update == 0)
        {
            g_last_tip_update = GetTime();
        }
        return g_last_tip_update < GetTime() - consensusParams.nPowTargetSpacing * 3 && mapBlocksInFlight.empty();
    }

    // Requires cs_main
    bool CanDirectFetch(const Consensus::Params &consensusParams)
    {
        GET_CHAIN_INTERFACE(ifChainObj);
        CChain &chainActive = ifChainObj->GetActiveChain();
        return chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - consensusParams.nPowTargetSpacing * 20;
    }

    // Requires cs_main
    bool PeerHasHeader(CNodeState *state, const CBlockIndex *pindex)
    {
        if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
            return true;
        if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
            return true;
        return false;
    }

    /** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
     *  at most count entries. */
    void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<const CBlockIndex *> &vBlocks,
                                  NodeId &nodeStaller, const Consensus::Params &consensusParams)
    {
        if (count == 0)
            return;

        vBlocks.reserve(vBlocks.size() + count);
        CNodeState *state = State(nodeid);
        assert(state != nullptr);

        // Make sure pindexBestKnownBlock is up to date, we'll need it.
        ProcessBlockAvailability(nodeid);

        GET_CHAIN_INTERFACE(ifChainObj);
        CChain &chainActive = ifChainObj->GetActiveChain();

        if (state->pindexBestKnownBlock == nullptr ||
            state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork ||
            state->pindexBestKnownBlock->nChainWork < nMinimumChainWork)
        {
            // This peer has nothing interesting.
            return;
        }

        if (state->pindexLastCommonBlock == nullptr)
        {
            // Bootstrap quickly by guessing a parent of our best tip is the forking point.
            // Guessing wrong in either direction is not a problem.
            state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight,
                                                                chainActive.Height())];
        }

        // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
        // of its current tip anymore. Go back enough to fix that.
        state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
        if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
            return;

        std::vector<const CBlockIndex *> vToFetch;
        const CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
        // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
        // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
        // download that next block if the window were 1 larger.
        int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
        int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
        NodeId waitingfor = -1;
        while (pindexWalk->nHeight < nMaxHeight)
        {
            // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
            // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
            // as iterating over ~100 CBlockIndex* entries anyway.
            int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
            vToFetch.resize(nToFetch);
            pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
            vToFetch[nToFetch - 1] = pindexWalk;
            for (unsigned int i = nToFetch - 1; i > 0; i--)
            {
                vToFetch[i - 1] = vToFetch[i]->pprev;
            }

            // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
            // are not yet downloaded and not in flight to vBlocks. In the mean time, update
            // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
            // already part of our chain (and therefore don't need it even if pruned).
            for (const CBlockIndex *pindex : vToFetch)
            {
                if (!pindex->IsValid(BLOCK_VALID_TREE))
                {
                    // We consider the chain that this peer is on invalid.
                    return;
                }
                if (!State(nodeid)->fHaveWitness && IsWitnessEnabled(pindex->pprev, consensusParams))
                {
                    // We wouldn't download this block or its descendants from this peer.
                    return;
                }
                if (pindex->nStatus & BLOCK_HAVE_DATA || chainActive.Contains(pindex))
                {
                    if (pindex->nChainTx)
                        state->pindexLastCommonBlock = pindex;
                } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0)
                {
                    // The block is not already downloaded, and not yet in flight.
                    if (pindex->nHeight > nWindowEnd)
                    {
                        // We reached the end of the window.
                        if (vBlocks.size() == 0 && waitingfor != nodeid)
                        {
                            // We aren't able to fetch anything, but we would be if the download window was one larger.
                            nodeStaller = waitingfor;
                        }
                        return;
                    }
                    vBlocks.push_back(pindex);
                    if (vBlocks.size() == count)
                    {
                        return;
                    }
                } else if (waitingfor == -1)
                {
                    // This is the first already-in-flight block.
                    waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
                }
            }
        }
    }

} // namespace

// This function is used for testing the stale tip eviction logic, see
// DoS_tests.cpp
void UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds)
{
    LOCK(cs_main);
    CNodeState *state = State(node);
    if (state)
        state->m_last_block_announcement = time_in_seconds;
}

// Returns true for outbound peers, excluding manual connections, feelers, and
// one-shots
bool IsOutboundDisconnectionCandidate(const CNode *node)
{
    return !(node->fInbound || node->m_manual_connection || node->fFeeler || node->fOneShot);
}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats)
{
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    if (state == nullptr)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    for (const QueuedBlock &queue : state->vBlocksInFlight)
    {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    CNodeState *state = State(pnode);
    if (state == nullptr)
        return;

    state->nMisbehavior += howmuch;
    int banscore = Args().GetArg<int>("-banscore", DEFAULT_BANSCORE_THRESHOLD);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore)
    {
        WLogFormat("%s: %s peer=%d (%d -> %d) BAN THRESHOLD EXCEEDED", __func__, state->name, pnode,
                   state->nMisbehavior - howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        NLogFormat("%s: %s peer=%d (%d -> %d)", __func__, state->name, pnode, state->nMisbehavior - howmuch,
                   state->nMisbehavior);
}

void UpdateNodeBlockAvailability(int64_t nodeid, uint256 hash)
{
    UpdateBlockAvailability(nodeid, hash);
}

int GetInFlightBlockCount()
{
    return (int)mapBlocksInFlight.size();
}

bool DoseBlockInFlight(uint256 hash)
{
    return mapBlocksInFlight.count(hash) > 0;
}

bool MarkNodeBlockInFlight(int64_t nodeid, uint256 hash, const CBlockIndex *pindex)
{
    return MarkBlockAsInFlight(nodeid, hash, pindex);
}

static uint32_t GetFetchFlags(CNode *pfrom)
{
    uint32_t nFetchFlags = 0;
    if ((pfrom->GetLocalServices() & NODE_WITNESS) && State(pfrom->GetId())->fHaveWitness)
    {
        nFetchFlags |= MSG_WITNESS_FLAG;
    }
    return nFetchFlags;
}

bool static AlreadyHave(const CInv &inv) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    switch (inv.type)
    {
        case MSG_TX:
        case MSG_WITNESS_TX:
        {
            GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
            return ifTxMempoolObj->DoesTxExist(inv.hash);
        }
        case MSG_BLOCK:
        case MSG_WITNESS_BLOCK:
        {
            GET_CHAIN_INTERFACE(ifChainObj);
            return ifChainObj->DoesBlockExist(inv.hash);
        }

    }
    // Don't know what it is, just say we already got one
    return true;
}

static void RelayAddress(const CAddress &addr, bool fReachable, CConnman *connman)
{
    unsigned int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)

    // Relay to a limited number of other nodes
    // Use deterministic randomness to send to the same nodes for 24 hours
    // at a time so the addrKnowns of the chosen nodes prevent repeats
    uint64_t hashAddr = addr.GetHash();
    const CSipHasher hasher = connman->GetDeterministicRandomizer(RANDOMIZER_ID_ADDRESS_RELAY).Write(
            hashAddr << 32).Write((GetTime() + hashAddr) / (24 * 60 * 60));
    FastRandomContext insecure_rand;

    std::array<std::pair<uint64_t, CNode *>, 2> best{{{0, nullptr}, {0, nullptr}}};
    assert(nRelayNodes <= best.size());

    auto sortfunc = [&best, &hasher, nRelayNodes](CNode *pnode)
    {
        if (pnode->nVersion >= CADDR_TIME_VERSION)
        {
            uint64_t hashKey = CSipHasher(hasher).Write(pnode->GetId()).Finalize();
            for (unsigned int i = 0; i < nRelayNodes; i++)
            {
                if (hashKey > best[i].first)
                {
                    std::copy(best.begin() + i, best.begin() + nRelayNodes - 1, best.begin() + i + 1);
                    best[i] = std::make_pair(hashKey, pnode);
                    break;
                }
            }
        }
    };

    auto pushfunc = [&addr, &best, nRelayNodes, &insecure_rand]
    {
        for (unsigned int i = 0; i < nRelayNodes && best[i].first != 0; i++)
        {
            best[i].second->PushAddress(addr, insecure_rand);
        }
    };

    connman->ForEachNodeThen(std::move(sortfunc), std::move(pushfunc));
}

static bool ProcessHeadersMessage(CNode *pfrom, CConnman *connman, const std::vector<CBlockHeader> &headers,
                                  const CChainParams &chainparams, bool punish_duplicate_invalid)
{
    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    size_t nCount = headers.size();

    if (nCount == 0)
    {
        // Nothing interesting. Stop asking this peers for more headers.
        return true;
    }

    GET_CHAIN_INTERFACE(ifChainObj);
    CChain &chainActive = ifChainObj->GetActiveChain();

    bool received_new_header = false;
    const CBlockIndex *pindexLast = nullptr;
    {
        LOCK(cs_main);
        CNodeState *nodestate = State(pfrom->GetId());

        // If this looks like it could be a block announcement (nCount <
        // MAX_BLOCKS_TO_ANNOUNCE), use special logic for handling headers that
        // don't connect:
        // - Send a getheaders message in response to try to connect the chain.
        // - The peer can send up to MAX_UNCONNECTING_HEADERS in a row that
        //   don't connect before giving DoS points
        // - Once a headers message is received that is valid and does connect,
        //   nUnconnectingHeaders gets reset back to 0.
        if (!ifChainObj->DoesBlockExist(headers[0].hashPrevBlock) && nCount < MAX_BLOCKS_TO_ANNOUNCE)
        {
            nodestate->nUnconnectingHeaders++;
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETHEADERS,
                                                      chainActive.GetLocator(ifChainObj->GetIndexBestHeader()),
                                                      uint256()));
            NLogFormat(
                    "received header %s: missing prev block %s, sending getheaders (%d) to end (peer=%d, nUnconnectingHeaders=%d)",
                    headers[0].GetHash().ToString(),
                    headers[0].hashPrevBlock.ToString(),
                    ifChainObj->GetIndexBestHeader()->nHeight,
                    pfrom->GetId(), nodestate->nUnconnectingHeaders);
            // Set hashLastUnknownBlock for this peer, so that if we
            // eventually get the headers - even from a different peer -
            // we can use this peer to download.
            UpdateBlockAvailability(pfrom->GetId(), headers.back().GetHash());

            if (nodestate->nUnconnectingHeaders % MAX_UNCONNECTING_HEADERS == 0)
            {
                Misbehaving(pfrom->GetId(), 20);
            }
            return true;
        }

        uint256 hashLastBlock;
        for (const CBlockHeader &header : headers)
        {
            if (!hashLastBlock.IsNull() && header.hashPrevBlock != hashLastBlock)
            {
                Misbehaving(pfrom->GetId(), 20);
                return rLogError("non-continuous headers sequence");
            }
            hashLastBlock = header.GetHash();
        }

        // If we don't have the last header, then they'll have given us
        // something new (if these headers are valid).
        if (!ifChainObj->DoesBlockExist(hashLastBlock))
        {
            received_new_header = true;
        }
    }

    CValidationState state;
    CBlockHeader first_invalid_header;
    if (!ifChainObj->ProcessNewBlockHeaders(headers, state, chainparams, &pindexLast, &first_invalid_header))
    {
        int nDoS;
        if (state.IsInvalid(nDoS))
        {
            LOCK(cs_main);
            if (nDoS > 0)
            {
                Misbehaving(pfrom->GetId(), nDoS);
            }
            if (punish_duplicate_invalid && ifChainObj->DoesBlockExist(first_invalid_header.GetHash()))
            {
                // Goal: don't allow outbound peers to use up our outbound
                // connection slots if they are on incompatible chains.
                //
                // We ask the caller to set punish_invalid appropriately based
                // on the peer and the method of header delivery (compact
                // blocks are allowed to be invalid in some circumstances,
                // under BIP 152).
                // Here, we try to detect the narrow situation that we have a
                // valid block header (ie it was valid at the time the header
                // was received, and hence stored in mapBlockIndex) but know the
                // block is invalid, and that a peer has announced that same
                // block as being on its active chain.
                // Disconnect the peer in such a situation.
                //
                // Note: if the header that is invalid was not accepted to our
                // mapBlockIndex at all, that may also be grounds for
                // disconnecting the peer, as the chain they are on is likely
                // to be incompatible. However, there is a circumstance where
                // that does not hold: if the header's timestamp is more than
                // 2 hours ahead of our current time. In that case, the header
                // may become valid in the future, and we don't want to
                // disconnect a peer merely for serving us one too-far-ahead
                // block header, to prevent an attacker from splitting the
                // network by mining a block right at the 2 hour boundary.
                //
                // update the DoS logic (or, rather, rewrite the
                // DoS-interface between validation and net_processing) so that
                // the interface is cleaner, and so that we disconnect on all the
                // reasons that a peer's headers chain is incompatible
                // with ours (eg block->nVersion softforks, MTP violations,
                // etc), and not just the duplicate-invalid case.
                pfrom->fDisconnect = true;
            }
            return rLogError("invalid header received");
        }
    }

    {
        LOCK(cs_main);
        CNodeState *nodestate = State(pfrom->GetId());
        if (nodestate->nUnconnectingHeaders > 0)
        {
            NLogFormat("peer=%d: resetting nUnconnectingHeaders (%d -> 0)", pfrom->GetId(),
                       nodestate->nUnconnectingHeaders);
        }
        nodestate->nUnconnectingHeaders = 0;

        assert(pindexLast);
        UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        // From here, pindexBestKnownBlock should be guaranteed to be non-null,
        // because it is set in UpdateBlockAvailability. Some nullptr checks
        // are still present, however, as belt-and-suspenders.

        if (received_new_header && pindexLast->nChainWork > chainActive.Tip()->nChainWork)
        {
            nodestate->m_last_block_announcement = GetTime();
        }

        if (nCount == MAX_HEADERS_RESULTS)
        {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            NLogFormat("more getheaders (%d) to end to peer=%d (startheight:%d)", pindexLast->nHeight,
                       pfrom->GetId(), pfrom->nStartingHeight);
            connman->PushMessage(pfrom,
                                 msgMaker.Make(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexLast), uint256()));
        }

        bool fCanDirectFetch = CanDirectFetch(chainparams.GetConsensus());
        // If this set of headers is valid and ends in a block with at least as
        // much work as our tip, download as much as possible.
        if (fCanDirectFetch && pindexLast->IsValid(BLOCK_VALID_TREE) &&
            chainActive.Tip()->nChainWork <= pindexLast->nChainWork)
        {
            std::vector<const CBlockIndex *> vToFetch;
            const CBlockIndex *pindexWalk = pindexLast;
            // Calculate all the blocks we'd need to switch to pindexLast, up to a limit.
            while (pindexWalk && !chainActive.Contains(pindexWalk) && vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER)
            {
                if (!(pindexWalk->nStatus & BLOCK_HAVE_DATA) &&
                    !mapBlocksInFlight.count(pindexWalk->GetBlockHash()) &&
                    (!IsWitnessEnabled(pindexWalk->pprev, chainparams.GetConsensus()) ||
                     State(pfrom->GetId())->fHaveWitness))
                {
                    // We don't have this block, and it's not yet in flight.
                    vToFetch.push_back(pindexWalk);
                }
                pindexWalk = pindexWalk->pprev;
            }
            // If pindexWalk still isn't on our main chain, we're looking at a
            // very large reorg at a time we think we're close to caught up to
            // the main chain -- this shouldn't really happen.  Bail out on the
            // direct fetch and rely on parallel download instead.
            if (!chainActive.Contains(pindexWalk))
            {
                NLogFormat("Large reorg, won't direct fetch to %s (%d)",
                           pindexLast->GetBlockHash().ToString(),
                           pindexLast->nHeight);
            } else
            {
                std::vector<CInv> vGetData;
                // Download as much as possible, from earliest to latest.
                for (const CBlockIndex *pindex : reverse_iterate(vToFetch))
                {
                    if (nodestate->nBlocksInFlight >= MAX_BLOCKS_IN_TRANSIT_PER_PEER)
                    {
                        // Can't download any more from this peer
                        break;
                    }
                    uint32_t nFetchFlags = GetFetchFlags(pfrom);
                    vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
                    MarkBlockAsInFlight(pfrom->GetId(), pindex->GetBlockHash(), pindex);
                    NLogFormat("Requesting block %s from  peer=%d",
                               pindex->GetBlockHash().ToString(), pfrom->GetId());
                }
                if (vGetData.size() > 1)
                {
                    NLogFormat("Downloading blocks toward %s (%d) via headers direct fetch",
                               pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
                }
                if (vGetData.size() > 0)
                {
                    if (nodestate->fSupportsDesiredCmpctVersion && vGetData.size() == 1 &&
                        mapBlocksInFlight.size() == 1 && pindexLast->pprev->IsValid(BLOCK_VALID_CHAIN))
                    {
                        // In any case, we want to download using a compact block, not a regular one
                        vGetData[0] = CInv(MSG_CMPCT_BLOCK, vGetData[0].hash);
                    }
                    connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                }
            }
        }
        // If we're in IBD, we want outbound peers that will serve us a useful
        // chain. Disconnect peers that are on chains with insufficient work.
        if (ifChainObj->IsInitialBlockDownload() && nCount != MAX_HEADERS_RESULTS)
        {
            // When nCount < MAX_HEADERS_RESULTS, we know we have no more
            // headers to fetch from this peer.
            if (nodestate->pindexBestKnownBlock && nodestate->pindexBestKnownBlock->nChainWork < nMinimumChainWork)
            {
                // This peer has too little work on their headers chain to help
                // us sync -- disconnect if using an outbound slot (unless
                // whitelisted or addnode).
                // Note: We compare their tip to nMinimumChainWork (rather than
                // chainActive.Tip()) because we won't start block download
                // until we have a headers chain that has at least
                // nMinimumChainWork, even if a peer has a chain past our tip,
                // as an anti-DoS measure.
                if (IsOutboundDisconnectionCandidate(pfrom))
                {
                    NLogFormat("Disconnecting outbound peer %d -- headers chain has insufficient work", pfrom->GetId());
                    pfrom->fDisconnect = true;
                }
            }
        }

        if (!pfrom->fDisconnect && IsOutboundDisconnectionCandidate(pfrom) &&
            nodestate->pindexBestKnownBlock != nullptr)
        {
            // If this is an outbound peer, check to see if we should protect
            // it from the bad/lagging chain logic.
            if (g_outbound_peers_with_protect_from_disconnect < MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT &&
                nodestate->pindexBestKnownBlock->nChainWork >= chainActive.Tip()->nChainWork &&
                !nodestate->m_chain_sync.m_protect)
            {
                NLogFormat("Protecting outbound peer=%d from eviction", pfrom->GetId());
                nodestate->m_chain_sync.m_protect = true;
                ++g_outbound_peers_with_protect_from_disconnect;
            }
        }
    }

    return true;
}

static inline NodeExchangeInfo FromCNode(CNode *pfrom)
{
    NodeExchangeInfo xnode;
    xnode.nodeID = pfrom->GetId();
    xnode.sendVersion = pfrom->GetSendVersion();
    xnode.nLocalServices = pfrom->GetLocalServices();
    xnode.flags = 0;
    xnode.retFlags = 0;
    xnode.nMisbehavior = 0;
    xnode.retInteger = 0;
    xnode.retPointer = nullptr;
    return xnode;
}

//////////////////////////////////////////////////////////////////////////////

PeerLogicValidation::PeerLogicValidation(CConnman *connmanIn, CScheduler &scheduler)
        : connman(connmanIn), m_stale_tip_check_time(0), appArgs(Args())
{
    // Stale tip checking and peer eviction are on two different timers, but we
    // don't want them to get out of sync due to drift in the scheduler, so we
    // combine them in one function and schedule at the quicker (peer-eviction)
    // timer.
    static_assert(EXTRA_PEER_CHECK_INTERVAL < STALE_CHECK_INTERVAL,
                  "peer eviction timer should be less than stale tip check timer");

    const Consensus::Params &consensusParams = Params().GetConsensus();
    scheduler.scheduleEvery(std::bind(&PeerLogicValidation::CheckForStaleTipAndEvictPeers, this, consensusParams),
                            EXTRA_PEER_CHECK_INTERVAL * 1000);
}

void PeerLogicValidation::BlockConnected(const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindex,
                                         const std::vector<CTransactionRef> &vtxConflicted)
{
    LOCK(cs_main);
    GET_TXMEMPOOL_INTERFACE(ifMempoolObj);
    ifMempoolObj->RemoveOrphanTxForBlock(pblock.get());
    g_last_tip_update = GetTime();
}

void
PeerLogicValidation::UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload)
{
    const int nNewHeight = pindexNew->nHeight;
    connman->SetBestHeight(nNewHeight);

    if (!fInitialDownload)
    {
        // Find the hashes of all blocks that weren't previously in the best chain.
        std::vector<uint256> vHashes;
        const CBlockIndex *pindexToAnnounce = pindexNew;
        while (pindexToAnnounce != pindexFork)
        {
            vHashes.push_back(pindexToAnnounce->GetBlockHash());
            pindexToAnnounce = pindexToAnnounce->pprev;
            if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE)
            {
                // Limit announcements in case of a huge reorganization.
                // Rely on the peer's synchronization mechanism in that case.
                break;
            }
        }
        // Relay inventory, but don't relay old inventory during initial block download.
        connman->ForEachNode([nNewHeight, &vHashes](CNode *pnode)
                             {
                                 if (nNewHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : 0))
                                 {
                                     for (const uint256 &hash : reverse_iterate(vHashes))
                                     {
                                         pnode->PushBlockHash(hash);
                                     }
                                 }
                             });
        connman->WakeMessageHandler();
    }

    nTimeBestReceived = GetTime();
}

void PeerLogicValidation::BlockChecked(const CBlock &block, const CValidationState &state)
{
    LOCK(cs_main);

    const uint256 hash(block.GetHash());
    std::map<uint256, std::pair<NodeId, bool>>::iterator it = mapBlockSource.find(hash);
    GET_CHAIN_INTERFACE(ifChainObj);

    int nDoS = 0;
    if (state.IsInvalid(nDoS))
    {
        // Don't send reject message with code 0 or an internal reject code.
        if (it != mapBlockSource.end() && State(it->second.first) && state.GetRejectCode() > 0 &&
            state.GetRejectCode() < REJECT_INTERNAL)
        {
            CBlockReject reject = {(unsigned char)state.GetRejectCode(),
                                   state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), hash};
            State(it->second.first)->rejects.push_back(reject);
            if (nDoS > 0 && it->second.second)
                Misbehaving(it->second.first, nDoS);
        }
    }
        // Check that:
        // 1. The block is valid
        // 2. We're not in initial block download
        // 3. This is currently the best block we're aware of. We haven't updated
        //    the tip yet so we have no way to check this directly here. Instead we
        //    just check that there are currently no other blocks in flight.
    else if (state.IsValid() &&
             !ifChainObj->IsInitialBlockDownload() &&
             mapBlocksInFlight.count(hash) == mapBlocksInFlight.size())
    {
        if (it != mapBlockSource.end())
        {
            MaybeSetPeerAsAnnouncingHeaderAndIDs(it->second.first, connman);
        }
    }
    if (it != mapBlockSource.end())
        mapBlockSource.erase(it);
}

void
PeerLogicValidation::NewPoWValidBlock(const CBlockIndex * /*pindex*/, const std::shared_ptr<const CBlock> &/*pblock*/)
{

}

bool PeerLogicValidation::RelayCmpctBlock(const CBlockIndex *pindex, void *pcmpctblock, bool fWitnessEnabled)
{
    if (!pindex || !pcmpctblock || !connman)
        return false;

    LOCK(cs_main);

    uint256 hashBlock(pindex->GetBlockHash());
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);
    connman->ForEachNode([this, pcmpctblock, pindex, &msgMaker, fWitnessEnabled, &hashBlock](CNode *pnode)
                         {
                             // Avoid the repeated-serialization here
                             if (pnode->nVersion < INVALID_CB_NO_BAN_VERSION || pnode->fDisconnect)
                                 return;
                             ProcessBlockAvailability(pnode->GetId());
                             CNodeState &state = *State(pnode->GetId());
                             // If the peer has, or we announced to them the previous block already,
                             // but we don't think they have this one, go ahead and announce it
                             if (state.fPreferHeaderAndIDs && (!fWitnessEnabled || state.fWantsCmpctWitness) &&
                                 !PeerHasHeader(&state, pindex) && PeerHasHeader(&state, pindex->pprev))
                             {

                                 ILogFormat("%s sending header-and-ids %s to peer=%d",
                                            "PeerLogicValidation::NewPoWValidBlock",
                                            hashBlock.ToString(), pnode->GetId());
                                 connman->PushMessage(pnode, msgMaker.Make(NetMsgType::CMPCTBLOCK,
                                                                           *(CBlockHeaderAndShortTxIDs *)pcmpctblock));
                                 state.pindexBestHeaderSent = pindex;
                             }
                         });
    return true;
}

bool PeerLogicValidation::ProcessMessages(CNode *pfrom, std::atomic<bool> &interruptMsgProc)
{
    const CChainParams &chainparams = Params();
    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fMoreWork = false;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom, interruptMsgProc);

    if (pfrom->fDisconnect)
        return false;

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty())
        return true;

    // Don't bother if send buffer is too full to respond anyway
    if (pfrom->fPauseSend)
        return false;

    std::list<CNetMessage> msgs;
    {
        LOCK(pfrom->cs_vProcessMsg);
        if (pfrom->vProcessMsg.empty())
            return false;
        // Just take one message
        msgs.splice(msgs.begin(), pfrom->vProcessMsg, pfrom->vProcessMsg.begin());
        pfrom->nProcessQueueSize -= msgs.front().vRecv.size() + CMessageHeader::HEADER_SIZE;
        pfrom->fPauseRecv = pfrom->nProcessQueueSize > connman->GetReceiveFloodSize();
        fMoreWork = !pfrom->vProcessMsg.empty();
    }
    CNetMessage &msg(msgs.front());

    msg.SetVersion(pfrom->GetRecvVersion());
    // Scan for message start
    if (memcmp(msg.hdr.pchMessageStart, chainparams.MessageStart(), CMessageHeader::MESSAGE_START_SIZE) != 0)
    {
        WLogFormat("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d", SanitizeString(msg.hdr.GetCommand()),
                   pfrom->GetId());
        pfrom->fDisconnect = true;
        return false;
    }

    // Read header
    CMessageHeader &hdr = msg.hdr;
    if (!hdr.IsValid(chainparams.MessageStart()))
    {
        ELogFormat("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d", SanitizeString(hdr.GetCommand()), pfrom->GetId());
        return fMoreWork;
    }
    std::string strCommand = hdr.GetCommand();

    // Message size
    unsigned int nMessageSize = hdr.nMessageSize;

    // Checksum
    CDataStream &vRecv = msg.vRecv;
    const uint256 &hash = msg.GetMessageHash();
    if (memcmp(hash.begin(), hdr.pchChecksum, CMessageHeader::CHECKSUM_SIZE) != 0)
    {
        ELogFormat("%s(%s, %u bytes): CHECKSUM ERROR expected %s was %s", __func__,
                   SanitizeString(strCommand), nMessageSize,
                   HexStr(hash.begin(), hash.begin() + CMessageHeader::CHECKSUM_SIZE),
                   HexStr(hdr.pchChecksum, hdr.pchChecksum + CMessageHeader::CHECKSUM_SIZE));
        return fMoreWork;
    }

    // Process message
    bool fRet = false;
    try
    {
        fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime, interruptMsgProc);
        if (interruptMsgProc)
            return false;
        if (!pfrom->vRecvGetData.empty())
            fMoreWork = true;
    }
    catch (const std::ios_base::failure &e)
    {
        connman->PushMessage(pfrom,
                             CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::REJECT, strCommand, REJECT_MALFORMED,
                                                                   std::string("error parsing message")));
        if (strstr(e.what(), "end of data"))
        {
            // Allow exceptions from under-length message on vRecv
            ELogFormat(
                    "%s(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than its stated length",
                    __func__, SanitizeString(strCommand), nMessageSize, e.what());
        } else if (strstr(e.what(), "size too large"))
        {
            // Allow exceptions from over-long size
            ELogFormat("%s(%s, %u bytes): Exception '%s' caught", __func__, SanitizeString(strCommand), nMessageSize,
                       e.what());
        } else if (strstr(e.what(), "non-canonical ReadCompactSize()"))
        {
            // Allow exceptions from non-canonical encoding
            ELogFormat("%s(%s, %u bytes): Exception '%s' caught", __func__, SanitizeString(strCommand), nMessageSize,
                       e.what());
        } else
        {
            PrintExceptionContinue(&e, "ProcessMessages()");
        }
    }
    catch (const std::exception &e)
    {
        PrintExceptionContinue(&e, "ProcessMessages()");
    } catch (...)
    {
        PrintExceptionContinue(nullptr, "ProcessMessages()");
    }

    if (!fRet)
    {
        ELogFormat("%s(%s, %u bytes) FAILED peer=%d", __func__, SanitizeString(strCommand), nMessageSize,
                   pfrom->GetId());
    }

    LOCK(cs_main);
    SendRejectsAndCheckIfBanned(pfrom);

    return fMoreWork;
}

bool PeerLogicValidation::SendMessages(CNode *pto, std::atomic<bool> &interruptMsgProc)
{
    const Consensus::Params &consensusParams = Params().GetConsensus();

    GET_CHAIN_INTERFACE(ifChainObj);
    CChain &chainActive = ifChainObj->GetActiveChain();

    {
        // Don't send anything until the version handshake is complete
        if (!pto->fSuccessfullyConnected || pto->fDisconnect)
            return true;

        // If we get here, the outgoing message serialization version is set and can't change.
        const CNetMsgMaker msgMaker(pto->GetSendVersion());

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued)
        {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros())
        {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend)
        {
            uint64_t nonce = 0;
            while (nonce == 0)
            {
                GetRandBytes((unsigned char *)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION)
            {
                pto->nPingNonceSent = nonce;
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::PING, nonce));
            } else
            {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::PING));
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        if (SendRejectsAndCheckIfBanned(pto))
            return true;
        CNodeState &state = *State(pto->GetId());

        // Address refresh broadcast
        int64_t nNow = GetTimeMicros();
        if (!ifChainObj->IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow)
        {
            AdvertiseLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        //
        // Message: addr
        //
        if (pto->nNextAddrSend < nNow)
        {
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            std::vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            for (const CAddress &addr : pto->vAddrToSend)
            {
                if (!pto->addrKnown.contains(addr.GetKey()))
                {
                    pto->addrKnown.insert(addr.GetKey());
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        connman->PushMessage(pto, msgMaker.Make(NetMsgType::ADDR, vAddr));
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::ADDR, vAddr));
            // we only send the big addr message once
            if (pto->vAddrToSend.capacity() > 40)
                pto->vAddrToSend.shrink_to_fit();
        }

        CBlockIndex *pindexBestHeader = ifChainObj->GetIndexBestHeader();
        // Start block sync
        bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient &&
                                                   !pto->fOneShot); // Download if this is a nice peer, or we have no nice peers and this one might do.
        if (!state.fSyncStarted && !pto->fClient && !fImporting && !ifChainObj->IsReindexing())
        {
            // Only actively request headers from a single peer, unless we're close to today.
            if ((nSyncStarted == 0 && fFetch) || pindexBestHeader->GetBlockTime() > GetAdjustedTime() - 24 * 60 * 60)
            {
                state.fSyncStarted = true;
                state.nHeadersSyncTimeout = GetTimeMicros() + HEADERS_DOWNLOAD_TIMEOUT_BASE +
                                            HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER *
                                            (GetAdjustedTime() - pindexBestHeader->GetBlockTime()) /
                                            (consensusParams.nPowTargetSpacing);
                nSyncStarted++;
                const CBlockIndex *pindexStart = pindexBestHeader;
                /* If possible, start at the block preceding the currently
                   best known header.  This ensures that we always get a
                   non-empty list of headers back as long as the peer
                   is up-to-date.  With a non-empty response, we can initialise
                   the peer's known best block.  This wouldn't be possible
                   if we requested starting at pindexBestHeader and
                   got back an empty response.  */
                if (pindexStart->pprev)
                    pindexStart = pindexStart->pprev;
                NLogFormat("initial getheaders (%d) to peer=%d (startheight:%d)", pindexStart->nHeight,
                           pto->GetId(), int(pto->nStartingHeight));
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexStart),
                                                        uint256()));
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!ifChainObj->IsReindexing() && !fImporting && !ifChainObj->IsInitialBlockDownload())
        {
            GetMainSignals().Broadcast(nTimeBestReceived, connman);
        }

        //
        // Try sending block announcements via headers
        //
        {
            // If we have less than MAX_BLOCKS_TO_ANNOUNCE in our
            // list of block hashes we're relaying, and our peer wants
            // headers announcements, then find the first header
            // not yet known to our peer but would connect, and send.
            // If no header would connect, or if we have too many
            // blocks, or if the peer doesn't want headers, just
            // add all to the inv queue.
            LOCK(pto->cs_inventory);
            std::vector<CBlock> vHeaders;
            bool fRevertToInv = ((!state.fPreferHeaders &&
                                  (!state.fPreferHeaderAndIDs || pto->vBlockHashesToAnnounce.size() > 1)) ||
                                 pto->vBlockHashesToAnnounce.size() > MAX_BLOCKS_TO_ANNOUNCE);
            const CBlockIndex *pBestIndex = nullptr; // last header queued for delivery
            ProcessBlockAvailability(pto->GetId()); // ensure pindexBestKnownBlock is up-to-date

            if (!fRevertToInv)
            {
                bool fFoundStartingHeader = false;
                // Try to find first header that our peer doesn't have, and
                // then send all headers past that one.  If we come across any
                // headers that aren't on chainActive, give up.
                for (const uint256 &hash : pto->vBlockHashesToAnnounce)
                {
                    const CBlockIndex *pindex = ifChainObj->GetBlockIndex(hash);
                    assert(pindex != nullptr);
                    if (chainActive[pindex->nHeight] != pindex)
                    {
                        // Bail out if we reorged away from this block
                        fRevertToInv = true;
                        break;
                    }
                    if (pBestIndex != nullptr && pindex->pprev != pBestIndex)
                    {
                        // This means that the list of blocks to announce don't
                        // connect to each other.
                        // This shouldn't really be possible to hit during
                        // regular operation (because reorgs should take us to
                        // a chain that has some block not on the prior chain,
                        // which should be caught by the prior check), but one
                        // way this could happen is by using invalidateblock /
                        // reconsiderblock repeatedly on the tip, causing it to
                        // be added multiple times to vBlockHashesToAnnounce.
                        // Robustly deal with this rare situation by reverting
                        // to an inv.
                        fRevertToInv = true;
                        break;
                    }
                    pBestIndex = pindex;
                    if (fFoundStartingHeader)
                    {
                        // add this to the headers message
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else if (PeerHasHeader(&state, pindex))
                    {
                        continue; // keep looking for the first new block
                    } else if (pindex->pprev == nullptr || PeerHasHeader(&state, pindex->pprev))
                    {
                        // Peer doesn't have this header but they do have the prior one.
                        // Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else
                    {
                        // Peer doesn't have this header or the prior one -- nothing will
                        // connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (!fRevertToInv && !vHeaders.empty())
            {
                if (vHeaders.size() == 1 && state.fPreferHeaderAndIDs)
                {
                    // We only send up to 1 block as header-and-ids, as otherwise
                    // probably means we're doing an initial-ish-sync or they're slow
                    NLogFormat("%s sending header-and-ids %s to peer=%d", __func__,
                               vHeaders.front().GetHash().ToString(), pto->GetId());

                    int nSendFlags = state.fWantsCmpctWitness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS;


                    NodeExchangeInfo xnode = FromCNode(pto);
                    InitFlagsBit(xnode.flags, NF_WANTCMPCTWITNESS, state.fWantsCmpctWitness);
                    bool fGotBlockFromCache = ifChainObj->NetRequestMostRecentCmpctBlock(&xnode,
                                                                                         pBestIndex->GetBlockHash());
                    if (!fGotBlockFromCache)
                    {
                        CBlock block;
                        bool ret = ReadBlockFromDisk(block, pBestIndex, consensusParams);
                        assert(ret);
                        CBlockHeaderAndShortTxIDs cmpctblock(block, state.fWantsCmpctWitness);
                        connman->PushMessage(pto, msgMaker.Make(nSendFlags, NetMsgType::CMPCTBLOCK, cmpctblock));
                    }
                    state.pindexBestHeaderSent = pBestIndex;
                } else if (state.fPreferHeaders)
                {
                    if (vHeaders.size() > 1)
                    {
                        NLogFormat("%s: %u headers, range (%s, %s), to peer=%d", __func__,
                                   vHeaders.size(),
                                   vHeaders.front().GetHash().ToString(),
                                   vHeaders.back().GetHash().ToString(), pto->GetId());
                    } else
                    {
                        NLogFormat("%s: sending header %s to peer=%d", __func__,
                                   vHeaders.front().GetHash().ToString(), pto->GetId());
                    }
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
                    state.pindexBestHeaderSent = pBestIndex;
                } else
                    fRevertToInv = true;
            }
            if (fRevertToInv)
            {
                // If falling back to using an inv, just try to inv the tip.
                // The last entry in vBlockHashesToAnnounce was our tip at some point
                // in the past.
                if (!pto->vBlockHashesToAnnounce.empty())
                {
                    const uint256 &hashToAnnounce = pto->vBlockHashesToAnnounce.back();
                    const CBlockIndex *pindex = ifChainObj->GetBlockIndex(hashToAnnounce);
                    assert(pindex != nullptr);

                    // Warn if we're announcing a block that is not on the main chain.
                    // This should be very rare and could be optimized out.
                    // Just log for now.
                    if (chainActive[pindex->nHeight] != pindex)
                    {
                        NLogFormat("Announcing block %s not on main chain (tip=%s)",
                                   hashToAnnounce.ToString(), chainActive.Tip()->GetBlockHash().ToString());
                    }

                    // If the peer's chain has this block, don't inv it back.
                    if (!PeerHasHeader(&state, pindex))
                    {
                        pto->PushInventory(CInv(MSG_BLOCK, hashToAnnounce));
                        NLogFormat("%s: sending inv peer=%d hash=%s", __func__,
                                   pto->GetId(), hashToAnnounce.ToString());
                    }
                }
            }
            pto->vBlockHashesToAnnounce.clear();
        }

        //
        // Message: inventory
        //
        std::vector<CInv> vInv;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(std::max<size_t>(pto->vInventoryBlockToSend.size(), INVENTORY_BROADCAST_MAX));

            // Add blocks
            for (const uint256 &hash : pto->vInventoryBlockToSend)
            {
                vInv.push_back(CInv(MSG_BLOCK, hash));
                if (vInv.size() == MAX_INV_SZ)
                {
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                    vInv.clear();
                }
            }
            pto->vInventoryBlockToSend.clear();

            // Check whether periodic sends should happen
            bool fSendTrickle = pto->fWhitelisted;
            if (pto->nNextInvSend < nNow)
            {
                fSendTrickle = true;
                // Use half the delay for outbound peers, as there is less privacy concern for them.
                pto->nNextInvSend = PoissonNextSend(nNow, INVENTORY_BROADCAST_INTERVAL >> !pto->fInbound);
            }

            // Time to send but the peer has requested we not relay transactions.
            if (fSendTrickle)
            {
                LOCK(pto->cs_filter);
                if (!pto->fRelayTxes)
                    pto->setInventoryTxToSend.clear();
            }

            // Respond to BIP35 mempool requests
            if (fSendTrickle)
            {
                NodeExchangeInfo xnode;
                xnode.sendVersion = pto->GetSendVersion();
                xnode.nodeID = pto->GetId();

                CAmount filterrate = 0;
                {
                    LOCK(pto->cs_feeFilter);
                    filterrate = pto->minFeeFilter;
                }

                std::vector<uint256> haveSentTxHashes;
                std::vector<uint256> toSendTxHashes(pto->setInventoryTxToSend.begin(), pto->setInventoryTxToSend.end());

                // LOCK(pto->cs_feeFilter);
                GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
                ifTxMempoolObj->NetRequestTxInventory(&xnode, pto->fSendMempool, filterrate, pto->pfilter,
                                                      toSendTxHashes, haveSentTxHashes);

                if (pto->fSendMempool)
                {
                    pto->fSendMempool = false;
                    pto->timeLastMempoolReq = GetTime();
                }

                pto->setInventoryTxToSend.clear();
                pto->setInventoryTxToSend.insert(toSendTxHashes.begin(), toSendTxHashes.end());

                for (const auto &hash : haveSentTxHashes)
                {
                    pto->filterInventoryKnown.insert(hash);
                }
            }

#if 0
            // Respond to BIP35 mempool requests
            if (fSendTrickle && pto->fSendMempool)
            {
                auto vtxinfo = mempool.infoAll();
                pto->fSendMempool = false;
                CAmount filterrate = 0;
                {
                    LOCK(pto->cs_feeFilter);
                    filterrate = pto->minFeeFilter;
                }

                LOCK(pto->cs_filter);

                for (const auto &txinfo : vtxinfo)
                {
                    const uint256 &hash = txinfo.tx->GetHash();
                    CInv inv(MSG_TX, hash);
                    pto->setInventoryTxToSend.erase(hash);
                    if (filterrate)
                    {
                        if (txinfo.feeRate.GetFeePerK() < filterrate)
                            continue;
                    }
                    if (pto->pfilter)
                    {
                        if (!pto->pfilter->IsRelevantAndUpdate(*txinfo.tx))
                            continue;
                    }
                    pto->filterInventoryKnown.insert(hash);
                    vInv.push_back(inv);
                    if (vInv.size() == MAX_INV_SZ)
                    {
                        connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                        vInv.clear();
                    }
                }
                pto->timeLastMempoolReq = GetTime();
            }

            // Determine transactions to relay
            if (fSendTrickle)
            {
                // Produce a vector with all candidates for sending
                std::vector<std::set<uint256>::iterator> vInvTx;
                vInvTx.reserve(pto->setInventoryTxToSend.size());
                for (std::set<uint256>::iterator it = pto->setInventoryTxToSend.begin();
                     it != pto->setInventoryTxToSend.end(); it++)
                {
                    vInvTx.push_back(it);
                }
                CAmount filterrate = 0;
                {
                    LOCK(pto->cs_feeFilter);
                    filterrate = pto->minFeeFilter;
                }
                // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
                // A heap is used so that not all items need sorting if only a few are being sent.
                CompareInvMempoolOrder compareInvMempoolOrder(&mempool);
                std::make_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                // No reason to drain out at many times the network's capacity,
                // especially since we have many peers and some will draw much shorter delays.
                unsigned int nRelayedTransactions = 0;
                LOCK(pto->cs_filter);
                while (!vInvTx.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX)
                {
                    // Fetch the top element from the heap
                    std::pop_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                    std::set<uint256>::iterator it = vInvTx.back();
                    vInvTx.pop_back();
                    uint256 hash = *it;
                    // Remove it from the to-be-sent set
                    pto->setInventoryTxToSend.erase(it);
                    // Check if not in the filter already
                    if (pto->filterInventoryKnown.contains(hash))
                    {
                        continue;
                    }
                    // Not in the mempool anymore? don't bother sending it.
                    auto txinfo = mempool.info(hash);
                    if (!txinfo.tx)
                    {
                        continue;
                    }
                    if (filterrate && txinfo.feeRate.GetFeePerK() < filterrate)
                    {
                        continue;
                    }
                    if (pto->pfilter && !pto->pfilter->IsRelevantAndUpdate(*txinfo.tx))
                        continue;
                    // Send
                    vInv.push_back(CInv(MSG_TX, hash));
                    nRelayedTransactions++;
                    {
                        // Expire old relay messages
                        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < nNow)
                        {
                            mapRelay.erase(vRelayExpiration.front().second);
                            vRelayExpiration.pop_front();
                        }

                        auto ret = mapRelay.insert(std::make_pair(hash, std::move(txinfo.tx)));
                        if (ret.second)
                        {
                            vRelayExpiration.push_back(std::make_pair(nNow + 15 * 60 * 1000000, ret.first));
                        }
                    }
                    if (vInv.size() == MAX_INV_SZ)
                    {
                        connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                        vInv.clear();
                    }
                    pto->filterInventoryKnown.insert(hash);
                }
            }
#endif

        }
        if (!vInv.empty())
            connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));

        // Detect whether we're stalling
        nNow = GetTimeMicros();
        if (state.nStallingSince && state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT)
        {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            NLogFormat("Peer=%d is stalling block download, disconnecting", pto->GetId());
            pto->fDisconnect = true;
            return true;
        }
        // In case there is a block that has been in flight from this peer for 2 + 0.5 * N times the block interval
        // (with N the number of peers from which we're downloading validated blocks), disconnect due to timeout.
        // We compensate for other peers to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertise non-existing block hashes
        // to unreasonably increase our timeout.
        if (state.vBlocksInFlight.size() > 0)
        {
            QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
            int nOtherPeersWithValidatedDownloads =
                    nPeersWithValidatedDownloads - (state.nBlocksInFlightValidHeaders > 0);
            if (nNow > state.nDownloadingSince + consensusParams.nPowTargetSpacing * (BLOCK_DOWNLOAD_TIMEOUT_BASE +
                                                                                      BLOCK_DOWNLOAD_TIMEOUT_PER_PEER *
                                                                                      nOtherPeersWithValidatedDownloads))
            {
                ELogFormat("Timeout downloading block %s from peer=%d, disconnecting", queuedBlock.hash.ToString(),
                           pto->GetId());
                pto->fDisconnect = true;
                return true;
            }
        }
        // Check for headers sync timeouts
        if (state.fSyncStarted && state.nHeadersSyncTimeout < std::numeric_limits<int64_t>::max())
        {
            // Detect whether this is a stalling initial-headers-sync peer
            if (pindexBestHeader->GetBlockTime() <= GetAdjustedTime() - 24 * 60 * 60)
            {
                if (nNow > state.nHeadersSyncTimeout && nSyncStarted == 1 &&
                    (nPreferredDownload - state.fPreferredDownload >= 1))
                {
                    // Disconnect a (non-whitelisted) peer if it is our only sync peer,
                    // and we have others we could be using instead.
                    // Note: If all our peers are inbound, then we won't
                    // disconnect our sync peer for stalling; we have bigger
                    // problems if we can't get any outbound peers.
                    if (!pto->fWhitelisted)
                    {
                        NLogFormat("Timeout downloading headers from peer=%d, disconnecting", pto->GetId());
                        pto->fDisconnect = true;
                        return true;
                    } else
                    {
                        NLogFormat("Timeout downloading headers from whitelisted peer=%d, not disconnecting",
                                   pto->GetId());
                        // Reset the headers sync state so that we have a
                        // chance to try downloading from a different peer.
                        // Note: this will also result in at least one more
                        // getheaders message to be sent to
                        // this peer (eventually).
                        state.fSyncStarted = false;
                        nSyncStarted--;
                        state.nHeadersSyncTimeout = 0;
                    }
                }
            } else
            {
                // After we've caught up once, reset the timeout so we can't trigger
                // disconnect later.
                state.nHeadersSyncTimeout = std::numeric_limits<int64_t>::max();
            }
        }

        // Check that outbound peers have reasonable chains
        // GetTime() is used by this anti-DoS logic so we can test this using mocktime
        ConsiderEviction(pto, GetTime());

        //
        // Message: getdata (blocks)
        //
        std::vector<CInv> vGetData;
        if (!pto->fClient && (fFetch || !ifChainObj->IsInitialBlockDownload()) &&
            state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER)
        {
            std::vector<const CBlockIndex *> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload,
                                     staller, consensusParams);
            for (const CBlockIndex *pindex : vToDownload)
            {
                uint32_t nFetchFlags = GetFetchFlags(pto);
                vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
                MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), pindex);
                NLogFormat("Requesting block %s (%d) peer=%d", pindex->GetBlockHash().ToString(),
                           pindex->nHeight, pto->GetId());
            }
            if (state.nBlocksInFlight == 0 && staller != -1)
            {
                if (State(staller)->nStallingSince == 0)
                {
                    State(staller)->nStallingSince = nNow;
                    NLogFormat("Stall started peer=%d", staller);
                }
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv &inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv))
            {
                NLogFormat("Requesting %s peer=%d", inv.ToString(), pto->GetId());
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                    vGetData.clear();
                }
            } else
            {
                //If we're not going to ask, don't expect a response.
                pto->setAskFor.erase(inv.hash);
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));

        //
        // Message: feefilter
        //
        // We don't want white listed peers to filter txs to us if we have -whitelistforcerelay
        if (pto->nVersion >= FEEFILTER_VERSION && appArgs.GetArg<bool>("-feefilter", DEFAULT_FEEFILTER) &&
            !(pto->fWhitelisted && appArgs.GetArg<bool>("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)))
        {
            GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
            CTxMemPool &mempool = ifTxMempoolObj->GetMemPool();
            CAmount currentFilter = mempool.GetMinFee(
                    appArgs.GetArg<uint32_t>("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFeePerK();
            int64_t timeNow = GetTimeMicros();
            if (timeNow > pto->nextSendTimeFeeFilter)
            {
                static CFeeRate default_feerate(DEFAULT_MIN_RELAY_TX_FEE);
                static FeeFilterRounder filterRounder(default_feerate);
                CAmount filterToSend = filterRounder.round(currentFilter);
                // We always have a fee filter of at least minRelayTxFee
                filterToSend = std::max(filterToSend, ::minRelayTxFee.GetFeePerK());
                if (filterToSend != pto->lastSentFeeFilter)
                {
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::FEEFILTER, filterToSend));
                    pto->lastSentFeeFilter = filterToSend;
                }
                pto->nextSendTimeFeeFilter = PoissonNextSend(timeNow, AVG_FEEFILTER_BROADCAST_INTERVAL);
            }
                // If the fee filter has changed substantially and it's still more than MAX_FEEFILTER_CHANGE_DELAY
                // until scheduled broadcast, then move the broadcast to within MAX_FEEFILTER_CHANGE_DELAY.
            else if (timeNow + MAX_FEEFILTER_CHANGE_DELAY * 1000000 < pto->nextSendTimeFeeFilter &&
                     (currentFilter < 3 * pto->lastSentFeeFilter / 4 || currentFilter > 4 * pto->lastSentFeeFilter / 3))
            {
                pto->nextSendTimeFeeFilter = timeNow + GetRandInt(MAX_FEEFILTER_CHANGE_DELAY) * 1000000;
            }
        }
    }
    return true;
}

void PeerLogicValidation::InitializeNode(CNode *pnode)
{
    CAddress addr = pnode->addr;
    std::string addrName = pnode->GetAddrName();
    NodeId nodeid = pnode->GetId();
    {
        LOCK(cs_main);
        mapNodeState.emplace_hint(mapNodeState.end(), std::piecewise_construct, std::forward_as_tuple(nodeid),
                                  std::forward_as_tuple(addr, std::move(addrName)));
    }
    if (!pnode->fInbound)
        PushNodeVersion(pnode, connman, GetTime());
}

void PeerLogicValidation::FinalizeNode(NodeId nodeid, bool &fUpdateConnectionTime)
{
    fUpdateConnectionTime = false;
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    if (state->fSyncStarted)
        nSyncStarted--;

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected)
    {
        fUpdateConnectionTime = true;
    }

    for (const QueuedBlock &entry : state->vBlocksInFlight)
    {
        mapBlocksInFlight.erase(entry.hash);
    }
    GET_TXMEMPOOL_INTERFACE(ifMempoolObj);
    ifMempoolObj->RemoveOrphanTxForNode(nodeid);
    nPreferredDownload -= state->fPreferredDownload;
    nPeersWithValidatedDownloads -= (state->nBlocksInFlightValidHeaders != 0);
    assert(nPeersWithValidatedDownloads >= 0);
    g_outbound_peers_with_protect_from_disconnect -= state->m_chain_sync.m_protect;
    assert(g_outbound_peers_with_protect_from_disconnect >= 0);

    mapNodeState.erase(nodeid);

    if (mapNodeState.empty())
    {
        // Do a consistency check after the last peer is removed.
        assert(mapBlocksInFlight.empty());
        assert(nPreferredDownload == 0);
        assert(nPeersWithValidatedDownloads == 0);
        assert(g_outbound_peers_with_protect_from_disconnect == 0);
    }
    NLogFormat("Cleared nodestate for peer=%d", nodeid);
}


bool PeerLogicValidation::ProcessMessage(CNode *pfrom, const std::string &strCommand, CDataStream &vRecv,
                                         int64_t nTimeReceived, const std::atomic<bool> &interruptMsgProc)
{
    NLogFormat("received: %s (%u bytes) peer=%d", SanitizeString(strCommand), vRecv.size(), pfrom->GetId());

    if (appArgs.IsArgSet("-dropmessagestest") && GetRand(appArgs.GetArg<uint64_t>("-dropmessagestest", 0)) == 0)
    {
        NLogFormat("dropmessagestest DROPPING RECV MESSAGE");
        return true;
    }

    GET_CHAIN_INTERFACE(ifChainObj);
    if ((ifChainObj->GetActiveChain().Tip()->nHeight > Params().GetConsensus().SBTCContractForkHeight) &&
        (pfrom->nVersion != 0) && (pfrom->nVersion < SBTC_CONTRACT_VERSION))
    {
        // disconnect from peers older than this proto version
        ELogFormat("peer=%d using obsolete version %i; disconnecting", pfrom->GetId(), pfrom->nVersion);
        connman->PushMessage(pfrom,
                             CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::REJECT, std::string(NetMsgType::VERSION),
                                                                   REJECT_OBSOLETE,
                                                                   strprintf("Version must be %d or greater",
                                                                             SBTC_CONTRACT_VERSION)));
        pfrom->fDisconnect = true;
        return false;
    }

    if (!(pfrom->GetLocalServices() & NODE_BLOOM) &&
        (strCommand == NetMsgType::FILTERLOAD ||
         strCommand == NetMsgType::FILTERADD))
    {
        if (pfrom->nVersion >= NO_BLOOM_VERSION)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
            return false;
        } else
        {
            pfrom->fDisconnect = true;
            return false;
        }
    }

    if (strCommand == NetMsgType::REJECT)
    {
        return ProcessRejectMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::VERSION)
    {
        return ProcessVersionMsg(pfrom, vRecv);
    }

    if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }

    if (strCommand == NetMsgType::VERACK)
    {
        return ProcessVerAckMsg(pfrom, vRecv);

    }

    if (!pfrom->fSuccessfullyConnected)
    {
        // Must have a verack message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }

    if (strCommand == NetMsgType::GETADDR)
    {
        return ProcessGetAddrMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::ADDR)
    {
        return ProcessAddrMsg(pfrom, vRecv, interruptMsgProc);
    }

    if (strCommand == NetMsgType::SENDHEADERS)
    {
        return ProcessSendHeadersMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::SENDCMPCT)
    {
        return ProcessSendCmpctMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::PING)
    {
        return ProcessPingMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::PONG)
    {
        return ProcessPongMsg(pfrom, vRecv, nTimeReceived);
    }

    if (strCommand == NetMsgType::FILTERLOAD)
    {
        return ProcessFilterLoadMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::FILTERADD)
    {
        return ProcessFilterAddMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::FILTERCLEAR)
    {
        return ProcessFilterClearMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::FEEFILTER)
    {
        return ProcessFeeFilterMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::CHECKPOINT)
    {
        return ProcessCheckPointMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::GET_CHECKPOINT)
    {
        return ProcessGetCheckPointMsg(pfrom, vRecv);
    }


    if (strCommand == NetMsgType::MEMPOOL)
    {
        return ProcessMemPoolMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::GETBLOCKS)
    {
        return ProcessGetBlocksMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::INV)
    {
        return ProcessInvMsg(pfrom, vRecv, interruptMsgProc);
    }

    if (strCommand == NetMsgType::GETHEADERS)
    {
        return ProcessGetHeadersMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::HEADERS && !fImporting && !ifChainObj->IsReindexing())
    {
        return ProcessHeadersMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::GETDATA)
    {
        return ProcessGetDataMsg(pfrom, vRecv, interruptMsgProc);
    }

    if (strCommand == NetMsgType::BLOCK && !fImporting && !ifChainObj->IsReindexing())
    {
        return ProcessBlockMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::TX)
    {
        return ProcessTxMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::GETBLOCKTXN)
    {
        return ProcessGetBlockTxnMsg(pfrom, vRecv, interruptMsgProc);
    }

    if (strCommand == NetMsgType::BLOCKTXN && !fImporting && !ifChainObj->IsReindexing())
    {
        return ProcessBlockTxnMsg(pfrom, vRecv);
    }

    if (strCommand == NetMsgType::CMPCTBLOCK && !fImporting && !ifChainObj->IsReindexing())
    {
        return ProcessCmpctBlockMsg(pfrom, vRecv, nTimeReceived, interruptMsgProc);

    }


    if (strCommand == NetMsgType::NOTFOUND)
    {
        // We do not care about the NOTFOUND message, but logging an Unknown Command
        // message would be undesirable as we transmit it ourselves.
        return true;
    }

    // Ignore unknown commands for extensibility
    NLogFormat("Unknown command \"%s\" from peer=%d", SanitizeString(strCommand), pfrom->GetId());
    return true;
}


bool PeerLogicValidation::ProcessRejectMsg(CNode *pfrom, CDataStream &vRecv)
{
    UNUSED(pfrom);

    try
    {
        std::string strMsg;
        unsigned char ccode;
        std::string strReason;
        vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode
              >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

        std::ostringstream ss;
        ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

        if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX)
        {
            uint256 hash;
            vRecv >> hash;
            ss << ": hash " << hash.ToString();
        }
        NLogFormat("Reject %s", SanitizeString(ss.str()));
    }
    catch (const std::ios_base::failure &)
    {
        // Avoid feedback loops by preventing reject messages from triggering a new reject message.
        ELogFormat("Unparseable reject message received");
    }

    return true;
}

bool PeerLogicValidation::ProcessVersionMsg(CNode *pfrom, CDataStream &vRecv)
{
    GET_CHAIN_INTERFACE(ifChainObj);

    // Each connection can only send one version message
    if (pfrom->nVersion != 0)
    {
        connman->PushMessage(pfrom,
                             CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::REJECT, std::string(NetMsgType::VERSION),
                                                                   REJECT_DUPLICATE,
                                                                   std::string("Duplicate version message")));
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }

    int64_t nTime;
    CAddress addrMe;
    CAddress addrFrom;
    uint64_t nNonce = 1;
    uint64_t nServiceInt;
    ServiceFlags nServices;
    int nVersion;
    int nSendVersion;
    std::string strSubVer;
    std::string cleanSubVer;
    int nStartingHeight = -1;
    bool fRelay = true;

    vRecv >> nVersion >> nServiceInt >> nTime >> addrMe;
    nSendVersion = std::min(nVersion, PROTOCOL_VERSION);
    nServices = ServiceFlags(nServiceInt);
    if (!pfrom->fInbound)
    {
        connman->SetServices(pfrom->addr, nServices);
    }

    if (pfrom->nServicesExpected & ~nServices)
    {
        NLogFormat(
                "peer=%d does not offer the expected services (%08x offered, %08x expected); disconnecting",
                pfrom->GetId(), nServices, pfrom->nServicesExpected);
        connman->PushMessage(pfrom,
                             CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::REJECT, std::string(NetMsgType::VERSION),
                                                                   REJECT_NONSTANDARD,
                                                                   strprintf(
                                                                           "Expected to offer services %08x",
                                                                           pfrom->nServicesExpected)));
        pfrom->fDisconnect = true;
        return false;
    }

    if (nServices & ((1 << 7) | (1 << 5)))
    {
        if (GetTime() < 1533096000)
        {
            // Immediately disconnect peers that use service bits 6 or 8 until August 1st, 2018
            // These bits have been used as a flag to indicate that a node is running incompatible
            // consensus rules instead of changing the network magic, so we're stuck disconnecting
            // based on these service bits, at least for a while.
            pfrom->fDisconnect = true;
            return false;
        }
    }

    int minVer = MIN_PEER_PROTO_VERSION;
    if (ifChainObj->GetActiveChain().Tip()->nVersion & (((uint32_t)1) << VERSIONBITS_SBTC_CONTRACT))
    {
        minVer = SBTC_CONTRACT_VERSION;
    }
    if (nVersion < minVer)
    {
        // disconnect from peers older than this proto version
        ELogFormat("peer=%d using obsolete version %i; disconnecting", pfrom->GetId(), nVersion);
        connman->PushMessage(pfrom,
                             CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::REJECT, std::string(NetMsgType::VERSION),
                                                                   REJECT_OBSOLETE,
                                                                   strprintf("Version must be %d or greater", minVer)));
        pfrom->fDisconnect = true;
        return false;
    }

    if (nVersion == 10300)
        nVersion = 300;
    if (!vRecv.empty())
        vRecv >> addrFrom >> nNonce;
    if (!vRecv.empty())
    {
        vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
        cleanSubVer = SanitizeString(strSubVer);
    }
    if (!vRecv.empty())
    {
        vRecv >> nStartingHeight;
    }
    if (!vRecv.empty())
        vRecv >> fRelay;
    // Disconnect if we connected to ourself
    if (pfrom->fInbound && !connman->CheckIncomingNonce(nNonce))
    {
        ELogFormat("connected to self at %s, disconnecting", pfrom->addr.ToString());
        pfrom->fDisconnect = true;
        return true;
    }

    if (pfrom->fInbound && addrMe.IsRoutable())
    {
        SeenLocal(addrMe);
    }

    // Be shy and don't send version until we hear
    if (pfrom->fInbound)
        PushNodeVersion(pfrom, connman, GetAdjustedTime());

    connman->PushMessage(pfrom, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::VERACK));

    pfrom->nServices = nServices;
    pfrom->SetAddrLocal(addrMe);
    {
        LOCK(pfrom->cs_SubVer);
        pfrom->strSubVer = strSubVer;
        pfrom->cleanSubVer = cleanSubVer;
    }
    pfrom->nStartingHeight = nStartingHeight;
    pfrom->fClient = !(nServices & NODE_NETWORK);
    {
        LOCK(pfrom->cs_filter);
        pfrom->fRelayTxes = fRelay; // set to true after we get the first filter* message
    }

    // Change version
    pfrom->SetSendVersion(nSendVersion);
    pfrom->nVersion = nVersion;

    if ((nServices & NODE_WITNESS))
    {
        LOCK(cs_main);
        State(pfrom->GetId())->fHaveWitness = true;
    }

    // Potentially mark this peer as a preferred download peer.
    {
        LOCK(cs_main);
        UpdatePreferredDownload(pfrom, State(pfrom->GetId()));
    }

    if (!pfrom->fInbound)
    {
        // Advertise our address
        if (fListen && !ifChainObj->IsInitialBlockDownload())
        {
            CAddress addr = GetLocalAddress(&pfrom->addr, pfrom->GetLocalServices());
            FastRandomContext insecure_rand;
            if (addr.IsRoutable())
            {
                NLogFormat("ProcessMessages: advertising address %s", addr.ToString());
                pfrom->PushAddress(addr, insecure_rand);
            } else if (IsPeerAddrLocalGood(pfrom))
            {
                addr.SetIP(addrMe);
                NLogFormat("ProcessMessages: advertising address %s", addr.ToString());
                pfrom->PushAddress(addr, insecure_rand);
            }
        }

        // Get recent addresses
        if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || connman->GetAddressCount() < 1000)
        {
            connman->PushMessage(pfrom, CNetMsgMaker(nSendVersion).Make(NetMsgType::GETADDR));
            pfrom->fGetAddr = true;
        }
        connman->MarkAddressGood(pfrom->addr);
    }

    std::string remoteAddr;
    //if (fLogIPs)
    remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

    NLogFormat("receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s",
               cleanSubVer, pfrom->nVersion.load(),
               pfrom->nStartingHeight.load(), addrMe.ToString(), pfrom->GetId(), remoteAddr);

    int64_t nTimeOffset = nTime - GetTime();
    pfrom->nTimeOffset = nTimeOffset;
    AddTimeData(pfrom->addr, nTimeOffset);

    // If the peer is old enough to have the old alert system, send it the final alert.
    if (pfrom->nVersion <= 70012)
    {
        CDataStream finalAlert(ParseHex(
                "60010000000000000000000000ffffff7f00000000ffffff7ffeffff7f01ffffff7f00000000ffffff7f00ffffff7f002f555247454e543a20416c657274206b657920636f6d70726f6d697365642c2075706772616465207265717569726564004630440220653febd6410f470f6bae11cad19c48413becb1ac2c17f908fd0fd53bdc3abd5202206d0e9c96fe88d4a0f01ed9dedae2b6f9e00da94cad0fecaae66ecf689bf71b50"),
                               SER_NETWORK, PROTOCOL_VERSION);
        connman->PushMessage(pfrom, CNetMsgMaker(nSendVersion).Make("alert", finalAlert));
    }

    // Feeler connections exist only to verify if address is online.
    if (pfrom->fFeeler)
    {
        assert(pfrom->fInbound == false);
        pfrom->fDisconnect = true;
    }

    return true;
}

bool PeerLogicValidation::ProcessVerAckMsg(CNode *pfrom, CDataStream &vRecv)
{
    pfrom->SetRecvVersion(std::min(pfrom->nVersion.load(), PROTOCOL_VERSION));

    if (!pfrom->fInbound)
    {
        // Mark this node as currently connected, so we update its timestamp later.
        LOCK(cs_main);
        State(pfrom->GetId())->fCurrentlyConnected = true;
    }

    if (pfrom->nVersion >= SENDHEADERS_VERSION)
    {
        // Tell our peer we prefer to receive headers rather than inv's
        // We send this to non-NODE NETWORK peers as well, because even
        // non-NODE NETWORK peers can announce blocks (such as pruning
        // nodes)
        connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SENDHEADERS));
    }

    if (pfrom->nVersion >= SHORT_IDS_BLOCKS_VERSION)
    {
        // Tell our peer we are willing to provide version 1 or 2 cmpctblocks
        // However, we do not request new block announcements using
        // cmpctblock messages.
        // We send this to non-NODE NETWORK peers as well, because
        // they may wish to request compact blocks from us
        bool fAnnounceUsingCMPCTBLOCK = false;
        uint64_t nCMPCTBLOCKVersion = 2;
        if (pfrom->GetLocalServices() & NODE_WITNESS)
            connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SENDCMPCT,
                                                                                   fAnnounceUsingCMPCTBLOCK,
                                                                                   nCMPCTBLOCKVersion));
        nCMPCTBLOCKVersion = 1;
        connman->PushMessage(pfrom,
                             CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SENDCMPCT, fAnnounceUsingCMPCTBLOCK,
                                                                        nCMPCTBLOCKVersion));
    }
    pfrom->fSuccessfullyConnected = true;
    return true;
}

bool PeerLogicValidation::ProcessGetAddrMsg(CNode *pfrom, CDataStream &vRecv)
{
    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making nodes which are behind NAT and can only make outgoing connections ignore
    // the getaddr message mitigates the attack.
    if (!pfrom->fInbound)
    {
        NLogFormat("Ignoring \"getaddr\" from outbound connection. peer=%d", pfrom->GetId());
        return true;
    }

    // Only send one GetAddr response per connection to reduce resource waste
    //  and discourage addr stamping of INV announcements.
    if (pfrom->fSentAddr)
    {
        NLogFormat("Ignoring repeated \"getaddr\". peer=%d", pfrom->GetId());
        return true;
    }
    pfrom->fSentAddr = true;

    pfrom->vAddrToSend.clear();
    std::vector<CAddress> vAddr = connman->GetAddresses();
    FastRandomContext insecure_rand;
    for (const CAddress &addr : vAddr)
        pfrom->PushAddress(addr, insecure_rand);

    return true;
}

bool PeerLogicValidation::ProcessAddrMsg(CNode *pfrom, CDataStream &vRecv, const std::atomic<bool> &interruptMsgProc)
{
    // Don't want addr from older versions unless seeding
    if (pfrom->nVersion < CADDR_TIME_VERSION && connman->GetAddressCount() > 1000)
        return true;

    std::vector<CAddress> vAddr;
    vRecv >> vAddr;
    if (vAddr.size() > 1000)
    {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 20);
        WLogFormat("message addr size() = %u", vAddr.size());
        return false;
    }

    // Store the new addresses
    std::vector<CAddress> vAddrOk;
    int64_t nNow = GetAdjustedTime();
    int64_t nSince = nNow - 10 * 60;
    for (CAddress &addr : vAddr)
    {
        if (interruptMsgProc)
            return true;

        if ((addr.nServices & REQUIRED_SERVICES) != REQUIRED_SERVICES)
            continue;

        if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
            addr.nTime = nNow - 5 * 24 * 60 * 60;
        pfrom->AddAddressKnown(addr);
        bool fReachable = IsReachable(addr);
        if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
        {
            // Relay to a limited number of other nodes
            RelayAddress(addr, fReachable, connman);
        }
        // Do not store addresses outside our network
        if (fReachable)
            vAddrOk.push_back(addr);
    }

    connman->AddNewAddresses(vAddrOk, pfrom->addr, 2 * 60 * 60);
    if (vAddr.size() < 1000)
        pfrom->fGetAddr = false;

    if (pfrom->fOneShot)
        pfrom->fDisconnect = true;

    return true;
}

bool PeerLogicValidation::ProcessSendHeadersMsg(CNode *pfrom, CDataStream &vRecv)
{
    LOCK(cs_main);
    State(pfrom->GetId())->fPreferHeaders = true;
    return true;
}

bool PeerLogicValidation::ProcessSendCmpctMsg(CNode *pfrom, CDataStream &vRecv)
{
    bool fAnnounceUsingCMPCTBLOCK = false;
    uint64_t nCMPCTBLOCKVersion = 0;
    vRecv >> fAnnounceUsingCMPCTBLOCK >> nCMPCTBLOCKVersion;
    if (nCMPCTBLOCKVersion == 1 || ((pfrom->GetLocalServices() & NODE_WITNESS) && nCMPCTBLOCKVersion == 2))
    {
        LOCK(cs_main);
        // fProvidesHeaderAndIDs is used to "lock in" version of compact blocks we send (fWantsCmpctWitness)
        if (!State(pfrom->GetId())->fProvidesHeaderAndIDs)
        {
            State(pfrom->GetId())->fProvidesHeaderAndIDs = true;
            State(pfrom->GetId())->fWantsCmpctWitness = nCMPCTBLOCKVersion == 2;
        }
        if (State(pfrom->GetId())->fWantsCmpctWitness ==
            (nCMPCTBLOCKVersion == 2)) // ignore later version announces
            State(pfrom->GetId())->fPreferHeaderAndIDs = fAnnounceUsingCMPCTBLOCK;
        if (!State(pfrom->GetId())->fSupportsDesiredCmpctVersion)
        {
            if (pfrom->GetLocalServices() & NODE_WITNESS)
                State(pfrom->GetId())->fSupportsDesiredCmpctVersion = (nCMPCTBLOCKVersion == 2);
            else
                State(pfrom->GetId())->fSupportsDesiredCmpctVersion = (nCMPCTBLOCKVersion == 1);
        }
    }
    return true;
}

bool PeerLogicValidation::ProcessPingMsg(CNode *pfrom, CDataStream &vRecv)
{
    if (pfrom->nVersion > BIP0031_VERSION)
    {
        uint64_t nonce = 0;
        vRecv >> nonce;
        // Echo the message back with the nonce. This allows for two useful features:
        //
        // 1) A remote node can quickly check if the connection is operational
        // 2) Remote nodes can measure the latency of the network thread. If this node
        //    is overloaded it won't respond to pings quickly and the remote node can
        //    avoid sending us more work, like chain download requests.
        //
        // The nonce stops the remote getting confused between different pings: without
        // it, if the remote node sends a ping once per second and this node takes 5
        // seconds to respond to each, the 5th ping the remote sends would appear to
        // return very quickly.
        connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::PONG, nonce));
    }
    return true;
}

bool PeerLogicValidation::ProcessPongMsg(CNode *pfrom, CDataStream &vRecv, int64_t nTimeReceived)
{
    int64_t pingUsecEnd = nTimeReceived;
    uint64_t nonce = 0;
    size_t nAvail = vRecv.in_avail();
    bool bPingFinished = false;
    std::string sProblem;

    if (nAvail >= sizeof(nonce))
    {
        vRecv >> nonce;

        // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
        if (pfrom->nPingNonceSent != 0)
        {
            if (nonce == pfrom->nPingNonceSent)
            {
                // Matching pong received, this ping is no longer outstanding
                bPingFinished = true;
                int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                if (pingUsecTime > 0)
                {
                    // Successful ping time measurement, replace previous
                    pfrom->nPingUsecTime = pingUsecTime;
                    pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime.load(), pingUsecTime);
                } else
                {
                    // This should never happen
                    sProblem = "Timing mishap";
                }
            } else
            {
                // Nonce mismatches are normal when pings are overlapping
                sProblem = "Nonce mismatch";
                if (nonce == 0)
                {
                    // This is most likely a bug in another implementation somewhere; cancel this ping
                    bPingFinished = true;
                    sProblem = "Nonce zero";
                }
            }
        } else
        {
            sProblem = "Unsolicited pong without ping";
        }
    } else
    {
        // This is most likely a bug in another implementation somewhere; cancel this ping
        bPingFinished = true;
        sProblem = "Short payload";
    }

    if (!(sProblem.empty()))
    {
        NLogFormat("pong peer=%d: %s, %x expected, %x received, %u bytes",
                   pfrom->GetId(),
                   sProblem,
                   pfrom->nPingNonceSent.load(),
                   nonce,
                   nAvail);
    }

    if (bPingFinished)
    {
        pfrom->nPingNonceSent = 0;
    }

    return true;
}

bool PeerLogicValidation::ProcessFilterLoadMsg(CNode *pfrom, CDataStream &vRecv)
{
    CBloomFilter filter;
    vRecv >> filter;
    if (!filter.IsWithinSizeConstraints())
    {
        // There is no excuse for sending a too-large filter
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 100);
    } else
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter(filter);
        pfrom->pfilter->UpdateEmptyFull();
        pfrom->fRelayTxes = true;
    }
    return true;
}

bool PeerLogicValidation::ProcessFilterAddMsg(CNode *pfrom, CDataStream &vRecv)
{
    std::vector<unsigned char> vData;
    vRecv >> vData;

    // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
    // and thus, the maximum size any matched object can have) in a filteradd message
    bool bad = false;
    if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
    {
        bad = true;
    } else
    {
        LOCK(pfrom->cs_filter);
        if (pfrom->pfilter)
        {
            pfrom->pfilter->insert(vData);
        } else
        {
            bad = true;
        }
    }
    if (bad)
    {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 100);
    }
    return true;
}

bool PeerLogicValidation::ProcessFilterClearMsg(CNode *pfrom, CDataStream &vRecv)
{
    LOCK(pfrom->cs_filter);
    if (pfrom->GetLocalServices() & NODE_BLOOM)
    {
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
    }
    pfrom->fRelayTxes = true;
    return true;
}

bool PeerLogicValidation::ProcessFeeFilterMsg(CNode *pfrom, CDataStream &vRecv)
{
    CAmount newFeeFilter = 0;
    vRecv >> newFeeFilter;
    if (MoneyRange(newFeeFilter))
    {
        {
            LOCK(pfrom->cs_feeFilter);
            pfrom->minFeeFilter = newFeeFilter;
        }
        NLogFormat("received: feefilter of %s from peer=%d", CFeeRate(newFeeFilter).ToString(),
                   pfrom->GetId());
    }
    return true;
}

bool PeerLogicValidation::ProcessCheckPointMsg(CNode *pfrom, CDataStream &vRecv)
{
    NodeExchangeInfo xnode = FromCNode(pfrom);

    GET_CHAIN_INTERFACE(ifChainObj);
    return ifChainObj->NetReceiveCheckPoint(&xnode, vRecv);
}

bool PeerLogicValidation::ProcessGetCheckPointMsg(CNode *pfrom, CDataStream &vRecv)
{
    int nHeight = 0;
    vRecv >> nHeight;

    NodeExchangeInfo xnode = FromCNode(pfrom);

    GET_CHAIN_INTERFACE(ifChainObj);
    return ifChainObj->NetRequestCheckPoint(&xnode, nHeight);
}

bool PeerLogicValidation::ProcessMemPoolMsg(CNode *pfrom, CDataStream &vRecv)
{
    if (!(pfrom->GetLocalServices() & NODE_BLOOM) && !pfrom->fWhitelisted)
    {
        NLogFormat("mempool request with bloom filters disabled, disconnect peer=%d", pfrom->GetId());
        pfrom->fDisconnect = true;
        return true;
    }

    if (connman->OutboundTargetReached(false) && !pfrom->fWhitelisted)
    {
        NLogFormat("mempool request with bandwidth limit reached, disconnect peer=%d", pfrom->GetId());
        pfrom->fDisconnect = true;
        return true;
    }

    LOCK(pfrom->cs_inventory);
    pfrom->fSendMempool = true;
    return true;
}

bool PeerLogicValidation::ProcessGetBlocksMsg(CNode *pfrom, CDataStream &vRecv)
{
    NodeExchangeInfo xnode = FromCNode(pfrom);

    std::vector<uint256> blockHashes;

    GET_CHAIN_INTERFACE(ifChainObj);
    if (ifChainObj->NetRequestBlocks(&xnode, vRecv, blockHashes))
    {
        for (const auto &hash : blockHashes)
        {
            pfrom->PushInventory(CInv(MSG_BLOCK, hash));
        }
        if ((int)blockHashes.size() >= 500)
        {
            pfrom->hashContinue = blockHashes.back();
        }
        return true;
    }

    return false;
}

bool PeerLogicValidation::ProcessInvMsg(CNode *pfrom, CDataStream &vRecv, const std::atomic<bool> &interruptMsgProc)
{
    GET_CHAIN_INTERFACE(ifChainObj);
    CChain &chainActive = ifChainObj->GetActiveChain();

    std::vector<CInv> vInv;
    vRecv >> vInv;
    if (vInv.size() > MAX_INV_SZ)
    {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 20);
        ELogFormat("message inv size() = %u", vInv.size());
        return false;
    }

    bool fBlocksOnly = !fRelayTxes;

    // Allow whitelisted peers to send data other than blocks in blocks only mode if whitelistrelay is true
    if (pfrom->fWhitelisted && appArgs.GetArg<bool>("-whitelistrelay", DEFAULT_WHITELISTRELAY))
        fBlocksOnly = false;

    LOCK(cs_main);

    uint32_t nFetchFlags = GetFetchFlags(pfrom);

    for (CInv &inv : vInv)
    {
        if (interruptMsgProc)
            return true;

        bool fAlreadyHave = AlreadyHave(inv);
        NLogFormat("got inv: %s  %s peer=%d", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->GetId());

        if (inv.type == MSG_TX)
        {
            inv.type |= nFetchFlags;
        }

        if (inv.type == MSG_BLOCK)
        {
            UpdateBlockAvailability(pfrom->GetId(), inv.hash);
            if (!fAlreadyHave && !fImporting && !ifChainObj->IsReindexing() && !mapBlocksInFlight.count(inv.hash))
            {
                // We used to request the full block here, but since headers-announcements are now the
                // primary method of announcement on the network, and since, in the case that a node
                // fell back to inv we probably have a reorg which we should get the headers for first,
                // we now only provide a getheaders response here. When we receive the headers, we will
                // then ask for the blocks we need.
                connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETHEADERS,
                                                                                       chainActive.GetLocator(
                                                                                               ifChainObj->GetIndexBestHeader()),
                                                                                       inv.hash));
                NLogFormat("getheaders (%d) %s to peer=%d", ifChainObj->GetIndexBestHeader()->nHeight,
                           inv.hash.ToString(),
                           pfrom->GetId());
            }
        } else
        {
            pfrom->AddInventoryKnown(inv);
            if (fBlocksOnly)
            {
                NLogFormat("transaction (%s) inv sent in violation of protocol peer=%d", inv.hash.ToString(),
                           pfrom->GetId());
            } else if (!fAlreadyHave && !fImporting && !ifChainObj->IsReindexing() &&
                       !ifChainObj->IsInitialBlockDownload())
            {
                pfrom->AskFor(inv);
            }
        }

        // Track requests for our stuff
        GetMainSignals().Inventory(inv.hash);
    }

    return true;
}

bool PeerLogicValidation::ProcessGetHeadersMsg(CNode *pfrom, CDataStream &vRecv)
{
    NodeExchangeInfo xnode = FromCNode(pfrom);
    InitFlagsBit(xnode.flags, NF_WHITELIST, pfrom->fWhitelisted);

    GET_CHAIN_INTERFACE(ifChainObj);
    bool ret = ifChainObj->NetRequestHeaders(&xnode, vRecv);
    if (xnode.retPointer)
    {
        LOCK(cs_main);
        CNodeState *nodestate = State(pfrom->GetId());
        nodestate->pindexBestHeaderSent = (const CBlockIndex *)xnode.retPointer;
    }

    return ret;
}

bool PeerLogicValidation::ProcessHeadersMsg(CNode *pfrom, CDataStream &vRecv)
{
    std::vector<CBlockHeader> headers;

    // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
    unsigned int nCount = ReadCompactSize(vRecv);
    if (nCount > MAX_HEADERS_RESULTS)
    {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 20);
        return rLogError("headers message size = %u", nCount);
    }
    headers.resize(nCount);
    for (unsigned int n = 0; n < nCount; n++)
    {
        vRecv >> headers[n];
        ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
    }

    // Headers received via a HEADERS message should be valid, and reflect
    // the chain the peer is on. If we receive a known-invalid header,
    // disconnect the peer if it is using one of our outbound connection
    // slots.
    bool should_punish = !pfrom->fInbound && !pfrom->m_manual_connection;
    return ProcessHeadersMessage(pfrom, connman, headers, Params(), should_punish);
}

bool PeerLogicValidation::ProcessGetDataMsg(CNode *pfrom, CDataStream &vRecv, const std::atomic<bool> &interruptMsgProc)
{
    std::vector<CInv> vInv;
    vRecv >> vInv;
    if (vInv.size() > MAX_INV_SZ)
    {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 20);
        return rLogError("message getdata size() = %u", vInv.size());
    }

    NLogFormat("received getdata (%u invsz) peer=%d", vInv.size(), pfrom->GetId());

    if (vInv.size() > 0)
    {
        NLogFormat("received getdata for: %s peer=%d", vInv[0].ToString(), pfrom->GetId());
    }

    pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
    ProcessGetData(pfrom, interruptMsgProc);
    return true;
}

bool PeerLogicValidation::ProcessBlockMsg(CNode *pfrom, CDataStream &vRecv)
{
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    vRecv >> *pblock;

    NLogFormat("received block %s peer=%d", pblock->GetHash().ToString(), pfrom->GetId());

    bool forceProcessing = false;
    const uint256 hash(pblock->GetHash());
    {
        LOCK(cs_main);
        // Also always process if we requested the block explicitly, as we may
        // need it even though it is not a candidate for a new best tip.
        forceProcessing |= MarkBlockAsReceived(hash);
        // mapBlockSource is only used for sending reject messages and DoS scores,
        // so the race between here and cs_main in ProcessNewBlock is fine.
        mapBlockSource.emplace(hash, std::make_pair(pfrom->GetId(), true));
    }

    bool fNewBlock = false;
    GET_CHAIN_INTERFACE(ifChainObj);
    ifChainObj->ProcessNewBlock(Params(), pblock, forceProcessing, &fNewBlock);
    if (fNewBlock)
    {
        pfrom->nLastBlockTime = GetTime();
    } else
    {
        LOCK(cs_main);
        mapBlockSource.erase(pblock->GetHash());
    }
    return true;
}

bool PeerLogicValidation::ProcessTxMsg(CNode *pfrom, CDataStream &vRecv)
{
    LOCK(cs_main);
    NodeExchangeInfo xnode = FromCNode(pfrom);
    InitFlagsBit(xnode.flags, NF_WHITELIST, pfrom->fWhitelisted);
    InitFlagsBit(xnode.flags, NF_DISCONNECT, pfrom->fDisconnect);
    InitFlagsBit(xnode.flags, NF_OUTBOUND, !pfrom->fInbound);
    InitFlagsBit(xnode.flags, NF_RELAYTX, fRelayTxes);
    CNodeState *state = State(pfrom->GetId());
    InitFlagsBit(xnode.flags, NF_WITNESS, state->fHaveWitness);

    uint256 txHash;

    GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
    bool ret = ifTxMempoolObj->NetReceiveTxData(&xnode, vRecv, txHash);

    pfrom->AddInventoryKnown(CInv(MSG_TX, txHash));
    pfrom->setAskFor.erase(txHash);
    mapAlreadyAskedFor.erase(txHash);

    if (IsFlagsBitOn(xnode.retFlags, NF_NEWTRANSACTION))
        pfrom->nLastTXTime = GetTime();

    if (xnode.nMisbehavior > 0)
        Misbehaving(xnode.nodeID, xnode.nMisbehavior);

    return ret;
}

bool
PeerLogicValidation::ProcessGetBlockTxnMsg(CNode *pfrom, CDataStream &vRecv, const std::atomic<bool> &interruptMsgProc)
{
    NodeExchangeInfo xnode = FromCNode(pfrom);
    InitFlagsBit(xnode.flags, NF_WHITELIST, pfrom->fWhitelisted);
    InitFlagsBit(xnode.flags, NF_DISCONNECT, pfrom->fDisconnect);
    {
        LOCK(cs_main);
        CNodeState *state = State(pfrom->GetId());
        InitFlagsBit(xnode.flags, NF_WANTCMPCTWITNESS, state->fWantsCmpctWitness);
    }

    GET_CHAIN_INTERFACE(ifChainObj);
    bool ret = ifChainObj->NetRequestBlockTxn(&xnode, vRecv);

    if (xnode.nMisbehavior > 0)
    {
        LOCK(cs_main);
        Misbehaving(xnode.nodeID, xnode.nMisbehavior);
    }

    if (IsFlagsBitOn(xnode.retFlags, NF_DISCONNECT))
        pfrom->fDisconnect = true;

    return ret;
}

bool PeerLogicValidation::ProcessBlockTxnMsg(CNode *pfrom, CDataStream &vRecv)
{
    BlockTransactions resp;
    vRecv >> resp;

    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    bool fBlockRead = false;
    {
        LOCK(cs_main);
        std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator it = mapBlocksInFlight.find(
                resp.blockhash);

        if (it == mapBlocksInFlight.end() || !it->second.second->partialBlock ||
            it->second.first != pfrom->GetId())
        {
            ELogFormat("Peer %d sent us block transactions for block we weren't expecting", pfrom->GetId());
            return true;
        }

        PartiallyDownloadedBlock &partialBlock = *it->second.second->partialBlock;
        ReadStatus status = partialBlock.FillBlock(*pblock, resp.txn);
        if (status == READ_STATUS_INVALID)
        {
            MarkBlockAsReceived(resp.blockhash); // Reset in-flight state in case of whitelist
            Misbehaving(pfrom->GetId(), 100);
            ELogFormat("Peer %d sent us invalid compact block/non-matching block transactions", pfrom->GetId());
            return true;
        } else if (status == READ_STATUS_FAILED)
        {
            // Might have collided, fall back to getdata now :(
            std::vector<CInv> invs;
            invs.push_back(CInv(MSG_BLOCK | GetFetchFlags(pfrom), resp.blockhash));
            connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETDATA, invs));
        } else
        {
            // Block is either okay, or possibly we received
            // READ_STATUS_CHECKBLOCK_FAILED.
            // Note that CheckBlock can only fail for one of a few reasons:
            // 1. bad-proof-of-work (impossible here, because we've already
            //    accepted the header)
            // 2. merkleroot doesn't match the transactions given (already
            //    caught in FillBlock with READ_STATUS_FAILED, so
            //    impossible here)
            // 3. the block is otherwise invalid (eg invalid coinbase,
            //    block is too big, too many legacy sigops, etc).
            // So if CheckBlock failed, #3 is the only possibility.
            // Under BIP 152, we don't DoS-ban unless proof of work is
            // invalid (we don't require all the stateless checks to have
            // been run).  This is handled below, so just treat this as
            // though the block was successfully read, and rely on the
            // handling in ProcessNewBlock to ensure the block index is
            // updated, reject messages go out, etc.
            MarkBlockAsReceived(resp.blockhash); // it is now an empty pointer
            fBlockRead = true;
            // mapBlockSource is only used for sending reject messages and DoS scores,
            // so the race between here and cs_main in ProcessNewBlock is fine.
            // BIP 152 permits peers to relay compact blocks after validating
            // the header only; we should not punish peers if the block turns
            // out to be invalid.
            mapBlockSource.emplace(resp.blockhash, std::make_pair(pfrom->GetId(), false));
        }
    } // Don't hold cs_main when we call into ProcessNewBlock

    if (fBlockRead)
    {
        bool fNewBlock = false;
        // Since we requested this block (it was in mapBlocksInFlight), force it to be processed,
        // even if it would not be a candidate for new tip (missing previous block, chain not long enough, etc)
        // This bypasses some anti-DoS logic in AcceptBlock (eg to prevent
        // disk-space attacks), but this should be safe due to the
        // protections in the compact block handler -- see related comment
        // in compact block optimistic reconstruction handling.
        GET_CHAIN_INTERFACE(ifChainObj);
        ifChainObj->ProcessNewBlock(pblock, /*fForceProcessing=*/true, &fNewBlock);
        if (fNewBlock)
        {
            pfrom->nLastBlockTime = GetTime();
        } else
        {
            LOCK(cs_main);
            mapBlockSource.erase(pblock->GetHash());
        }
    }
    return true;
}


bool PeerLogicValidation::ProcessCmpctBlockMsg(CNode *pfrom, CDataStream &vRecv, int64_t nTimeReceived,
                                               const std::atomic<bool> &interruptMsgProc)
{
    GET_CHAIN_INTERFACE(ifChainObj);
    CChain &chainActive = ifChainObj->GetActiveChain();

    CBlockHeaderAndShortTxIDs cmpctblock;
    vRecv >> cmpctblock;

    bool received_new_header = false;
    {
        LOCK(cs_main);
        if (!ifChainObj->DoesBlockExist(cmpctblock.header.hashPrevBlock))
        {
            // Doesn't connect (or is genesis), instead of DoSing in AcceptBlockHeader, request deeper headers
            if (!ifChainObj->IsInitialBlockDownload())
                connman->PushMessage(pfrom,
                                     CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETHEADERS,
                                                                                chainActive.GetLocator(
                                                                                        ifChainObj->GetIndexBestHeader()),
                                                                                uint256()));
            return true;
        }

        if (!ifChainObj->DoesBlockExist(cmpctblock.header.GetHash()))
        {
            received_new_header = true;
        }
    }

    const CChainParams &chainparams = Params();

    const CBlockIndex *pindex = nullptr;
    CValidationState state;
    if (!ifChainObj->ProcessNewBlockHeaders({cmpctblock.header}, state, chainparams, &pindex, nullptr))
    {
        int nDoS;
        if (state.IsInvalid(nDoS))
        {
            if (nDoS > 0)
            {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), nDoS);
            }
            ELogFormat("Peer %d sent us invalid header via cmpctblock", pfrom->GetId());
            return true;
        }
    }

    // When we succeed in decoding a block's txids from a cmpctblock
    // message we typically jump to the BLOCKTXN handling code, with a
    // dummy (empty) BLOCKTXN message, to re-use the logic there in
    // completing processing of the putative block (without cs_main).
    bool fProcessBLOCKTXN = false;
    CDataStream blockTxnMsg(SER_NETWORK, PROTOCOL_VERSION);

    // If we end up treating this as a plain headers message, call that as well
    // without cs_main.
    bool fRevertToHeaderProcessing = false;

    // Keep a CBlock for "optimistic" compactblock reconstructions (see
    // below)
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    bool fBlockReconstructed = false;

    {
        LOCK(cs_main);
        // If AcceptBlockHeader returned true, it set pindex
        assert(pindex);
        UpdateBlockAvailability(pfrom->GetId(), pindex->GetBlockHash());

        CNodeState *nodestate = State(pfrom->GetId());

        // If this was a new header with more work than our tip, update the
        // peer's last block announcement time
        if (received_new_header && pindex->nChainWork > chainActive.Tip()->nChainWork)
        {
            nodestate->m_last_block_announcement = GetTime();
        }

        std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator blockInFlightIt = mapBlocksInFlight.find(
                pindex->GetBlockHash());
        bool fAlreadyInFlight = blockInFlightIt != mapBlocksInFlight.end();

        if (pindex->nStatus & BLOCK_HAVE_DATA) // Nothing to do here
            return true;

        if (pindex->nChainWork <= chainActive.Tip()->nChainWork || // We know something better
            pindex->nTx != 0)
        { // We had this block at some point, but pruned it
            if (fAlreadyInFlight)
            {
                // We requested this block for some reason, but our mempool will probably be useless
                // so we just grab the block via normal getdata
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(pfrom), cmpctblock.header.GetHash());
                connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETDATA, vInv));
            }
            return true;
        }

        // If we're not close to tip yet, give up and let parallel block fetch work its magic
        if (!fAlreadyInFlight && !CanDirectFetch(chainparams.GetConsensus()))
            return true;

        if (IsWitnessEnabled(pindex->pprev, chainparams.GetConsensus()) && !nodestate->fSupportsDesiredCmpctVersion)
        {
            // Don't bother trying to process compact blocks from v1 peers
            // after segwit activates.
            return true;
        }

        GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
        CTxMemPool &mempool = ifTxMempoolObj->GetMemPool();

        // We want to be a bit conservative just to be extra careful about DoS
        // possibilities in compact block processing...
        if (pindex->nHeight <= chainActive.Height() + 2)
        {
            if ((!fAlreadyInFlight && nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) ||
                (fAlreadyInFlight && blockInFlightIt->second.first == pfrom->GetId()))
            {
                std::list<QueuedBlock>::iterator *queuedBlockIt = nullptr;
                if (!MarkBlockAsInFlight(pfrom->GetId(), pindex->GetBlockHash(), pindex, &queuedBlockIt))
                {
                    if (!(*queuedBlockIt)->partialBlock)
                        (*queuedBlockIt)->partialBlock.reset(new PartiallyDownloadedBlock(&mempool));
                    else
                    {
                        // The block was already in flight using compact blocks from the same peer
                        NLogFormat("Peer sent us compact block we were already syncing!");
                        return true;
                    }
                }

                PartiallyDownloadedBlock &partialBlock = *(*queuedBlockIt)->partialBlock;
                ReadStatus status = partialBlock.InitData(cmpctblock);
                if (status == READ_STATUS_INVALID)
                {
                    MarkBlockAsReceived(pindex->GetBlockHash()); // Reset in-flight state in case of whitelist
                    Misbehaving(pfrom->GetId(), 100);
                    ELogFormat("Peer %d sent us invalid compact block", pfrom->GetId());
                    return true;
                } else if (status == READ_STATUS_FAILED)
                {
                    // Duplicate txindexes, the block is now in-flight, so just request it
                    std::vector<CInv> vInv(1);
                    vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(pfrom), cmpctblock.header.GetHash());
                    connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETDATA, vInv));
                    return true;
                }

                BlockTransactionsRequest req;
                for (size_t i = 0; i < cmpctblock.BlockTxCount(); i++)
                {
                    if (!partialBlock.IsTxAvailable(i))
                        req.indexes.push_back(i);
                }
                if (req.indexes.empty())
                {
                    // Dirty hack to jump to BLOCKTXN code (TODO: move message handling into their own functions)
                    BlockTransactions txn;
                    txn.blockhash = cmpctblock.header.GetHash();
                    blockTxnMsg << txn;
                    fProcessBLOCKTXN = true;
                } else
                {
                    req.blockhash = pindex->GetBlockHash();
                    connman->PushMessage(pfrom,
                                         CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETBLOCKTXN, req));
                }
            } else
            {
                // This block is either already in flight from a different
                // peer, or this peer has too many blocks outstanding to
                // download from.
                // Optimistically try to reconstruct anyway since we might be
                // able to without any round trips.
                PartiallyDownloadedBlock tempBlock(&mempool);
                ReadStatus status = tempBlock.InitData(cmpctblock);
                if (status != READ_STATUS_OK)
                {
                    // TODO: don't ignore failures
                    return true;
                }
                std::vector<CTransactionRef> dummy;
                status = tempBlock.FillBlock(*pblock, dummy);
                if (status == READ_STATUS_OK)
                {
                    fBlockReconstructed = true;
                }
            }
        } else
        {
            if (fAlreadyInFlight)
            {
                // We requested this block, but its far into the future, so our
                // mempool will probably be useless - request the block normally
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(pfrom), cmpctblock.header.GetHash());
                connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::GETDATA, vInv));
                return true;
            } else
            {
                // If this was an announce-cmpctblock, we want the same treatment as a header message
                fRevertToHeaderProcessing = true;
            }
        }
    } // cs_main

    if (fProcessBLOCKTXN)
        return ProcessMessage(pfrom, NetMsgType::BLOCKTXN, blockTxnMsg, nTimeReceived, interruptMsgProc);

    if (fRevertToHeaderProcessing)
    {
        // Headers received from HB compact block peers are permitted to be
        // relayed before full validation (see BIP 152), so we don't want to disconnect
        // the peer if the header turns out to be for an invalid block.
        // Note that if a peer tries to build on an invalid chain, that
        // will be detected and the peer will be banned.
        return ProcessHeadersMessage(pfrom, connman, {cmpctblock.header}, chainparams, /*punish_duplicate_invalid=*/
                                     false);
        return true;
    }

    if (fBlockReconstructed)
    {
        // If we got here, we were able to optimistically reconstruct a
        // block that is in flight from some other peer.
        {
            LOCK(cs_main);
            mapBlockSource.emplace(pblock->GetHash(), std::make_pair(pfrom->GetId(), false));
        }

        bool fNewBlock = false;
        // Setting fForceProcessing to true means that we bypass some of
        // our anti-DoS protections in AcceptBlock, which filters
        // unrequested blocks that might be trying to waste our resources
        // (eg disk space). Because we only try to reconstruct blocks when
        // we're close to caught up (via the CanDirectFetch() requirement
        // above, combined with the behavior of not requesting blocks until
        // we have a chain with at least nMinimumChainWork), and we ignore
        // compact blocks with less work than our tip, it is safe to treat
        // reconstructed compact blocks as having been requested.
        ifChainObj->ProcessNewBlock(pblock, /*fForceProcessing=*/true, &fNewBlock);
        if (fNewBlock)
        {
            pfrom->nLastBlockTime = GetTime();
        } else
        {
            LOCK(cs_main);
            mapBlockSource.erase(pblock->GetHash());
        }

        LOCK(cs_main); // hold cs_main for CBlockIndex::IsValid()
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS))
        {
            // Clear download state for this block, which is in
            // process from some other peer.  We do this after calling
            // ProcessNewBlock so that a malleated cmpctblock announcement
            // can't be used to interfere with block relay.
            MarkBlockAsReceived(pblock->GetHash());
        }
    }
    return true;
}


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void PeerLogicValidation::ProcessGetData(CNode *pfrom, const std::atomic<bool> &interruptMsgProc)
{
    std::vector<CInv> vNotFound;
    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    LOCK(cs_main);

    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();
    while (it != pfrom->vRecvGetData.end())
    {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->fPauseSend)
            break;

        const CInv &inv = *it;
        {
            if (interruptMsgProc)
                return;

            it++;

            if (inv.type == MSG_BLOCK ||
                inv.type == MSG_FILTERED_BLOCK ||
                inv.type == MSG_CMPCT_BLOCK ||
                inv.type == MSG_WITNESS_BLOCK)
            {
                NodeExchangeInfo xnode = FromCNode(pfrom);
                InitFlagsBit(xnode.flags, NF_WHITELIST, pfrom->fWhitelisted);
                {
                    LOCK(cs_main);
                    CNodeState *state = State(pfrom->GetId());
                    InitFlagsBit(xnode.flags, NF_WANTCMPCTWITNESS, state->fWantsCmpctWitness);
                }

                bool filteredBlock = inv.type == MSG_FILTERED_BLOCK;
                CBloomFilter filter;
                if (filteredBlock)
                {
                    filteredBlock = false;
                    LOCK(pfrom->cs_filter);
                    if (pfrom->pfilter)
                    {
                        filteredBlock = true;
                        filter = *pfrom->pfilter;
                    }
                }

                GET_CHAIN_INTERFACE(ifChainObj);
                bool ret = ifChainObj->NetRequestBlockData(&xnode, inv.hash, inv.type,
                                                           filteredBlock ? &filter : nullptr);
                if (ret)
                {
                    // Trigger the peer node to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        std::vector<CInv> vInv;
                        uint256 tipHash;
                        ifChainObj->GetActiveChainTipHash(tipHash);
                        vInv.push_back(CInv(MSG_BLOCK, tipHash));
                        connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, vInv));
                        pfrom->hashContinue.SetNull();
                    }
                } else
                {
                    if (IsFlagsBitOn(xnode.retFlags, NF_DISCONNECT))
                    {
                        pfrom->fDisconnect = true;
                    }
                }
            } else if (inv.type == MSG_TX || inv.type == MSG_WITNESS_TX)
            {
                NodeExchangeInfo xnode = FromCNode(pfrom);

                GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
                if (!ifTxMempoolObj->NetRequestTxData(&xnode, inv.hash, inv.type == MSG_WITNESS_TX,
                                                      pfrom->timeLastMempoolReq))
                {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            // why process just one block getdata msg here?
            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK || inv.type == MSG_CMPCT_BLOCK ||
                inv.type == MSG_WITNESS_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty())
    {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::NOTFOUND, vNotFound));
    }
}


bool PeerLogicValidation::SendRejectsAndCheckIfBanned(CNode *pnode)
{
    AssertLockHeld(cs_main);
    CNodeState &state = *State(pnode->GetId());

    for (const CBlockReject &reject : state.rejects)
    {
        connman->PushMessage(pnode,
                             CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::REJECT, (std::string)NetMsgType::BLOCK,
                                                                   reject.chRejectCode, reject.strRejectReason,
                                                                   reject.hashBlock));
    }
    state.rejects.clear();

    if (state.fShouldBan)
    {
        state.fShouldBan = false;
        if (pnode->fWhitelisted)
            WLogFormat("Warning: not punishing whitelisted peer %s!", pnode->addr.ToString());
        else if (pnode->m_manual_connection)
            WLogFormat("Warning: not punishing addnoded peer %s!", pnode->addr.ToString());
        else
        {
            pnode->fDisconnect = true;
            if (pnode->addr.IsLocal())
                WLogFormat("Warning: not banning local peer %s!", pnode->addr.ToString());
            else
            {
                connman->Ban(pnode->addr, BanReasonNodeMisbehaving);
            }
        }
        return true;
    }
    return false;
}


void PeerLogicValidation::ConsiderEviction(CNode *pto, int64_t time_in_seconds)
{
    AssertLockHeld(cs_main);

    GET_CHAIN_INTERFACE(ifChainObj);
    CChain &chainActive = ifChainObj->GetActiveChain();

    CNodeState &state = *State(pto->GetId());
    const CNetMsgMaker msgMaker(pto->GetSendVersion());

    if (!state.m_chain_sync.m_protect && IsOutboundDisconnectionCandidate(pto) && state.fSyncStarted)
    {
        // This is an outbound peer subject to disconnection if they don't
        // announce a block with as much work as the current tip within
        // CHAIN_SYNC_TIMEOUT + HEADERS_RESPONSE_TIME seconds (note: if
        // their chain has more work than ours, we should sync to it,
        // unless it's invalid, in which case we should find that out and
        // disconnect from them elsewhere).
        if (state.pindexBestKnownBlock != nullptr &&
            state.pindexBestKnownBlock->nChainWork >= chainActive.Tip()->nChainWork)
        {
            if (state.m_chain_sync.m_timeout != 0)
            {
                state.m_chain_sync.m_timeout = 0;
                state.m_chain_sync.m_work_header = nullptr;
                state.m_chain_sync.m_sent_getheaders = false;
            }
        } else if (state.m_chain_sync.m_timeout == 0 ||
                   (state.m_chain_sync.m_work_header != nullptr && state.pindexBestKnownBlock != nullptr &&
                    state.pindexBestKnownBlock->nChainWork >= state.m_chain_sync.m_work_header->nChainWork))
        {
            // Our best block known by this peer is behind our tip, and we're either noticing
            // that for the first time, OR this peer was able to catch up to some earlier point
            // where we checked against our tip.
            // Either way, set a new timeout based on current tip.
            state.m_chain_sync.m_timeout = time_in_seconds + CHAIN_SYNC_TIMEOUT;
            state.m_chain_sync.m_work_header = chainActive.Tip();
            state.m_chain_sync.m_sent_getheaders = false;
        } else if (state.m_chain_sync.m_timeout > 0 && time_in_seconds > state.m_chain_sync.m_timeout)
        {
            // No evidence yet that our peer has synced to a chain with work equal to that
            // of our tip, when we first detected it was behind. Send a single getheaders
            // message to give the peer a chance to update us.
            if (state.m_chain_sync.m_sent_getheaders)
            {
                // They've run out of time to catch up!
                NLogFormat("Disconnecting outbound peer %d for old chain, best known block = %s", pto->GetId(),
                           state.pindexBestKnownBlock != nullptr
                           ? state.pindexBestKnownBlock->GetBlockHash().ToString()
                           : "<none>");
                pto->fDisconnect = true;
            } else
            {
                NLogFormat(
                        "sending getheaders to outbound peer=%d to verify chain work (current best known block:%s, benchmark blockhash: %s)",
                        pto->GetId(),
                        state.pindexBestKnownBlock != nullptr ? state.pindexBestKnownBlock->GetBlockHash().ToString()
                                                              : "<none>",
                        state.m_chain_sync.m_work_header->GetBlockHash().ToString());
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETHEADERS,
                                                        chainActive.GetLocator(state.m_chain_sync.m_work_header->pprev),
                                                        uint256()));
                state.m_chain_sync.m_sent_getheaders = true;
                constexpr int64_t HEADERS_RESPONSE_TIME = 120; // 2 minutes
                // Bump the timeout to allow a response, which could clear the timeout
                // (if the response shows the peer has synced), reset the timeout (if
                // the peer syncs to the required work but not to our tip), or result
                // in disconnect (if we advance to the timeout and pindexBestKnownBlock
                // has not sufficiently progressed)
                state.m_chain_sync.m_timeout = time_in_seconds + HEADERS_RESPONSE_TIME;
            }
        }
    }
}

void PeerLogicValidation::EvictExtraOutboundPeers(int64_t time_in_seconds)
{
    // Check whether we have too many outbound peers
    int extra_peers = connman->GetExtraOutboundCount();
    if (extra_peers > 0)
    {
        // If we have more outbound peers than we target, disconnect one.
        // Pick the outbound peer that least recently announced
        // us a new block, with ties broken by choosing the more recent
        // connection (higher node id)
        NodeId worst_peer = -1;
        int64_t oldest_block_announcement = std::numeric_limits<int64_t>::max();

        LOCK(cs_main);

        connman->ForEachNode([&](CNode *pnode)
                             {
                                 // Ignore non-outbound peers, or nodes marked for disconnect already
                                 if (!IsOutboundDisconnectionCandidate(pnode) || pnode->fDisconnect)
                                     return;
                                 CNodeState *state = State(pnode->GetId());
                                 if (state == nullptr)
                                     return; // shouldn't be possible, but just in case
                                 // Don't evict our protected peers
                                 if (state->m_chain_sync.m_protect)
                                     return;
                                 if (state->m_last_block_announcement < oldest_block_announcement ||
                                     (state->m_last_block_announcement == oldest_block_announcement &&
                                      pnode->GetId() > worst_peer))
                                 {
                                     worst_peer = pnode->GetId();
                                     oldest_block_announcement = state->m_last_block_announcement;
                                 }
                             });
        if (worst_peer != -1)
        {
            bool disconnected = connman->ForNode(worst_peer, [&](CNode *pnode)
            {
                // Only disconnect a peer that has been connected to us for
                // some reasonable fraction of our check-frequency, to give
                // it time for new information to have arrived.
                // Also don't disconnect any peer we're trying to download a
                // block from.
                CNodeState &state = *State(pnode->GetId());
                if (time_in_seconds - pnode->nTimeConnected > MINIMUM_CONNECT_TIME && state.nBlocksInFlight == 0)
                {
                    NLogFormat(
                            "disconnecting extra outbound peer=%d (last block announcement received at time %d)",
                            pnode->GetId(), oldest_block_announcement);
                    pnode->fDisconnect = true;
                    return true;
                } else
                {
                    NLogFormat(
                            "keeping outbound peer=%d chosen for eviction (connect time: %d, blocks_in_flight: %d)",
                            pnode->GetId(), pnode->nTimeConnected, state.nBlocksInFlight);
                    return false;
                }
            });
            if (disconnected)
            {
                // If we disconnected an extra peer, that means we successfully
                // connected to at least one peer after the last time we
                // detected a stale tip. Don't try any more extra peers until
                // we next detect a stale tip, to limit the load we put on the
                // network from these extra connections.
                connman->SetTryNewOutboundPeer(false);
            }
        }
    }
}

void PeerLogicValidation::CheckForStaleTipAndEvictPeers(const Consensus::Params &consensusParams)
{
    if (connman == nullptr)
        return;

    int64_t time_in_seconds = GetTime();

    EvictExtraOutboundPeers(time_in_seconds);

    if (time_in_seconds > m_stale_tip_check_time)
    {
        LOCK(cs_main);
        // Check whether our tip is stale, and if so, allow using an extra
        // outbound peer
        if (TipMayBeStale(consensusParams))
        {
            NLogFormat(
                    "Potential stale tip detected, will try using extra outbound peer (last tip update: %d seconds ago)",
                    time_in_seconds - g_last_tip_update);
            connman->SetTryNewOutboundPeer(true);
        } else if (connman->GetTryNewOutboundPeer())
        {
            connman->SetTryNewOutboundPeer(false);
        }
        m_stale_tip_check_time = time_in_seconds + STALE_CHECK_INTERVAL;
    }
}

int GetnScore(const CService &addr)
{
    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == LOCAL_NONE)
        return 0;
    return mapLocalHost[addr].nScore;
}

void PeerLogicValidation::AdvertiseLocal(CNode *pnode)
{
    if (fListen && pnode->fSuccessfullyConnected)
    {
        CAddress addrLocal = GetLocalAddress(&pnode->addr, pnode->GetLocalServices());
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
                                           GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8 : 2) == 0))
        {
            addrLocal.SetIP(pnode->GetAddrLocal());
        }
        if (addrLocal.IsRoutable())
        {
            NLogFormat("AdvertiseLocal: advertising address %s", addrLocal.ToString());
            FastRandomContext insecure_rand;
            pnode->PushAddress(addrLocal, insecure_rand);
        }
    }
}

