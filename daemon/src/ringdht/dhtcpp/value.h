/*
Copyright (c) 2014 Savoir-Faire Linux Inc.

Author : Adrien Béraud <adrien.beraud@savoirfairelinux.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#pragma once

#include "infohash.h"
#include "crypto.h"
#include "serialize.h"

#include <sys/socket.h>
#include <netdb.h>

#include <string>
#include <sstream>
#include <bitset>
#include <vector>
#include <iostream>
#include <algorithm>
#include <functional>
#include <memory>

#include <cstdarg>

namespace dht {

/**
 * Wrapper for logging methods
 */
struct LogMethod {
    LogMethod() = default;

    template<typename T>
    LogMethod( T&& t) : func(std::forward<T>(t)) {}

    void operator()(char const* format, ...) const {
        va_list args;
        va_start(args, format);
        func(format, args);
        va_end(args);
    }

    void logPrintable(const uint8_t *buf, unsigned buflen) const {
        std::string buf_clean(buflen, '\0');
        for (unsigned i=0; i<buflen; i++)
            buf_clean[i] = buf[i] >= 32 && buf[i] <= 126 ? buf[i] : '.';
        (*this)("%s", buf_clean.c_str());
    }
private:
    std::function<void(char const*, va_list)> func;
};

/**
 * Dummy function used to disable logging
 */
constexpr int NOLOG(char const*, va_list) { return 0; }


class Value;

typedef std::function<bool(InfoHash, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t)> StorePolicy;
typedef std::function<bool(InfoHash, const std::shared_ptr<Value>&, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t)> EditPolicy;

struct ValueType {
    typedef uint16_t Id;
    ValueType () {}

    ValueType (Id id, std::string name, time_t e = 60 * 60)
    : id(id), name(name), expiration(e) {}

    ValueType (Id id, std::string name, time_t e, StorePolicy&& sp, EditPolicy&& ep)
     : id(id), name(name), expiration(e), storePolicy(std::move(sp)), editPolicy(std::move(ep)) {}

    virtual ~ValueType() {}

    bool operator==(const ValueType& o) {
       return id == o.id;
    }

    // Generic value type
    static const ValueType USER_DATA;

    static bool DEFAULT_STORE_POLICY(InfoHash, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t) {
        return true;
    }
    static bool DEFAULT_EDIT_POLICY(InfoHash, const std::shared_ptr<Value>&, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t) {
        return false;
    }

    Id id {0};
    std::string name {};
    time_t expiration {60 * 60};
    StorePolicy storePolicy {DEFAULT_STORE_POLICY};
    EditPolicy editPolicy {DEFAULT_EDIT_POLICY};
};

/**
 * A "value" is data potentially stored on the Dht, with some metadata.
 *
 * It can be an IP:port announced for a service, a public key, or any kind of
 * light user-defined data (recommended: less than 512 bytes).
 *
 * Values are stored at a given InfoHash in the Dht, but also have a
 * unique ID to distinguish between values stored at the same location.
 */
struct Value : public Serializable
{
    typedef uint64_t Id;
    static const Id INVALID_ID {0};

    typedef std::function<bool(const Value&)> Filter;

    static const Filter AllFilter() {
        return [](const Value&){return true;};
    }

    static Filter TypeFilter(const ValueType& t) {
        const auto tid = t.id;
        return [tid](const Value& v) {
            return v.type == tid;
        };
    }

    static Filter chainFilters(Filter& f1, Filter& f2) {
        return [f1,f2](const Value& v){
            return f1(v) && f2(v);
        };
    }

    /**
     * Hold information about how the data is signed/encrypted.
     * Class is final because bitset have no virtual destructor.
     */
    class ValueFlags final : public std::bitset<3> {
    public:
        using std::bitset<3>::bitset;
        ValueFlags() {}
        ValueFlags(bool sign, bool encrypted, bool have_recipient = false) : bitset<3>((sign ? 1:0) | (encrypted ? 2:0) | (have_recipient ? 4:0)) {}
        bool isSigned() const {
            return (*this)[0];
        }
        bool isEncrypted() const {
            return (*this)[1];
        }
        bool haveRecipient() const {
            return (*this)[2];
        }
    };

    bool isEncrypted() const {
        return flags.isEncrypted();
    }
    bool isSigned() const {
        return flags.isSigned();
    }

    Value() {}

    Value (Id id) : id(id) {}

    /** Generic constructor */
    Value(ValueType::Id t, const Blob& data, Id id = INVALID_ID)
     : id(id), type(t), data(data) {}
    Value(ValueType::Id t, const Serializable& d, Id id = INVALID_ID)
     : id(id), type(t), data(d.getPacked()) {}
    Value(const ValueType& t, const Serializable& d, Id id = INVALID_ID)
     : id(id), type(t.id), data(d.getPacked()) {}

    /** Custom user data constructor */
    Value(const Blob& userdata) : data(userdata) {}
    Value(Blob&& userdata) : data(std::move(userdata)) {}

    Value(Value&& o) noexcept
     : id(o.id), flags(o.flags), owner(std::move(o.owner)), recipient(o.recipient),
     type(o.type), data(std::move(o.data)), seq(o.seq), signature(std::move(o.signature)), cypher(std::move(o.cypher)) {}

    inline bool operator== (const Value& o) {
        return id == o.id &&
        (flags.isEncrypted() ? cypher == o.cypher :
        (owner == o.owner && type == o.type && data == o.data && signature == o.signature));
    }

    void setRecipient(const InfoHash& r) {
        recipient = r;
        flags[2] = true;
    }

    void setCypher(Blob&& c) {
        cypher = std::move(c);
        flags = {true, true, true};
    }

    /**
     * Pack part of the data to be signed
     */
    void packToSign(Blob& res) const;
    Blob getToSign() const;

    /**
     * Pack part of the data to be encrypted
     */
    void packToEncrypt(Blob& res) const;
    Blob getToEncrypt() const;

    void pack(Blob& res) const;

    void unpackBody(Blob::const_iterator& begin, Blob::const_iterator& end);
    virtual void unpack(Blob::const_iterator& begin, Blob::const_iterator& end);

    /** print value for debugging */
    friend std::ostream& operator<< (std::ostream& s, const Value& v);

    std::string toString() const {
        std::stringstream ss;
        ss << *this;
        return ss.str();
    }

    Id id {INVALID_ID};

    // data (part that is signed / encrypted)

    ValueFlags flags {};

    /**
     * Public key of the signer.
     */
    crypto::PublicKey owner {};

    /**
     * Hash of the recipient (optional).
     * Should only be present for encrypted values.
     * Can optionally be present for signed values.
     */
    InfoHash recipient {};

    /**
     * Type of data.
     */
    ValueType::Id type {ValueType::USER_DATA.id};
    Blob data {};

    /**
     * Sequence number to avoid replay attacks
     */
    uint16_t seq {0};

    /**
     * Optional signature.
     */
    Blob signature {};

    /**
     * Hold encrypted version of the data.
     */
    Blob cypher {};
};


/* "Peer" announcement
 */
struct ServiceAnnouncement : public Serializable
{
    ServiceAnnouncement(in_port_t p = 0) {
        ss.ss_family = 0;
        setPort(p);
    }

    ServiceAnnouncement(const sockaddr* sa, socklen_t sa_len) {
        if (sa)
            std::copy_n((const uint8_t*)sa, sa_len, (uint8_t*)&ss);
    }

    ServiceAnnouncement(const Blob& b) {
        unpackBlob(b);
    }

    virtual void pack(Blob& res) const;
    virtual void unpack(Blob::const_iterator& begin, Blob::const_iterator& end);

    in_port_t getPort() const {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&ss)->sin_port);
    }
    void setPort(in_port_t p) {
        reinterpret_cast<sockaddr_in*>(&ss)->sin_port = htons(p);
    }

    sockaddr_storage getPeerAddr() const {
        return ss;
    }

    static const ValueType TYPE;
    static bool storePolicy(InfoHash, std::shared_ptr<Value>&, InfoHash, const sockaddr*, socklen_t);

    /** print value for debugging */
    friend std::ostream& operator<< (std::ostream&, const ServiceAnnouncement&);

private:
    sockaddr_storage ss;
};

}