/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>

#include "nvmf_internal.h"
#include "request.h"
#include "session.h"
#include "subsystem.h"
#include "transport.h"

#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"

int
spdk_nvmf_request_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	response->sqid = 0;
	response->status.p = 0;
	response->sqhd = req->conn->sq_head;
	response->cid = req->cmd->nvme_cmd.cid;

	SPDK_TRACELOG(SPDK_TRACE_NVMF,
		      "cpl: cid=%u cdw0=0x%08x rsvd1=%u sqhd=%u status=0x%04x\n",
		      response->cid, response->cdw0, response->rsvd1, response->sqhd,
		      *(uint16_t *)&response->status);

	if (req->conn->transport->req_complete(req)) {
		SPDK_ERRLOG("Transport request completion error!\n");
		return -1;
	}

	return 0;
}

static bool
nvmf_process_discovery_cmd(struct spdk_nvmf_request *req)
{
	struct nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_discovery_log_page *log;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	if (req->data == NULL) {
		SPDK_ERRLOG("discovery command with no buffer\n");
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return true;
	}

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		/* Only identify controller can be supported */
		if ((cmd->cdw10 & 0xFF) == SPDK_NVME_IDENTIFY_CTRLR) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Controller\n");
			memcpy(req->data, (char *)&session->vcdata, sizeof(struct spdk_nvme_ctrlr_data));
			return true;
		} else {
			SPDK_ERRLOG("Unsupported identify command\n");
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return true;
		}
		break;
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		if ((cmd->cdw10 & 0xFF) == SPDK_NVME_LOG_DISCOVERY) {
			log = (struct spdk_nvmf_discovery_log_page *)req->data;
			/*
			 * Does not support change discovery
			 *  information at runtime now.
			 */
			log->genctr = 0;
			log->numrec = 0;
			spdk_format_discovery_log(log, req->length);
			return true;
		} else {
			SPDK_ERRLOG("Unsupported log page %u\n", cmd->cdw10 & 0xFF);
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return true;
		}
		break;
	default:
		SPDK_ERRLOG("Unsupported Opcode 0x%x for Discovery service\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return true;
	}

	return true;
}

static void
nvmf_complete_cmd(void *ctx, const struct spdk_nvme_cpl *cmp)
{
	struct spdk_nvmf_request *req = ctx;
	struct spdk_nvme_cpl *response;

	spdk_trace_record(TRACE_NVMF_LIB_COMPLETE, 0, 0, (uint64_t)req, 0);

	response = &req->rsp->nvme_cpl;
	memcpy(response, cmp, sizeof(*cmp));

	spdk_nvmf_request_complete(req);
}

static bool
nvmf_process_admin_cmd(struct spdk_nvmf_request *req)
{
	struct nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_nvmf_subsystem *subsystem = session->subsys;
	int rc = 0;
	uint8_t feature;

	/* pre-set response details for this command */
	response->status.sc = SPDK_NVME_SC_SUCCESS;

	switch (cmd->opc) {
	case SPDK_NVME_OPC_IDENTIFY:
		if ((cmd->cdw10 & 0xFF) == SPDK_NVME_IDENTIFY_CTRLR) {
			if (req->data == NULL || req->length < sizeof(struct spdk_nvme_ctrlr_data)) {
				SPDK_ERRLOG("identify command with no buffer\n");
				response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
				return true;
			}

			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Identify Controller\n");
			/* pull from virtual controller context */
			memcpy(req->data, &session->vcdata, sizeof(struct spdk_nvme_ctrlr_data));
			return true;
		}
		goto passthrough;

	case SPDK_NVME_OPC_GET_FEATURES:
		feature = cmd->cdw10 & 0xff; /* mask out the FID value */
		switch (feature) {
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Get Features - Number of Queues\n");
			response->cdw0 = ((session->max_connections_allowed - 1) << 16) |
					 (session->max_connections_allowed - 1);
			return true;
		default:
			goto passthrough;
		}
		break;
	case SPDK_NVME_OPC_SET_FEATURES:
		feature = cmd->cdw10 & 0xff; /* mask out the FID value */
		switch (feature) {
		case SPDK_NVME_FEAT_NUMBER_OF_QUEUES:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Set Features - Number of Queues, cdw11 0x%x\n", cmd->cdw11);

			/* verify that the contoller is ready to process commands */
			if (session->num_connections > 1) {
				SPDK_TRACELOG(SPDK_TRACE_NVMF, "Queue pairs already active!\n");
				response->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			} else {
				response->cdw0 = ((session->max_connections_allowed - 1) << 16) |
						 (session->max_connections_allowed - 1);
			}
			return true;
		default:
			goto passthrough;
		}
		break;
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Async Event Request\n");
		/*
		  Trap request here and save in the session context
		  until NVMe library indicates some event.
		*/
		if (session->aer_req == NULL) {
			session->aer_req = req;
			return false;
		} else {
			/* AER already recorded, send error response */
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "AER already active!\n");
			response->status.sc = SPDK_NVME_SC_ASYNC_EVENT_REQUEST_LIMIT_EXCEEDED;
			return true;
		}
		break;
	case SPDK_NVME_OPC_KEEP_ALIVE:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Keep Alive\n");
		/*
		  To handle keep alive just clear or reset the
		  session based keep alive duration counter.
		  When added, a separate timer based process
		  will monitor if the time since last recorded
		  keep alive has exceeded the max duration and
		  take appropriate action.
		*/
		//session->keep_alive_timestamp = ;
		return true;

	case SPDK_NVME_OPC_CREATE_IO_SQ:
	case SPDK_NVME_OPC_CREATE_IO_CQ:
	case SPDK_NVME_OPC_DELETE_IO_SQ:
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		SPDK_ERRLOG("Admin opc 0x%02X not allowed in NVMf\n", cmd->opc);
		response->status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return true;

	default:
passthrough:
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "admin_cmd passthrough: opc 0x%02x\n", cmd->opc);
		rc = spdk_nvme_ctrlr_cmd_admin_raw(subsystem->ctrlr,
						   cmd,
						   req->data, req->length,
						   nvmf_complete_cmd,
						   req);
		if (rc) {
			SPDK_ERRLOG("Error submitting admin opc 0x%02x\n", cmd->opc);
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return true;
		}
		return false;
	}
}

static bool
nvmf_process_io_cmd(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_subsystem *subsystem = req->conn->sess->subsys;
	int rc;

	rc = spdk_nvme_ctrlr_cmd_io_raw(subsystem->ctrlr, subsystem->io_qpair,
					&req->cmd->nvme_cmd,
					req->data, req->length,
					nvmf_complete_cmd,
					req);

	if (rc) {
		SPDK_ERRLOG("Failed to submit request %p\n", req);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return true;
	}

	return false;
}

static bool
nvmf_process_property_get(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_get_rsp *response;
	struct spdk_nvmf_fabric_prop_get_cmd *cmd;

	cmd = &req->cmd->prop_get_cmd;
	response = &req->rsp->prop_get_rsp;

	nvmf_property_get(req->conn->sess, cmd, response);

	return true;
}

static bool
nvmf_process_property_set(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_fabric_prop_set_cmd *cmd;

	cmd = &req->cmd->prop_set_cmd;

	nvmf_property_set(req->conn->sess, cmd, &req->rsp->nvme_cpl);

	return true;
}

static void
nvmf_handle_connect(spdk_event_t event)
{
	struct spdk_nvmf_request *req = spdk_event_get_arg1(event);
	struct spdk_nvmf_fabric_connect_cmd *connect = &req->cmd->connect_cmd;
	struct spdk_nvmf_fabric_connect_data *connect_data = (struct spdk_nvmf_fabric_connect_data *)
			req->data;
	struct spdk_nvmf_fabric_connect_rsp *response = &req->rsp->connect_rsp;
	struct spdk_nvmf_conn *conn = req->conn;

	spdk_nvmf_session_connect(conn, connect, connect_data, response);

	SPDK_TRACELOG(SPDK_TRACE_NVMF, "connect capsule response: cntlid = 0x%04x\n",
		      response->status_code_specific.success.cntlid);

	spdk_nvmf_request_complete(req);
	return;
}

static void
invalid_connect_response(struct spdk_nvmf_fabric_connect_rsp *rsp, uint8_t iattr, uint16_t ipo)
{
	rsp->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
	rsp->status.sc = SPDK_NVMF_FABRIC_SC_INVALID_PARAM;
	rsp->status_code_specific.invalid.iattr = iattr;
	rsp->status_code_specific.invalid.ipo = ipo;
}

static bool
nvmf_process_connect(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_subsystem	*subsystem;
	spdk_event_t			event;
	struct spdk_nvmf_fabric_connect_data *data = (struct spdk_nvmf_fabric_connect_data *)
			req->data;
	struct spdk_nvmf_fabric_connect_rsp *rsp = &req->rsp->connect_rsp;

#define INVALID_CONNECT_DATA(field) invalid_connect_response(rsp, 1, offsetof(struct spdk_nvmf_fabric_connect_data, field))

	if (req->length < sizeof(struct spdk_nvmf_fabric_connect_data)) {
		SPDK_ERRLOG("Connect command data length 0x%x too small\n", req->length);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return true;
	}

	/* Look up the requested subsystem */
	subsystem = nvmf_find_subsystem(data->subnqn, data->hostnqn);
	if (subsystem == NULL) {
		SPDK_ERRLOG("Could not find subsystem '%s'\n", data->subnqn);
		INVALID_CONNECT_DATA(subnqn);
		return true;
	}

	/* Pass an event to the lcore that owns this subsystem */
	event = spdk_event_allocate(subsystem->poller.lcore, nvmf_handle_connect, req, NULL, NULL);
	spdk_event_call(event);

	return false;
}

static bool
nvmf_process_fabrics_command(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_conn *conn = req->conn;
	struct spdk_nvmf_capsule_cmd *cap_hdr;

	cap_hdr = &req->cmd->nvmf_cmd;

	if (conn->sess == NULL) {
		/* No session established yet; the only valid command is Connect */
		if (cap_hdr->fctype == SPDK_NVMF_FABRIC_COMMAND_CONNECT) {
			return nvmf_process_connect(req);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "Got fctype 0x%x, expected Connect\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
			return true;
		}
	} else if (conn->type == CONN_TYPE_AQ) {
		/*
		 * Session is established, and this is an admin queue.
		 * Disallow Connect and allow other fabrics commands.
		 */
		switch (cap_hdr->fctype) {
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_SET:
			return nvmf_process_property_set(req);
		case SPDK_NVMF_FABRIC_COMMAND_PROPERTY_GET:
			return nvmf_process_property_get(req);
		default:
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "recv capsule header type invalid [%x]!\n",
				      cap_hdr->fctype);
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
			return true;
		}
	} else {
		/* Session is established, and this is an I/O queue */
		/* For now, no I/O-specific Fabrics commands are implemented (other than Connect) */
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "Unexpected I/O fctype 0x%x\n", cap_hdr->fctype);
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		return true;
	}
}

static void
nvmf_trace_command(union nvmf_h2c_msg *h2c_msg, enum conn_type conn_type)
{
	struct spdk_nvmf_capsule_cmd *cap_hdr = &h2c_msg->nvmf_cmd;
	struct spdk_nvme_cmd *cmd = &h2c_msg->nvme_cmd;
	struct spdk_nvme_sgl_descriptor *sgl = &cmd->dptr.sgl1;
	uint8_t opc;

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		opc = cap_hdr->fctype;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "%s Fabrics cmd: fctype 0x%02x cid %u\n",
			      conn_type == CONN_TYPE_AQ ? "Admin" : "I/O",
			      cap_hdr->fctype, cap_hdr->cid);
	} else {
		opc = cmd->opc;
		SPDK_TRACELOG(SPDK_TRACE_NVMF, "%s cmd: opc 0x%02x fuse %u cid %u nsid %u cdw10 0x%08x\n",
			      conn_type == CONN_TYPE_AQ ? "Admin" : "I/O",
			      cmd->opc, cmd->fuse, cmd->cid, cmd->nsid, cmd->cdw10);
		if (cmd->mptr) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "mptr 0x%" PRIx64 "\n", cmd->mptr);
		}
		if (cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_CONTIG &&
		    cmd->psdt != SPDK_NVME_PSDT_SGL_MPTR_SGL) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "psdt %u\n", cmd->psdt);
		}
	}

	if (spdk_nvme_opc_get_data_transfer(opc) != SPDK_NVME_DATA_NONE) {
		if (sgl->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF,
				      "SGL: Keyed%s: addr 0x%" PRIx64 " key 0x%x len 0x%x\n",
				      sgl->generic.subtype == SPDK_NVME_SGL_SUBTYPE_INVALIDATE_KEY ? " (Inv)" : "",
				      sgl->address, sgl->keyed.key, sgl->keyed.length);
		} else if (sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK) {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "SGL: Data block: %s 0x%" PRIx64 " len 0x%x\n",
				      sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET ? "offs" : "addr",
				      sgl->address, sgl->unkeyed.length);
		} else {
			SPDK_TRACELOG(SPDK_TRACE_NVMF, "SGL type 0x%x subtype 0x%x\n",
				      sgl->generic.type, sgl->generic.subtype);
		}
	}
}

int
spdk_nvmf_request_exec(struct spdk_nvmf_request *req)
{
	struct nvmf_session *session = req->conn->sess;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	bool done;

	nvmf_trace_command(req->cmd, req->conn->type);

	if (cmd->opc == SPDK_NVME_OPC_FABRIC) {
		done = nvmf_process_fabrics_command(req);
	} else if (session == NULL || !session->vcprop.cc.bits.en) {
		/* Only Fabric commands are allowed when the controller is disabled */
		SPDK_ERRLOG("Non-Fabric command sent to disabled controller\n");
		rsp->status.sc = SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR;
		done = true;
	} else if (req->conn->type == CONN_TYPE_AQ) {
		struct spdk_nvmf_subsystem *subsystem;

		subsystem = session->subsys;
		assert(subsystem != NULL);
		if (subsystem->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY) {
			done = nvmf_process_discovery_cmd(req);
		} else {
			done = nvmf_process_admin_cmd(req);
		}
	} else {
		done = nvmf_process_io_cmd(req);
	}

	if (done) {
		/* Synchronous command - response is already filled out */
		return spdk_nvmf_request_complete(req);
	}

	/*
	 * Asynchronous command.
	 * The completion callback will call spdk_nvmf_request_complete().
	 */
	return 0;
}
