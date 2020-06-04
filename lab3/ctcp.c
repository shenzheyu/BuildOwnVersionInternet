/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include "ctcp_bbr.h"

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state
{
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;            /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *segments; /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */

  ctcp_config_t *cfg;        /* cTCP configuration for this connection state. */
  uint32_t seqno;            /* Current sequence number */
  uint32_t ackno;            /* Current ack number */
  uint16_t retransmition;    /* Current retransmition time */
  bool last_acked;           /* Whether last send segment is acknowledged */
  long last_retransmit_time; /* Last retransmit time, in ms */

  ctcp_segment_t *last_received_segment; /* Last received segment */
  ctcp_segment_t *last_send_segment;     /* Last send segment */

  bool send_fin;        /* Whether send fin or not */
  bool receive_fin;     /* Whether receive fin */
  bool receive_ack_fin; /* Whether send a ack for fin */

  linked_list_t *unacked;  /* Unacknowledged segments */
  linked_list_t *unoutput; /* Unoutput segments */

  bbr_state_t bbr;

};

struct bbr_segment
{
  ctcp_segment_t *segment;
  long send_time;
  long delivered_time;
  long delivered;
};

typedef struct bbr_segment bbr_segment_t;

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

/**
 * Computer the total data length are not acknowledged.
 */
uint16_t on_air(ctcp_state_t *state)
{
  uint16_t data_len = 0;

  if (ll_length(state->unacked) != 0)
  {
    ctcp_segment_t *front = (ctcp_segment_t *)ll_front(state->unacked)->object;
    ctcp_segment_t *back = (ctcp_segment_t *)ll_back(state->unacked)->object;
    data_len += ntohl(back->seqno) - ntohl(front->seqno);
    data_len += ntohs(back->len) - sizeof(ctcp_segment_t);
  }

  return data_len;
}

/**
 *  Check whether checksum in segment is right.
 *  If it's not right, return True.
 */
bool corrupted(ctcp_segment_t *segment)
{
  uint16_t net_cksum = segment->cksum;
  uint16_t len = ntohs(segment->len);
  segment->cksum = 0;
  uint16_t host_cksum = cksum(segment, len);
  segment->cksum = net_cksum;
  return net_cksum != host_cksum;
}

/* Send ACK segment. */
void send_ack_segment(ctcp_state_t *state)
{
  uint16_t ack_len = sizeof(ctcp_segment_t);
  ctcp_segment_t *ack = calloc(sizeof(ctcp_segment_t), 1);
  ack->seqno = htonl(state->seqno);
  ack->ackno = htonl(state->ackno);
  ack->len = htons(ack_len);
  ack->flags = 0 | htonl(ACK);
  ack->window = htons(state->cfg->recv_window);
  ack->cksum = 0;
  ack->cksum = cksum(ack, ack_len);
  conn_send(state->conn, ack, ack_len);
  free(ack);
}

/* Send FIN segment. */
ctcp_segment_t *send_fin_segment(ctcp_state_t *state)
{
  uint16_t fin_len = sizeof(ctcp_segment_t);
  ctcp_segment_t *fin = calloc(sizeof(ctcp_segment_t), 1);
  fin->seqno = htonl(state->seqno);
  fin->ackno = htonl(state->ackno);
  fin->len = htons(fin_len);
  fin->flags = 0 | htonl(FIN);
  fin->window = htons(state->cfg->recv_window);
  fin->cksum = 0;
  fin->cksum = cksum(fin, fin_len);
  conn_send(state->conn, fin, fin_len);
  return fin;
}

/* Send data segment. */
ctcp_segment_t *send_data_segment(ctcp_state_t *state, void *buf, size_t len)
{
  uint16_t seg_len = sizeof(ctcp_segment_t) + len;
  ctcp_segment_t *seg = calloc(seg_len, 1);
  seg->seqno = htonl(state->seqno);
  seg->ackno = htonl(state->ackno);
  seg->len = htons(seg_len);
  seg->flags = 0 | htonl(ACK);
  seg->window = htons(state->cfg->recv_window);
  seg->cksum = 0;
  memcpy(seg->data, buf, len);
  seg->cksum = cksum(seg, seg_len);
  conn_send(state->conn, seg, seg_len);
  return seg;
}

/* Called by the library when a new connection is made. */
ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg)
{
  /* Connection could not be established. */
  if (conn == NULL)
  {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */

  /* fprintf(stderr, "Call ctcp_init() function.\n"); */

  /* Create linked list for segments. */
  state->segments = ll_create();

  /* Set the parameters.*/
  state->cfg = cfg;
  state->ackno = 1;
  state->seqno = 1;
  state->retransmition = 0;
  state->last_acked = true;
  state->last_retransmit_time = 0;

  state->last_received_segment = NULL;
  state->last_send_segment = NULL;

  state->send_fin = false;
  state->receive_fin = false;
  state->receive_ack_fin = false;

  state->unacked = ll_create();
  state->unoutput = ll_create();

  return state;
}

/* Called when normal tear_down or other side is unresponsive. */
void ctcp_destroy(ctcp_state_t *state)
{
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */

  /* fprintf(stderr, "Call ctcp_destroy() function.\n"); */

  /* Free last send segment. */
  if (state->last_send_segment != NULL)
  {
    free(state->last_send_segment);
  }

  /* Free last receive segment. */
  if (state->last_received_segment != NULL)
  {
    free(state->last_received_segment);
  }

  /* Destroy unacked linked list. */
  while (ll_length(state->unacked) != 0)
  {
    ll_node_t *front = ll_front(state->unacked);
    free((ctcp_segment_t *)ll_remove(state->unacked, front));
  }
  ll_destroy(state->unacked);

  /* Destroy unoutoput linked list. */
  while (ll_length(state->unoutput) != 0)
  {
    ll_node_t *front = ll_front(state->unoutput);
    free((ctcp_segment_t *)ll_remove(state->unoutput, front));
  }
  ll_destroy(state->unoutput);

  free(state);
  end_client();
}

/* Called if there is input to be read. */
void ctcp_read(ctcp_state_t *state)
{
  /* FIXME */

  /* fprintf(stderr, "[cTCP] Call ctcp_read() function.\n"); */

  /* If data length in unacked linked list is equal to send window size, don't send new one. */
  if (state->cfg->send_window <= on_air(state))
  {
    return;
  }

  /* If already send FIN segment, don't send new one. */
  if (state->send_fin)
  {
    return;
  }

  /* Call conn_input() with a buffer of the correct size. */
  uint16_t buf_len = MAX_SEG_DATA_SIZE;
  char buf[buf_len];
  memset(buf, 0, buf_len);

  int len = conn_input(state->conn, &buf, buf_len);

  /* If no data is available, return 0. */
  if (len == 0)
  {
    return;
  }

  /* When it reads an EOF, return -1. And send a FIN to the other side. Then, 
  destroy any connection state.*/
  if (len == -1)
  {
    ctcp_segment_t *fin = send_fin_segment(state);

    state->seqno += 1;
    state->send_fin = true;
    state->retransmition = 0;
    state->last_retransmit_time = current_time();

    bbr_segment_t *bbr_fin = calloc(1, sizeof(bbr_segment_t));
    bbr_fin->segment = fin;
    bbr_fin->send_time = current_time();
    bbr_fin->delivered_time = state->bbr->

    ll_add(state->unacked, fin);

    return;
  }

  /* fprintf(stderr, "Prepare to send a data segment.\n"); */

  /* Create a segment from the input and send it to the connection. */
  ctcp_segment_t *segment = send_data_segment(state, &buf, len);
  fprintf(stderr, "Send:");
  print_hdr_ctcp(segment);

  state->seqno += len;
  state->retransmition = 0;
  state->last_retransmit_time = current_time();
  ll_add(state->unacked, segment);
}

/* Called by the library when a segment is received. */
void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
  /* FIXME */

  fprintf(stderr, "[cTCP] Receive a segment.");

  print_hdr_ctcp(segment);
  /* fprintf(stderr, "[RECEIVE] Data in segment is: %s\n", segment->data); */

  /* Ignore the corrupted segment. */
  if (corrupted(segment))
  {
    /* fprintf(stderr, "The segment is corrupted.\n"); */

    return;
  }

  /* Deal with the stale segment. */
  if (ntohl(segment->seqno) < state->ackno)
  {
    /* fprintf(stderr, "[Receive] Deal with the stale segment.\n"); */

    if (ntohs(segment->len) - sizeof(ctcp_segment_t) > 0 || (segment->flags & htonl(FIN)) != 0)
    {
      free(segment);
      send_ack_segment(state);
      return;
    }
  }

  /* Deal with duplicate segment issue. */
  if (ll_length(state->unoutput) != 0)
  {
    ll_node_t *unoutput_walker = ll_front(state->unoutput);
    uint16_t ind = ll_length(state->unoutput);
    while (ind > 0)
    {
      ctcp_segment_t *unoutput_segment = (ctcp_segment_t *)unoutput_walker->object;
      if (ntohl(unoutput_segment->seqno) == ntohl(segment->seqno))
      {
        /* fprintf(stderr, "[Receive] Dectect duplicate segment.\n"); */

        free(segment);
        send_ack_segment(state);
        return;
      }
      unoutput_walker = unoutput_walker->next;
      ind -= 1;
    }
  }

  /* Deal with ACK segment. */
  if ((segment->flags & htonl(ACK)) != 0)
  {
    /* fprintf(stderr, "[Receive] Deal with ACK segment.\n"); */

    /* Ignore stale ACK segment. */
    /* 
    if (ll_length(state->unacked) != 0)
    {
      if (ntohl(segment->ackno) <= ntohl(((ctcp_segment_t *)ll_front(state->unacked)->object)->seqno))
      {
        fprintf(stderr, "The ACK segment is stale.\n");
        free(segment);
        return;
      }
    }
    */


    /* Remove segments in unacked which have been acked. */
    while (ll_length(state->unacked) != 0)
    {
      ll_node_t *unacked_walker = ll_front(state->unacked);
      ctcp_segment_t *unacked_segment = (ctcp_segment_t *)unacked_walker->object;

      if (ntohl(unacked_segment->seqno) < ntohl(segment->ackno))
      {
        free(unacked_segment);
        unacked_walker->object = NULL;
        ll_remove(state->unacked, unacked_walker);
      }
      else
      {
        break;
      }
    }

    /* If have send fin, set receive ack for fin to true. */
    if (state->send_fin)
    {
      /* fprintf(stderr, "Has receive ack for my fin.\n"); */

      state->receive_ack_fin = true;
    }
  }

  /* If receive FIN segment, send ACK and FIN_ACK and set send_FIN_ACK parameter. */
  if ((segment->flags & htonl(FIN)) != 0)
  {
    /* fprintf(stderr, "[Receive] Deal with the FIN segment.\n"); */

    state->receive_fin = true;
  }


  /* Insert segment with data into unoutput. */
  if (ntohs(segment->len) - sizeof(ctcp_segment_t) > 0 || (segment->flags & htonl(FIN)) != 0)
  {
    /* fprintf(stderr, "[Receive] Insert segment with data or FIN flag into unoutput.\n"); */
    if (ll_length(state->unoutput) != 0)
    {
      ll_node_t *first_unoutput_node = ll_front(state->unoutput);
      ll_node_t *last_unoutput_node = ll_back(state->unoutput);
      ctcp_segment_t *first_unoutput_segment = (ctcp_segment_t *)first_unoutput_node->object;
      ctcp_segment_t *last_unoutput_segment = (ctcp_segment_t *)last_unoutput_node->object;
      if (ntohl(segment->seqno) < ntohl(first_unoutput_segment->seqno))
      {
        ll_add_front(state->unoutput, segment);
      }
      else if (ntohl(segment->seqno) > ntohl(last_unoutput_segment->seqno))
      {
        ll_add(state->unoutput, segment);
      }
      else
      {
        ll_node_t *current_node = first_unoutput_node;
        ll_node_t *next_node = current_node;
        ctcp_segment_t *current_segment = (ctcp_segment_t *)current_node->object;
        ctcp_segment_t *next_segment = (ctcp_segment_t *)next_node->object;
        while (current_node != last_unoutput_node)
        {
          next_node = current_node->next;

          current_segment = (ctcp_segment_t *)current_node->object;
          next_segment = (ctcp_segment_t *)next_node->object;
          if (ntohl(segment->seqno) > ntohl(current_segment->seqno) && ntohl(segment->seqno) < ntohl(next_segment->seqno))
          {
            ll_add_after(state->unoutput, current_node, segment);
          }

          current_node = next_node;
        }
      }
    }
    else
    {
      ll_add(state->unoutput, segment);
    }
  }

  /* fprintf(stderr, "[Receive] Call ctp_output().\n"); */

  /* Output segment's data. */
  ctcp_output(state);
}

/* Called by ctcp_receive() if a segment is ready to outputted. */
void ctcp_output(ctcp_state_t *state)
{
  /* FIXME */

  /* fprintf(stderr, "[cTCP] Call ctcp_output() function.\n"); */

  bool has_output = false;

  while (ll_length(state->unoutput) != 0)
  {
    ll_node_t *unoutput_node = ll_front(state->unoutput);
    ctcp_segment_t *unoutput_segment = (ctcp_segment_t *)unoutput_node->object;
    if (state->ackno == ntohl(unoutput_segment->seqno))
    {
      /* Call conn_bufspace() to see how many bytes can be outputted to STDOUT */
      size_t buf_space = conn_bufspace(state->conn);

      /* If there is no room, wait for next time. */
      uint16_t data_len = ntohs(unoutput_segment->len) - sizeof(ctcp_segment_t);
      if (buf_space < data_len)
      {
        return;
      }

      conn_output(state->conn, unoutput_segment->data, data_len);

      /* Set ackno. */
      state->ackno += data_len;
      has_output = true;

      if ((unoutput_segment->flags & htonl(FIN)) != 0)
      {
        state->ackno += 1;
        conn_output(state->conn, NULL, 0);
      }

      /* Free segment after output. */
      free(unoutput_segment);
      unoutput_node->object = NULL;
      ll_remove(state->unoutput, unoutput_node);
    }
    else
    {
      break;
    }
  }

  if (has_output)
  {
    send_ack_segment(state);
  }
}

/* Called periodically at specified rate. */
void ctcp_timer()
{
  /* FIXME */

  /* Deal with empty state_list. */
  if (state_list == NULL)
  {
    /* fprintf(stderr, "State list is empty.\n"); */
    return;
  }

  /* Iterate state_list using state_walker. */
  ctcp_state_t *state_walker = state_list;
  ctcp_state_t *state_next = state_walker->next;

  while (state_walker != NULL)
  {
    state_next = state_walker->next;

    /* If unacked list is not empty. */
    if (ll_length(state_walker->unacked) != 0)
    {
      /* Assume the other end of the connection is unresponsive. */
      if (state_walker->retransmition == 5)
      {
        /* fprintf(stderr, "[Timer] Assume the other end of the connection is unresponsive.\n"); */
        ctcp_destroy(state_walker);
      }

      /* Check whether it achives rt_timeout. */
      long rt_timeout = state_walker->cfg->rt_timeout;
      if (current_time() - state_walker->last_retransmit_time >= rt_timeout)
      {
        /* Resend the first unacked segment. */
        ll_node_t *first_unacked_node = ll_front(state_walker->unacked);
        ctcp_segment_t *first_unacked_segment = (ctcp_segment_t *)first_unacked_node->object;
        uint16_t len = ntohs(first_unacked_segment->len);
        conn_send(state_walker->conn, first_unacked_segment, len);
        state_walker->retransmition += 1;
        state_walker->last_retransmit_time = current_time();
      }
    }

    /* Ater send an FIN, receive an ACK for the send FIN, and receive a FIN, could normaly tear down the connection. */
    if (state_walker->send_fin && state_walker->receive_ack_fin && state_walker->receive_fin)
    {
      /* fprintf(stderr, "[Timer] Output all unoutput segment and wait for tear down.\n"); */

      while (ll_length(state_walker->unoutput) != 0)
      {
        ll_node_t *unoutput_node = ll_front(state_walker->unoutput);
        ctcp_segment_t *unoutput_segment = (ctcp_segment_t *)unoutput_node->object;
        uint16_t len = ntohs(unoutput_segment->len);
        conn_send(state_walker->conn, unoutput_segment, len);
        free(unoutput_segment);
        unoutput_node->object = NULL;
        ll_remove(state_walker->unoutput, unoutput_node);
      }

      ctcp_destroy(state_walker);
    }

    state_walker = state_next;
  }
}