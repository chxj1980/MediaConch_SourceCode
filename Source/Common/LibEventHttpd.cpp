/*  Copyright (c) MediaArea.net SARL. All Rights Reserved.
 *
 *  Use of this source code is governed by a GPLv3+/MPLv2+ license that can
 *  be found in the License.html file in the root of the source tree.
 */

//---------------------------------------------------------------------------
#ifdef HAVE_LIBEVENT

//---------------------------------------------------------------------------
#ifdef __BORLANDC__
    #pragma hdrstop
#endif

//---------------------------------------------------------------------------
#include "LibEventHttpd.h"
#include "Common/MediaConchLib.h"
#include <sstream>
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
namespace MediaConch {

//***************************************************************************
// Httpd
//***************************************************************************

//***************************************************************************
// Constructor/Destructor
//***************************************************************************

//---------------------------------------------------------------------------
LibEventHttpd::LibEventHttpd(void* arg) : Httpd(arg), base(NULL), http(NULL), handle(NULL)
{
}

//---------------------------------------------------------------------------
LibEventHttpd::~LibEventHttpd()
{
    finish();
}

//---------------------------------------------------------------------------
int LibEventHttpd::init()
{
    base = event_base_new();
    if (!base)
    {
        error = std::string("cannot create an event-base");
        return -1;
    }

    http = evhttp_new(base);
    if (!http)
    {
        error = std::string("cannot create event-http");
        return -1;
    }

    evhttp_set_gencb(http, request_coming, this);
    return 0;
}

//---------------------------------------------------------------------------
int LibEventHttpd::start()
{
    handle = evhttp_bind_socket_with_handle(http, address.c_str(), port);
    if (!handle)
    {
        std::stringstream ss;
        ss << "couldn't bind to address " << address << " and port " << port;
        error = ss.str();
        return 1;
    }


    event_base_dispatch(base);
    return 0;
}

//---------------------------------------------------------------------------
int LibEventHttpd::finish()
{
    if (handle)
    {
        evhttp_del_accept_socket(http, handle);
        handle = NULL;
    }
    if (http)
    {
        evhttp_free(http);
        http = NULL;
    }
    if (base)
    {
        event_base_free(base);
        base = NULL;
    }
    return 0;
}

//---------------------------------------------------------------------------
int LibEventHttpd::send_result(int ret_code, std::string& ret_msg, void *arg)
{
    struct evhttp_request *req = (struct evhttp_request *)arg;
    struct evbuffer *evOutBuf = evbuffer_new();

    if (evOutBuf)
    {
        if (error.length())
            evbuffer_add_printf(evOutBuf, "%s\n", error.c_str());
        else if (result.length())
            evbuffer_add_printf(evOutBuf, "%s\n", result.c_str());
    }
    evhttp_send_reply(req, ret_code, ret_msg.c_str(), evOutBuf);
    if (evOutBuf)
        evbuffer_free(evOutBuf);

    // clean error
    error.clear();
    return 0;
}

//---------------------------------------------------------------------------
void LibEventHttpd::request_get_coming(struct evhttp_request *req)
{
    int code = HTTP_OK;
    std::string ret_msg("OK");

    const evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
    std::string uri_path(evhttp_uri_get_path(uri));

    if (uri_api_version_is_valid(uri_path, req) < 0)
        return;
    const char* query_str = evhttp_uri_get_query(uri);
    if (query_str && !std::string("/status").compare(uri_path))
    {
        std::string query(query_str);
        RESTAPI::Status_Req *r = NULL;
        get_uri_request(query, &r);

        RESTAPI::Status_Res res;
        if (commands.status_cb && commands.status_cb(r, res, parent) < 0)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        result = rest.serialize_status_res(res);
        if (!result.length())
            error = rest.get_error();
    }
    else
    {
        code = HTTP_NOTFOUND;
        ret_msg = std::string("NOTFOUND");
        error = std::string("invalid URI");
    }
send:
    send_result(code, ret_msg, req);
}

//---------------------------------------------------------------------------
void LibEventHttpd::request_post_coming(struct evhttp_request *req)
{
    std::string json;
    int code = HTTP_OK;
    std::string ret_msg("OK");

    if (get_body(req, json, ret_msg) < 0)
    {
        send_result(HTTP_NOTFOUND, ret_msg, req);
        return;
    }

    const evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
    std::string uri_path(evhttp_uri_get_path(uri));

    if (uri_api_version_is_valid(uri_path, req) < 0)
        return;
    if (!std::string("/analyze").compare(uri_path))
    {
        RESTAPI::Analyze_Req *r = NULL;
        get_request(json, &r);
        if (!r)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        RESTAPI::Analyze_Res res;
        if (commands.analyze_cb && commands.analyze_cb(r, res, parent) < 0)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        result = rest.serialize_analyze_res(res);
        if (!result.length())
            error = rest.get_error();
    }
    else if (!std::string("/report").compare(uri_path))
    {
        RESTAPI::Report_Req *r = NULL;
        get_request(json, &r);
        if (!r)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        RESTAPI::Report_Res res;
        if (commands.report_cb && commands.report_cb(r, res, parent) < 0)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        result = rest.serialize_report_res(res);
        if (!result.length())
            error = rest.get_error();
    }
    else
    {
        code = HTTP_NOTFOUND;
        ret_msg = std::string("NOTFOUND");
        error = std::string("invalid URI");
    }
send:
    send_result(code, ret_msg, req);
}

//---------------------------------------------------------------------------
void LibEventHttpd::request_put_coming(struct evhttp_request *req)
{
    std::string json;
    int code = HTTP_OK;
    std::string ret_msg("OK");

    if (get_body(req, json, ret_msg))
    {
        send_result(HTTP_NOTFOUND, ret_msg, req);
        return;
    }

    const evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
    std::string uri_path(evhttp_uri_get_path(uri));

    if (uri_api_version_is_valid(uri_path, req) < 0)
        return;
    if (!std::string("/retry").compare(uri_path))
    {
        RESTAPI::Retry_Req *r = NULL;
        get_request(json, &r);
        if (!r)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        RESTAPI::Retry_Res res;
        if (commands.retry_cb && commands.retry_cb(r, res, parent) < 0)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }
        result = rest.serialize_retry_res(res);
        if (!result.length())
            error = rest.get_error();
    }
    else
    {
        code = HTTP_NOTFOUND;
        ret_msg = std::string("NOTFOUND");
        error = std::string("invalid URI");
    }
send:
    send_result(code, ret_msg, req);
}

//---------------------------------------------------------------------------
void LibEventHttpd::request_delete_coming(struct evhttp_request *req)
{
    int code = HTTP_OK;
    std::string ret_msg("OK");

    const evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
    std::string uri_path(evhttp_uri_get_path(uri));

    if (uri_api_version_is_valid(uri_path, req) < 0)
        return;

    const char* query_str = evhttp_uri_get_query(uri);
    if (query_str && !std::string("/clear").compare(uri_path))
    {
        std::string query(query_str);
        RESTAPI::Clear_Req *r = NULL;
        get_uri_request(query, &r);

        RESTAPI::Clear_Res res;
        if (commands.clear_cb && commands.clear_cb(r, res, parent) < 0)
        {
            ret_msg = "NOVALIDCONTENT";
            code = HTTP_BADREQUEST;
            goto send;
        }

        result = rest.serialize_clear_res(res);
        if (!result.length())
            error = rest.get_error();
    }
    else
    {
        code = HTTP_NOTFOUND;
        ret_msg = std::string("NOTFOUND");
        error = std::string("invalid URI");
    }
send:
    send_result(code, ret_msg, req);
}

//---------------------------------------------------------------------------
void LibEventHttpd::request_coming(struct evhttp_request *req, void *arg)
{
    LibEventHttpd    *evHttp = (LibEventHttpd*)arg;
    std::string       ret_msg("OK");

    switch (evhttp_request_get_command(req))
    {
        case EVHTTP_REQ_GET:
            evHttp->request_get_coming(req);
            break;
        case EVHTTP_REQ_POST:
            evHttp->request_post_coming(req);
            break;
        case EVHTTP_REQ_PUT:
            evHttp->request_put_coming(req);
            break;
        case EVHTTP_REQ_DELETE:
            evHttp->request_delete_coming(req);
            break;
        default:
            evHttp->error = std::string("HTTP Request command not supported");
            evHttp->send_result(HTTP_BADREQUEST, ret_msg, req);
            return;
    }
}

//---------------------------------------------------------------------------
int LibEventHttpd::get_body(struct evhttp_request *req, std::string& json, std::string& ret_msg)
{
    struct evbuffer *evBuf = evhttp_request_get_input_buffer(req);
    if (!evBuf)
    {
        error = std::string("body of the request should contain the command");
        ret_msg = "NOCONTENT";
        send_result(HTTP_BADREQUEST, ret_msg, req);
        return -1;
    }

    int len = evbuffer_get_length(evBuf);
    char tmpBuf[len + 1];
    int n = evbuffer_remove(evBuf, tmpBuf, len);
    if (n <= 0)
    {
        error = std::string("body of the request should contain the command");
        ret_msg = "NOVALIDCONTENT";
        send_result(HTTP_BADREQUEST, ret_msg, req);
        return -1;
    }
    tmpBuf[n] = '\0';

    json = std::string(tmpBuf);
    return 0;
}

int LibEventHttpd::uri_api_version_is_valid(std::string& uri, struct evhttp_request *req)
{
    std::string search("/" + RESTAPI::API_VERSION);
    size_t pos = uri.find(search);
    if (pos != 0)
    {
        error = std::string("The API version is not valid");
        std::string ret_msg = "WRONG_API_VERSION";
        send_result(HTTP_BADREQUEST, ret_msg, req);
        return -1;
    }
    uri = uri.substr(search.length());
    return 0;
}

}

#endif // !HAVE_LIBEVENT