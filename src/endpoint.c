/*
    endpoint.c -- Create and manage listening endpoints.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void acceptConn(HttpEndpoint *endpoint);
static int manageEndpoint(HttpEndpoint *endpoint, int flags);

/************************************ Code ************************************/
/*
    Create a listening endpoint on ip:port. NOTE: ip may be empty which means bind to all addresses.
 */
PUBLIC HttpEndpoint *httpCreateEndpoint(cchar *ip, int port, MprDispatcher *dispatcher)
{
    HttpEndpoint    *endpoint;

    if ((endpoint = mprAllocObj(HttpEndpoint, manageEndpoint)) == 0) {
        return 0;
    }
    endpoint->http = HTTP;
    endpoint->async = 1;
    endpoint->port = port;
    endpoint->ip = sclone(ip);
    endpoint->dispatcher = dispatcher;
    endpoint->hosts = mprCreateList(-1, MPR_LIST_STABLE);
    endpoint->mutex = mprCreateLock();
    httpAddEndpoint(endpoint);
    return endpoint;
}


PUBLIC void httpDestroyEndpoint(HttpEndpoint *endpoint)
{
    if (endpoint->sock) {
        mprCloseSocket(endpoint->sock, 0);
        endpoint->sock = 0;
    }
    httpRemoveEndpoint(endpoint);
}


static int manageEndpoint(HttpEndpoint *endpoint, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(endpoint->http);
        mprMark(endpoint->hosts);
        mprMark(endpoint->ip);
        mprMark(endpoint->context);
        mprMark(endpoint->limits);
        mprMark(endpoint->sock);
        mprMark(endpoint->dispatcher);
        mprMark(endpoint->ssl);
        mprMark(endpoint->mutex);
    }
    return 0;
}


/*
    Convenience function to create and configure a new endpoint without using a config file.
 */
PUBLIC HttpEndpoint *httpCreateConfiguredEndpoint(HttpHost *host, cchar *home, cchar *documents, cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    HttpRoute       *route;

    if (host == 0) {
        host = httpGetDefaultHost();
    }
    if (host == 0) {
        return 0;
    }
    if (ip == 0 && port <= 0) {
        /*
            If no IP:PORT specified, find the first endpoint
         */
        if ((endpoint = mprGetFirstItem(HTTP->endpoints)) != 0) {
            ip = endpoint->ip;
            port = endpoint->port;
        } else {
            ip = "localhost";
            if (port <= 0) {
                port = ME_HTTP_PORT;
            }
            if ((endpoint = httpCreateEndpoint(ip, port, NULL)) == 0) {
                return 0;
            }
        }
    } else if ((endpoint = httpCreateEndpoint(ip, port, NULL)) == 0) {
        return 0;
    }
    route = host->defaultRoute;
    httpAddHostToEndpoint(endpoint, host);
    if (documents) {
        httpSetRouteDocuments(route, documents);
    }
    if (home) {
        httpSetRouteHome(route, home);
    }
    httpFinalizeRoute(route);
    return endpoint;
}


/*
    Add the default host to the unassigned endpoints
 */
PUBLIC void httpAddHostToEndpoints(HttpHost *host)
{
    HttpEndpoint    *endpoint;
    int             next;

    if (host == 0) {
        host = httpGetDefaultHost();
    }
    for (next = 0; (endpoint = mprGetNextItem(HTTP->endpoints, &next)) != 0; ) {
        httpAddHostToEndpoint(endpoint, host);
        if (!host->name) {
            httpSetHostName(host, sfmt("%s:%d", endpoint->ip, endpoint->port));
        }
    }
}


static bool validateEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    HttpRoute   *route;
    int         nextRoute;

    if ((host = mprGetFirstItem(endpoint->hosts)) == 0) {
        host = httpGetDefaultHost();
        httpAddHostToEndpoint(endpoint, host);
        if (!host->name) {
            httpSetHostName(host, sfmt("%s:%d", endpoint->ip, endpoint->port));
        }
        for (nextRoute = 0; (route = mprGetNextItem(host->routes, &nextRoute)) != 0; ) {
            if (!route->handler && !mprLookupKey(route->extensions, "")) {
                httpAddRouteHandler(route, "fileHandler", "");
                httpAddRouteIndex(route, "index.html");
            }
        }
    }
    return 1;
}


PUBLIC int httpStartEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    cchar       *proto, *ip;
    int         next;

    if (!validateEndpoint(endpoint)) {
        return MPR_ERR_BAD_ARGS;
    }
    for (ITERATE_ITEMS(endpoint->hosts, host, next)) {
        httpStartHost(host);
    }
    if ((endpoint->sock = mprCreateSocket()) == 0) {
        return MPR_ERR_MEMORY;
    }
    if (mprListenOnSocket(endpoint->sock, endpoint->ip, endpoint->port, 
                MPR_SOCKET_NODELAY | MPR_SOCKET_THREAD) == SOCKET_ERROR) {
        if (mprGetError() == EADDRINUSE) {
            mprLog("error http", 0, "Cannot open a socket on %s:%d, socket already bound.", 
                *endpoint->ip ? endpoint->ip : "*", endpoint->port);
        } else {
            mprLog("error http", 0, "Cannot open a socket on %s:%d", *endpoint->ip ? endpoint->ip : "*", endpoint->port);
        }
        return MPR_ERR_CANT_OPEN;
    }
    if (endpoint->http->listenCallback && (endpoint->http->listenCallback)(endpoint) < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    if (endpoint->async && !endpoint->sock->handler) {
        mprAddSocketHandler(endpoint->sock, MPR_SOCKET_READABLE, endpoint->dispatcher, acceptConn, endpoint, 
            (endpoint->dispatcher ? 0 : MPR_WAIT_NEW_DISPATCHER) | MPR_WAIT_IMMEDIATE);
    } else {
        mprSetSocketBlockingMode(endpoint->sock, 1);
    }
    proto = endpoint->ssl ? "HTTPS" : "HTTP";
    ip = *endpoint->ip ? endpoint->ip : "*";
    if (mprIsSocketV6(endpoint->sock)) {
        mprLog("info http", HTTP->startLevel, "Started %s service on [%s]:%d", proto, ip, endpoint->port);
    } else {
        mprLog("info http", HTTP->startLevel, "Started %s service on %s:%d", proto, ip, endpoint->port);
    }
    return 0;
}


PUBLIC void httpStopEndpoint(HttpEndpoint *endpoint)
{
    HttpHost    *host;
    int         next;

    for (ITERATE_ITEMS(endpoint->hosts, host, next)) {
        httpStopHost(host);
    }
    if (endpoint->sock) {
        mprCloseSocket(endpoint->sock, 0);
        endpoint->sock = 0;
    }
}


/*
    This routine runs using the service event thread. It accepts the socket and creates an event on a new dispatcher to 
    manage the connection. When it returns, it immediately can listen for new connections without having to modify the 
    event listen masks.
 */
static void acceptConn(HttpEndpoint *endpoint)
{
    MprDispatcher   *dispatcher;
    MprEvent        *event;
    MprSocket       *sock;
    MprWaitHandler  *wp;

    if ((sock = mprAcceptSocket(endpoint->sock)) == 0) {
        return;
    }
    wp = endpoint->sock->handler;
    if (wp->flags & MPR_WAIT_NEW_DISPATCHER) {
        dispatcher = mprCreateDispatcher("IO", MPR_DISPATCHER_AUTO);
    } else if (wp->dispatcher) {
        dispatcher = wp->dispatcher;
    } else {
        dispatcher = mprGetDispatcher();
    }
    event = mprCreateEvent(dispatcher, "AcceptConn", 0, httpAcceptConn, endpoint, MPR_EVENT_DONT_QUEUE);
    event->mask = wp->presentMask;
    event->sock = sock;
    event->handler = wp;
    /*
        Optimization to wake the event service in this amount of time. This ensures that when the HttpTimer is scheduled,
        it won't need to awaken the notifier.
     */
    mprSetEventServiceSleep(HTTP_TIMER_PERIOD);
    mprQueueEvent(dispatcher, event);
}


PUBLIC void httpMatchHost(HttpConn *conn)
{ 
    MprSocket       *listenSock;
    HttpEndpoint    *endpoint;
    HttpHost        *host;

    listenSock = conn->sock->listenSock;

    if ((endpoint = httpLookupEndpoint(listenSock->ip, listenSock->port)) == 0) {
        conn->host = mprGetFirstItem(endpoint->hosts);
        httpError(conn, HTTP_CODE_NOT_FOUND, "No listening endpoint for request from %s:%d", 
            listenSock->ip, listenSock->port);
        return;
    }
    host = httpLookupHostOnEndpoint(endpoint, conn->rx->hostHeader);
    if (host == 0) {
        conn->host = mprGetFirstItem(endpoint->hosts);
        httpError(conn, HTTP_CODE_NOT_FOUND, "No host to serve request. Searching for %s", conn->rx->hostHeader);
        return;
    }
    conn->host = host;
}


PUBLIC void *httpGetEndpointContext(HttpEndpoint *endpoint)
{
    assert(endpoint);
    if (endpoint) {
        return endpoint->context;
    }
    return 0;
}


PUBLIC int httpIsEndpointAsync(HttpEndpoint *endpoint) 
{
    assert(endpoint);
    if (endpoint) {
        return endpoint->async;
    }
    return 0;
}


PUBLIC int httpSetEndpointAddress(HttpEndpoint *endpoint, cchar *ip, int port)
{
    assert(endpoint);

    if (ip) {
        endpoint->ip = sclone(ip);
    }
    if (port >= 0) {
        endpoint->port = port;
    }
    if (endpoint->sock) {
        httpStopEndpoint(endpoint);
        if (httpStartEndpoint(endpoint) < 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    return 0;
}


PUBLIC void httpSetEndpointAsync(HttpEndpoint *endpoint, int async)
{
    if (endpoint->sock) {
        if (endpoint->async && !async) {
            mprSetSocketBlockingMode(endpoint->sock, 1);
        }
        if (!endpoint->async && async) {
            mprSetSocketBlockingMode(endpoint->sock, 0);
        }
    }
    endpoint->async = async;
}


PUBLIC void httpSetEndpointContext(HttpEndpoint *endpoint, void *context)
{
    assert(endpoint);
    endpoint->context = context;
}


PUBLIC void httpSetEndpointNotifier(HttpEndpoint *endpoint, HttpNotifier notifier)
{
    assert(endpoint);
    endpoint->notifier = notifier;
}


PUBLIC int httpSecureEndpoint(HttpEndpoint *endpoint, struct MprSsl *ssl)
{
#if ME_COM_SSL
    endpoint->ssl = ssl;
    return 0;
#else
    mprLog("error http", 0, "Configuration lacks SSL support");
    return MPR_ERR_BAD_STATE;
#endif
}


PUBLIC int httpSecureEndpointByName(cchar *name, struct MprSsl *ssl)
{
    HttpEndpoint    *endpoint;
    char            *ip;
    int             port, next, count;

    mprParseSocketAddress(name, &ip, &port, NULL, -1);
    if (ip == 0) {
        ip = "";
    }
    for (count = 0, next = 0; (endpoint = mprGetNextItem(HTTP->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assert(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                httpSecureEndpoint(endpoint, ssl);
                count++;
            }
        }
    }
    return (count == 0) ? MPR_ERR_CANT_FIND : 0;
}


PUBLIC void httpAddHostToEndpoint(HttpEndpoint *endpoint, HttpHost *host)
{
    if (mprLookupItem(endpoint->hosts, host) < 0) {
        mprAddItem(endpoint->hosts, host);
    }
    if (endpoint->limits == 0) {
        endpoint->limits = host->defaultRoute->limits;
    }
}


PUBLIC HttpHost *httpLookupHostOnEndpoint(HttpEndpoint *endpoint, cchar *hostHeader)
{
    HttpHost    *host;
    int         next;

    if (hostHeader == 0 || *hostHeader == '\0' || mprGetListLength(endpoint->hosts) <= 1) {
        return mprGetFirstItem(endpoint->hosts);
    }
    for (next = 0; (host = mprGetNextItem(endpoint->hosts, &next)) != 0; ) {
        if (smatch(host->name, hostHeader)) {
            return host;
        }
        if (*host->name == '\0') {
            /* Match all hosts */
            return host;
        }
        if (host->flags & HTTP_HOST_WILD_STARTS) {
            if (sstarts(hostHeader, host->name)) {
                return host;
            }
        } else if (host->flags & HTTP_HOST_WILD_CONTAINS) {
            if (scontains(hostHeader, host->name)) {
                return host;
            }
        }
    }
    return 0;
}


PUBLIC void httpSetInfoLevel(int level)
{
    HTTP->startLevel = level;
}

/*
    @copy   default

    Copyright (c) Embedthis Software. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
