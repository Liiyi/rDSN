/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

# include <dsn/ports.h>
# include <dsn/service_api_c.h>
# include <dsn/cpp/address.h>

# ifdef _WIN32


# else
# include <sys/socket.h>
# include <netdb.h>
# include <ifaddrs.h>
# include <netinet/in.h>
# include <arpa/inet.h>

# if defined(__FreeBSD__)
# include <netinet/in.h>
# endif

# endif

# include <mutex>
# include <unordered_map>
# include <dsn/internal/singleton.h>
# include <dsn/internal/synchronize.h>

DSN_API int dsn_address_use_ip_as_name = 0;

static ::dsn::utils::rw_lock_nr s_name_cache_ipv4_lock;
static std::unordered_map<uint32_t, std::string> s_name_cache_ipv4;

//static std::unordered_map<uint128_t, std::string> s_name_cache_ipv6;

static void net_init()
{
    static std::once_flag flag;
    static bool flag_inited = false;
    if (!flag_inited)
    {
        std::call_once(flag, [&]()
        {
#ifdef _WIN32
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
            flag_inited = true;
        });
    }
}

DSN_API void dsn_address_build(dsn_address_t* ep, const char* host, uint16_t port)
{
    net_init();

    ::dsn::rpc_address addr(HOST_TYPE_IPV4, host, port);
    *ep = addr.c_addr();
}

DSN_API void dsn_address_build_ipv4(
    /*out*/ dsn_address_t* ep,
    uint32_t ipv4,
    uint16_t port
    )
{
    net_init();
    ::dsn::rpc_address addr(ipv4, port);
    *ep = addr.c_addr();
}

// ip etc. to name
DSN_API void dsn_host_to_name(const dsn_address_t* addr, /*out*/ char* name_buffer, int length)
{
    switch (addr->type)
    {
    case HOST_TYPE_IPV4:
    {
        uint32_t nip = htonl(addr->ip);
        if (dsn_address_use_ip_as_name)
        {
    # if defined(_WIN32)
            sprintf_s(
    # else
            std::snprintf(
    # endif
                name_buffer, (size_t)length,
                "%u.%u.%u.%u",
                nip & 0xff,
                (nip >> 8) & 0xff,
                (nip >> 16) & 0xff,
                (nip >> 24) & 0xff
                );
            return;
        }

        // query cache
        s_name_cache_ipv4_lock.lock_read();
        auto it = s_name_cache_ipv4.find(addr->ip);
        if (it != s_name_cache_ipv4.end())
        {
            strncpy(name_buffer, it->second.c_str(), length);
            s_name_cache_ipv4_lock.unlock_read();
            return;
        }
        s_name_cache_ipv4_lock.unlock_read();

        // if not cache hit
        // TODO: using global cache
        auto host = gethostbyaddr((char*)&nip, 4, AF_INET);
        if (host == nullptr)
        {
# if defined(_WIN32)
            sprintf_s(
# else
            std::snprintf(
# endif
                name_buffer, (size_t)length,
                "%u.%u.%u.%u",
                nip & 0xff,
                (nip >> 8) & 0xff,
                (nip >> 16) & 0xff,
                (nip >> 24) & 0xff
                );
        }
        else
        {
            strncpy(name_buffer, host->h_name, length);
        }
        name_buffer[length - 1] = '\0';

        // update cache
        s_name_cache_ipv4_lock.lock_write();
        it = s_name_cache_ipv4.find(addr->ip);
        if (it == s_name_cache_ipv4.end())
        {
            s_name_cache_ipv4[addr->ip] = std::string(name_buffer);
        }
        s_name_cache_ipv4_lock.unlock_write();
    }
    break;
    case HOST_TYPE_IPV6:
        dassert(false, "to be implemented");
        break;
    case HOST_TYPE_URI:
        dassert(false, "to be implemented");
        break;
    default:
        break;
    }
}

// name to ip etc.
DSN_API void dsn_host_from_name(dsn_host_type_t type, const char* name, /*out*/ dsn_address_t* daddr)
{
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    daddr->type = type;
    switch (type)
    {
    case HOST_TYPE_IPV4:
        addr.sin_family = AF_INET;
        if ((addr.sin_addr.s_addr = inet_addr(name)) == (unsigned int)(-1))
        {
            hostent* hp = ::gethostbyname(name);
            int err =
# ifdef _WIN32
                (int)::WSAGetLastError()
# else
                h_errno
# endif
                ;
            dassert(hp != nullptr, "gethostbyname failed, name = %s, err = %d.", name, err);

            if (hp != nullptr)
            {
                memcpy(
                    (void*)&(addr.sin_addr.s_addr),
                    (const void*)hp->h_addr,
                    (size_t)hp->h_length
                    );
            }
        }

        // converts from network byte order to host byte order
        daddr->ip = (uint32_t)ntohl(addr.sin_addr.s_addr);
        break;

    case HOST_TYPE_IPV6:
        dassert(false, "to be implemented");
        break;

    case HOST_TYPE_URI:
        daddr->uri = name;
        break;

    default:
        break;
    }
}

DSN_API void dsn_address_local(/*out*/ dsn_address_t* addr, const char* network_interface)
{
# ifdef _WIN32
    dassert(false, "not implemented");
# else
    struct ifaddrs* ifa = nullptr;
    getifaddrs(&ifa);

    struct ifaddrs* i = ifa;
    while (i != nullptr)
    {
        if (i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, network_interface) == 0)
        {
            addr->type = HOST_TYPE_IPV4;
            addr->ip = (uint32_t)ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
            break;
        }
        i = i->ifa_next;
    }
    dassert(i != nullptr, "get local ip failed, network_interface=", network_interface);

    if (ifa != nullptr)
    {
        // remember to free it
        freeifaddrs(ifa);
    }
#endif
}