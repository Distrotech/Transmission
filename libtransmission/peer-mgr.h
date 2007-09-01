/*
 * This file Copyright (C) 2007 Charles Kerr <charles@rebelbase.com>
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license. 
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#ifndef TR_PEER_MGR_H
#define TR_PEER_MGR_H

struct tr_handle;
struct tr_peer_stat;
struct in_addr;
typedef struct tr_peerMgr tr_peerMgr;

tr_peerMgr* tr_peerMgrNew( struct tr_handle * );

void tr_peerMgrFree( tr_peerMgr * manager );

int tr_peerMgrIsAcceptingConnections( const tr_peerMgr * manager );

void tr_peerMgrAddIncoming( tr_peerMgr      * manager,
                            struct in_addr  * addr,
                            int               socket );

void tr_peerMgrAddPeers( tr_peerMgr     * manager,
                         const uint8_t  * torrentHash,
                         int              from,
                         const uint8_t  * peerCompact,
                         int              peerCount );

void tr_peerMgrSetBlame( tr_peerMgr     * manager,
                         const uint8_t  * torrentHash,
                         int              pieceIndex,
                         int              success );

int tr_peerMgrGetPeers( tr_peerMgr      * manager,
                        const uint8_t   * torrentHash,
                        uint8_t        ** setme_compact );

void tr_peerMgrStartTorrent( tr_peerMgr     * manager,
                             const uint8_t  * torrentHash );

void tr_peerMgrStopTorrent( tr_peerMgr     * manager,
                            const uint8_t  * torrentHash );

void tr_peerMgrDisablePex( tr_peerMgr    * manager, 
                           const uint8_t * torrentHash,
                           int             disable );

void tr_peerMgrTorrentAvailability( const tr_peerMgr * manager,
                                    const uint8_t    * torrentHash,
                                    int8_t           * tab,
                                    int                tabCount );

void tr_peerMgrTorrentStats( const tr_peerMgr * manager,
                             const uint8_t    * torrentHash,
                             int              * setmePeersTotal,
                             int              * setmePeersConnected,
                             int              * setmePeersSendingToUs,
                             int              * setmePeersGettingFromUs,
                             int              * setmePeersFrom ); /* <-- array of TR_PEER_FROM__MAX */

struct tr_peer_stat * tr_peerMgrPeerStats( const tr_peerMgr  * manager,
                                           const uint8_t     * torrentHash,
                                           int               * setmeCount );





#endif
