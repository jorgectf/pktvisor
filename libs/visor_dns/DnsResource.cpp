#define LOG_MODULE PacketLogModuleDnsLayer

#include "DnsResource.h"
#include "EndianPortable.h"
#include "fmt/format.h"
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif
#include <Logger.h>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <sstream>
#include <string.h>

namespace visor::lib::dns {

IDnsResource::IDnsResource(DnsLayer *dnsLayer, size_t offsetInLayer)
    : m_DnsLayer(dnsLayer)
    , m_OffsetInLayer(offsetInLayer)
    , m_NextResource(NULL)
{
    char decodedName[256];
    m_NameLength = decodeName((const char *)getRawData(), decodedName);
    if (m_NameLength > 0) {
        m_DecodedName = decodedName;
        m_DecodedNameLower = m_DecodedName;
        std::transform(m_DecodedNameLower.begin(), m_DecodedNameLower.end(), m_DecodedNameLower.begin(),
            [](unsigned char c) { return std::tolower(c); });
    }
}

IDnsResource::IDnsResource(uint8_t *emptyRawData)
    : m_DnsLayer(NULL)
    , m_OffsetInLayer(0)
    , m_NextResource(NULL)
    , m_DecodedName("")
    , m_DecodedNameLower("")
    , m_NameLength(0)
    , m_ExternalRawData(emptyRawData)
{
}

uint8_t *IDnsResource::getRawData() const
{
    if (m_DnsLayer == NULL)
        return m_ExternalRawData;

    return m_DnsLayer->m_Data + m_OffsetInLayer;
}

size_t IDnsResource::decodeName(const char *encodedName, char *result, int iteration)
{
    size_t encodedNameLength = 0;
    size_t decodedNameLength = 0;
    char *resultPtr = result;
    resultPtr[0] = 0;

    size_t curOffsetInLayer = (uint8_t *)encodedName - m_DnsLayer->m_Data;
    if (curOffsetInLayer + 1 > m_DnsLayer->m_DataLen)
        return encodedNameLength;

    if (iteration > 20) {
        return encodedNameLength;
    }

    uint8_t wordLength = encodedName[0];

    // A string to parse
    while (wordLength != 0) {
        // A pointer to another place in the packet
        if ((wordLength & 0xc0) == 0xc0) {
            if (curOffsetInLayer + 2 > m_DnsLayer->m_DataLen || encodedNameLength > 255)
                return encodedNameLength;

            uint16_t offsetInLayer = (wordLength & 0x3f) * 256 + (0xFF & encodedName[1]);
            if (offsetInLayer < sizeof(dnshdr) || offsetInLayer >= m_DnsLayer->m_DataLen) {
                PCPP_LOG_ERROR("DNS parsing error: name pointer is illegal");
                return 0;
            }

            char tempResult[256];
            memset(tempResult, 0, 256);
            int i = 0;
            decodeName((const char *)(m_DnsLayer->m_Data + offsetInLayer), tempResult, iteration + 1);
            while (tempResult[i] != 0 && decodedNameLength < 255) {
                resultPtr[0] = tempResult[i++];
                resultPtr++;
                decodedNameLength++;
            }

            resultPtr[0] = 0;

            // in this case the length of the pointer is: 1 byte for 0xc0 + 1 byte for the offset itself
            return encodedNameLength + sizeof(uint16_t);
        } else {
            // return if next word would be outside of the DNS layer or overflow the buffer behind resultPtr
            if (curOffsetInLayer + wordLength + 1 > m_DnsLayer->m_DataLen || encodedNameLength + wordLength > 255) {
                // add the last '\0' to the decoded string
                if (encodedNameLength == 256) {
                    resultPtr--;
                    decodedNameLength--;
                } else {
                    encodedNameLength++;
                }

                resultPtr[0] = 0;
                return encodedNameLength;
            }

            memcpy(resultPtr, encodedName + 1, wordLength);
            resultPtr += wordLength;
            resultPtr[0] = '.';
            resultPtr++;
            decodedNameLength += wordLength + 1;
            encodedName += wordLength + 1;
            encodedNameLength += wordLength + 1;

            curOffsetInLayer = (uint8_t *)encodedName - m_DnsLayer->m_Data;
            if (curOffsetInLayer + 1 > m_DnsLayer->m_DataLen) {
                // add the last '\0' to the decoded string
                if (encodedNameLength == 256) {
                    decodedNameLength--;
                    resultPtr--;
                } else {
                    encodedNameLength++;
                }

                resultPtr[0] = 0;
                return encodedNameLength;
            }

            wordLength = encodedName[0];
        }
    }

    // remove the last "."
    if (resultPtr > result) {
        result[resultPtr - result - 1] = 0;
    }

    // add the last '\0' to encodedNameLength
    resultPtr[0] = 0;
    encodedNameLength++;

    return encodedNameLength;
}

void IDnsResource::encodeName(const std::string &decodedName, char *result, size_t &resultLen)
{
    resultLen = 0;
    std::stringstream strstream(decodedName);
    std::string word;
    while (getline(strstream, word, '.')) {
        // pointer to a different hostname in the packet
        if (word[0] == '#') {
            // convert the number from string to int
            std::stringstream stream(word.substr(1));
            int pointerInPacket = 0;
            stream >> pointerInPacket;

            // verify it's indeed a number and that is in the range of [0-255]
            if (stream.fail() || pointerInPacket < 0 || pointerInPacket > 0xff) {
                PCPP_LOG_ERROR(fmt::format("Error encoding the string '{}'", decodedName));
                return;
            }

            // set the pointer to the encoded string result
            result[0] = (uint8_t)0xc0;
            result[1] = (uint8_t)pointerInPacket;
            result += 2;
            resultLen += 2;
            return; // pointer always comes last
        }

        result[0] = word.length();
        result++;
        memcpy(result, word.c_str(), word.length());
        result += word.length();
        resultLen += word.length() + 1;
    }

    result[0] = 0;
    resultLen++;
}

DnsType IDnsResource::getDnsType() const
{
    uint16_t dnsType = *(uint16_t *)(getRawData() + m_NameLength);
    return (DnsType)be16toh(dnsType);
}

void IDnsResource::setDnsType(DnsType newType)
{
    uint16_t newTypeAsInt = htobe16((uint16_t)newType);
    memcpy(getRawData() + m_NameLength, &newTypeAsInt, sizeof(uint16_t));
}

DnsClass IDnsResource::getDnsClass() const
{
    uint16_t dnsClass = *(uint16_t *)(getRawData() + m_NameLength + sizeof(uint16_t));
    return (DnsClass)be16toh(dnsClass);
}

void IDnsResource::setDnsClass(DnsClass newClass)
{
    uint16_t newClassAsInt = htobe16((uint16_t)newClass);
    memcpy(getRawData() + m_NameLength + sizeof(uint16_t), &newClassAsInt, sizeof(uint16_t));
}

bool IDnsResource::setName(const std::string &newName)
{
    char encodedName[256];
    size_t encodedNameLen = 0;
    encodeName(newName, encodedName, encodedNameLen);
    if (m_DnsLayer != NULL) {
        if (encodedNameLen > m_NameLength) {
            if (!m_DnsLayer->extendLayer(m_OffsetInLayer, encodedNameLen - m_NameLength, this)) {
                PCPP_LOG_ERROR("Couldn't set name for DNS query, unable to extend layer");
                return false;
            }
        } else if (encodedNameLen < m_NameLength) {
            if (!m_DnsLayer->shortenLayer(m_OffsetInLayer, m_NameLength - encodedNameLen, this)) {
                PCPP_LOG_ERROR("Couldn't set name for DNS query, unable to shorten layer");
                return false;
            }
        }
    } else {
        size_t size = getSize();
        char *tempData = new char[size];
        memcpy(tempData, m_ExternalRawData, size);
        memcpy(m_ExternalRawData + encodedNameLen, tempData, size);
        delete[] tempData;
    }

    memcpy(getRawData(), encodedName, encodedNameLen);
    m_NameLength = encodedNameLen;
    m_DecodedName = newName;
    m_DecodedNameLower = m_DecodedName;
    std::transform(m_DecodedNameLower.begin(), m_DecodedNameLower.end(), m_DecodedNameLower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    return true;
}

void IDnsResource::setDnsLayer(DnsLayer *dnsLayer, size_t offsetInLayer)
{
    memcpy(dnsLayer->m_Data + offsetInLayer, m_ExternalRawData, getSize());
    m_DnsLayer = dnsLayer;
    m_OffsetInLayer = offsetInLayer;
    m_ExternalRawData = NULL;
}

std::basic_string_view<uint8_t> IDnsResource::getRawName() const
{
    if (m_NameLength == 0) {
        // malformed name
        return {};
    }
    // scan starts at the domain name
    auto scan = std::basic_string_view<uint8_t>{m_DnsLayer->m_Data, m_DnsLayer->m_DataLen}
                    .substr(m_OffsetInLayer) // skip to the name offset
                    .substr(0, 255);         // enforce name length limit

    // find the end of the scan
    size_t pos = 0;
    while (pos < scan.size()) {
        if (scan[pos] == 0) {
            // root label at the end
            pos += 1;
            break;
        } else if (scan[pos] < 0xc0) {
            // normal scan label
            pos += scan[pos] + 1;
        } else if (scan[pos] == 0xc0) {
            // compression label at the end
            pos += 3;
            break;
        } else {
            // malformed name
            pos = std::string_view::npos;
            break;
        }
    }

    if (pos >= scan.size()) {
        // malformed name
        return {};
    }

    return scan.substr(0, pos);
}

uint32_t DnsResource::getTTL() const
{
    uint32_t ttl = *(uint32_t *)(getRawData() + m_NameLength + 2 * sizeof(uint16_t));
    return be32toh(ttl);
}

void DnsResource::setTTL(uint32_t newTTL)
{
    newTTL = htobe32(newTTL);
    memcpy(getRawData() + m_NameLength + 2 * sizeof(uint16_t), &newTTL, sizeof(uint32_t));
}

size_t DnsResource::getDataLength() const
{
    uint16_t dataLength = *(uint16_t *)(getRawData() + m_NameLength + 2 * sizeof(uint16_t) + sizeof(uint32_t));
    return be16toh(dataLength);
}

DnsResourceDataPtr DnsResource::getData() const
{
    uint8_t *resourceRawData = getRawData() + m_NameLength + 3 * sizeof(uint16_t) + sizeof(uint32_t);
    size_t dataLength = getDataLength();

    switch (getDnsType()) {
    case DNS_TYPE_A: {
        return DnsResourceDataPtr(new IPv4DnsResourceData(resourceRawData, dataLength));
    }

    case DNS_TYPE_AAAA: {
        return DnsResourceDataPtr(new IPv6DnsResourceData(resourceRawData, dataLength));
    }

    case DNS_TYPE_NS:
    case DNS_TYPE_CNAME:
    case DNS_TYPE_DNAM:
    case DNS_TYPE_PTR: {
        return DnsResourceDataPtr(new StringDnsResourceData(resourceRawData, dataLength, const_cast<IDnsResource *>(static_cast<const IDnsResource *>(this))));
    }

    case DNS_TYPE_MX: {
        return DnsResourceDataPtr(new MxDnsResourceData(resourceRawData, dataLength, const_cast<IDnsResource *>(static_cast<const IDnsResource *>(this))));
    }

    default: {
        return DnsResourceDataPtr(new GenericDnsResourceData(resourceRawData, dataLength));
    }
    }
}

size_t DnsResource::getDataOffset() const
{
    return (size_t)(m_OffsetInLayer + m_NameLength + 3 * sizeof(uint16_t) + sizeof(uint32_t));
}

bool DnsResource::setData(IDnsResourceData *data)
{
    // convert data to byte array according to the DNS type
    size_t dataLength = 0;
    uint8_t dataAsByteArr[256];

    if (data == NULL) {
        PCPP_LOG_ERROR("Given data is NULL");
        return false;
    }

    switch (getDnsType()) {
    case DNS_TYPE_A: {
        if (!data->isTypeOf<IPv4DnsResourceData>()) {
            PCPP_LOG_ERROR("DNS record is of type A but given data isn't of type IPv4DnsResourceData");
            return false;
        }
        break;
    }

    case DNS_TYPE_AAAA: {
        if (!data->isTypeOf<IPv6DnsResourceData>()) {
            PCPP_LOG_ERROR("DNS record is of type AAAA but given data isn't of type IPv6DnsResourceData");
            return false;
        }
        break;
    }

    case DNS_TYPE_NS:
    case DNS_TYPE_CNAME:
    case DNS_TYPE_DNAM:
    case DNS_TYPE_PTR: {
        if (!data->isTypeOf<StringDnsResourceData>()) {
            PCPP_LOG_ERROR("DNS record is of type NS, CNAME, DNAM or PTR but given data isn't of type StringDnsResourceData");
            return false;
        }
        break;
    }

    case DNS_TYPE_MX: {
        if (!data->isTypeOf<MxDnsResourceData>()) {
            PCPP_LOG_ERROR("DNS record is of type MX but given data isn't of type MxDnsResourceData");
            return false;
        }
        break;
    }

    default: {
        // do nothing
    }
    }

    // convert the IDnsResourceData to byte array
    if (!data->toByteArr(dataAsByteArr, dataLength, this)) {
        PCPP_LOG_ERROR("Cannot convert DNS resource data to byte array, data is probably invalid");
        return false;
    }

    size_t dataLengthOffset = m_NameLength + (2 * sizeof(uint16_t)) + sizeof(uint32_t);
    size_t dataOffset = dataLengthOffset + sizeof(uint16_t);

    if (m_DnsLayer != NULL) {
        size_t curLength = getDataLength();
        if (dataLength > curLength) {
            if (!m_DnsLayer->extendLayer(m_OffsetInLayer + dataOffset, dataLength - curLength, this)) {
                PCPP_LOG_ERROR("Couldn't set data for DNS query, unable to extend layer");
                return false;
            }
        } else if (dataLength < curLength) {
            if (!m_DnsLayer->shortenLayer(m_OffsetInLayer + dataOffset, curLength - dataLength, this)) {
                PCPP_LOG_ERROR("Couldn't set data for DNS query, unable to shorten layer");
                return false;
            }
        }
    }

    // write data to resource
    memcpy(getRawData() + dataOffset, dataAsByteArr, dataLength);
    // update data length in resource
    dataLength = htobe16((uint16_t)dataLength);
    memcpy(getRawData() + dataLengthOffset, &dataLength, sizeof(uint16_t));

    return true;
}

uint16_t DnsResource::getCustomDnsClass() const
{
    uint16_t value = *(uint16_t *)(getRawData() + m_NameLength + sizeof(uint16_t));
    return be16toh(value);
}

void DnsResource::setCustomDnsClass(uint16_t customValue)
{
    memcpy(getRawData() + m_NameLength + sizeof(uint16_t), &customValue, sizeof(uint16_t));
}

}
