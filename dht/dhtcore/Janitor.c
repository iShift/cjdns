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
#include "crypto/random/Random.h"
#include "dht/Address.h"
#include "dht/dhtcore/Janitor.h"
#include "dht/dhtcore/Node.h"
#include "dht/dhtcore/NodeList.h"
#include "dht/dhtcore/NodeHeader.h"
#include "dht/dhtcore/RouterModule.h"
#include "dht/dhtcore/SearchRunner.h"
#include "dht/dhtcore/RouteTracer.h"
#include "benc/Object.h"
#include "memory/Allocator.h"
#include "util/AverageRoller.h"
#include "util/Bits.h"
#include "util/events/EventBase.h"
#include "util/Hex.h"
#include "util/events/Timeout.h"
#include "util/events/Time.h"

#include "util/platform/libc/string.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * The goal of this is to run searches in the local area of this node.
 * it searches for hashes every localMaintainenceSearchPeriod milliseconds.
 * it runs searches by picking hashes at random, if a hash is chosen and there is a
 * non-zero-reach node which services that space, it stops. This way it will run many
 * searches early on but as the number of known nodes increases, it begins to taper off.
 */
struct Janitor
{
    struct RouterModule* routerModule;

    struct NodeStore* nodeStore;

    struct SearchRunner* searchRunner;

    struct RouteTracer* routeTracer;

    struct Timeout* timeout;

    struct Log* logger;

    uint64_t globalMaintainenceMilliseconds;
    uint64_t timeOfNextGlobalMaintainence;

    struct Allocator* allocator;

    uint64_t timeOfNextSearchRepeat;
    uint64_t searchRepeatMilliseconds;

    struct EventBase* eventBase;
    struct Random* rand;

    /** Number of concurrent searches taking place. */
    int searches;
};

struct Janitor_Search
{
    struct Janitor* janitor;

    struct Address best;

    uint8_t target[16];

    #define Janitor_Search_searchType_COMPLETE 1
    #define Janitor_Search_searchType_PARTIAL 2
    int searchType;

    struct Allocator* alloc;

    Identity
};

static void responseCallback(struct RouterModule_Promise* promise,
                             uint32_t lagMilliseconds,
                             struct Node* fromNode,
                             Dict* result)
{
    struct Janitor_Search* search = Identity_cast((struct Janitor_Search*)promise->userData);
    if (fromNode) {
        Bits_memcpyConst(&search->best, &fromNode->address, sizeof(struct Address));

        if (search->searchType == Janitor_Search_searchType_COMPLETE) {
            return;
        }
        struct Node* n = RouterModule_lookup(search->target, search->janitor->routerModule);
        if (!n || n->reach == 0) {
            return;
        } else {
            Log_debug(search->janitor->logger, "Found a nearby target, aborting search");
        }
    }

    search->janitor->searches--;

    if (!search->best.path) {
        Log_debug(search->janitor->logger, "Search completed with no nodes found");

    } else {
        // end of the line, now lets trace

        #ifdef Log_DEBUG
            uint8_t printed[60];
            Address_print(printed, &search->best);
            Log_debug(search->janitor->logger, "Tracing path to [%s]", printed);
        #endif
        RouteTracer_trace(search->best.path,
                          search->janitor->routeTracer,
                          search->janitor->allocator);
    }
    Allocator_free(search->alloc);
}

#define search_searchType_COMPLETE Janitor_Search_searchType_COMPLETE
#define search_searchType_PARTIAL Janitor_Search_searchType_PARTIAL
static void search(uint8_t target[16], struct Janitor* janitor, int searchType)
{
    if (janitor->searches >= 20) {
        Log_debug(janitor->logger, "Skipping search because 20 are in progress");
        return;
    }

    struct Allocator* searchAlloc = Allocator_child(janitor->allocator);
    struct RouterModule_Promise* rp =
        SearchRunner_search(target, janitor->searchRunner, searchAlloc);

    if (!rp) {
        Log_debug(janitor->logger, "RouterModule_search() returned NULL, probably full.");
        return;
    }

    janitor->searches++;

    struct Janitor_Search* search = Allocator_clone(rp->alloc, (&(struct Janitor_Search) {
        .janitor = janitor,
        .searchType = searchType,
        .alloc = searchAlloc,
    }));
    Identity_set(search);
    Bits_memcpyConst(search->target, target, 16);

    rp->callback = responseCallback;
    rp->userData = search;
}

static void maintanenceCycle(void* vcontext)
{
    struct Janitor* const janitor = (struct Janitor*) vcontext;

    uint64_t now = Time_currentTimeMilliseconds(janitor->eventBase);

    if (NodeStore_size(janitor->nodeStore) == 0) {
        if (now > janitor->timeOfNextGlobalMaintainence) {
            Log_warn(janitor->logger,
                     "No nodes in routing table, check network connection and configuration.");
            janitor->timeOfNextGlobalMaintainence += janitor->globalMaintainenceMilliseconds;
        }
        return;
    }

    struct Address targetAddr;

    // Ping a random node.
    struct Node* randomNode = RouterModule_getNode(0, janitor->routerModule);
    if (randomNode) {
        RouterModule_pingNode(randomNode, 0, janitor->routerModule, janitor->allocator);
    }

    // If the node's reach is zero, run a search for it, otherwise run a random search.
    int searchType;
    if (randomNode && randomNode->reach == 0) {
        searchType = search_searchType_COMPLETE;
        Bits_memcpyConst(&targetAddr, &randomNode->address, Address_SIZE);
    } else {
        searchType = search_searchType_PARTIAL;
        Random_bytes(janitor->rand, targetAddr.ip6.bytes, Address_SEARCH_TARGET_SIZE);
    }

    struct Node* n = RouterModule_lookup(targetAddr.ip6.bytes, janitor->routerModule);

    // If the best next node doesn't exist or has 0 reach, run a local maintenance search.
    if (n == NULL || n->reach == 0) {
        #ifdef Log_DEBUG
            uint8_t printable[40];
            Address_printIp(printable, &targetAddr);
            Log_debug(janitor->logger,
                       "Running search for %s, node count: %u\n",
                       printable,
                       (unsigned int) NodeStore_size(janitor->nodeStore));
        #endif

        search(targetAddr.ip6.bytes, janitor, searchType);
        return;
    }

    #ifdef Log_DEBUG
        int nonZeroNodes = NodeStore_nonZeroNodes(janitor->nodeStore);
        Log_debug(janitor->logger,
                  "Global Mean Response Time: %u non-zero nodes: [%d] zero nodes [%d] total [%d]",
                  RouterModule_globalMeanResponseTime(janitor->routerModule),
                  nonZeroNodes,
                  janitor->nodeStore->size - nonZeroNodes,
                  janitor->nodeStore->size);
    #endif

    if (now > janitor->timeOfNextGlobalMaintainence) {
        search(targetAddr.ip6.bytes, janitor, search_searchType_COMPLETE);
        janitor->timeOfNextGlobalMaintainence += janitor->globalMaintainenceMilliseconds;
    }
}

struct Janitor* Janitor_new(uint64_t localMaintainenceMilliseconds,
                            uint64_t globalMaintainenceMilliseconds,
                            struct RouterModule* routerModule,
                            struct NodeStore* nodeStore,
                            struct SearchRunner* searchRunner,
                            struct RouteTracer* routeTracer,
                            struct Log* logger,
                            struct Allocator* alloc,
                            struct EventBase* eventBase,
                            struct Random* rand)
{
    struct Janitor* janitor = Allocator_clone(alloc, (&(struct Janitor) {
        .eventBase = eventBase,
        .routerModule = routerModule,
        .nodeStore = nodeStore,
        .searchRunner = searchRunner,
        .routeTracer = routeTracer,
        .logger = logger,
        .globalMaintainenceMilliseconds = globalMaintainenceMilliseconds,
        .timeOfNextGlobalMaintainence = Time_currentTimeMilliseconds(eventBase),
        .allocator = alloc,
        .rand = rand
    }));

    janitor->timeout = Timeout_setInterval(maintanenceCycle,
                                           janitor,
                                           localMaintainenceMilliseconds,
                                           eventBase,
                                           alloc);

    return janitor;
}
