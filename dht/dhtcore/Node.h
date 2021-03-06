/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef Node_H
#define Node_H

#include "dht/Address.h"
#include "switch/EncodingScheme.h"
#include "memory/Allocator.h"
#include "util/Assert.h"
#include "util/Identity.h"

/** A network address for reaching a peer, in the format which is sent over the wire. */
struct Node
{
    /**
     * The reach of the node (how big/fast/close it is).
     * Since reach is a fraction, the reach number represents a percentage where 0xFFFFFFFF = 100%
     */
    uint32_t reach;

    /** The version of the node, must be synchronized with NodeHeader */
    uint32_t version;

    /** The address of the node. */
    struct Address address;

    /**
     * If we lookup a node and the current time is later than this, ping it.
     * In ms, as per Time_currentTimeMilliseconds.
     */
    uint64_t timeOfNextPing;

    /**
     * Used to count the number of consecutive missed pings when testing reach.
     * Not allowing 1 or 2 before penalizing was causing us to switch paths too often,
     * leading to latency spikes.
     */
    uint8_t missedPings;
};

struct Node_Link;

struct Node_Two
{
    /**
     * The reach of the node (how big/fast/close it is).
     * Since reach is a fraction, the reach number represents a percentage where 0xFFFFFFFF = 100%
     */
    uint32_t reach;

    /** The version of the node, must be synchronized with NodeHeader */
    uint32_t version;

    /** The address of the node. */
    struct Address address;

    /**
     * If we lookup a node and the current time is later than this, ping it.
     * In ms, as per Time_currentTimeMilliseconds.
     */
    uint64_t timeOfNextPing;

    // new stuff

    /** The encoding method used by this node. */
    struct EncodingScheme* encodingScheme;

    /**
     * Peers of this node for which we know the forward direction.
     * Use RB_NFIND(PeerRBTree, node->peerTree, struct type* elm)
     */
    struct PeerRBTree {
        struct Node_Link* rbh_root;
    } peerTree;

    /** Used for freeing the links associated with this node. */
    struct Node_Link* reversePeers;

    struct Allocator* alloc;

    Identity
};

// Make sure Node_Two is castable to Node
#define Node_ASSERT_MATCHES(field) \
    Assert_compileTime(offsetof(struct Node_Two, field) == offsetof(struct Node, field))
Node_ASSERT_MATCHES(reach);
Node_ASSERT_MATCHES(version);
Node_ASSERT_MATCHES(address);
Node_ASSERT_MATCHES(timeOfNextPing);
#undef Node_ASSERT_MATCHES

/**
 * A link represents a link between two nodes.
 * Links are unidirectional because deriving the inverse of a route is non-trivial.
 * (it cannot be calculated)
 */
struct Node_Link
{
    /** Used by the parent's RBTree of links. */
    struct {
        struct Node_Link* rbe_left;
        struct Node_Link* rbe_right;
        struct Node_Link* rbe_parent;
        int rbe_color;
    } peerTree;

    /**
     * The Encoding Form number which is used to represent the first director in the path from
     * child to parent.
     */
    int encodingFormNumber;

    /**
     * The quality of the link between parent and child,
     * between 0xFFFFFFFF (perfect) and 0 (intolerable).
     */
    uint32_t linkState;

    /** The parent of this peer, this is where the root of the RBTree is. */
    struct Node_Two* parent;

    /** The child of this link. */
    struct Node_Two* child;

    /**
     * The next link which points to the same child.
     * For each child there are many links pointing to it,
     * they are represented here as a linked list.
     */
    struct Node_Link* nextPeer;

    /**
     * The label which would be used to reach the child from the parent.
     * This label is in a cannonical state and must be altered so that the first Director uses
     * at least as many bits as are required to reach the grandparent from the parent
     * in the reverse direction.
     */
    uint64_t cannonicalLabel;

    /** The path which the incoming packet followed when this node was discovered. */
    uint64_t discoveredPath;

    unsigned long linkAddr;

    Identity
};

#endif
