/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Functions for allocating requests and storing internal data in them.
 * @file src/lib/server/request.c
 *
 * @copyright 2015 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/rad_assert.h>
#include <freeradius-devel/unlang/base.h>

/** The thread local free list
 *
 * Any entries remaining in the list will be freed when the thread is joined
 */
fr_thread_local_setup(fr_dlist_head_t *, request_free_list)

/** Setup logging and other fields for a request
 *
 * @param[in] request		to (re)-initialise.
 */
static void request_init(REQUEST *request)
{
#ifndef NDEBUG
	request->magic = REQUEST_MAGIC;
#endif

	request->request_state = REQUEST_INIT;
	request->master_state = REQUEST_ACTIVE;

	/*
	 *	Initialise the stack
	 */
	MEM(request->stack = unlang_interpret_stack_alloc(request));

	/*
	 *	Initialise the request data list
	 */
	request_data_list_init(&request->data);

	/*
	 *	Initialise the state_ctx
	 */
	if (!request->state_ctx) request->state_ctx = talloc_init("session-state");

	/*
	 *	These may be changed later by request_pre_handler
	 */
	request->log.lvl = fr_debug_lvl;	/* Default to global debug level */
	if (!request->log.dst) {
		request->log.dst = talloc_zero(request, log_dst_t);
	} else {
		memset(request->log.dst, 0, sizeof(*request->log.dst));
	}
	request->log.dst->func = vlog_request;
	request->log.dst->uctx = &default_log;

	request->module = NULL;
	request->component = "<core>";

	request->seq_start = 0;
	request->runnable_id = -1;
	request->time_order_id = -1;

	fr_dlist_entry_init(&request->free_entry);	/* Needs to be initialised properly, else bad things happen */
}

/** Callback for freeing a request struct
 *
 * @param[in] request		to free or return to the free list.
 * @return
 *	- 0 in the request was freed.
 *	- -1 if the request was inserted into the free list.
 */
static int _request_free(REQUEST *request)
{
	rad_assert(!request->ev);

	/*
	 *	Reinsert into the free list if it's not already
	 *	in the free list.
	 *
	 *	If it *IS* already in the free list, then free it.
	 */
	if (unlikely(fr_dlist_entry_in_list(&request->free_entry))) {
		fr_dlist_entry_unlink(&request->free_entry);	/* Don't trust the list head to be available */
		talloc_free(request->state_ctx);
		goto really_free;
	}

	/*
	 *	We keep a buffer of <active> + N requests per
	 *	thread, to avoid spurious allocations.
	 */
	if (fr_dlist_num_elements(request_free_list) <= 256) {
		TALLOC_CTX		*state_ctx;
		fr_dlist_head_t		*free_list;

		/*
		 *	Ensure any data associated
		 *	with the state ctx is freed.
		 */
		if (request->state_ctx) {
			rad_assert(!request->parent || (request->state_ctx != request->parent->state_ctx));
			talloc_free_children(request->state_ctx);
 			state_ctx = request->state_ctx;
		} else {
			state_ctx = NULL;
		}
		free_list = request_free_list;

		/*
		 *	Reinitialise the request
		 */
		talloc_free_children(request);
		memset(request, 0, sizeof(*request));
		request->component = "free_list";
		request->state_ctx = state_ctx;		/* Use the old, now cleared, state_ctx */

		/*
		 *	Reinsert into the free list
		 */
		fr_dlist_insert_head(free_list, request);
		request_free_list = free_list;

		return -1;	/* Prevent free */
 	}

	/*
	 *	state_ctx is parented separately.
	 *
	 *	The reason why it's OK to do this, is if the state attributes
	 *	need to persist across requests, they will already have been
	 *	moved to a fr_state_entry_t, with the state pointers in the
	 *	request being set to NULL, before the request is freed.
	 *
	 *	Our state ctx can be the same as the parents if
	 *	request_data_restore_to_child() was called.  That
	 *	function resets the childs state_ctx to be the same as
	 *	the parents.  Adding this check here means that we
	 *	don't need to call request_detach() on a child which
	 *	will be freed immediately after the detach.
	 *
	 *	Note also that we do NOT call TALLOC_FREE(), which
	 *	sets state_ctx=NULL.  We don't control the order in
	 *	which talloc frees the children.  And the parents
	 *	state_ctx pointer needs to stick around so that all of
	 *	the children can check it.
	 *
	 *	If this assertion hits, it means that someone didn't
	 *	call fr_state_store_in_parent()
	 */
	if (request->state_ctx) {
		rad_assert(!request->parent || (request->state_ctx != request->parent->state_ctx));
		talloc_free(request->state_ctx);
	}

	/*
	 *	Ensure anything that might reference the request is
	 *	freed before it is.
	 */
	talloc_free_children(request);

really_free:
#ifndef NDEBUG
	request->magic = 0x01020304;	/* set the request to be nonsense */
#endif

	return 0;
}

/** Free any free requests when the thread is joined
 *
 */
static void _request_free_list_free_on_exit(void *arg)
{
	fr_dlist_head_t *list = talloc_get_type_abort(arg, fr_dlist_head_t);
	REQUEST		*request;

	/*
	 *	See the destructor for why this works
	 */
	while ((request = fr_dlist_head(list))) talloc_free(request);
	talloc_free(list);
}

/** Create a new REQUEST data structure
 *
 */
REQUEST *request_alloc(TALLOC_CTX *ctx)
{
	REQUEST			*request;
	fr_dlist_head_t		*free_list;

	/*
	 *	Setup the free list, or return the free
	 *	list for this thread.
	 */
	if (unlikely(!request_free_list)) {
		MEM(free_list = talloc(NULL, fr_dlist_head_t));
		fr_dlist_init(free_list, REQUEST, free_entry);
		fr_thread_local_set_destructor(request_free_list, _request_free_list_free_on_exit, free_list);
	} else {
		free_list = request_free_list;
	}

	request = fr_dlist_head(free_list);
	if (!request) {
		/*
		 *	Only allocate requests in the NULL
		 *	ctx.  There's no scenario where it's
		 *	appropriate to allocate them in a
		 *	pool, and using a strict talloc
		 *	hierarchy means that child requests
		 *	cannot be returned to a free list
		 *	and would have to be freed.
		 */
		MEM(request = talloc_zero_pooled_object(NULL, REQUEST,
							1 + 				/* Stack pool */
							UNLANG_STACK_MAX + 		/* Stack Frames */
							2 + 				/* packets */
							10,				/* extra */
							(UNLANG_FRAME_PRE_ALLOC * UNLANG_STACK_MAX) +	/* Stack memory */
							(sizeof(RADIUS_PACKET) * 2) +	/* packets */
							128				/* extra */
							));
		talloc_set_destructor(request, _request_free);
	} else {
		/*
		 *	Remove from the free list, as we're
		 *	about to use it!
		 */
		fr_dlist_remove(free_list, request);
	}

	request_init(request);

	/*
	 *	Bind lifetime to a parent.
	 *
	 *	If the parent is freed the destructor
	 *	will fire, and return the request
	 *	to a "top level" free list.
	 */
	if (ctx) talloc_link_ctx(ctx, request);

	return request;
}

static int _request_local_free(REQUEST *request)
{
	talloc_free(request->state_ctx);

	return 0;
}

/** Allocate a request that's not in the free list
 *
 * This can be useful if modules need a persistent request for their own purposes
 * which needs to be outside of the normal free list, so that it can be freed
 * when the module requires, not when the thread destructor runs.
 */
REQUEST *request_local_alloc(TALLOC_CTX *ctx)
{
	REQUEST *request;

	MEM(request = talloc_zero(ctx, REQUEST));

	request_init(request);

	talloc_set_destructor(request, _request_local_free);

	return request;
}

static REQUEST *request_init_fake(REQUEST *request, REQUEST *fake)
{
	fake->number = request->child_number++;
	fake->name = talloc_typed_asprintf(fake, "%s.%" PRIu64 , request->name, fake->number);

	fake->seq_start = 0;	/* children always start with their own sequence */

	fake->parent = request;
	fake->dict = request->dict;
	fake->config = request->config;
	fake->client = request->client;

	/*
	 *	For new server support.
	 *
	 *	FIXME: Key instead off of a "virtual server" data structure.
	 *
	 *	FIXME: Permit different servers for inner && outer sessions?
	 */
	fake->server_cs = request->server_cs;

	fake->packet = fr_radius_alloc(fake, true);
	if (!fake->packet) {
		talloc_free(fake);
		return NULL;
	}

	fake->reply = fr_radius_alloc(fake, false);
	if (!fake->reply) {
		talloc_free(fake);
		return NULL;
	}

	fake->master_state = REQUEST_ACTIVE;

	/*
	 *	Fill in the fake request.
	 */
	fake->packet->sockfd = -1;
	fake->packet->src_ipaddr = request->packet->src_ipaddr;
	fake->packet->src_port = request->packet->src_port;
	fake->packet->dst_ipaddr = request->packet->dst_ipaddr;
	fake->packet->dst_port = 0;

	/*
	 *	This isn't STRICTLY required, as the fake request MUST NEVER
	 *	be put into the request list.  However, it's still reasonable
	 *	practice.
	 */
	fake->packet->id = fake->number & 0xff;
	fake->packet->code = request->packet->code;
	fake->packet->timestamp = request->packet->timestamp;

	/*
	 *	Required for new identity support
	 */
	fake->listener = request->listener;

	/*
	 *	Fill in the fake reply, based on the fake request.
	 */
	fake->reply->sockfd = fake->packet->sockfd;
	fake->reply->src_ipaddr = fake->packet->dst_ipaddr;
	fake->reply->src_port = fake->packet->dst_port;
	fake->reply->dst_ipaddr = fake->packet->src_ipaddr;
	fake->reply->dst_port = fake->packet->src_port;
	fake->reply->id = fake->packet->id;
	fake->reply->code = 0; /* UNKNOWN code */

	/*
	 *	Copy debug information.
	 */
	memcpy(&(fake->log), &(request->log), sizeof(fake->log));
	fake->log.unlang_indent = 0;	/* Apart from the indent which we reset */
	fake->log.module_indent = 0;	/* Apart from the indent which we reset */

	return fake;
}

/*
 *	Create a new REQUEST, based on an old one.
 *
 *	This function allows modules to inject fake requests
 *	into the server, for tunneled protocols like TTLS & PEAP.
 */
REQUEST *request_alloc_fake(REQUEST *request, fr_dict_t const *namespace)
{
	REQUEST *fake;

	fake = request_alloc(request);
	if (!fake) return NULL;

	if (!request_init_fake(request, fake)) return NULL;

	if (namespace) fake->dict = namespace;

	return fake;
}


/** Allocate a fake request which is detachable from the parent.
 * i.e. if the parent goes away, sometimes the child MAY continue to
 * run.
 *
 */
REQUEST *request_alloc_detachable(REQUEST *request, fr_dict_t const *namespace)
{
	REQUEST *fake;

	fake = request_alloc(NULL);
	if (!fake) return NULL;

	if (!request_init_fake(request, fake)) return NULL;

	if (namespace) fake->dict = namespace;

	/*
	 *	Ensure that we use our own version of the logging
	 *	information, and not the original request one.
	 */
	fake->log.dst = talloc_zero(fake, log_dst_t);
	memcpy(fake->log.dst, request->log.dst, sizeof(*fake->log.dst));

	/*
	 *	Associate the child with the parent, using the child's
	 *	pointer as a unique identifier.  Free it if the parent
	 *	goes away, but don't persist it across
	 *	challenge-response boundaries.
	 */
	if (request_data_talloc_add(request, fake, 0, REQUEST, fake, true, true, false) < 0) {
		talloc_free(fake);
		return NULL;
	}

	return fake;
}

/** Unlink a subrequest from its parent
 *
 * @note This should be used for requests in preparation for freeing them.
 *
 * @param[in] fake		request to unlink.
 * @param[in] will_free		Caller super pinky swears to free
 *				the request ASAP, and that it wont
 *				touch persistable request data,
 *				request->state_ctx or request->state.
 * @return
 *	 - 0 on success.
 *	 - -1 on failure.
 */
int request_detach(REQUEST *fake, bool will_free)
{
	REQUEST		*request = fake->parent;

	rad_assert(request != NULL);

	/*
	 *	Unlink the child from the parent.
	 */
	request_data_get(request, fake, 0);

	/*
	 *	Fixup any sate or persistent
	 *	request data.
	 */
	fr_state_detach(fake, will_free);

	fake->parent = NULL;

	while (!request->backlog) {
		rad_assert(request->parent != NULL);
		request = request->parent;
	}

	fake->backlog = request->backlog;

	return 0;
}

#ifdef WITH_VERIFY_PTR
/*
 *	Verify a packet.
 */
static void packet_verify(char const *file, int line, REQUEST const *request, RADIUS_PACKET const *packet, char const *type)
{
	TALLOC_CTX *parent;

	if (!packet) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%i]: RADIUS_PACKET %s pointer was NULL", file, line, type);
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	parent = talloc_parent(packet);
	if (parent != request) {
		ERROR("CONSISTENCY CHECK FAILED %s[%i]: Expected RADIUS_PACKET %s to be parented by %p (%s), "
		      "but parented by %p (%s)", file, line, type, request, talloc_get_name(request),
		      parent, parent ? talloc_get_name(parent) : "NULL");

		fr_log_talloc_report(packet);
		if (parent) fr_log_talloc_report(parent);

		rad_assert(0);
	}

	PACKET_VERIFY(packet);

	if (!packet->vps) return;

	fr_pair_list_verify(file, line, packet, packet->vps);
}

/*
 *	Catch horrible talloc errors.
 */
void request_verify(char const *file, int line, REQUEST const *request)
{
	request_data_t *rd = NULL;

	if (!request) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%i]: REQUEST pointer was NULL", file, line);
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	(void) talloc_get_type_abort_const(request, REQUEST);

	rad_assert(request->magic == REQUEST_MAGIC);

	if (talloc_get_size(request) != sizeof(REQUEST)) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%i]: expected REQUEST size of %zu bytes, got %zu bytes",
			file, line, sizeof(REQUEST), talloc_get_size(request));
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	fr_pair_list_verify(file, line, request, request->control);
	fr_pair_list_verify(file, line, request->state_ctx, request->state);

	rad_assert(request->server_cs != NULL);

	if (request->packet) {
		packet_verify(file, line, request, request->packet, "request");
		if ((request->packet->code == FR_CODE_ACCESS_REQUEST) &&
		    (request->reply && !request->reply->code)) {
			rad_assert(request->state_ctx != NULL);
		}
	}
	if (request->reply) packet_verify(file, line, request, request->reply, "reply");

	if (request->async) {
		(void) talloc_get_type_abort(request->async, fr_async_t);
		rad_assert(talloc_parent(request->async) == request);
	}

	while ((rd = fr_dlist_next(&request->data, rd))) {
		(void) talloc_get_type_abort(rd, request_data_t);

		if (request_data_persistable(rd)) {
			rad_assert(request->state_ctx);
			rad_assert(talloc_parent(rd) == request->state_ctx);
		} else {
			rad_assert(talloc_parent(rd) == request);
		}
	}
}
#endif
