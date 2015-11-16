#include <msp430.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include <wisp-base.h>
#include <libchain/chain.h>
#include <libio/log.h>

#ifdef CONFIG_LIBEDB_PRINTF
#include <libedb/edb.h>
#endif

#include "pins.h"

#define NIL 0 // like NULL, but for indexes, not real pointers
#define ROOT_IDX 0 // index of the root node in the dictionary tree

#define DICT_SIZE 128
#define BLOCK_SIZE 8

#define DELAY() do { \
    uint32_t delay = 0x2ffff; \
    while (delay--); \
 } while (0);

#ifdef CONT_POWER
#define TASK_PROLOGUE() DELAY()
#else // !CONT_POWER
#define TASK_PROLOGUE()
#endif // !CONT_POWER


typedef unsigned index_t;
typedef unsigned letter_t;

// NOTE: can't use pointers, since need to ChSync, etc
typedef struct _node_t {
    letter_t letter; // 'letter' of the alphabet
    index_t sibling; // this node is a member of the parent's children list
    index_t child;   // link-list of children
} node_t;

struct msg_dict {
    CHAN_FIELD_ARRAY(node_t, dict, DICT_SIZE);
};

struct msg_root {
    CHAN_FIELD_ARRAY(node_t, dict, 1); // like, dict, but only with root node
};

struct msg_compressed_data {
    CHAN_FIELD_ARRAY(node_t, compressed_data, BLOCK_SIZE);
};

struct msg_index {
    CHAN_FIELD(index_t, index);
};

struct msg_parent {
    CHAN_FIELD(index_t, parent);
};

struct msg_sibling {
    CHAN_FIELD(index_t, sibling);
};

struct msg_self_sibling {
    SELF_CHAN_FIELD(index_t, sibling);
};
#define FIELD_INIT_msg_self_sibling {\
    SELF_FIELD_INITIALIZER \
}

struct msg_letter {
    CHAN_FIELD(letter_t, letter);
};

struct msg_parent_node {
    CHAN_FIELD(node_t, parent_node);
};

struct msg_last_sibling {
    CHAN_FIELD(index_t, sibling);
    CHAN_FIELD(node_t, sibling_node);
};

struct msg_child {
    CHAN_FIELD(index_t, child);
};

struct msg_parent_info {
    CHAN_FIELD(index_t, parent);
    CHAN_FIELD(node_t, parent_node);
};

struct msg_node_count {
    CHAN_FIELD(index_t, node_count);
};

struct msg_self_node_count {
    SELF_CHAN_FIELD(index_t, node_count);
};
#define FIELD_INIT_msg_self_node_count {\
    SELF_FIELD_INITIALIZER \
}

struct msg_out_len {
    CHAN_FIELD(index_t, out_len);
};

struct msg_self_out_len {
    SELF_CHAN_FIELD(index_t, out_len);
};
#define FIELD_INIT_msg_self_out_len {\
    SELF_FIELD_INITIALIZER \
}

struct msg_symbol {
    CHAN_FIELD(index_t, symbol);
};

TASK(1, task_init)
TASK(2, task_sample)
TASK(3, task_compress)
TASK(4, task_find_sibling)
TASK(5, task_add_node)
TASK(6, task_add_insert)
TASK(7, task_append_compressed)
TASK(8, task_print)

CHANNEL(task_init, task_compress, msg_parent);
CHANNEL(task_init, task_find_sibling, msg_root);
MULTICAST_CHANNEL(msg_root, ch_root, task_init,
                  task_compress, msg_root);
CHANNEL(task_init, task_add_insert, msg_node_count);
CHANNEL(task_init, task_append_compressed, msg_out_len);
MULTICAST_CHANNEL(msg_dict, ch_dict, task_add_insert,
                  task_compress, task_find_sibling, task_add_node);
MULTICAST_CHANNEL(msg_letter, ch_letter, task_sample,
                  task_find_sibling, task_add_insert);
CHANNEL(task_compress, task_add_insert, msg_parent_info);
CHANNEL(task_compress, task_find_sibling, msg_child);
MULTICAST_CHANNEL(msg_parent, ch_parent, task_compress,
                  task_add_insert, task_append_compressed);
MULTICAST_CHANNEL(msg_sibling, ch_sibling, task_compress,
                  task_find_sibling, task_add_node);
CHANNEL(task_find_sibling, task_compress, msg_parent);
SELF_CHANNEL(task_find_sibling, msg_self_sibling);
SELF_CHANNEL(task_add_node, msg_self_sibling);
CHANNEL(task_add_node, task_add_insert, msg_last_sibling);
SELF_CHANNEL(task_add_insert, msg_self_node_count);
CHANNEL(task_add_insert, task_append_compressed, msg_symbol);
SELF_CHANNEL(task_append_compressed, msg_self_out_len);
CHANNEL(task_append_compressed, task_print, msg_compressed_data);

void init()
{
    WISP_init();

    GPIO(PORT_LED_1, DIR) |= BIT(PIN_LED_1);
    GPIO(PORT_LED_2, DIR) |= BIT(PIN_LED_2);
#if defined(PORT_LED_3)
    GPIO(PORT_LED_3, DIR) |= BIT(PIN_LED_3);
#endif

    INIT_CONSOLE();

    __enable_interrupt();

#if defined(PORT_LED_3) // when available, this LED indicates power-on
    GPIO(PORT_LED_3, OUT) |= BIT(PIN_LED_3);
#endif

    PRINTF(".%u.\r\n", curctx->task->idx);
}

void task_init()
{
    TASK_PROLOGUE();
    LOG("init\r\n");

    index_t root = ROOT_IDX;
    node_t root_node = {
        .letter = 0, // undefined
        .sibling = NIL, // no siblings ever
        .child = NIL, // init an empty list for children
    };
    CHAN_OUT1(index_t, dict[ROOT_IDX], root_node,
              MC_OUT_CH(ch_root, task_init, task_compress, task_add_node));

    // Assume that all data starts with a fixed letter (0). This
    // removes the need to have an out-of-band call to sample.
    index_t prefix_letter_index = 0;
    CHAN_OUT1(index_t, parent, prefix_letter_index, CH(task_init, task_compress));

    index_t node_count = 1; // count root node
    CHAN_OUT1(index_t, node_count, node_count, CH(task_init, task_add_insert));

    index_t out_len = 0;
    CHAN_OUT1(index_t, out_len, out_len, CH(task_init, task_append_compressed));

    TRANSITION_TO(task_sample);
}

void task_sample()
{
    TASK_PROLOGUE();

    letter_t sample = rand() & 0x0F; // TODO: replace with temp sensor reading

    LOG("sample: %u\r\n", sample);

    CHAN_OUT1(letter_t, letter, sample,
              MC_OUT_CH(ch_letter, task_sample,
                        task_find_sibling, task_add_insert));

    TRANSITION_TO(task_compress);
}

void task_compress()
{
    TASK_PROLOGUE();

    node_t parent_node;

    // pointer into the dictionary tree; starts at a root's child
    index_t parent = *CHAN_IN2(index_t, parent,
                               CH(task_init, task_compress),
                               CH(task_find_sibling, task_compress));

    LOG("compress: parent %u\r\n", parent);

    // We special-case the root for optimization reason:
    // (1) to avoid having to allocate a whole array for the init->find_sibling
    // channel, of which only root would be ever used, and (2) to avoid having
    // to sync for every non-root node. If it weren't for (1), we would
    // probably forgo the optimization, and sync for sake of code simplicity.
    // But (1) is too costly.
    if (parent == ROOT_IDX) {
        // NOTE: we still need to sync for root because root is subject
        // to modification as children are added, just like any other node.
        // NOTE: init->find_sibling channel has allocated array field of size 1
        parent_node = *CHAN_IN2(node_t, dict[parent],
                                       MC_IN_CH(ch_root, task_init, task_compress),
                                       MC_IN_CH(ch_dict, task_add_insert,
                                                task_compress));
    } else { // normal case
        parent_node = *CHAN_IN1(node_t, dict[parent],
                                MC_IN_CH(ch_dict, task_add_insert,
                                         task_compress));
    }

    LOG("compress: parent node: l %u s %u c %u\r\n",
        parent_node.letter, parent_node.sibling, parent_node.child);

    CHAN_OUT1(index_t, sibling, parent_node.child,
              MC_OUT_CH(ch_sibling, task_compress,
                        task_find_sibling, task_add_node));

    // Send a full node instead of only the index to avoid the need for
    // task_add to channel the dictionary to self and thus avoid
    // duplicating the memory for the dictionary (premature opt?).
    // In summary: instead of self-channeling the whole array, we
    // proxy only one element of the array.
    //
    // NOTE: source of inefficiency: we execute this on every step of traversal
    // over the nodes in the tree, but really need this only for the last one.
    CHAN_OUT1(node_t, parent_node, parent_node,
              CH(task_compress, task_add_insert));
    CHAN_OUT1(index_t, parent, parent,
              CH(task_compress, task_add_insert));

    CHAN_OUT1(index_t, child, parent_node.child, CH(task_compress, task_find_sibling));

    TRANSITION_TO(task_find_sibling);
}

void task_find_sibling()
{
    DELAY();

    index_t sibling = *CHAN_IN2(index_t, sibling,
                         MC_IN_CH(ch_sibling, task_compress, task_find_sibling),
                         SELF_IN_CH(task_find_sibling));

    index_t letter = *CHAN_IN1(letter_t, letter,
                               MC_IN_CH(ch_letter, task_sample,
                                        task_find_sibling));

    LOG("find sibling: l %u s %u\r\n", letter, sibling);

    if (sibling != NIL) {

        node_t *sibling_node = CHAN_IN1(node_t, dict[sibling],
                                MC_IN_CH(ch_dict, task_add_insert, task_find_sibling));

        LOG("find sibling: l %u, sn: l %u s %u %c\r\n", letter,
            sibling_node->letter, sibling_node->sibling, sibling_node->child);

        if (sibling_node->letter == letter) { // found
            CHAN_OUT1(index_t, parent, sibling,
                      CH(task_find_sibling, task_compress));
            TRANSITION_TO(task_sample);
        } else { // continue traversing the siblings
            CHAN_OUT1(index_t, sibling, sibling_node->sibling,
                      SELF_OUT_CH(task_find_sibling));
            TRANSITION_TO(task_find_sibling);
        }

    } else { // not found in any of the siblings

        LOG("find sibling: not found\r\n");

        // Reset the node pointer into the dictionary tree to point to the
        // root's child corresponding to the letter we are about to insert
        // NOTE: The cast here relies on the fact that root's children are
        // initialized in by inserting them in order of the letter value.
        index_t starting_node_idx = (index_t)letter;
        CHAN_OUT1(index_t, parent, starting_node_idx,
                  CH(task_find_sibling, task_compress));

        // Add new node to dictionary tree, and, after that, append the
        // compressed symbol to the result
        //
        index_t child = *CHAN_IN1(index_t, child,
                                  CH(task_compress, task_find_sibling));
        LOG("find sibling: child %u\r\n", child);
        if (child == NIL) {
            TRANSITION_TO(task_add_insert);
        } else {
            TRANSITION_TO(task_add_node); 
        }
    }
}

void task_add_node()
{
    TASK_PROLOGUE();

    index_t sibling = *CHAN_IN2(index_t, sibling,
                            MC_IN_CH(ch_sibling, task_compress, task_add_node),
                            SELF_IN_CH(task_add_node));

    node_t *sibling_node = CHAN_IN1(node_t, dict[sibling],
                             MC_IN_CH(ch_dict, task_add_insert, task_add_node));

    LOG("add node: s %u, sn: l %u s %u c %u\r\n", sibling,
        sibling_node->letter, sibling_node->sibling, sibling_node->child);

    if (sibling_node->sibling != NIL) {

        index_t next_sibling = sibling_node->sibling;
        CHAN_OUT1(index_t, sibling, next_sibling, SELF_OUT_CH(task_add_node));
        TRANSITION_TO(task_add_node);

    } else { // found last sibling in the list

        LOG("add node: found last\r\n");

        node_t sibling_node_obj = *sibling_node;

        CHAN_OUT1(index_t, sibling, sibling,
                  CH(task_add_node, task_add_insert));
        CHAN_OUT1(node_t, sibling_node, sibling_node_obj,
                  CH(task_add_node, task_add_insert));

        TRANSITION_TO(task_add_insert);
    }
}

void task_add_insert()
{
    TASK_PROLOGUE();

    index_t node_count = *CHAN_IN2(index_t, node_count,
                              CH(task_init, task_add_insert),
                              SELF_IN_CH(task_add_insert));

    LOG("add insert: nodes %u\r\n", node_count);

    if (node_count == DICT_SIZE) { // wipe the table if full

        LOG("add insert: dict full\r\n");

        // ... reset node count
        node_count = 1; // count root node

        // ... init root node
        node_t root_node = {
            .letter = 0, // undefined
            .sibling = NIL, // no siblings ever
            .child = NIL, // init an empty list for children
        };
        CHAN_OUT1(node_t, dict[ROOT_IDX], root_node,
                  MC_OUT_CH(ch_dict, task_add_insert,
                            task_compress, task_find_sibling, task_add_node));

        TRANSITION_TO(task_append_compressed);
    }

    index_t parent = *CHAN_IN1(index_t, parent,
                           CH(task_compress, task_add_insert));
    node_t *parent_node = CHAN_IN1(node_t, parent_node,
                                   CH(task_compress, task_add_insert));

    index_t letter = *CHAN_IN1(letter_t, letter,
                           MC_IN_CH(ch_letter, task_sample, task_add_insert));

    LOG("add insert: l %u p %u, pn l %u s %u c%u\r\n", letter, parent,
        parent_node->letter, parent_node->sibling, parent_node->child);

    index_t child = node_count;
    node_t child_node = {
        .letter = letter,
        .sibling = NIL,
        .child = NIL,
    };

    if (parent_node->child == NIL) { // the only child

        LOG("add insert: only child\r\n");

        node_t parent_node_obj = *parent_node;
        parent_node_obj.child = child;

        CHAN_OUT1(node_t, dict[parent], parent_node_obj,
                  MC_OUT_CH(ch_dict, task_add_insert,
                            task_compress, task_find_sibling, task_add_node));

    } else { // a sibling

        index_t last_sibling = *CHAN_IN1(index_t, sibling,
                                         CH(task_add_node, task_add_insert));

        node_t last_sibling_node = *CHAN_IN1(node_t, sibling_node,
                                             CH(task_add_node, task_add_insert));

        LOG("add insert: sibling %u\r\n", last_sibling);

        last_sibling_node.sibling = child;

        CHAN_OUT1(node_t, dict[last_sibling], last_sibling_node, 
                  MC_OUT_CH(ch_dict, task_add_insert,
                            task_compress, task_find_sibling, task_add_node));
    }

    CHAN_OUT1(node_t, dict[child], child_node,
              MC_OUT_CH(ch_dict, task_add_insert,
                        task_compress, task_find_sibling, task_add_node));

    // A shortcut instead of initializing dict explicitly, i.e. by adding
    // each letter as a child to the root. In the shortcut version, if we're
    // adding an child to root here, then, in an initialized table, this child
    // would have been the leaf (what we call 'parent' here). The leaf index
    // is the compressed symbol.
    index_t symbol = parent == ROOT_IDX ? child : parent;

    CHAN_OUT1(index_t, symbol, symbol,
              CH(task_add_insert, task_append_compressed));

    node_count++;

    CHAN_OUT1(index_t, node_count, node_count, SELF_OUT_CH(task_add_insert));

    TRANSITION_TO(task_append_compressed);
}

void task_append_compressed()
{
    TASK_PROLOGUE();

    index_t symbol = *CHAN_IN1(index_t, symbol,
                               CH(task_add_insert, task_append_compressed));

    // TODO: encode with huffman code here

    unsigned out_len = *CHAN_IN2(unsigned, out_len,
                                 CH(task_init, task_append_compressed),
                                 SELF_IN_CH(task_append_compressed));

    LOG("append comp: sym %u len %u \r\n", symbol, out_len);

    CHAN_OUT1(index_t, compressed_data[out_len], symbol,
              CH(task_append_compressed, task_print));

    if (out_len == BLOCK_SIZE) {
        out_len = 0;
        CHAN_OUT1(unsigned, out_len, out_len,
                  SELF_OUT_CH(task_append_compressed));
        TRANSITION_TO(task_print);
    } else {
        out_len++;
        CHAN_OUT1(unsigned, out_len, out_len,
                  SELF_OUT_CH(task_append_compressed));
        TRANSITION_TO(task_sample);
    }
}

void task_print()
{
    TASK_PROLOGUE();

    unsigned i;

    BLOCK_PRINTF_BEGIN();
    BLOCK_PRINTF("compressed block: ");
    for (i = 0; i < BLOCK_SIZE; ++i) {
        index_t index = *CHAN_IN1(index_t, compressed_data[i],
                                  CH(task_append_compressed, task_print));
        BLOCK_PRINTF("%04u ", index);
    }
    BLOCK_PRINTF("\r\n");
    BLOCK_PRINTF_END();

    TRANSITION_TO(task_sample);
}

ENTRY_TASK(task_init)
INIT_FUNC(init)
