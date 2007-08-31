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

#ifndef TR_HANDSHAKE_H
#define TR_HANDSHAKE_H

enum EncryptionPreference
{
    HANDSHAKE_ENCRYPTION_PREFERRED,
    HANDSHAKE_ENCRYPTION_REQUIRED,
    HANDSHAKE_PLAINTEXT_PREFERRED,
    HANDSHAKE_PLAINTEXT_REQUIRED
};

struct tr_peerIo;
typedef void (*handshakeDoneCB)(struct tr_peerIo*, int isConnected, void*);

void tr_handshakeAdd( struct tr_peerIo * io,
                      int                encryptionPreference,
                      handshakeDoneCB    doneCB,
                      void             * doneUserData );

#endif
