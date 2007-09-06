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

#ifndef TR_P_H
#define TR_P_H

struct tr_torrent;
struct tr_peer;

typedef struct tr_peermsgs tr_peermsgs;

tr_peermsgs* tr_peerMsgsNew( struct tr_torrent  * torrent,
                             struct tr_peer     * peer );

void         tr_peerMsgsSetChoke( tr_peermsgs *, int doChoke );

void         tr_peerMsgsFree( tr_peermsgs* );

int          tr_peerMsgsAddRequest( tr_peermsgs * peer,
                                    uint32_t      index,
                                    uint32_t      begin,
                                    uint32_t      length );


#endif
