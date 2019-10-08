/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */

/**
 * @file    process_request.c
 *
 * @brief
 *  process_request - this function gets, checks, and invokes the proper
 *	function to deal with a batch request received over the network.
 *
 *	All data encoding/decoding dependencies are moved to a lower level
 *	routine.  That routine must convert
 *	the data received into the internal server structures regardless of
 *	the data structures used by the encode/decode routines.  This provides
 *	the "protocol" and "protocol generation tool" freedom to the bulk
 *	of the server.
 *
 * Functions included are:
 *	pbs_crypt_des()
 *	get_credential()
 *	process_request()
 *	set_to_non_blocking()
 *	clear_non_blocking()
 *	dispatch_request()
 *	close_client()
 *	alloc_br()
 *	close_quejob()
 *	free_rescrq()
 *	arrayfree()
 *	read_carray()
 *	decode_DIS_PySpawn()
 *	free_br()
 *	freebr_manage()
 *	freebr_cpyfile()
 *	freebr_cpyfile_cred()
 *	parse_servername()
 *	get_servername()
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <memory.h>
#include <assert.h>
#ifndef WIN32
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <dlfcn.h>
#endif
#include <ctype.h>
#include "libpbs.h"
#include "pbs_error.h"
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "job.h"
#include "server.h"
#include "user.h"
#include "credential.h"
#include "ticket.h"
#include "net_connect.h"
#include "batch_request.h"
#include "log.h"
#include "rpp.h"
#include "dis_init.h"
#include "dis.h"
#include "pbs_nodes.h"
#include "svrfunc.h"
#include "pbs_sched.h"

/* global data items */

pbs_list_head svr_requests;


extern struct server server;
extern char      server_host[];
extern pbs_list_head svr_newjobs;
extern time_t    time_now;
extern char  *msg_err_noqueue;
extern char  *msg_err_malloc;
extern char  *msg_reqbadhost;
extern char  *msg_request;

extern int    is_local_root(char *, char *);
extern void   req_stat_hook(struct batch_request *);

/* Private functions local to this file */

static void freebr_manage(struct rq_manage *);
static void freebr_cpyfile(struct rq_cpyfile *);
static void freebr_cpyfile_cred(struct rq_cpyfile_cred *);
static void close_quejob(int sfds);

/**
 * @brief
 *		Return 1 if there is no credential, 0 if there is and -1 on error.
 *
 * @param[in]	remote	- server name
 * @param[in]	jobp	- job whose credentials needs to be read.
 * @param[in]	from	- can have the following values,
 * 							PBS_GC_BATREQ, PBS_GC_CPYFILE and PBS_GC_EXEC
 * @param[out]	data	- kerberos credential
 * @param[out]	dsize	- kerberos credential data length
 *
 * @return	int
 * @retval	1	- there is no credential
 * @retval	0	- there is credential
 * @retval	-1	- error
 */
int
get_credential(char *remote, job *jobp, int from, char **data, size_t *dsize)
{
	int	ret;

	switch (jobp->ji_extended.ji_ext.ji_credtype) {

		default:

#ifndef PBS_MOM

			/*   ensure job's euser exists as this can be called */
			/*   from pbs_send_job who is moving a job from a routing */
			/*   queue which doesn't have euser set */
			if ( ((jobp->ji_wattr[JOB_ATR_euser].at_flags & ATR_VFLAG_SET) \
		        && jobp->ji_wattr[JOB_ATR_euser].at_val.at_str) &&   \
		     (server.sv_attr[SRV_ATR_ssignon_enable].at_flags &      \
							   ATR_VFLAG_SET) && \
                     (server.sv_attr[SRV_ATR_ssignon_enable].at_val.at_long  \
								      == 1) ) {
				ret = user_read_password(
					jobp->ji_wattr[(int)JOB_ATR_euser].at_val.at_str,
					data, dsize);

				/* we have credential but type is NONE, force DES */
				if( ret == 0 && \
		  	    (jobp->ji_extended.ji_ext.ji_credtype == \
							PBS_CREDTYPE_NONE) )
				jobp->ji_extended.ji_ext.ji_credtype = \
							PBS_CREDTYPE_AES;
			} else
				ret = read_cred(jobp, data, dsize);
#else
			ret = read_cred(jobp, data, dsize);
#endif
			break;
	}
	return ret;
}

/**
 * @brief
 *      Authenticate user on successfully decoding munge key received
 *      from PBS batch request
 *
 * @param[in]  auth_data     opaque auth data that is to be verified
 * @param[out] from_svr		 1 - sender is server, 0 - sender not a server
 *
 * @return error code
 * @retval  0 - Success
 * @retval -1 - Authentication failure
 * @retval -2 - Authentication method not supported
 *
 */
int
authenticate_external(conn_t *conn, struct batch_request *request)
{
	int fromsvr = 0;
	int rc = 0;

	switch(request->rq_ind.rq_authen_external.rq_auth_type) {
#ifndef WIN32
		case AUTH_MUNGE:
			if (pbs_conf.auth_method != AUTH_MUNGE) {
				rc = -1;
				snprintf(log_buffer, sizeof(log_buffer), "PBS Server not enabled for MUNGE Authentication");
				goto err;
			}

			rc = pbs_munge_validate(request->rq_ind.rq_authen_external.rq_authen_un.rq_munge.rq_authkey, &fromsvr, log_buffer, sizeof(log_buffer));
			if (rc != 0)
				goto err;

			(void) strcpy(conn->cn_username, request->rq_user);
			(void) strcpy(conn->cn_hostname, request->rq_host);
			conn->cn_timestamp = time_now;
			conn->cn_authen |= PBS_NET_CONN_AUTHENTICATED;
			if (fromsvr == 1)
				conn->cn_authen |= PBS_NET_CONN_FROM_PRIVIL; /* set priv connection */

			return rc;
#endif
		default:
			snprintf(log_buffer, sizeof(log_buffer), "Authentication method not supported");
			rc = -2;
	}

err:
	log_err(-1, __func__, log_buffer);
	return rc;
}

/*
* @brief
 * 		process_request - process an request from the network:
 *		Call function to read in the request and decode it.
 *		Validate requesting host and user.
 *		Call function to process request based on type.
 *		That function MUST free the request by calling free_br()
 *
 * @param[in]	sfds	- file descriptor (socket) to get request
 */

void
process_request(int sfds)
{
	int		      rc;
	struct batch_request *request;
	conn_t		     *conn;


	time_now = time(NULL);

	conn = get_conn(sfds);

	if (!conn) {
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_REQUEST, LOG_ERR,
			"process_request", "did not find socket in connection table");
#ifdef WIN32
		(void)closesocket(sfds);
#else
		(void)close(sfds);
#endif
		return;
	}

	if ((request = alloc_br(0)) == NULL) {
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_REQUEST, LOG_ERR,
			"process_request", "Unable to allocate request structure");
		close_conn(sfds);
		return;
	}
	request->rq_conn = sfds;

	/*
	 * Read in the request and decode it to the internal request structure.
	 */

	if (get_connecthost(sfds, request->rq_host, PBS_MAXHOSTNAME)) {

		(void)sprintf(log_buffer, "%s: %lu", msg_reqbadhost,
			get_connectaddr(sfds));
		log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_REQUEST, LOG_DEBUG,
			"", log_buffer);
		req_reject(PBSE_BADHOST, 0, request);
		return;
	}

#ifndef PBS_MOM

	if (conn->cn_active == FromClientDIS) {
		rc = dis_request_read(sfds, request);
	} else {
		log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_REQUEST, LOG_ERR,
			"process_req", "request on invalid type of connection");
		close_conn(sfds);
		free_br(request);
		return;
	}
#else	/* PBS_MOM */
	rc = dis_request_read(sfds, request);
#endif	/* PBS_MOM */

	if (rc == -1) {		/* End of file */
		close_client(sfds);
		free_br(request);
		return;

	} else if ((rc == PBSE_SYSTEM) || (rc == PBSE_INTERNAL)) {

		/* read error, likely cannot send reply so just disconnect */

		/* ??? not sure about this ??? */

		close_client(sfds);
		free_br(request);
		return;

	} else if (rc > 0) {

		/*
		 * request didn't decode, either garbage or  unknown
		 * request type, in ether case, return reject-reply
		 */

		req_reject(rc, 0, request);
		close_client(sfds);
		return;
	}

#ifndef PBS_MOM
	/* If the request is coming on the socket we opened to the  */
	/* scheduler,  change the "user" from "root" to "Scheduler" */
	if (find_sched_from_sock(request->rq_conn) != NULL) {
		strncpy(request->rq_user, PBS_SCHED_DAEMON_NAME, PBS_MAXUSER);
		request->rq_user[PBS_MAXUSER] = '\0';
	}
#endif	/* PBS_MOM */

	(void)sprintf(log_buffer, msg_request, request->rq_type,
		request->rq_user, request->rq_host, sfds);
	log_event(PBSEVENT_DEBUG2, PBS_EVENTCLASS_REQUEST, LOG_DEBUG,
		"", log_buffer);

	/* is the request from a host acceptable to the server */
	if (request->rq_type == PBS_BATCH_AuthExternal) {
		rc = authenticate_external(conn, request);
		if (rc == 0)
			reply_ack(request);
		else if (rc == -2)
			req_reject(PBSE_NOSUP, 0, request);
		else
			req_reject(PBSE_BADCRED, 0, request);
		return;
	}

#ifndef PBS_MOM
	if (server.sv_attr[(int)SRV_ATR_acl_host_enable].at_val.at_long) {
		/* acl enabled, check it; always allow myself	*/

		struct pbsnode *isanode = NULL;
		if ((server.sv_attr[SRV_ATR_acl_host_moms_enable].at_flags & ATR_VFLAG_SET) &&
			(server.sv_attr[(int)SRV_ATR_acl_host_moms_enable].at_val.at_long == 1)) {
			isanode = find_nodebyaddr(get_connectaddr(sfds));

			if ((isanode != NULL) && (isanode->nd_state & INUSE_DELETED))
				isanode = NULL;
		}

		if (isanode == NULL) {
			if ((acl_check(&server.sv_attr[(int)SRV_ATR_acl_hosts],
				request->rq_host, ACL_Host) == 0) &&
				(strcasecmp(server_host, request->rq_host) != 0)) {
					req_reject(PBSE_BADHOST, 0, request);
					close_client(sfds);
					return;
			}
                }
	}

	/*
	 * determine source (user client or another server) of request.
	 * set the permissions granted to the client
	 */
	if (conn->cn_authen & PBS_NET_CONN_FROM_PRIVIL) {

		/* request came from another server */

		request->rq_fromsvr = 1;
		request->rq_perm = ATR_DFLAG_USRD | ATR_DFLAG_USWR |
				   ATR_DFLAG_OPRD | ATR_DFLAG_OPWR |
				   ATR_DFLAG_MGRD | ATR_DFLAG_MGWR |
				   ATR_DFLAG_SvWR;

	} else {

		/* request not from another server */

		request->rq_fromsvr = 0;

		/*
		 * Client must be authenticated by a Authenticate User Request,
		 * if not, reject request and close connection.
		 * -- The following is retained for compat with old cmds --
		 * The exception to this is of course the Connect Request which
		 * cannot have been authenticated, because it contains the
		 * needed ticket; so trap it here.  Of course, there is no
		 * prior authentication on the Authenticate User request either,
		 * but it comes over a reserved port and appears from another
		 * server, hence is automatically granted authorization.

		 */

		if (request->rq_type == PBS_BATCH_Connect) {
			req_connect(request);
			return;
		}

		if ((conn->cn_authen & PBS_NET_CONN_AUTHENTICATED) ==0) {
			rc = PBSE_BADCRED;
		} else {
			rc = authenticate_user(request, conn);
		}
		if (rc != 0) {
			req_reject(rc, 0, request);
			if (rc == PBSE_BADCRED)
				close_client(sfds);
			return;
		}

		request->rq_perm =
			svr_get_privilege(request->rq_user, request->rq_host);
	}

	/* if server shutting down, disallow new jobs and new running */

	if (server.sv_attr[(int)SRV_ATR_State].at_val.at_long > SV_STATE_RUN) {
		switch (request->rq_type) {
			case PBS_BATCH_AsyrunJob:
			case PBS_BATCH_JobCred:
			case PBS_BATCH_UserCred:
			case PBS_BATCH_UserMigrate:
			case PBS_BATCH_MoveJob:
			case PBS_BATCH_QueueJob:
			case PBS_BATCH_RunJob:
			case PBS_BATCH_StageIn:
			case PBS_BATCH_jobscript:
				req_reject(PBSE_SVRDOWN, 0, request);
				return;
		}
	}


#else	/* THIS CODE FOR MOM ONLY */

	/* check connecting host against allowed list of ok clients */
	if (!addrfind(conn->cn_addr)) {
		req_reject(PBSE_BADHOST, 0, request);
		close_client(sfds);
		return;
	}

	if ((conn->cn_authen & PBS_NET_CONN_FROM_PRIVIL) == 0) {
		req_reject(PBSE_BADCRED, 0, request);
		close_client(sfds);
		return;
	}

	request->rq_fromsvr = 1;
	request->rq_perm = ATR_DFLAG_USRD | ATR_DFLAG_USWR |
			   ATR_DFLAG_OPRD | ATR_DFLAG_OPWR |
			   ATR_DFLAG_MGRD | ATR_DFLAG_MGWR |
			   ATR_DFLAG_SvWR | ATR_DFLAG_MOM;
#endif

	/*
	 * dispatch the request to the correct processing function.
	 * The processing function must call reply_send() to free
	 * the request struture.
	 */

	dispatch_request(sfds, request);
	return;
}

#ifndef PBS_MOM		/* Server Only Functions */
/**
 * @brief
 *		Set socket to non-blocking to prevent write from hanging up the
 *		Server for a long time.
 *
 *		This is called from dispatch_request() below for requests that will
 *		typically produce a large amout of output, such as stating all jobs.
 *		It is called after the incoming request has been read.  After the
 *		request is processed and replied to, the socket will be reset, see
 *		clear_non_blocking().  The existing socket flags are saved in the
 *		connection table entry cn_sockflgs for use by clear_non_blocking().
 *
 * @param[in] conn - the connection structure.
 *
 * @return	success or failure
 * @retval	-l	- failure
 * @retval 	0	- success
 */

static int
set_to_non_blocking(conn_t *conn)
{

	if (conn->cn_sock != PBS_LOCAL_CONNECTION) {

#ifndef WIN32

		int flg;
		flg = fcntl(conn->cn_sock, F_GETFL);
		if (((flg = fcntl(conn->cn_sock, F_GETFL)) == -1) ||
			(fcntl(conn->cn_sock, F_SETFL, flg|O_NONBLOCK) == -1)) {
			log_err(errno, __func__,
				"Unable to set client socking non-blocking");
			return -1;
		}
		conn->cn_sockflgs = flg;
#endif	/* WIN32 */
	}
	return 0;
}

/**
 * @brief
 *		Clear non-blocking from a socket.
 *
 *		The function set_to_non_blocking() must be called first, it saved
 *		the prior socket flags in the connection table.  This function resets
 *		the socket flags to that value.
 *
 @param[in] conn - the connection structure.
 */

static void
clear_non_blocking(conn_t *conn)
{
	if(!conn)
		return;
	if (conn->cn_sock != PBS_LOCAL_CONNECTION) {
#ifndef WIN32
		int flg;
		if ((flg = conn->cn_sockflgs) != -1)
			/* reset socket flag to prior value */
			(void)fcntl(conn->cn_sock, F_SETFL, flg);
		conn->cn_sockflgs = 0;
#endif /* WIN32 */
	}
}
#endif	/* !PBS_MOM */

/**
 * @brief
 * 		Determine the request type and invoke the corresponding
 *		function.
 * @par
 *		The function will perform the request action and return the
 *		reply.  The function MUST also reply and free the request by calling
 *		reply_send().
 *
 * @param[in]	sfds	- socket connection
 * @param[in]	request - the request information
 */

void
dispatch_request(int sfds, struct batch_request *request)
{

	conn_t *conn = NULL;
	int rpp = request->isrpp;

	if (!rpp) {
		if (sfds != PBS_LOCAL_CONNECTION) {
			conn = get_conn(sfds);
			if (!conn) {
				log_event(PBSEVENT_SYSTEM, PBS_EVENTCLASS_REQUEST,
				LOG_ERR,
							"dispatch_request", "did not find socket in connection table");
				req_reject(PBSE_SYSTEM, 0, request);
				close_client(sfds);
				return;
			}
		}
	}

	switch (request->rq_type) {

		case PBS_BATCH_QueueJob:
			if (rpp) {
				request->rpp_ack = 0;
				rpp_add_close_func(sfds, close_quejob);
			} else
				net_add_close_func(sfds, close_quejob);
			req_quejob(request);
			break;

		case PBS_BATCH_JobCred:
#ifndef  PBS_MOM

			/* Reject if a user client (qsub -Wpwd) and not a */
			/* server (qmove) enqueued a job with JobCredential */
			if ( !request->rq_fromsvr && \
			     (server.sv_attr[SRV_ATR_ssignon_enable].at_flags \
                                                         & ATR_VFLAG_SET) &&  \
                             (server.sv_attr[SRV_ATR_ssignon_enable].at_val.at_long == 1) ) {
				req_reject(PBSE_SSIGNON_SET_REJECT, 0, request);
				close_client(sfds);
				break;
			}
#endif
			if (rpp)
				request->rpp_ack = 0;
			req_jobcredential(request);
			break;

		case PBS_BATCH_UserCred:
#ifdef PBS_MOM
#ifdef	WIN32
			req_reject(PBSE_NOSUP, 0, request);
#else
			req_reject(PBSE_UNKREQ, 0, request);
#endif
			close_client(sfds);
#else
			req_usercredential(request);
#endif
			break;

		case PBS_BATCH_UserMigrate:
#ifdef	PBS_MOM
#ifdef	WIN32
			req_reject(PBSE_NOSUP, 0, request);
#else
			req_reject(PBSE_UNKREQ, 0, request);
#endif	/* WIN32 */
			close_client(sfds);
#else
			req_user_migrate(request);
#endif	/* PBS_MOM */
			break;

		case PBS_BATCH_jobscript:
			if (rpp)
				request->rpp_ack = 0;
			req_jobscript(request);
			break;

			/*
			 * The PBS_BATCH_Rdytocommit message is deprecated.
			 * The server does not do anything with it anymore, but
			 * simply acks the request (in case some client makes this call)
			 */
		case PBS_BATCH_RdytoCommit:
			if (request->isrpp)
				request->rpp_ack = 0;
			reply_ack(request);
			break;

		case PBS_BATCH_Commit:
			if (rpp)
				request->rpp_ack = 0;
			req_commit(request);
			if (rpp)
				rpp_add_close_func(sfds, (void (*)(int))0);
			else
				net_add_close_func(sfds, (void (*)(int))0);
			break;

		case PBS_BATCH_DeleteJob:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				request->rq_ind.rq_delete.rq_objname,
				"delete job request received");
			req_deletejob(request);
			break;

#ifndef PBS_MOM
		case PBS_BATCH_SubmitResv:
			req_resvSub(request);
			break;

		case PBS_BATCH_DeleteResv:
			req_deleteReservation(request);
			break;

		case PBS_BATCH_ModifyResv:
			req_modifyReservation(request);
			break;

		case PBS_BATCH_ResvOccurEnd:
			req_reservationOccurrenceEnd(request);
			break;
#endif

		case PBS_BATCH_HoldJob:
			if (sfds != PBS_LOCAL_CONNECTION && !rpp)
				conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;
			req_holdjob(request);
			break;
#ifndef PBS_MOM
		case PBS_BATCH_LocateJob:
			req_locatejob(request);
			break;

		case PBS_BATCH_Manager:
			req_manager(request);
			break;

		case PBS_BATCH_RelnodesJob:
			req_relnodesjob(request);
			break;

#endif
		case PBS_BATCH_MessJob:
			req_messagejob(request);
			break;

		case PBS_BATCH_PySpawn:
			if (sfds != PBS_LOCAL_CONNECTION && !rpp)
				conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;
			req_py_spawn(request);
			break;

		case PBS_BATCH_ModifyJob:
			req_modifyjob(request);
			break;

		case PBS_BATCH_Rerun:
			req_rerunjob(request);
			break;
#ifndef PBS_MOM
		case PBS_BATCH_MoveJob:
			req_movejob(request);
			break;

		case PBS_BATCH_OrderJob:
			req_orderjob(request);
			break;

		case PBS_BATCH_Rescq:
			req_reject(PBSE_NOSUP, 0, request);
			break;

		case PBS_BATCH_ReserveResc:
			req_reject(PBSE_NOSUP, 0, request);
			break;

		case PBS_BATCH_ReleaseResc:
			req_reject(PBSE_NOSUP, 0, request);
			break;

		case PBS_BATCH_ReleaseJob:
			if (sfds != PBS_LOCAL_CONNECTION && !rpp)
				conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;
			req_releasejob(request);
			break;

		case PBS_BATCH_RunJob:
		case PBS_BATCH_AsyrunJob:
			req_runjob(request);
			break;

		case PBS_BATCH_DefSchReply:
			req_defschedreply(request);
			break;

		case PBS_BATCH_ConfirmResv:
			req_confirmresv(request);
			break;

		case PBS_BATCH_SelectJobs:
		case PBS_BATCH_SelStat:
			req_selectjobs(request);
			break;

#endif /* !PBS_MOM */

		case PBS_BATCH_Shutdown:
			req_shutdown(request);
			break;

		case PBS_BATCH_SignalJob:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				request->rq_ind.rq_signal.rq_jid,
				"signal job request received");
			req_signaljob(request);
			break;

		case PBS_BATCH_MvJobFile:
			req_mvjobfile(request);
			break;

#ifndef PBS_MOM		/* Server Only Functions */

		case PBS_BATCH_StatusJob:
			if (set_to_non_blocking(conn) == -1) {
				req_reject(PBSE_SYSTEM, 0, request);
				close_client(sfds);
				return;
			}
			req_stat_job(request);
			clear_non_blocking(conn);
			break;

		case PBS_BATCH_StatusQue:
			if (set_to_non_blocking(conn) == -1) {
				req_reject(PBSE_SYSTEM, 0, request);
				close_client(sfds);
				return;
			}
			req_stat_que(request);
			clear_non_blocking(conn);
			break;

		case PBS_BATCH_StatusNode:
			if (set_to_non_blocking(conn) == -1) {
				req_reject(PBSE_SYSTEM, 0, request);
				close_client(sfds);
				return;
			}
			req_stat_node(request);
			clear_non_blocking(conn);
			break;

		case PBS_BATCH_StatusResv:
			if (set_to_non_blocking(conn) == -1) {
				req_reject(PBSE_SYSTEM, 0, request);
				close_client(sfds);
				return;
			}
			req_stat_resv(request);
			clear_non_blocking(conn);
			break;

		case PBS_BATCH_StatusSvr:
			req_stat_svr(request);
			break;

		case PBS_BATCH_StatusSched:
			req_stat_sched(request);
			break;

		case PBS_BATCH_StatusHook:
			if (!is_local_root(request->rq_user,
				request->rq_host)) {
				sprintf(log_buffer, "%s@%s is unauthorized to "
					"access hooks data from server %s",
					request->rq_user, request->rq_host, server_host);
				reply_text(request, PBSE_HOOKERROR, log_buffer);
				log_event(PBSEVENT_ADMIN, PBS_EVENTCLASS_HOOK,
					LOG_INFO, "", log_buffer);
				/* don't call close_client() to allow other */
				/* non-hook related requests to continue */
				break;
			}

			if (set_to_non_blocking(conn) == -1) {
				req_reject(PBSE_SYSTEM, 0, request);
				close_client(sfds);
				return;
			}
			req_stat_hook(request);
			clear_non_blocking(conn);
			break;

		case PBS_BATCH_TrackJob:
			req_track(request);
			break;

		case PBS_BATCH_RegistDep:
			req_register(request);
			break;

		case PBS_BATCH_AuthenResvPort:
			if (pbs_conf.auth_method == AUTH_MUNGE) {
                                req_reject(PBSE_BADCRED, 0, request);
                                close_client(sfds);
                                return;
                        }
			req_authenResvPort(request);
			break;

		case PBS_BATCH_StageIn:
			req_stagein(request);
			break;

		case PBS_BATCH_FailOver:
			req_failover(request);
			break;

		case PBS_BATCH_StatusRsc:
			req_stat_resc(request);
			break;

		case PBS_BATCH_MomRestart:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_NODE,
				LOG_INFO,
				request->rq_ind.rq_momrestart.rq_momhost,
				"Mom restarted on host");
			req_momrestart(request);
			break;
#else	/* MOM only functions */

		case PBS_BATCH_CopyFiles:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				request->rq_ind.rq_cpyfile.rq_jobid,
				"copy file request received");
			/* don't time-out as copy may take long time */
			if (sfds != PBS_LOCAL_CONNECTION && !rpp)
				conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;
			req_cpyfile(request);
			break;
		case PBS_BATCH_CopyFiles_Cred:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				request->rq_ind.rq_cpyfile_cred.rq_copyfile.rq_jobid,
				"copy file cred request received");
			/* don't time-out as copy may take long time */
			if (sfds != PBS_LOCAL_CONNECTION && !rpp)
				conn->cn_authen |= PBS_NET_CONN_NOTIMEOUT;
			req_cpyfile(request);
			break;

		case PBS_BATCH_DelFiles:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				request->rq_ind.rq_cpyfile.rq_jobid,
				"delete file request received");
			req_delfile(request);
			break;
		case PBS_BATCH_DelFiles_Cred:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_JOB, LOG_INFO,
				request->rq_ind.rq_cpyfile_cred.rq_copyfile.rq_jobid,
				"delete file cred request received");
			req_delfile(request);
			break;
		case PBS_BATCH_CopyHookFile:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_HOOK,
				LOG_INFO,
				request->rq_ind.rq_hookfile.rq_filename,
				"copy hook-related file request received");
			req_copy_hookfile(request);
			break;
		case PBS_BATCH_DelHookFile:
			log_event(PBSEVENT_DEBUG, PBS_EVENTCLASS_HOOK,
				LOG_INFO,
				request->rq_ind.rq_hookfile.rq_filename,
				"delete hook-related file request received");
			req_del_hookfile(request);
			break;

#endif
		default:
			req_reject(PBSE_UNKREQ, 0, request);
			close_client(sfds);
			break;
	}
	return;
}

/**
 * @brief
 * 		close_client - close a connection to a client, also "inactivate"
 *		  any outstanding batch requests on that connection.
 *
 * @param[in]	sfds	- connection socket
 */

void
close_client(int sfds)
{
	struct batch_request *preq;

	close_conn(sfds);	/* close the connection */
	preq = (struct batch_request *)GET_NEXT(svr_requests);
	while (preq) {			/* list of outstanding requests */
		if (preq->rq_conn == sfds)
			preq->rq_conn = -1;
		if (preq->rq_orgconn == sfds)
			preq->rq_orgconn = -1;
		preq = (struct batch_request *)GET_NEXT(preq->rq_link);
	}
}

/**
 * @brief
 * 		alloc_br - allocate and clear a batch_request structure
 *
 * @param[in]	type	- type of request
 *
 * @return	batch_request *
 * @retval	NULL	- error
 */

struct batch_request *alloc_br(int type)
{
	struct batch_request *req;

	req= (struct batch_request *)malloc(sizeof(struct batch_request));
	if (req== NULL)
		log_err(errno, "alloc_br", msg_err_malloc);
	else {
		memset((void *)req, (int)0, sizeof(struct batch_request));
		req->rq_type = type;
		CLEAR_LINK(req->rq_link);
		req->rq_conn = -1;		/* indicate not connected */
		req->rq_orgconn = -1;		/* indicate not connected */
		req->rq_time = time_now;
		req->rpp_ack = 1; /* enable acks to be passed by rpp by default */
		req->isrpp = 0; /* not rpp by default */
		req->rppcmd_msgid = NULL; /* NULL msgid to boot */
		req->rq_reply.brp_choice = BATCH_REPLY_CHOICE_NULL;
		append_link(&svr_requests, &req->rq_link, req);
	}
	return (req);
}

/**
 * @brief
 * 		close_quejob - locate and deal with the new job that was being received
 *		  when the net connection closed.
 *
 * @param[in]	sfds	- file descriptor (socket) to get request
 */

static void
close_quejob(int sfds)
{
	job *pjob;

	pjob = (job *)GET_NEXT(svr_newjobs);
	while (pjob  != NULL) {
		if (pjob->ji_qs.ji_un.ji_newt.ji_fromsock == sfds) {
			if (pjob->ji_qs.ji_substate == JOB_SUBSTATE_TRANSICM) {

#ifndef PBS_MOM
				if (pjob->ji_qs.ji_svrflags & JOB_SVFLG_HERE) {

					/*
					 * the job was being created here for the first time
					 * go ahead and enqueue it as QUEUED; otherwise, hold
					 * it here as TRANSICM until we hear from the sending
					 * server again to commit.
					 */
					delete_link(&pjob->ji_alljobs);
					pjob->ji_qs.ji_state = JOB_STATE_QUEUED;
					pjob->ji_qs.ji_substate = JOB_SUBSTATE_QUEUED;
					if (svr_enquejob(pjob))
						(void)job_abt(pjob, msg_err_noqueue);

				}
#endif	/* PBS_MOM */

			} else {

				/* else delete the job */

				delete_link(&pjob->ji_alljobs);
				job_purge(pjob);
			}
			break;
		}
		pjob = GET_NEXT(pjob->ji_alljobs);
	}
	return;
}

#ifndef PBS_MOM		/* Server Only */
/**
 * @brief
 * 		free_rescrq - free resource queue.
 *
 * @param[in,out]	pq	- resource queue
 */
static void
free_rescrq(struct rq_rescq *pq)
{
	int i;

	i = pq->rq_num;
	while (i--) {
		if (*(pq->rq_list + i))
			(void)free(*(pq->rq_list + i));
	}
	if (pq->rq_list)
		(void)free(pq->rq_list);
}
#endif	/* Server Only */

/**
 * @brief
 * 		Free malloc'ed array of strings.  Used in MOM to deal with tm_spawn
 * 		equests and both MOM and the server for py_spawn requests.
 *
 * @param[out]	array	- malloc'ed array of strings
 */
void
arrayfree(char **array)
{
	int	i;

	if (array == NULL)
		return;
	for (i=0; array[i]; i++)
		free(array[i]);
	free(array);
}

/**
 * @brief
 *		Read a bunch of strings into a NULL terminated array.
 *		The strings are regular null terminated char arrays
 *		and the string array is NULL terminated.
 *
 *		Pass in array location to hold the allocated array
 *		and return an error value if there is a problem.  If
 *		an error does occur, arrloc is not changed.
 *
 * @param[in]	stream	- socket where you reads the request.
 * @param[out]	arrloc	- NULL terminated array where strings are stored.
 *
 * @return	error code
 */
static int
read_carray(int stream, char ***arrloc)
{
	int	i, num, ret;
	char	*cp, **carr;

	if (arrloc == NULL)
		return PBSE_INTERNAL;

	num = 4;	/* keep track of the number of array slots */
	carr = (char **)calloc(sizeof(char **), num);
	if (carr == NULL)
		return PBSE_SYSTEM;

	for (i=0;; i++) {
		cp = disrst(stream, &ret);
		if ((cp == NULL) || (ret != DIS_SUCCESS)) {
			arrayfree(carr);
			if (cp != NULL)
				free(cp);
			return PBSE_SYSTEM;
		}
		if (*cp == '\0') {
			free(cp);
			break;
		}
		if (i == num-1) {
			char	**hold;

			hold = (char **)realloc(carr,
				num * 2 * sizeof(char **));
			if (hold == NULL) {
				arrayfree(carr);
				free(cp);
				return PBSE_SYSTEM;
			}
			carr = hold;

			/* zero the last half of the now doubled carr */
			memset(&carr[num], 0, num * sizeof(char **));
			num *= 2;
		}
		carr[i] = cp;
	}
	carr[i] = NULL;
	*arrloc = carr;
	return ret;
}

/**
 * @brief
 *		Read a python spawn request off the wire.
 *		Each of the argv and envp arrays is sent by writing a counted
 *		string followed by a zero length string ("").
 *
 * @param[in]	sock	- socket where you reads the request.
 * @param[in]	preq	- the batch_request structure to free up.
 */
int
decode_DIS_PySpawn(int sock, struct batch_request *preq)
{
	int	rc;

	rc = disrfst(sock, sizeof(preq->rq_ind.rq_py_spawn.rq_jid),
		preq->rq_ind.rq_py_spawn.rq_jid);
	if (rc)
		return rc;

	rc = read_carray(sock, &preq->rq_ind.rq_py_spawn.rq_argv);
	if (rc)
		return rc;

	rc = read_carray(sock, &preq->rq_ind.rq_py_spawn.rq_envp);
	if (rc)
		return rc;

	return rc;
}

/**
 * @brief
 *		Read a release nodes from job request off the wire.
 *
 * @param[in]	sock	- socket where you reads the request.
 * @param[in]	preq	- the batch_request structure containing the request details.
 *
 * @return int
 *
 * @retval	0	- if successful
 * @retval	!= 0	- if not successful (an error encountered along the way)
 */
int
decode_DIS_RelnodesJob(int sock, struct batch_request *preq)
{
	int rc;

	preq->rq_ind.rq_relnodes.rq_node_list = NULL;

	rc = disrfst(sock, PBS_MAXSVRJOBID+1, preq->rq_ind.rq_relnodes.rq_jid);
	if (rc)
		return rc;

	preq->rq_ind.rq_relnodes.rq_node_list = disrst(sock, &rc);
	return rc;
}


/**
 * @brief
 * 		Free space allocated to a batch_request structure
 *		including any sub-structures
 *
 * @param[in]	preq - the batch_request structure to free up.
 */

void
free_br(struct batch_request *preq)
{
	delete_link(&preq->rq_link);
	reply_free(&preq->rq_reply);

	if (preq->rq_parentbr) {
		/*
		 * have a parent who has the original info, so we cannot
		 * free any data malloc-ed outside of the basic structure;
		 * decrement the reference count in the parent and when it
		 * goes to zero,  reply_send() it
		 */
		if (preq->rq_parentbr->rq_refct > 0) {
			if (--preq->rq_parentbr->rq_refct == 0)
				reply_send(preq->rq_parentbr);
		}

		if (preq->rppcmd_msgid)
			free(preq->rppcmd_msgid);

		(void)free(preq);
		return;
	}

	/*
	 * IMPORTANT - free any data that is malloc-ed outside of the
	 * basic batch_request structure below here so it is not freed
	 * when a copy of the structure (for a Array subjob) is freed
	 */
	if (preq->rq_extend)
		(void)free(preq->rq_extend);

	switch (preq->rq_type) {
		case PBS_BATCH_QueueJob:
			free_attrlist(&preq->rq_ind.rq_queuejob.rq_attr);
			break;
		case PBS_BATCH_JobCred:
			if (preq->rq_ind.rq_jobcred.rq_data)
				(void)free(preq->rq_ind.rq_jobcred.rq_data);
			break;
		case PBS_BATCH_UserCred:
			if (preq->rq_ind.rq_usercred.rq_data)
				(void)free(preq->rq_ind.rq_usercred.rq_data);
			break;
		case PBS_BATCH_jobscript:
			if (preq->rq_ind.rq_jobfile.rq_data)
				(void)free(preq->rq_ind.rq_jobfile.rq_data);
			break;
		case PBS_BATCH_CopyHookFile:
			if (preq->rq_ind.rq_hookfile.rq_data)
				(void)free(preq->rq_ind.rq_hookfile.rq_data);
			break;
		case PBS_BATCH_HoldJob:
			freebr_manage(&preq->rq_ind.rq_hold.rq_orig);
			break;
		case PBS_BATCH_MessJob:
			if (preq->rq_ind.rq_message.rq_text)
				(void)free(preq->rq_ind.rq_message.rq_text);
			break;
		case PBS_BATCH_RelnodesJob:
			if (preq->rq_ind.rq_relnodes.rq_node_list)
				(void)free(preq->rq_ind.rq_relnodes.rq_node_list);
			break;
		case PBS_BATCH_PySpawn:
			arrayfree(preq->rq_ind.rq_py_spawn.rq_argv);
			arrayfree(preq->rq_ind.rq_py_spawn.rq_envp);
			break;
		case PBS_BATCH_ModifyJob:
		case PBS_BATCH_ModifyResv:
			freebr_manage(&preq->rq_ind.rq_modify);
			break;

		case PBS_BATCH_RunJob:
		case PBS_BATCH_AsyrunJob:
		case PBS_BATCH_StageIn:
		case PBS_BATCH_ConfirmResv:
			if (preq->rq_ind.rq_run.rq_destin)
				(void)free(preq->rq_ind.rq_run.rq_destin);
			break;
		case PBS_BATCH_StatusJob:
		case PBS_BATCH_StatusQue:
		case PBS_BATCH_StatusNode:
		case PBS_BATCH_StatusSvr:
		case PBS_BATCH_StatusSched:
		case PBS_BATCH_StatusHook:
		case PBS_BATCH_StatusRsc:
		case PBS_BATCH_StatusResv:
			if (preq->rq_ind.rq_status.rq_id)
				free(preq->rq_ind.rq_status.rq_id);
			free_attrlist(&preq->rq_ind.rq_status.rq_attr);
			break;
		case PBS_BATCH_CopyFiles:
		case PBS_BATCH_DelFiles:
			freebr_cpyfile(&preq->rq_ind.rq_cpyfile);
			break;
		case PBS_BATCH_CopyFiles_Cred:
		case PBS_BATCH_DelFiles_Cred:
			freebr_cpyfile_cred(&preq->rq_ind.rq_cpyfile_cred);
			break;
		case PBS_BATCH_MvJobFile:
			if (preq->rq_ind.rq_jobfile.rq_data)
				free(preq->rq_ind.rq_jobfile.rq_data);
			break;

#ifndef PBS_MOM		/* Server Only */

		case PBS_BATCH_SubmitResv:
			free_attrlist(&preq->rq_ind.rq_queuejob.rq_attr);
			break;
		case PBS_BATCH_Manager:
			freebr_manage(&preq->rq_ind.rq_manager);
			break;
		case PBS_BATCH_ReleaseJob:
			freebr_manage(&preq->rq_ind.rq_release);
			break;
		case PBS_BATCH_Rescq:
		case PBS_BATCH_ReserveResc:
		case PBS_BATCH_ReleaseResc:
			free_rescrq(&preq->rq_ind.rq_rescq);
			break;
		case PBS_BATCH_DefSchReply:
			free(preq->rq_ind.rq_defrpy.rq_id);
			free(preq->rq_ind.rq_defrpy.rq_txt);
			break;
		case PBS_BATCH_SelectJobs:
		case PBS_BATCH_SelStat:
			free_attrlist(&preq->rq_ind.rq_select.rq_selattr);
			free_attrlist(&preq->rq_ind.rq_select.rq_rtnattr);
			break;
#endif /* PBS_MOM */
	}
	if (preq->rppcmd_msgid)
		free(preq->rppcmd_msgid);
	(void)free(preq);
}
/**
 * @brief
 * 		it is a wrapper function of free_attrlist()
 *
 * @param[in]	pmgr - request manage structure.
 */
static void
freebr_manage(struct rq_manage *pmgr)
{
	free_attrlist(&pmgr->rq_attr);
}
/**
 * @brief
 * 		remove all the rqfpair and free their memory
 *
 * @param[in]	pcf - rq_cpyfile structure on which rq_pairs needs to be freed.
 */
static void
freebr_cpyfile(struct rq_cpyfile *pcf)
{
	struct rqfpair *ppair;

	while ((ppair = (struct rqfpair *)GET_NEXT(pcf->rq_pair)) != NULL) {
		delete_link(&ppair->fp_link);
		if (ppair->fp_local)
			(void)free(ppair->fp_local);
		if (ppair->fp_rmt)
			(void)free(ppair->fp_rmt);
		(void)free(ppair);
	}
}
/**
 * @brief
 * 		remove list of rqfpair along with encrpyted credential.
 *
 * @param[in]	pcfc - rq_cpyfile_cred structure
 */
static void
freebr_cpyfile_cred(struct rq_cpyfile_cred *pcfc)
{
	struct rqfpair *ppair;

	while ((ppair = (struct rqfpair *)GET_NEXT(pcfc->rq_copyfile.rq_pair))
		!= NULL) {
		delete_link(&ppair->fp_link);
		if (ppair->fp_local)
			(void)free(ppair->fp_local);
		if (ppair->fp_rmt)
			(void)free(ppair->fp_rmt);
		(void)free(ppair);
	}
	if (pcfc->rq_pcred)
		free(pcfc->rq_pcred);
}

/**
 * @brief
 * 		parse_servername - parse a server/vnode name in the form:
 *		[(]name[:service_port][:resc=value[:...]][+name...]
 *		from exec_vnode or from exec_hostname
 *		name[:service_port]/NUMBER[*NUMBER][+...]
 *		or basic servername:port string
 *
 *		Returns ptr to the node name as the function value and the service_port
 *		number (int) into service if :port is found, otherwise port is unchanged
 *		host name is also terminated by a ':', '+' or '/' in string
 *
 * @param[in]	name	- server/node/exec_vnode string
 * @param[out]	service	-  RETURN: service_port if :port
 *
 * @return	 ptr to the node name
 *
 * @par MT-safe: No
 */

char *
parse_servername(char *name, unsigned int *service)
{
	static char  buf[PBS_MAXSERVERNAME + PBS_MAXPORTNUM + 2];
	int   i = 0;
	char *pc;

	if ((name == NULL) || (*name == '\0'))
		return NULL;
	if (*name ==  '(')   /* skip leading open paren found in exec_vnode */
		name++;

	/* look for a ':', '+' or '/' in the string */

	pc = name;
	while (*pc && (i < PBS_MAXSERVERNAME+PBS_MAXPORTNUM+2)) {
		if ((*pc == '+') || (*pc == '/')) {
			break;
		} else if (*pc == ':') {
			if (isdigit((int)*(pc+1)) && (service != NULL))
				*service = (unsigned int)atoi(pc + 1);
			break;
		} else {
			buf[i++] = *pc++;
		}
	}
	buf[i] = '\0';
	return (buf);
}

/**
 * @brief
 * 		Obtain the name and port of the server as defined by pbs_conf
 *
 * @param[out] port - Passed through to parse_servername(), not modified here.
 *
 * @return char *
 * @return NULL - failure
 * @retval !NULL - pointer to server name
 */
char *
get_servername(unsigned int *port)
{
	char *name = NULL;

	if (pbs_conf.pbs_primary)
		name = parse_servername(pbs_conf.pbs_primary, port);
	else if (pbs_conf.pbs_server_host_name)
		name = parse_servername(pbs_conf.pbs_server_host_name, port);
	else
		name = parse_servername(pbs_conf.pbs_server_name, port);

	return name;
}

