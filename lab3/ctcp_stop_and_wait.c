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
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

/* Check whether checksum in segment is right.
 * If it's not right, return True.
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
  /* memcpy(ack->data, NULL, 0); */
  ack->cksum = cksum(ack, ack_len);
  conn_send(state->conn, ack, ack_len);

  /* fprintf(stderr, "Send an ACK segment.\n"); */
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
  /* memcpy(fin->data, NULL, 0); */
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

  free(state);
  end_client();
}

/* Called if there is input to be read. */
void ctcp_read(ctcp_state_t *state)
{
  /* FIXME */

  /* fprintf(stderr, "Call ctcp_read() function.\n"); */

  /* If last send segment is not acknowledged, don't send new one. */
  if (!state->last_acked)
  {
    return;
  }

  /* If already send FIN segment, don't send new one. */
  if (state->send_fin)
  {
    return;
  }

  /* Call conn_input() with a buffer of the correct size. */
  char buf[MAX_SEG_DATA_SIZE];
  memset(buf, 0, MAX_SEG_DATA_SIZE);

  int len = conn_input(state->conn, &buf, MAX_SEG_DATA_SIZE);

  /* fprintf(stderr, "Read in len is %d\n", len); */

  /* If no data is available, return 0. */
  if (len == 0)
  {
    /* fprintf(stderr, "No data is available.\n"); */
    return;
  }

  /* When it reads an EOF, return -1. And send a FIN to the other side. Then, 
  destroy any connection state.*/
  if (len == -1)
  {
    /* fprintf(stderr, "Read an EOF.\n"); */

    /* fprintf(stderr, "Send a FIN segment to the other side.\n"); */

    ctcp_segment_t *fin = send_fin_segment(state);

    state->send_fin = true;
    state->last_send_segment = fin;
    state->retransmition = 0;
    state->last_acked = false;
    state->last_retransmit_time = current_time();
    return;
  }

  /* fprintf(stderr, "Prepare to send a data segment.\n"); */

  /* Create a segment from the input and send it to the connection. */
  ctcp_segment_t *segment = send_data_segment(state, &buf, len);

  /* fprintf(stderr, "Send a data segment.\n"); */
  /* print_hdr_ctcp(segment); */

  /* Set parameter for ctcp state*/
  state->last_send_segment = segment;
  state->retransmition = 0;
  state->last_acked = false;
  state->last_retransmit_time = current_time();
}

/* Called by the library when a segment is received. */
void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
  /* FIXME */

  /* fprintf(stderr, "Call ctcp_receive() function.\n"); */

  /* fprintf(stderr, "Receive a segment.\n");*/

  /* print_hdr_ctcp(segment); */
  /* fprintf(stderr, "Data in segment is: %s\n", segment->data); */

  /* Ignore the corrupted segment. */
  if (corrupted(segment))
  {
    /* fprintf(stderr, "The segment is corrupted.\n"); */

    return;
  }

  /* Deal with duplicate segment issue. */
  if (state->last_received_segment != NULL)
  {
    /* Ignore duplicate segment. */
    if (state->last_received_segment->cksum == segment->cksum)
    {

      /* fprintf(stderr, "Detect duplicate segment.\n"); */

      /* If segment contains data or FIN flag, send ACK segment and return. */
      if (ntohs(segment->len) - sizeof(ctcp_segment_t) > 0 || (segment->flags & htonl(FIN)) != 0)
      {
        /* fprintf(stderr, "Send ACK segment for duplicate segment.\n"); */

        send_ack_segment(state);
      }
      return;
    }
    else
    {
      free(state->last_received_segment);
      state->last_received_segment = NULL;
    }
  }

  /* If receive ACK segment, set the parameter for ctcp state. */
  if ((segment->flags & htonl(ACK)) != 0)
  {
    /* fprintf(stderr, "Receive an ACK segment.\n"); */

    /* Ignore stale ACK segment. */
    if (ntohl(segment->ackno) < state->seqno)
    {
      /* fprintf(stderr, "The ACK segment is stale.\n"); */

      return;
    }

    /* Set seqno to be the ackno in ACK segment. */
    state->seqno = ntohl(segment->ackno);

    /* Set last send segment to be acknowledged, so that to send next segment. */
    if (state->last_send_segment != NULL)
    {
      /* fprintf(stderr, "Try to free(last_send_segment).\n"); */

      free(state->last_send_segment);
      state->last_send_segment = NULL;
      state->last_acked = true;
    }

    /* If have send fin, set receive ack for fin to true. */
    if (state->send_fin)
    {
      state->receive_ack_fin = true;
    }
  }

  /* fprintf(stderr, "Update last received segment.\n"); */

  /* Update last received segment. */
  state->last_received_segment = segment;

  /* If receive FIN segment, send ACK and FIN_ACK and set send_FIN_ACK parameter. */
  if ((segment->flags & htonl(FIN)) != 0)
  {
    /* fprintf(stderr, "Receive an FIN segment.\n"); */

    state->ackno += 1;
    send_ack_segment(state);
    conn_output(state->conn, NULL, 0);
    state->receive_fin = true;

    return;
  }

  /* Output segment's data. */
  if (ntohs(segment->len) - sizeof(ctcp_segment_t) > 0)
  {
    /* fprintf(stderr, "Prepare to output segment's data.\n"); */

    ctcp_output(state);
  }
}

/* Called by ctcp_receive() if a segment is ready to outputted. */
void ctcp_output(ctcp_state_t *state)
{
  /* FIXME */

  /* fprintf(stderr, "Call ctcp_output() function.\n"); */

  /* Call conn_bufspace() to see how many bytes can be outputted to STDOUT */
  size_t buf_space = conn_bufspace(state->conn);

  /* If there is no room, wait for next time. */
  if (buf_space == 0)
  {
    return;
  }

  /* Call conn_output() in order to actually output the segment. */
  ctcp_segment_t *segment = state->last_received_segment;
  uint16_t data_len = ntohs(segment->len) - sizeof(ctcp_segment_t);

  /* fprintf(stderr, "Call conn_output() to output the segment of length %d.\n", data_len); */

  conn_output(state->conn, segment->data, data_len);

  /* fprintf(stderr, "Prepare to send an ACK segment.\n"); */

  /* Send ACK. */
  state->ackno += data_len;
  send_ack_segment(state);
}

/* Called periodically at specified rate. */
void ctcp_timer()
{
  /* FIXME */

  /* fprintf(stderr, "Call ctcp_timer() function.\n"); */

  /* Deal with empty state_list. */
  if (state_list == NULL)
  {
    /* fprintf(stderr, "State list is empty.\n"); */
    return;
  }

  /* Iterate state_list using state_walker. */
  ctcp_state_t *state_walker = state_list;

  while (state_walker != NULL)
  {
    /* If this connection has segment not be acked. */
    if (!state_walker->last_acked)
    {
      /* Assume the other end of the connection is unresponsive. */
      if (state_walker->retransmition == 5)
      {
        /* fprintf(stderr, "Assume the other end of the connection is unresponsive.\n"); */

        ctcp_destroy(state_walker);
      }

      /* Check whether it achives rt_timeout. */
      long rt_timeout = state_walker->cfg->rt_timeout;
      if (current_time() - state_walker->last_retransmit_time >= rt_timeout)
      {
        /* Resend the last send segment. */
        uint16_t len = ntohs(state_walker->last_send_segment->len);
        conn_send(state_walker->conn, state_walker->last_send_segment, len);
        state_walker->retransmition += 1;
        state_walker->last_retransmit_time = current_time();
      }
    }

    if (state_walker->send_fin && state_walker->receive_ack_fin && state_walker->receive_fin)
    {
      ctcp_destroy(state_walker);
    }

    state_walker = state_walker->next;
  }
}