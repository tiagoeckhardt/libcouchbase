/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014-2020 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "internal.h"
#include "packetutils.h"
#include "mc/mcreq.h"
#include "mc/compress.h"
#include "trace.h"
#include "collections.h"

#include "capi/cmd_store.hh"
#include "capi/cmd_get.hh"
#include "capi/cmd_remove.hh"
#include "capi/cmd_touch.hh"
#include "capi/cmd_counter.hh"
#include "capi/cmd_unlock.hh"

#define LOGARGS(obj, lvl) (obj)->settings, "handler", LCB_LOG_##lvl, __FILE__, __LINE__

using lcb::MemcachedResponse;

template <typename T>
class ResponsePack
{
  public:
    T resp;
    lcb_MUTATION_TOKEN mt;
    const char *value;
    lcb_SIZE nvalue;
    char *err_ref;
    char *err_ctx;

    ~ResponsePack()
    {
        free(err_ref);
        free(err_ctx);
    }

    static const lcb_MUTATION_TOKEN *get_mt(const lcb_RESPBASE *rb)
    {
        const auto *rp = reinterpret_cast<const ResponsePack *>(rb);
        return &rp->mt;
    }

    static const char *get_err_ctx(const lcb_RESPBASE *rb)
    {
        const auto *rp = reinterpret_cast<const ResponsePack *>(rb);
        if (rp->resp.rflags & LCB_RESP_F_ERRINFO) {
            if (rp->err_ctx) {
                return rp->err_ctx;
            } else {
                parse_enhanced_error(rp);
                return rp->err_ctx;
            }
        }
        return nullptr;
    }

    static const char *get_err_ref(const lcb_RESPBASE *rb)
    {
        const auto *rp = reinterpret_cast<const ResponsePack *>(rb);
        if (rp->resp.rflags & LCB_RESP_F_ERRINFO) {
            if (rp->err_ref) {
                return rp->err_ref;
            } else {
                parse_enhanced_error(rp);
                return rp->err_ref;
            }
        }
        return nullptr;
    }

  private:
    static void parse_enhanced_error(const ResponsePack *rp)
    {
        auto *mrp = const_cast<ResponsePack *>(rp);
        lcb_STATUS rc = MemcachedResponse::parse_enhanced_error(mrp->value, mrp->nvalue, &mrp->err_ref, &mrp->err_ctx);
        if (rc != LCB_SUCCESS) {
            mrp->resp.rflags &= ~LCB_RESP_F_ERRINFO;
        }
    }
};

LIBCOUCHBASE_API
lcb_STATUS lcb_errmap_default(lcb_INSTANCE *instance, lcb_uint16_t in)
{
    switch (in) {
        case PROTOCOL_BINARY_RESPONSE_NOT_MY_VBUCKET:
            return LCB_ERR_TIMEOUT;
        case PROTOCOL_BINARY_RESPONSE_AUTH_CONTINUE:
            return LCB_ERR_AUTH_CONTINUE;
        case PROTOCOL_BINARY_RESPONSE_EBUSY:
        case PROTOCOL_BINARY_RESPONSE_ETMPFAIL:
            return LCB_ERR_TEMPORARY_FAILURE;

        case PROTOCOL_BINARY_RESPONSE_EINTERNAL:
        default:
            if (instance) {
                lcb_log(LOGARGS(instance, ERROR), "Got unhandled memcached error 0x%X", in);
            } else {
                fprintf(stderr, "COUCHBASE: Unhandled memcached status=0x%x\n", in);
            }
            return LCB_ERR_KVENGINE_UNKNOWN_ERROR;
    }
}

lcb_STATUS lcb_map_error(lcb_INSTANCE *instance, int in)
{
    switch (in) {
        case PROTOCOL_BINARY_RESPONSE_SUCCESS:
            return LCB_SUCCESS;
        case PROTOCOL_BINARY_RESPONSE_KEY_ENOENT:
            return LCB_ERR_DOCUMENT_NOT_FOUND;
        case PROTOCOL_BINARY_RESPONSE_E2BIG:
            return LCB_ERR_VALUE_TOO_LARGE;
        case PROTOCOL_BINARY_RESPONSE_ENOMEM:
            return LCB_ERR_TEMPORARY_FAILURE;
        case PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS:
            return LCB_ERR_DOCUMENT_EXISTS;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_ENOENT:
            return LCB_ERR_SUBDOC_PATH_NOT_FOUND;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_MISMATCH:
            return LCB_ERR_SUBDOC_PATH_MISMATCH;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_EINVAL:
            return LCB_ERR_SUBDOC_PATH_INVALID;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_E2BIG:
            return LCB_ERR_SUBDOC_PATH_TOO_BIG;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_DOC_E2DEEP:
            return LCB_ERR_SUBDOC_PATH_TOO_DEEP;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_VALUE_ETOODEEP:
            return LCB_ERR_SUBDOC_VALUE_TOO_DEEP;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_VALUE_CANTINSERT:
            return LCB_ERR_SUBDOC_VALUE_INVALID;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_DOC_NOTJSON:
            return LCB_ERR_SUBDOC_DOCUMENT_NOT_JSON;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_NUM_ERANGE:
            return LCB_ERR_SUBDOC_NUMBER_TOO_BIG;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_DELTA_ERANGE:
            return LCB_ERR_SUBDOC_DELTA_INVALID;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_PATH_EEXISTS:
            return LCB_ERR_SUBDOC_PATH_EXISTS;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_MULTI_PATH_FAILURE:
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_MULTI_PATH_FAILURE_DELETED:
            return LCB_SUCCESS; /* the real codes must be discovered on sub-result level */
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_INVALID_COMBO:
            return LCB_ERR_INVALID_ARGUMENT;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_SUCCESS_DELETED:
            return LCB_SUCCESS;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_XATTR_INVALID_FLAG_COMBO:
            return LCB_ERR_SUBDOC_XATTR_INVALID_FLAG_COMBO;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_XATTR_INVALID_KEY_COMBO:
            return LCB_ERR_SUBDOC_XATTR_INVALID_KEY_COMBO;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_XATTR_UNKNOWN_MACRO:
            return LCB_ERR_SUBDOC_XATTR_UNKNOWN_MACRO;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_XATTR_UNKNOWN_VATTR:
            return LCB_ERR_SUBDOC_XATTR_UNKNOWN_VIRTUAL_ATTRIBUTE;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_XATTR_CANT_MODIFY_VATTR:
            return LCB_ERR_SUBDOC_XATTR_CANNOT_MODIFY_VIRTUAL_ATTRIBUTE;
        case PROTOCOL_BINARY_RESPONSE_SUBDOC_INVALID_XATTR_ORDER:
            return LCB_ERR_SUBDOC_XATTR_INVALID_ORDER;
        case PROTOCOL_BINARY_RESPONSE_EINVAL:
            return LCB_ERR_KVENGINE_INVALID_PACKET;
        case PROTOCOL_BINARY_RESPONSE_NOT_STORED:
            return LCB_ERR_NOT_STORED;
        case PROTOCOL_BINARY_RESPONSE_DELTA_BADVAL:
            return LCB_ERR_INVALID_DELTA;
        case PROTOCOL_BINARY_RESPONSE_ERANGE:
            return LCB_ERR_INVALID_RANGE;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND:
            return LCB_ERR_UNSUPPORTED_OPERATION;
        case PROTOCOL_BINARY_RESPONSE_EACCESS:
        case PROTOCOL_BINARY_RESPONSE_AUTH_ERROR:
        case PROTOCOL_BINARY_RESPONSE_AUTH_STALE:
            return LCB_ERR_AUTHENTICATION_FAILURE;
        case PROTOCOL_BINARY_RESPONSE_NO_BUCKET:
        case PROTOCOL_BINARY_RESPONSE_NOT_INITIALIZED:
            return LCB_ERR_BUCKET_NOT_FOUND;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_COLLECTION:
            return LCB_ERR_COLLECTION_NOT_FOUND;
        case PROTOCOL_BINARY_RESPONSE_UNKNOWN_SCOPE:
            return LCB_ERR_SCOPE_NOT_FOUND;
        case PROTOCOL_BINARY_RESPONSE_NO_COLLECTIONS_MANIFEST:
            return LCB_ERR_COLLECTION_NO_MANIFEST;
        case PROTOCOL_BINARY_RESPONSE_CANNOT_APPLY_COLLECTIONS_MANIFEST:
            return LCB_ERR_COLLECTION_CANNOT_APPLY_MANIFEST;
        case PROTOCOL_BINARY_RESPONSE_COLLECTIONS_MANIFEST_IS_AHEAD:
            return LCB_ERR_COLLECTION_MANIFEST_IS_AHEAD;
        case PROTOCOL_BINARY_RESPONSE_DURABILITY_INVALID_LEVEL:
            return LCB_ERR_DURABILITY_LEVEL_NOT_AVAILABLE;
        case PROTOCOL_BINARY_RESPONSE_DURABILITY_IMPOSSIBLE:
            return LCB_ERR_DURABILITY_IMPOSSIBLE;
        case PROTOCOL_BINARY_RESPONSE_SYNC_WRITE_IN_PROGRESS:
            return LCB_ERR_DURABLE_WRITE_IN_PROGRESS;
        case PROTOCOL_BINARY_RESPONSE_SYNC_WRITE_RE_COMMIT_IN_PROGRESS:
            return LCB_ERR_DURABLE_WRITE_RE_COMMIT_IN_PROGRESS;
        case PROTOCOL_BINARY_RESPONSE_SYNC_WRITE_AMBIGUOUS:
            return LCB_ERR_DURABILITY_AMBIGUOUS;
        case PROTOCOL_BINARY_RESPONSE_LOCKED:
            return LCB_ERR_DOCUMENT_LOCKED;
        default:
            if (instance != nullptr) {
                return instance->callbacks.errmap(instance, in);
            } else {
                return lcb_errmap_default(nullptr, in);
            }
    }
}

static lcb_RESPCALLBACK find_callback(lcb_INSTANCE *instance, lcb_CALLBACK_TYPE type)
{
    lcb_RESPCALLBACK cb = instance->callbacks.v3callbacks[type];
    if (!cb) {
        cb = lcb_find_callback(instance, type);
    }
    return cb;
}

/**
 * This file contains the mapping of various protocol response codes for
 * a given command. Each handler receives the following parameters:
 *
 * @param pipeline the pipeline (or "Server") upon which the request was sent
 * (and response was received)
 *
 * @param request the original request (including associated data). The request
 *  may be used to determine additional information about it, such as the
 *  user-defined "Cookie", number of related requests remaining, and more.
 *
 * @param response the response which was received. This is an opaque
 *  representation of a memcached response packet
 *
 * @param immerr in the case of an abnormal failure (i.e. network failure) the
 *  handler will be invoked with this callback set to a non-success value. The
 *  'info' structure will still contain a valid (albeit empty and cryptic)
 *  header. If the user depends on special data being found in the payload then
 *  the handler must check that this variable is set to LCB_SUCCESS before
 *  continuing. Also note that a negative reply may also be present within
 *  the response itself; however this is not the purpose of this parameter.
 *
 * @return request status
 *  The return value should indicate whether outstanding responses remain
 *  to be received for this request, or if this request is deemed to be
 *  satisfied.
 */

template <typename T>
void make_error(lcb_INSTANCE *instance, T *resp, const MemcachedResponse *response, lcb_STATUS imm,
                const mc_PACKET *req)
{
    if (imm) {
        resp->ctx.rc = imm;
        resp->rflags |= LCB_RESP_F_CLIENTGEN;
    } else if (response->status() == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        resp->ctx.rc = LCB_SUCCESS;
    } else {
        resp->ctx.rc = lcb_map_error(instance, response->status());
    }
    if (resp->ctx.rc == LCB_ERR_DOCUMENT_EXISTS && (req->flags & MCREQ_F_REPLACE_SEMANTICS) != 0) {
        resp->ctx.rc = LCB_ERR_CAS_MISMATCH;
    }
}

template <typename T>
void handle_error_info(const MemcachedResponse *mc_resp, ResponsePack<T> *rp)
{
    if (mc_resp->status() != PROTOCOL_BINARY_RESPONSE_SUCCESS && mc_resp->datatype() & PROTOCOL_BINARY_DATATYPE_JSON &&
        mc_resp->vallen() > 0) {
        rp->resp.rflags |= LCB_RESP_F_ERRINFO;
        rp->value = mc_resp->value();
        rp->nvalue = mc_resp->vallen();
    }
}

template <typename T>
void init_resp(lcb_INSTANCE *instance, mc_PIPELINE *pipeline, const MemcachedResponse *mc_resp, const mc_PACKET *req,
               lcb_STATUS immerr, T *resp)
{
    make_error(instance, resp, mc_resp, immerr, req);
    resp->ctx.status_code = mc_resp->status();
    resp->ctx.cas = mc_resp->cas();
    resp->ctx.opaque = mc_resp->opaque();
    if (instance) {
        resp->ctx.bucket = LCBT_VBCONFIG(instance)->bname;
        resp->ctx.bucket_len = LCBT_VBCONFIG(instance)->bname_len;
    }
    resp->cookie = const_cast<void *>(MCREQ_PKT_COOKIE(req));
    mcreq_get_key(instance, req, &resp->ctx.key, &resp->ctx.key_len);

    auto *server = static_cast<lcb::Server *>(pipeline);
    const lcb_host_t *remote = server->curhost;
    if (remote) {
        char *ptr = resp->ctx.endpoint;
        if (remote->ipv6) {
            *ptr = '[';
            ptr++;
        }
        strcpy(ptr, remote->host);
        ptr += strlen(remote->host);
        if (remote->ipv6) {
            *ptr = ']';
            ptr++;
        }
        *ptr = ':';
        ptr++;
        strcpy(ptr, remote->port);
        ptr += strlen(remote->port);
        *ptr = '\0';
        ptr++;
        resp->ctx.endpoint_len = ptr - resp->ctx.endpoint - 1;
    }
}

/**
 * Handles the propagation and population of the 'mutation token' information.
 * @param mc_resp The response packet
 * @param req The request packet (used to get the vBucket)
 * @param tgt Pointer to mutation token which should be populated.
 */
static void handle_mutation_token(lcb_INSTANCE *instance, const MemcachedResponse *mc_resp, const mc_PACKET *req,
                                  lcb_MUTATION_TOKEN *stok)
{
    const char *sbuf;
    uint16_t vbid;
    if (mc_resp->extlen() == 0) {
        return; /* No extras */
    }

    if (instance != nullptr && instance->dcpinfo == nullptr) {
        size_t nvb = LCBT_VBCONFIG(instance)->nvb;
        if (nvb) {
            instance->dcpinfo = new lcb_MUTATION_TOKEN[nvb];
            memset(instance->dcpinfo, 0, sizeof(*instance->dcpinfo) * nvb);
        }
    }

    sbuf = mc_resp->ext();
    vbid = mcreq_get_vbucket(req);
    stok->vbid_ = vbid;
    memcpy(&stok->uuid_, sbuf, 8);
    memcpy(&stok->seqno_, sbuf + 8, 8);

    stok->uuid_ = lcb_ntohll(stok->uuid_);
    stok->seqno_ = lcb_ntohll(stok->seqno_);

    if (instance != nullptr && instance->dcpinfo) {
        instance->dcpinfo[vbid] = *stok;
    }
}

static lcb_INSTANCE *get_instance(mc_PIPELINE *pipeline)
{
    auto cq = pipeline->parent;
    if (cq == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<lcb_INSTANCE *>(cq->cqdata);
}

template <typename T>
void invoke_callback(const mc_PACKET *pkt, lcb_INSTANCE *instance, T *resp, lcb_CALLBACK_TYPE cbtype)
{
    if (instance != nullptr) {
        std::string collection_path = instance->collcache->id_to_name(mcreq_get_cid(instance, pkt));
        if (!collection_path.empty()) {
            size_t dot = collection_path.find('.');
            if (dot != std::string::npos) {
                resp->ctx.scope = collection_path.c_str();
                resp->ctx.scope_len = dot;
                resp->ctx.collection = collection_path.c_str() + dot + 1;
                resp->ctx.collection_len = collection_path.size() - (dot + 1);
            }
        }
    }
    if (!(pkt->flags & MCREQ_F_INVOKED)) {
        resp->cookie = const_cast<void *>(MCREQ_PKT_COOKIE(pkt));
        const auto *base = reinterpret_cast<const lcb_RESPBASE *>(resp);
        if ((pkt->flags & MCREQ_F_PRIVCALLBACK) == 0) {
            if (instance != nullptr) {
                find_callback(instance, cbtype)(instance, cbtype, base);
            }
        } else {
            (*(lcb_RESPCALLBACK *)resp->cookie)(instance, cbtype, base);
        }
    }
}

template <typename T>
void invoke_callback(const mc_PACKET *pkt, mc_PIPELINE *pipeline, T *resp, lcb_CALLBACK_TYPE cbtype)
{
    invoke_callback(pkt, get_instance(pipeline), cbtype, resp);
}

/**
 * Optionally decompress an incoming payload.
 * @param o The instance
 * @param resp The response received
 * @param[out] bytes pointer to the final payload
 * @param[out] nbytes pointer to the size of the final payload
 * @param[out] freeptr pointer to free. This should be initialized to `nullptr`.
 * If temporary dynamic storage is required this will be set to the allocated
 * pointer upon return. Otherwise it will be set to nullptr. In any case it must
 */
static void maybe_decompress(lcb_INSTANCE *o, const MemcachedResponse *respkt, lcb_RESPGET *rescmd, void **freeptr)
{
    lcb_U8 dtype = 0;
    if (!respkt->vallen()) {
        return;
    }

    if (respkt->datatype() & PROTOCOL_BINARY_DATATYPE_JSON) {
        dtype = LCB_VALUE_F_JSON;
    }

    if (respkt->datatype() & PROTOCOL_BINARY_DATATYPE_COMPRESSED) {
        if (LCBT_SETTING(o, compressopts) & LCB_COMPRESS_IN) {
            /* if we inflate, we don't set the flag */
            mcreq_inflate_value(respkt->value(), respkt->vallen(), &rescmd->value, &rescmd->nvalue, freeptr);

        } else {
            /* user doesn't want inflation. signal it's compressed */
            dtype |= LCB_VALUE_F_SNAPPYCOMP;
        }
    }
    rescmd->datatype = dtype;
}

static void H_get(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    ResponsePack<lcb_RESPGET> w{};
    lcb_RESPGET &resp = w.resp;

    lcb_INSTANCE *o = get_instance(pipeline);
    init_resp(o, pipeline, response, request, immerr, &resp);
    handle_error_info(response, &w);
    resp.rflags |= LCB_RESP_F_FINAL;

    if (resp.ctx.rc == LCB_SUCCESS) {
        resp.datatype = response->datatype();
        resp.value = response->value();
        resp.nvalue = response->vallen();
        resp.bufh = response->bufseg();
        if (response->extlen() == sizeof(uint32_t)) {
            memcpy(&resp.itmflags, response->ext(), sizeof(uint32_t));
            resp.itmflags = ntohl(resp.itmflags);
        }
    }

    void *freeptr = nullptr;
    maybe_decompress(o, response, &resp, &freeptr);
    LCBTRACE_KV_FINISH(pipeline, request, resp, response);
    TRACE_GET_END(o, request, response, &resp);
    invoke_callback(request, o, &resp, LCB_CALLBACK_GET);
    free(freeptr);
}

static void H_exists(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    lcb_RESPEXISTS resp{};
    init_resp(root, pipeline, response, request, immerr, &resp);
    resp.cookie = const_cast<void *>(MCREQ_PKT_COOKIE(request));
    resp.rflags |= LCB_RESP_F_FINAL;
    if (resp.ctx.rc == LCB_SUCCESS) {
        if (response->extlen() == (sizeof(uint32_t) * 3 + sizeof(uint64_t))) {
            const char *ptr = response->ext();
            memcpy(&resp.deleted, ptr, sizeof(uint32_t));
            resp.deleted = ntohl(resp.deleted);
            ptr += sizeof(uint32_t);
            memcpy(&resp.flags, ptr, sizeof(uint32_t));
            resp.flags = ntohl(resp.flags);
            ptr += sizeof(uint32_t);
            memcpy(&resp.expiry, ptr, sizeof(uint32_t));
            resp.expiry = ntohl(resp.expiry);
            ptr += sizeof(uint32_t);
            memcpy(&resp.seqno, ptr, sizeof(uint64_t));
            resp.seqno = lcb_ntohll(resp.seqno);
        }
    }
    LCBTRACE_KV_FINISH(pipeline, request, resp, response);
    TRACE_EXISTS_END(root, request, response, &resp);
    invoke_callback(request, root, &resp, LCB_CALLBACK_EXISTS);
}

static void H_getreplica(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    ResponsePack<lcb_RESPGET> w{};
    lcb_RESPGET &resp = w.resp;
    lcb_INSTANCE *instance = get_instance(pipeline);
    void *freeptr = nullptr;
    mc_REQDATAEX *rd = request->u_rdata.exdata;

    init_resp(instance, pipeline, response, request, immerr, &resp);
    handle_error_info(response, &w);

    if (resp.ctx.rc == LCB_SUCCESS) {
        resp.datatype = response->datatype();
        resp.value = response->value();
        resp.nvalue = response->vallen();
        resp.bufh = response->bufseg();
        if (response->extlen() == sizeof(uint32_t)) {
            memcpy(&resp.itmflags, response->ext(), sizeof(uint32_t));
            resp.itmflags = ntohl(resp.itmflags);
        }
    }

    maybe_decompress(instance, response, &resp, &freeptr);
    rd->procs->handler(pipeline, request, resp.ctx.rc, &resp);
    free(freeptr);
}

static int lcb_sdresult_next(const lcb_RESPSUBDOC *resp, lcb_SDENTRY *ent, size_t *iter);

static void lcb_sdresult_parse(lcb_RESPSUBDOC *resp, lcb_CALLBACK_TYPE type)
{
    std::vector<lcb_SDENTRY> results;
    size_t iter = 0, oix = 0;
    lcb_SDENTRY ent;
    results.resize(resp->nres);

    while (lcb_sdresult_next(resp, &ent, &iter)) {
        size_t index = oix++;
        if (type == LCB_CALLBACK_SDMUTATE) {
            index = ent.index;
        }
        results[index] = ent;
    }
    if (resp->nres) {
        resp->res = (lcb_SDENTRY *)calloc(resp->nres, sizeof(lcb_SDENTRY));
        for (size_t ii = 0; ii < resp->nres; ii++) {
            resp->res[ii] = results[ii];
        }
    }
}

static void H_subdoc(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *o = get_instance(pipeline);
    ResponsePack<lcb_RESPSUBDOC> w{};
    lcb_CALLBACK_TYPE cbtype;
    init_resp(o, pipeline, response, request, immerr, &w.resp);
    w.resp.rflags |= LCB_RESP_F_FINAL;
    w.resp.res = nullptr;

    /* For mutations, add the mutation token */
    switch (response->opcode()) {
        case PROTOCOL_BINARY_CMD_SUBDOC_GET:
        case PROTOCOL_BINARY_CMD_SUBDOC_EXISTS:
        case PROTOCOL_BINARY_CMD_SUBDOC_GET_COUNT:
        case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP:
            cbtype = LCB_CALLBACK_SDLOOKUP;
            break;

        default:
            handle_mutation_token(o, response, request, &w.mt);
            w.resp.rflags |= LCB_RESP_F_EXTDATA;
            cbtype = LCB_CALLBACK_SDMUTATE;
            break;
    }

    if (response->opcode() == PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP ||
        response->opcode() == PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION) {
        if (w.resp.ctx.rc == LCB_SUCCESS) {
            w.resp.responses = response;
            w.resp.nres = MCREQ_PKT_RDATA(request)->nsubreq;
            lcb_sdresult_parse(&w.resp, cbtype);
        } else {
            handle_error_info(response, &w);
        }
    } else {
        /* Single response */
        w.resp.rflags |= LCB_RESP_F_SDSINGLE;
        if (w.resp.ctx.rc == LCB_SUCCESS || LCB_ERROR_IS_SUBDOC(w.resp.ctx.rc)) {
            w.resp.responses = response;
            lcb_sdresult_parse(&w.resp, cbtype);
        } else {
            handle_error_info(response, &w);
        }
    }
    invoke_callback(request, o, &w.resp, cbtype);
    free(w.resp.res);
}

static int sdlookup_next(const MemcachedResponse *response, lcb_SDENTRY *ent, size_t *iter)
{
    const char *buf;
    uint16_t rc;
    uint32_t vlen;

    if (*iter == response->vallen()) {
        return 0;
    }

    buf = response->value();
    buf += *iter;

    memcpy(&rc, buf, 2);
    memcpy(&vlen, buf + 2, 4);

    rc = ntohs(rc);
    vlen = ntohl(vlen);

    ent->status = lcb_map_error(nullptr, rc);
    ent->nvalue = vlen;

    if (ent->status == LCB_SUCCESS) {
        ent->value = buf + 6;
    } else {
        ent->value = nullptr;
        ent->nvalue = 0;
    }

    *iter += (6 + vlen);
    return 1;
}

static int sdmutate_next(const MemcachedResponse *response, lcb_SDENTRY *ent, size_t *iter)
{
    const char *buf, *buf_end;
    uint16_t rc;
    uint32_t vlen;

    if (*iter == response->vallen()) {
        return 0;
    }

    buf_end = (const char *)response->value() + response->vallen();
    buf = ((const char *)(response->value())) + *iter;

#define ADVANCE_BUF(sz)                                                                                                \
    buf += sz;                                                                                                         \
    *iter += sz;                                                                                                       \
    lcb_assert(buf <= buf_end)

    /* Index */
    ent->index = *(lcb_U8 *)buf;
    ADVANCE_BUF(1);

    /* Status */
    memcpy(&rc, buf, 2);
    ADVANCE_BUF(2);

    rc = ntohs(rc);
    ent->status = lcb_map_error(nullptr, rc);

    if (rc == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
        memcpy(&vlen, buf, 4);
        ADVANCE_BUF(4);

        vlen = ntohl(vlen);
        ent->nvalue = vlen;
        ent->value = buf;
        ADVANCE_BUF(vlen);

    } else {
        ent->value = nullptr;
        ent->nvalue = 0;
    }

    (void)buf_end;
    return 1;
#undef ADVANCE_BUF
}

static int lcb_sdresult_next(const lcb_RESPSUBDOC *resp, lcb_SDENTRY *ent, size_t *iter)

{
    size_t iter_s = 0;
    const auto *response = reinterpret_cast<const MemcachedResponse *>(resp->responses);
    if (!response) {
        fprintf(stderr, "no response set\n");
        return 0;
    }
    if (!iter) {
        /* Single response */
        iter = &iter_s;
    }

    switch (response->opcode()) {
        case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP:
            return sdlookup_next(response, ent, iter);
        case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION:
            return sdmutate_next(response, ent, iter);
        default:
            if (*iter) {
                return 0;
            }
            *iter = 1;

            ent->status = lcb_map_error(nullptr, response->status());
            ent->value = response->value();
            ent->nvalue = response->vallen();
            ent->index = 0;
            return 1;
    }
}

static void H_delete(mc_PIPELINE *pipeline, mc_PACKET *packet, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPREMOVE> w{};
    w.resp.rflags |= LCB_RESP_F_EXTDATA | LCB_RESP_F_FINAL;
    init_resp(root, pipeline, response, packet, immerr, &w.resp);
    handle_error_info(response, &w);
    handle_mutation_token(root, response, packet, &w.mt);
    LCBTRACE_KV_FINISH(pipeline, packet, w.resp, response);
    TRACE_REMOVE_END(root, packet, response, &w.resp);
    invoke_callback(packet, root, &w.resp, LCB_CALLBACK_REMOVE);
}

static void H_observe(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    uint32_t ttp;
    uint32_t ttr;
    size_t pos;
    lcbvb_CONFIG *config;
    const char *end, *ptr;
    mc_REQDATAEX *rd = request->u_rdata.exdata;

    lcb_RESPOBSERVE resp{};
    make_error(root, &resp, response, immerr, request);

    if (resp.ctx.rc != LCB_SUCCESS) {
        if (!(request->flags & MCREQ_F_INVOKED)) {
            rd->procs->handler(pipeline, request, resp.ctx.rc, nullptr);
        }
        return;
    }

    /** The CAS field is split into TTP/TTR values */
    uint64_t tmpcas = lcb_htonll(response->cas());
    ptr = reinterpret_cast<char *>(&tmpcas);
    memcpy(&ttp, ptr, sizeof(ttp));
    memcpy(&ttr, ptr + sizeof(ttp), sizeof(ttp));

    ttp = ntohl(ttp);
    ttr = ntohl(ttr);

    /** Actual payload sequence of (vb, nkey, key). Repeats multiple times */
    ptr = response->value();
    end = ptr + response->vallen();
    config = pipeline->parent->config;

    for (pos = 0; ptr < end; pos++) {
        uint64_t cas;
        uint8_t obs;
        uint16_t nkey, vb;
        const char *key;

        memcpy(&vb, ptr, sizeof(vb));
        vb = ntohs(vb);
        ptr += sizeof(vb);
        memcpy(&nkey, ptr, sizeof(nkey));
        nkey = ntohs(nkey);
        ptr += sizeof(nkey);
        key = (const char *)ptr;
        ptr += nkey;
        obs = *((lcb_uint8_t *)ptr);
        ptr += sizeof(obs);
        memcpy(&cas, ptr, sizeof(cas));
        ptr += sizeof(cas);

        int ncid = 0;
        if (LCBT_SETTING(root, use_collections)) {
            uint32_t cid = 0;
            ncid = leb128_decode((uint8_t *)key, nkey, &cid);
        }
        resp.ctx.key = key + ncid;
        resp.ctx.key_len = nkey - ncid;
        resp.ctx.cas = lcb_ntohll(cas);
        resp.status = obs;
        resp.ismaster = pipeline->index == lcbvb_vbmaster(config, vb);
        resp.ttp = ttp;
        resp.ttr = ttr;
        TRACE_OBSERVE_PROGRESS(root, request, response, &resp);
        LCBTRACE_KV_FINISH(pipeline, request, resp, response);
        if (!(request->flags & MCREQ_F_INVOKED)) {
            rd->procs->handler(pipeline, request, resp.ctx.rc, &resp);
        }
    }
}

static void H_observe_seqno(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    lcb_RESPOBSEQNO resp{};
    init_resp(root, pipeline, response, request, immerr, &resp);

    resp.server_index = pipeline->index;

    if (resp.ctx.rc == LCB_SUCCESS) {
        const auto *data = reinterpret_cast<const uint8_t *>(response->value());
        bool is_failover = *data != 0;

        data++;
#define COPY_ADV(dstfld, n, conv_fn)                                                                                   \
    memcpy(&resp.dstfld, data, n);                                                                                     \
    data += n;                                                                                                         \
    resp.dstfld = conv_fn(resp.dstfld)

        COPY_ADV(vbid, 2, ntohs);
        COPY_ADV(cur_uuid, 8, lcb_ntohll);
        COPY_ADV(persisted_seqno, 8, lcb_ntohll);
        COPY_ADV(mem_seqno, 8, lcb_ntohll);
        if (is_failover) {
            COPY_ADV(old_uuid, 8, lcb_ntohll);
            COPY_ADV(old_seqno, 8, lcb_ntohll);
        }
#undef COPY_ADV

        /* Get the server for this command. Note that since this is a successful
         * operation, the server is never a dummy */
    }
    LCBTRACE_KV_FINISH(pipeline, request, resp, response);
    invoke_callback(request, root, &resp, LCB_CALLBACK_OBSEQNO);
}

static void H_store(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPSTORE> w{};
    uint8_t opcode;
    init_resp(root, pipeline, response, request, immerr, &w.resp);
    handle_error_info(response, &w);
    if (!immerr) {
        opcode = response->opcode();
    } else {
        protocol_binary_request_header hdr;
        mcreq_read_hdr(request, &hdr);
        opcode = hdr.request.opcode;
    }
    if (opcode == PROTOCOL_BINARY_CMD_ADD) {
        w.resp.op = LCB_STORE_INSERT;
    } else if (opcode == PROTOCOL_BINARY_CMD_REPLACE) {
        w.resp.op = LCB_STORE_REPLACE;
    } else if (opcode == PROTOCOL_BINARY_CMD_APPEND) {
        w.resp.op = LCB_STORE_APPEND;
    } else if (opcode == PROTOCOL_BINARY_CMD_PREPEND) {
        w.resp.op = LCB_STORE_PREPEND;
    } else if (opcode == PROTOCOL_BINARY_CMD_SET) {
        w.resp.op = LCB_STORE_UPSERT;
    }
    w.resp.rflags |= LCB_RESP_F_EXTDATA | LCB_RESP_F_FINAL;
    handle_mutation_token(root, response, request, &w.mt);
    TRACE_STORE_END(root, request, response, &w.resp);
    if (request->flags & MCREQ_F_REQEXT) {
        LCBTRACE_KV_COMPLETE(pipeline, request, w.resp, response);
        request->u_rdata.exdata->procs->handler(pipeline, request, immerr, &w.resp);
    } else {
        LCBTRACE_KV_FINISH(pipeline, request, w.resp, response);
        invoke_callback(request, root, &w.resp, LCB_CALLBACK_STORE);
    }
}

static void H_arithmetic(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPCOUNTER> w{};
    init_resp(root, pipeline, response, request, immerr, &w.resp);

    if (w.resp.ctx.rc == LCB_SUCCESS) {
        memcpy(&w.resp.value, response->value(), sizeof(w.resp.value));
        w.resp.value = lcb_ntohll(w.resp.value);
        w.resp.rflags |= LCB_RESP_F_EXTDATA;
        handle_mutation_token(root, response, request, &w.mt);
    } else {
        handle_error_info(response, &w);
    }
    w.resp.rflags |= LCB_RESP_F_FINAL;
    w.resp.ctx.cas = response->cas();
    LCBTRACE_KV_FINISH(pipeline, request, w.resp, response);
    TRACE_ARITHMETIC_END(root, request, response, &w.resp);
    invoke_callback(request, root, &w.resp, LCB_CALLBACK_COUNTER);
}

static void H_stats(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    lcb_RESPSTATS resp{};
    mc_REQDATAEX *exdata;

    make_error(root, &resp, response, immerr, request);

    exdata = request->u_rdata.exdata;
    if (resp.ctx.rc != LCB_SUCCESS || response->keylen() == 0) {
        /* Call the handler without a response, this indicates that this server
         * has finished responding */
        exdata->procs->handler(pipeline, request, resp.ctx.rc, nullptr);
        return;
    }

    if ((resp.ctx.key_len = response->keylen())) {
        resp.ctx.key = response->key();
        if ((resp.value = response->value())) {
            resp.nvalue = response->vallen();
        }
    }

    exdata->procs->handler(pipeline, request, resp.ctx.rc, &resp);
}

static void H_collections_get_manifest(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response,
                                       lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPGETMANIFEST> w{};
    lcb_RESPGETMANIFEST &resp = w.resp;
    init_resp(root, pipeline, response, request, immerr, &resp);
    handle_error_info(response, &w);
    resp.rflags |= LCB_RESP_F_FINAL;
    resp.value = response->value();
    resp.nvalue = response->vallen();
    invoke_callback(request, root, &resp, LCB_CALLBACK_COLLECTIONS_GET_MANIFEST);
}

static void H_collections_get_cid(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response,
                                  lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPGETCID> w{};
    lcb_RESPGETCID &resp = w.resp;
    init_resp(root, pipeline, response, request, immerr, &resp);
    handle_error_info(response, &w);
    resp.rflags |= LCB_RESP_F_FINAL;

    if (resp.ctx.rc == LCB_SUCCESS) {
        const char *ptr = response->ext();
        if (ptr) {
            memcpy(&resp.manifest_id, ptr, sizeof(uint64_t));
            resp.manifest_id = lcb_ntohll(resp.manifest_id);
            ptr += sizeof(uint64_t);
            memcpy(&resp.collection_id, ptr, sizeof(uint32_t));
            resp.collection_id = ntohl(resp.collection_id);
        } else {
            resp.manifest_id = 0;
            resp.collection_id = 0;
            resp.ctx.rc = LCB_ERR_UNSUPPORTED_OPERATION;
        }
    }

    if (request->flags & MCREQ_F_REQEXT) {
        if (resp.ctx.key && resp.ctx.key_len) {
            const char *dot = strchr(resp.ctx.key, '.');
            if (dot) {
                resp.ctx.scope = resp.ctx.key;
                resp.ctx.scope_len = dot - resp.ctx.key;
                resp.ctx.collection = dot + 1;
                resp.ctx.collection_len = resp.ctx.key_len - (dot - resp.ctx.key + 1);
            }
        }
        request->u_rdata.exdata->procs->handler(pipeline, request, resp.ctx.rc, &resp);
    } else {
        invoke_callback(request, root, &resp, LCB_CALLBACK_GETCID);
    }
}

static void H_noop(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    lcb_RESPNOOP resp{};
    mc_REQDATAEX *exdata = request->u_rdata.exdata;

    make_error(root, &resp, response, immerr, request);

    exdata->procs->handler(pipeline, request, resp.ctx.rc, &resp);
}

static void H_touch(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPTOUCH> w{};
    lcb_RESPTOUCH &resp = w.resp;
    init_resp(root, pipeline, response, request, immerr, &resp);
    handle_error_info(response, &w);
    resp.rflags |= LCB_RESP_F_FINAL;
    LCBTRACE_KV_FINISH(pipeline, request, resp, response);
    TRACE_TOUCH_END(root, request, response, &resp);
    invoke_callback(request, root, &resp, LCB_CALLBACK_TOUCH);
}

static void H_unlock(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_INSTANCE *root = get_instance(pipeline);
    ResponsePack<lcb_RESPUNLOCK> w{};
    lcb_RESPUNLOCK &resp = w.resp;
    init_resp(root, pipeline, response, request, immerr, &resp);
    handle_error_info(response, &w);
    resp.rflags |= LCB_RESP_F_FINAL;
    LCBTRACE_KV_FINISH(pipeline, request, resp, response);
    TRACE_UNLOCK_END(root, request, response, &resp);
    invoke_callback(request, root, &resp, LCB_CALLBACK_UNLOCK);
}

static void H_config(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    if (pipeline->parent == nullptr) {
        return;
    }
    /** We just jump to the normal config handler */
    lcb_RESPBASE dummy{};
    mc_REQDATAEX *exdata = request->u_rdata.exdata;
    make_error(get_instance(pipeline), &dummy, response, immerr, request);

    exdata->procs->handler(pipeline, request, dummy.ctx.rc, response);
}

static void H_select_bucket(mc_PIPELINE *pipeline, mc_PACKET *request, MemcachedResponse *response, lcb_STATUS immerr)
{
    lcb_RESPBASE dummy{};
    mc_REQDATAEX *exdata = request->u_rdata.exdata;
    if (exdata) {
        make_error(get_instance(pipeline), &dummy, response, immerr, request);
        exdata->procs->handler(pipeline, request, dummy.ctx.rc, response);
    }
}

static void record_metrics(mc_PIPELINE *pipeline, mc_PACKET *req, MemcachedResponse *)
{
    lcb_INSTANCE *instance = get_instance(pipeline);
    if (instance == nullptr) {
        return; /* the instance already destroyed */
    }
    if (
#ifdef HAVE_DTRACE
        1
#else
        instance->kv_timings
#endif
    ) {
        MCREQ_PKT_RDATA(req)->dispatch = gethrtime();
    }
    if (instance->kv_timings) {
        lcb_histogram_record(instance->kv_timings, MCREQ_PKT_RDATA(req)->dispatch - MCREQ_PKT_RDATA(req)->start);
    }
}

static void dispatch_ufwd_error(mc_PIPELINE *pipeline, mc_PACKET *req, lcb_STATUS immerr)
{
    lcb_PKTFWDRESP resp = {0};
    lcb_INSTANCE *instance = static_cast<lcb::Server *>(pipeline)->get_instance();
    lcb_assert(immerr != LCB_SUCCESS);
    resp.version = 0;
    instance->callbacks.pktfwd(instance, MCREQ_PKT_COOKIE(req), immerr, &resp);
}

int mcreq_dispatch_response(mc_PIPELINE *pipeline, mc_PACKET *req, MemcachedResponse *res, lcb_STATUS immerr)
{
    record_metrics(pipeline, req, res);

    if (req->flags & MCREQ_F_UFWD) {
        dispatch_ufwd_error(pipeline, req, immerr);
        return 0;
    }

#define INVOKE_OP(handler)                                                                                             \
    handler(pipeline, req, res, immerr);                                                                               \
    return 0;                                                                                                          \
    break

    switch (res->opcode()) {
        case PROTOCOL_BINARY_CMD_GET:
        case PROTOCOL_BINARY_CMD_GAT:
        case PROTOCOL_BINARY_CMD_GET_LOCKED:
            INVOKE_OP(H_get);

        case PROTOCOL_BINARY_CMD_ADD:
        case PROTOCOL_BINARY_CMD_REPLACE:
        case PROTOCOL_BINARY_CMD_SET:
        case PROTOCOL_BINARY_CMD_APPEND:
        case PROTOCOL_BINARY_CMD_PREPEND:
            INVOKE_OP(H_store);

        case PROTOCOL_BINARY_CMD_INCREMENT:
        case PROTOCOL_BINARY_CMD_DECREMENT:
            INVOKE_OP(H_arithmetic);

        case PROTOCOL_BINARY_CMD_SUBDOC_GET:
        case PROTOCOL_BINARY_CMD_SUBDOC_EXISTS:
        case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE:
        case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST:
        case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST:
        case PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT:
        case PROTOCOL_BINARY_CMD_SUBDOC_DICT_ADD:
        case PROTOCOL_BINARY_CMD_SUBDOC_DICT_UPSERT:
        case PROTOCOL_BINARY_CMD_SUBDOC_REPLACE:
        case PROTOCOL_BINARY_CMD_SUBDOC_DELETE:
        case PROTOCOL_BINARY_CMD_SUBDOC_COUNTER:
        case PROTOCOL_BINARY_CMD_SUBDOC_GET_COUNT:
        case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP:
        case PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION:
            INVOKE_OP(H_subdoc);

        case PROTOCOL_BINARY_CMD_OBSERVE:
            INVOKE_OP(H_observe);

        case PROTOCOL_BINARY_CMD_GET_REPLICA:
            INVOKE_OP(H_getreplica);

        case PROTOCOL_BINARY_CMD_UNLOCK_KEY:
            INVOKE_OP(H_unlock);

        case PROTOCOL_BINARY_CMD_DELETE:
            INVOKE_OP(H_delete);

        case PROTOCOL_BINARY_CMD_TOUCH:
            INVOKE_OP(H_touch);

        case PROTOCOL_BINARY_CMD_OBSERVE_SEQNO:
            INVOKE_OP(H_observe_seqno);

        case PROTOCOL_BINARY_CMD_STAT:
            INVOKE_OP(H_stats);

        case PROTOCOL_BINARY_CMD_NOOP:
            INVOKE_OP(H_noop);

        case PROTOCOL_BINARY_CMD_GET_CLUSTER_CONFIG:
            INVOKE_OP(H_config);

        case PROTOCOL_BINARY_CMD_SELECT_BUCKET:
            INVOKE_OP(H_select_bucket);

        case PROTOCOL_BINARY_CMD_COLLECTIONS_GET_MANIFEST:
            INVOKE_OP(H_collections_get_manifest);

        case PROTOCOL_BINARY_CMD_COLLECTIONS_GET_CID:
            INVOKE_OP(H_collections_get_cid);

        case PROTOCOL_BINARY_CMD_GET_META:
            INVOKE_OP(H_exists);

        default:
            fprintf(stderr, "COUCHBASE: Received unknown opcode=0x%x\n", res->opcode());
            return -1;
    }
}

const lcb_MUTATION_TOKEN *lcb_resp_get_mutation_token(int cbtype, const lcb_RESPBASE *rb)
{
    const lcb_MUTATION_TOKEN *ss = nullptr;
    if ((rb->rflags & LCB_RESP_F_EXTDATA) == 0) {
        return nullptr;
    }

    switch (cbtype) {
        case LCB_CALLBACK_STORE:
            ss = ResponsePack<lcb_RESPSTORE>::get_mt(rb);
            break;

        case LCB_CALLBACK_COUNTER:
            ss = ResponsePack<lcb_RESPCOUNTER>::get_mt(rb);
            break;

        case LCB_CALLBACK_REMOVE:
            ss = ResponsePack<lcb_RESPREMOVE>::get_mt(rb);
            break;

        case LCB_CALLBACK_SDMUTATE:
            ss = ResponsePack<lcb_RESPSUBDOC>::get_mt(rb);
            break;

        default:
            return nullptr;
    }

    if (ss->uuid_ == 0 && ss->seqno_ == 0) {
        return nullptr;
    }
    return ss;
}

#define ERRINFO_CALLBACKS(X)                                                                                           \
    X(LCB_CALLBACK_GET, lcb_RESPGET)                                                                                   \
    X(LCB_CALLBACK_STORE, lcb_RESPSTORE)                                                                               \
    X(LCB_CALLBACK_COUNTER, lcb_RESPCOUNTER)                                                                           \
    X(LCB_CALLBACK_TOUCH, lcb_RESPTOUCH)                                                                               \
    X(LCB_CALLBACK_REMOVE, lcb_RESPREMOVE)                                                                             \
    X(LCB_CALLBACK_UNLOCK, lcb_RESPUNLOCK)                                                                             \
    X(LCB_CALLBACK_SDLOOKUP, lcb_RESPSUBDOC)                                                                           \
    X(LCB_CALLBACK_SDMUTATE, lcb_RESPSUBDOC)

LIBCOUCHBASE_API
const char *lcb_resp_get_error_context(int cbtype, const lcb_RESPBASE *rb)
{
    if ((rb->rflags & LCB_RESP_F_ERRINFO) == 0) {
        return nullptr;
    }

#define X(CBTYPE, RESP)                                                                                                \
    if (cbtype == CBTYPE) {                                                                                            \
        return ResponsePack<RESP>::get_err_ctx(rb);                                                                    \
    }
    ERRINFO_CALLBACKS(X)
#undef X
    return nullptr;
}

LIBCOUCHBASE_API
const char *lcb_resp_get_error_ref(int cbtype, const lcb_RESPBASE *rb)
{
    if ((rb->rflags & LCB_RESP_F_ERRINFO) == 0) {
        return nullptr;
    }

#define X(CBTYPE, RESP)                                                                                                \
    if (cbtype == CBTYPE) {                                                                                            \
        return ResponsePack<RESP>::get_err_ref(rb);                                                                    \
    }
    ERRINFO_CALLBACKS(X)
#undef X
    return nullptr;
}
