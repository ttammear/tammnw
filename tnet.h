#ifndef TNET_H
#define TNET_H

#include <stdlib.h>
#include <stdio.h>

//linux
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
//TODO: remove
#include <assert.h>

#include <cstdint>

typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;

#define Megabytes(Value) (Value*1024LL*1024LL)

#define SEND_DATA_FLAG_REQCON 1
#define SEND_DATA_FLAG_ACCCON 2
//#define SEND_DATA_FLAG_REQCON 4

#define MAX_PACKET_SIZE 512
#define CONFIRMED_PACKETS_HISTORY_SIZE  512

enum ConnectionState
{
    CEDisconnected,
    CEConnecting,
    CEConnected
};

#define RECEIVED_MESSAGE_HISTORY_SIZE   4096 // POWER OF 2 ONLY

enum RingQueueState
{
    Empty,
    None,
    Full
};

struct RingQueue
{
    char* startAddr = NULL;
    size_t size = 0;
    char* dequeuePointer = NULL;
    char* queuePointer = NULL;
    RingQueueState state = RingQueueState::Empty;
};

typedef timespec net_time_point;

struct connectionState
{
    i32 socket;
    u32 destIP;
    u16 destPort;
    i32 seqId;
    u32 ack;
    i32 ackBits;
    u16 messageId;
    ConnectionState state;
    net_time_point lastPacketReceived;
    u32 confirmedPackets[CONFIRMED_PACKETS_HISTORY_SIZE];
    u16 receivedMessages[RECEIVED_MESSAGE_HISTORY_SIZE];
    pthread_mutex_t conMutex;
};

struct Host
{
    u32 maxConnections;
    i32 socket;
    connectionState* conStates;
    // resend buffer (doesn't need mutex, accessed only from 1 thread)
    // holds SentMessages structs
    RingQueue resendBuffer;
    // receive buffer
    // holds ReceivedMessage structs
    pthread_mutex_t receiveBufMutex;
    RingQueue receiveBuffer;
    // send buffer
    // holds QueuedMessage structs
    RingQueue sendBuffer;
    pthread_mutex_t sendBufMutex;
    pthread_t recWorker;
    pthread_t sendWorker;
    volatile bool sendDone;
    pthread_mutex_t sendMut;
    pthread_cond_t sendCon;

};

enum HostEvent
{
    HENothing,
    HEConnect,
    HEDisconnect,
    HEData
};

struct ReceivedEvent
{
    HostEvent type;
    i32 connection;
    u32 size;
    char data[MAX_PACKET_SIZE];
};

struct QueuedData
{
    bool reliable;
    i32 connection;
    i32 size;
    u32 flags;
    char data[MAX_PACKET_SIZE];
};


struct SentReliableData
{
    net_time_point sendTime;
    u32 pId;
    u16 messageId;
    QueuedData qData;
};

#define REL_HEADER_SIZE 18
#define UREL_HEADER_SIZE 2

#pragma pack(push,1)
struct urelbody
{
    unsigned char data[MAX_PACKET_SIZE];
};

struct relbody
{
    i32 seqId; // 4
    i32 ack;   // 8
    i32 ackBits; // 12
    u16 size; // 14
    u16 messageId; // 16
    unsigned char data[MAX_PACKET_SIZE];
};

struct packet
{
    unsigned char hasRel : 1;
    unsigned char reqCon : 1;
    unsigned char acceptCon : 1;
    unsigned char reserved : 3;
    u16 size : 10;
    union
    {
        urelbody urelBody;
        relbody relBody;
    };
};
#pragma pack(pop)

// actual API
void CreateHost(Host* host, u16 port, i32 maxConnections);
void FreeHost(Host* host);
HostEvent getNextEvent(i32 connection, char* buf, int& received);
void sendData(Host* host, i32 connection, const char* data, const i32 dataSize, const bool reliable, u32 flags = 0);
void sendPendingData(Host* host);

//#ifdef TNET_IMPLEMENTATION

// RINGBUFFER
bool RingQueueInitialize(RingQueue* dq, size_t size);
void RingQueueFreeMemory(RingQueue* dq);
void RingQueueReset(RingQueue* dq);
void RingQueueZeroMemory(RingQueue* dq);
bool RingQueueQueueData(RingQueue* dq, const void* data, size_t size);
size_t RingQueueDequeueData(RingQueue* dq, void* data, unsigned int size);
size_t RingQueuePeekData(RingQueue* dq, void* data, unsigned int size);
RingQueueState RingQueueGetState(RingQueue* dq);
bool RingQueueDequeue(RingQueue* dq);
// RINGBUFFER END

void closeSocket(i32 socket)
{
    if(socket != -1)
    {
        shutdown(socket, SHUT_RDWR);
    }
    else
    {
        printf("trying to close an invalid socket!\n");
    }
}

i32 openSocket(u16 port)
{
    //create socket
    i32 nsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(nsock < 0)
    {
        printf("Creating socket failed!\n");
        return -1;
    }
    // bind
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((unsigned short)port);
    if(bind(nsock, (const sockaddr*)&address, sizeof(sockaddr_in)) < 0)
    {
        printf("Binding socket failed!\n");
        return -1;
    }
    // set blocking mode
    u32 blocking = 1;
    u32 flags = fcntl(nsock, F_GETFL, 0);
    if (blocking)
        flags &= ~O_NONBLOCK;
    else
        flags |= O_NONBLOCK;
    if (fcntl(nsock, F_SETFL, flags) == -1)
    {
        printf("Failed to set socket blocking mode!\n");
        closeSocket(nsock);
        return -1;
    }
    return nsock;
}

bool pl = false;

i32 sendToSocket(i32 socket, void* data, u16 size, u32 destIp, u16 destPort)
{
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(destIp);
    address.sin_port = htons((unsigned short)destPort);
    i32 sentBytes = 1;
    // uncomment for 33% packetloss
    //if(random() % 3 != 1/* || pl == false*/)
        sentBytes = sendto(socket, data, size, 0, (sockaddr*)&address, sizeof(sockaddr_in));
    if(sentBytes <= 0)
    {
        printf("sendToSocket failed!\n");
    }
    return sentBytes;
}

bool recvFromSocket(i32 socket, char* data, i32& received, u32& fromAddr, u16& fromPort)
{
    bool ret;
    sockaddr_in from;
    socklen_t fromLength = sizeof(from);
    i32 ss;
    //TODO: replace max packet size
    ss = recvfrom(socket, (char*)data, MAX_PACKET_SIZE, 0, (sockaddr*)&from, &fromLength);
    if(ss == -1)
    {
        printf("recvfrom failed errno:%d\n",errno);
    }
    ret = ss > 0;
    if (ret)
    {
        received = (i32)ss;
        fromAddr = ntohl(from.sin_addr.s_addr);
        fromPort = ntohs(from.sin_port);
        return true;
    }
    return false;
}

void initConnectionState(connectionState& state)
{
    state.ack = 0;
    state.ackBits = 0;
    state.seqId = 0;
    state.destIP = 0;
    state.destPort = 0;
    state.socket = -1;
    state.state = ConnectionState::CEDisconnected;
    // TODO: could save few cycles here
    state.conMutex = PTHREAD_MUTEX_INITIALIZER;
    state.messageId = 0;
    clock_gettime(CLOCK_MONOTONIC, &state.lastPacketReceived);
    for(int i=0; i< CONFIRMED_PACKETS_HISTORY_SIZE;i++)
    {
        state.confirmedPackets[i] = 0xFFFFFFFF;
    }
    for(int i=0; i< RECEIVED_MESSAGE_HISTORY_SIZE;i++)
    {
        state.receivedMessages[i] = 0xFFFF;
    }
}

i32 findAndResetInactiveConnectionSlot(connectionState* connections, int maxConnections)
{
    i32 ret = -1;
    for(int i = 0; i < maxConnections; i++)
    {
        pthread_mutex_lock(&connections[i].conMutex);
        if(connections[i].state == ConnectionState::CEDisconnected)
        {
            initConnectionState(connections[i]);
            ret = i;
        }
        pthread_mutex_unlock(&connections[i].conMutex);
        if(ret != -1)
            break;
    }
    return ret;
}

i32 findActiveConnectionByDest(connectionState* connections, int maxConnections, u32 ip, u16 port)
{
    i32 ret = -1;
    for(int i = 0; i < maxConnections; i++)
    {
        pthread_mutex_lock(&connections[i].conMutex);
        if(connections[i].state != ConnectionState::CEDisconnected && connections[i].destIP == ip && connections[i].destPort == port)
        {
            ret = i;
        }
        pthread_mutex_unlock(&connections[i].conMutex);
        if(ret != -1)
            break;
    }
    return ret;
}

// only used by receiveProc
void proccessRemoteAck(connectionState& connection, u32 ack, u32 ackBits)
{
    unsigned int cabits;
    // REMCONST
    for (int i = 0; i < 32; i++) // TODO: dumb solution like this cant be the best way?
    {
        cabits = ackBits;
        cabits >>= i; // shift the interested bit into bit 0
        cabits &= 1; // mask so that only bit 0 remains
        assert(cabits == 1 || cabits == 0);
        u32 remPacketId = ack - i; // the remoteSequenceId the packet represents
        if (cabits == 1)
        {
            if (connection.confirmedPackets[remPacketId%CONFIRMED_PACKETS_HISTORY_SIZE] == remPacketId) // already confirmed
                continue;
            connection.confirmedPackets[remPacketId%CONFIRMED_PACKETS_HISTORY_SIZE] = remPacketId;
            //connection.confirmedPacketCount++; // do we even use this anywhere??
        }
    }
}

// only used by receiveProc
void ackPacket(connectionState& connection, u32 remSeq)
{
    if (remSeq >= connection.ack) // newer packet
    {
        unsigned int difference = remSeq - connection.ack;
        if (difference < 32) // REMCONST
        {
            connection.ackBits <<= difference;
            connection.ackBits |= 1;
        }
        else
        {
            //assert(false); // extreme case that should never happen when testing (but our protocol should be able to deal with it anyways, possible bug catch place)
            connection.ackBits = 0;
        }
        connection.ack = remSeq; // only update if its more recent
    }
    else // older packet
    {
        unsigned int difference = connection.ack - remSeq;
        unsigned int mask = 1 << difference;
        connection.ackBits |= mask;
    }
}

// TODO: accept strings
i32 hostConnect(Host* host, u32 destIp, u16 destPort)
{
    i32 conId = findAndResetInactiveConnectionSlot(host->conStates, host->maxConnections);
    if(conId >= 0)
    {
        pthread_mutex_lock(&host->conStates[conId].conMutex);
        host->conStates[conId].destIP = destIp;
        host->conStates[conId].destPort = destPort;
        host->conStates[conId].state = ConnectionState::CEConnecting;
        pthread_mutex_unlock(&host->conStates[conId].conMutex);
        sendData(host, conId, (char*)0, 0, true, SEND_DATA_FLAG_REQCON);
    }
    else
    {
        printf("Tried to start new connection when out of slots!\n");
    }
    return conId;
}

// TODO: accept strings
i32 hostAccept(Host* host, i32 conId)
{
    if(conId >= 0)
    {
        pthread_mutex_lock(&host->conStates[conId].conMutex);
        host->conStates[conId].state = ConnectionState::CEConnected;
        pthread_mutex_unlock(&host->conStates[conId].conMutex);
        sendData(host, conId, (char*)0, 0, true, SEND_DATA_FLAG_ACCCON);
    }
    else
    {
        printf("Tried to accept invalid connection!\n");
    }
    return conId;
}

void QDataToPacket(QueuedData& q, packet& p, u32 ack, u32 ackBits, u32 seqId, u16 messageId)
{
    p.hasRel = q.reliable;
    p.reqCon = (q.flags & SEND_DATA_FLAG_REQCON) != 0;
    p.acceptCon = (q.flags & SEND_DATA_FLAG_ACCCON) != 0;
    p.reserved = 0;
    if(q.reliable)
    {
        // REMCONST
        p.size = q.size + REL_HEADER_SIZE; // 16 = reliable packet header size
        p.relBody.ack = ack;
        p.relBody.ackBits = ackBits;
        p.relBody.seqId = seqId;
        p.relBody.messageId = messageId;
        p.relBody.size = q.size;
        memcpy(p.relBody.data, q.data, q.size);
    }
    else
    {
        p.size = q.size + UREL_HEADER_SIZE; // 2 = unreiliable packet header size
        memcpy(p.urelBody.data, q.data, q.size);
    }
}

// connection mutex must be locked !!
void sendPacket(Host* host, packet& p, QueuedData& q, u16 messageId)
{
    connectionState* connections = host->conStates;
    if(connections[q.connection].state == ConnectionState::CEDisconnected)
        return;

    SentReliableData rdata;
    QDataToPacket(q, p, connections[q.connection].ack, connections[q.connection].ackBits, connections[q.connection].seqId++, messageId);

    if(q.reliable)
    {
        clock_gettime(CLOCK_MONOTONIC, &rdata.sendTime);
        rdata.pId = p.relBody.seqId;
        rdata.messageId = messageId;

        memcpy(&rdata.qData,&q,sizeof(QueuedData)-MAX_PACKET_SIZE+q.size);
        bool qd = RingQueueQueueData(&host->resendBuffer, &rdata, sizeof(SentReliableData)-MAX_PACKET_SIZE+q.size);
        assert(qd);
    }

    // TODO: is the size correct?
    sendToSocket(host->socket, &p, p.size, connections[q.connection].destIP, connections[q.connection].destPort);
}

i32 getDurationToNowMs(net_time_point pastPoint)
{
    net_time_point now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dif = (now.tv_sec-pastPoint.tv_sec)*1000+(now.tv_nsec-pastPoint.tv_nsec)/1000000;
    return (i32)dif;
}

#define CONNECTION_TIMEOUT_MS   10000

// lock mutex!
void disconnectConnection(Host* host, u32 connectionId)
{
    // TODO: send a disconnect message to the other side too
    host->conStates[connectionId].state = ConnectionState::CEDisconnected;
    pthread_mutex_lock(&host->receiveBufMutex);
    ReceivedEvent buf;
    buf.connection = connectionId;
    buf.size = 0;
    buf.type = HostEvent::HEDisconnect;
    bool qd = RingQueueQueueData(&host->receiveBuffer, &buf, sizeof(buf)-MAX_PACKET_SIZE+buf.size);
    assert(qd);
    pthread_mutex_unlock(&host->receiveBufMutex);
}

void checkConnectionStates(Host* host)
{
    connectionState* connections = host->conStates;
    for(u32 i = 0; i < host->maxConnections; i++)
    {
        pthread_mutex_lock(&connections[i].conMutex);
        if(connections[i].state != ConnectionState::CEDisconnected)
        {
            if(getDurationToNowMs(connections[i].lastPacketReceived) > CONNECTION_TIMEOUT_MS)
            {
                disconnectConnection(host, i);
                printf("Disconnected\n");
            }
        }
        pthread_mutex_unlock(&connections[i].conMutex);
    }
}


void sendPackets(Host* host)
{
    QueuedData q;
    packet p;

    connectionState* connections = host->conStates;
    pthread_mutex_lock(&host->sendBufMutex);
    // TODO: what happens if buf too small?
    // TODO: can invalid connectionid get here?
    while(RingQueueDequeueData(&host->sendBuffer, &q, sizeof(q)) > 0)
    {
        pthread_mutex_lock(&connections[q.connection].conMutex);
        sendPacket(host, p, q, connections[q.connection].messageId++);
        pthread_mutex_unlock(&connections[q.connection].conMutex);
    }
    pthread_mutex_unlock(&host->sendBufMutex);
    // check if everything needs resend
    SentReliableData rdata;
    bool got = RingQueuePeekData(&host->resendBuffer, &rdata, sizeof(rdata));
    // dif in msec
    i32 dif = getDurationToNowMs(rdata.sendTime); // REMCONST
    while(got && dif >= 200) // REMCONST
    {
        bool isReceived = host->conStates[rdata.qData.connection].confirmedPackets[rdata.pId%CONFIRMED_PACKETS_HISTORY_SIZE] == rdata.pId;
        if(!isReceived)
        {
            //printf("Message resent\n");

            sendPacket(host, p, rdata.qData, rdata.messageId);
        }
        RingQueueDequeue(&host->resendBuffer);
        got = RingQueuePeekData(&host->resendBuffer, &rdata, sizeof(rdata));
        if (got)
            dif = getDurationToNowMs(rdata.sendTime); // REMCONST
    }
}

void receivePacket(u32 connectionId, connectionState& connection, packet& p, i32 received, RingQueue* recBuf, HostEvent event)
{
    i32 size = p.size;
    if(size != received) // packet size didn't match, corrupted or incomplete
    {
        printf("Corrupted packet 1\n");
        return;
    }
    if(p.hasRel)
    {
        if(p.relBody.size != received - REL_HEADER_SIZE) // REMCONST
        {
            printf("Corrupted packet 2\n");
            return;
        }
        pthread_mutex_lock(&connection.conMutex); // <------------ CONNECTION MUTEX LOCK
        clock_gettime(CLOCK_MONOTONIC, &connection.lastPacketReceived);
        u16 messageId = p.relBody.messageId;
        bool received = connection.receivedMessages[messageId%RECEIVED_MESSAGE_HISTORY_SIZE] == messageId;
        if(!received)
        {
            // TODO: replace assert with a useful measure (dc?)
            i32 cur = connection.receivedMessages[messageId%RECEIVED_MESSAGE_HISTORY_SIZE];
           // assert(cur == 0xFFFF || cur == (messageId-RECEIVED_MESSAGE_HISTORY_SIZE)); // if this fails, it means that either a reliable data was dropped or receivedMessages array overflowed
            connection.receivedMessages[messageId%RECEIVED_MESSAGE_HISTORY_SIZE] = messageId;
        }
        // confirm what the remote party has received
        proccessRemoteAck(connection, p.relBody.ack, p.relBody.ackBits);
        // acknowledge the packet we received
        ackPacket(connection, p.relBody.seqId);
        pthread_mutex_unlock(&connection.conMutex);  // <------------ CONNECTION MUTEX UNLOCK
        if(!received) // only receive if not received before
        {
            // TODO: check buffer overflow?
            ReceivedEvent buf;
            buf.connection = connectionId;
            buf.size = p.relBody.size;
            buf.type = event;
            memcpy(buf.data, p.relBody.data, p.relBody.size);
            bool qd = RingQueueQueueData(recBuf, &buf, sizeof(buf)-MAX_PACKET_SIZE+p.relBody.size);
            assert(qd);
            //printf("reliable data: %s \n", (char*)p.relBody.data);
        }
    }
    else
    {
        ReceivedEvent buf;
        buf.connection = connectionId;
        buf.size = p.size-UREL_HEADER_SIZE; // 2 is the size of main header TODO:if you ever change header size
        buf.type = event;
        memcpy(buf.data, p.urelBody.data, buf.size);
        bool qd = RingQueueQueueData(recBuf, &buf, sizeof(buf)-MAX_PACKET_SIZE+buf.size);
        assert(qd);
        //printf("unreliable data: %s \n", (char*)p.urelBody.data);
    }
}

struct receiveProcArgs
{
    Host* host;
};

void* receiveProc(void* context)
{
    receiveProcArgs* args = (receiveProcArgs*)context;
    i32 socket = args->host->socket;
    connectionState* connections = args->host->conStates;
    u32 maxConnections = args->host->maxConnections;
    RingQueue* receiveBuf = &args->host->receiveBuffer;
    Host* host = args->host;
    free(context);
    packet buf;
    i32 received;
    u32 fromAddr;
    u16 fromPort;
    // TODO: packet bigger than 512 ends receiver thread!
    while(recvFromSocket(socket, (char*)&buf, received, fromAddr, fromPort))
    {
        i32 conId = findActiveConnectionByDest(connections, maxConnections, fromAddr, fromPort);
        if(conId != -1) // connection exists and active
        {
            // TODO: locking and unlocking 2 times in  a row
            pthread_mutex_lock(&connections[conId].conMutex);
            ConnectionState cstate = host->conStates[conId].state;
            pthread_mutex_unlock(&connections[conId].conMutex);

            if(cstate == ConnectionState::CEConnected)
            {
                pthread_mutex_lock(&host->receiveBufMutex);
                receivePacket(conId, connections[conId],buf, received, receiveBuf, HostEvent::HEData);
                pthread_mutex_unlock(&host->receiveBufMutex);
            }
            else if(cstate == ConnectionState::CEConnecting && buf.acceptCon)
            {
                pthread_mutex_lock(&connections[conId].conMutex);
                connections[conId].state = ConnectionState::CEConnected;
                pthread_mutex_unlock(&connections[conId].conMutex);

                pthread_mutex_lock(&host->receiveBufMutex);
                receivePacket(conId, connections[conId],buf, received, receiveBuf, HostEvent::HEConnect);
                pthread_mutex_unlock(&host->receiveBufMutex);
            }
        }
        else if(buf.reqCon) // inactive or didn't exist
        {
            i32 newConId = findAndResetInactiveConnectionSlot(connections, maxConnections);
            if(newConId != -1)
            {
                pthread_mutex_lock(&connections[newConId].conMutex);
                connections[newConId].state = ConnectionState::CEConnecting;
                connections[newConId].destIP = fromAddr;
                connections[newConId].destPort = fromPort;
                pthread_mutex_unlock(&connections[newConId].conMutex);

                pthread_mutex_lock(&host->receiveBufMutex);
                receivePacket(newConId, connections[newConId],buf, received, receiveBuf, HostEvent::HEConnect);
                pthread_mutex_unlock(&host->receiveBufMutex);
            }
            else
            {
                printf("New connection, but no slots left!\n");
            }
        }
        else
        {
            printf("weird packet? \n");
        }
    }
    printf("Receive worker closed!\n");
    return 0;
}

struct sendProcArgs
{
    Host* host;
};

void* sendProc(void* context)
{
    printf("Send worker started!\n");
    sendProcArgs* args = (sendProcArgs*)context;
    Host* host = args->host;
    free(context);
    pthread_mutex_lock (&host->sendMut);
    while(true)                      //if while loop with signal complete first don't wait
    {
        while(host->sendDone)
        {
            pthread_cond_wait(&host->sendCon, &host->sendMut);
        }
        checkConnectionStates(host);
        sendPackets(host);
        host->sendDone = true;
    }
    pthread_mutex_unlock (&host->sendMut);
    printf("Send worker closed!\n");
    return 0;
}

void CreateHost(Host* host, u16 port, i32 maxConnections)
{
    host->maxConnections = maxConnections;
    //host->
    host->socket = openSocket(port);
    if(host->socket < 0)
        return;
    host->conStates = (connectionState*)malloc(maxConnections*sizeof(connectionState));
    for(int i=0; i<maxConnections;i++)
    {
        initConnectionState(host->conStates[i]);
        host->conStates[i].socket = host->socket;
    }
    // TODO: scale buffer size based on max connections?
    RingQueueInitialize(&host->resendBuffer, Megabytes(10));
    RingQueueInitialize(&host->sendBuffer, Megabytes(1));
    RingQueueInitialize(&host->receiveBuffer, Megabytes(1));
    host->sendDone = true;
    host->sendMut=PTHREAD_MUTEX_INITIALIZER;
    host->sendCon=PTHREAD_COND_INITIALIZER;
    host->sendBufMutex=PTHREAD_MUTEX_INITIALIZER;
    host->receiveBufMutex=PTHREAD_MUTEX_INITIALIZER;
    receiveProcArgs* wargs = new receiveProcArgs;
    wargs->host = host;
    if(pthread_create(&host->recWorker, 0, receiveProc, wargs))
    {
        printf("Creating worker thread for host failed!\n");
        FreeHost(host);
        // TODO: return false?
        return;
    }
    sendProcArgs* swargs = new sendProcArgs;
    swargs->host = host;
    if(pthread_create(&host->sendWorker, 0, sendProc, swargs))
    {
        printf("Creating worker thread for host failed!\n");
        FreeHost(host);
        // TODO: return false?
        return;
    }
}

void FreeHost(Host* host)
{
    // TODO: end thread
    //host->recWorker.join
    closeSocket(host->socket);
    free(host->conStates);
    RingQueueFreeMemory(&host->resendBuffer);
    RingQueueFreeMemory(&host->sendBuffer);
    RingQueueFreeMemory(&host->receiveBuffer);
}

HostEvent getNextEvent(Host* host, i32& connection, char* data, u32 size, i32& recSize)
{
    // TODO: 2 memcpys, remove 1
    // TODO: sure that no buffer overflow can happen?
    ReceivedEvent event;
    pthread_mutex_lock(&host->receiveBufMutex);
    size_t result = RingQueueDequeueData(&host->receiveBuffer, &event, sizeof(event));
    pthread_mutex_unlock(&host->receiveBufMutex);
    if(result > 0)
    {
        connection = event.connection;
        if(event.type == HostEvent::HEData && size >= event.size)
        {
            memcpy(data, event.data, event.size);
            recSize = event.size;
        }
        return event.type;
    }
    else
    {
        return HostEvent::HENothing;
    }
}

void sendPendingData(Host* host)
{
    pthread_mutex_lock (&host->sendMut);
    /*if(!host->sendDone)
        printf("main thread next iteration, but send thread not even started!\n");*/
    host->sendDone = false;
    //printf("signaling send worker\n");
    pthread_cond_signal(&host->sendCon);
    pthread_mutex_unlock (&host->sendMut);
    //pthread_yield(); // just in case
}

void sendData(Host* host, i32 connection, const char* data, const i32 dataSize, const bool reliable, u32 flags)
{
    // TODO: basically 2 memcpys, remove 1
    QueuedData buf;
    buf.flags = flags;
    buf.connection = connection;
    buf.size = dataSize;
    buf.reliable = reliable;
    memcpy(buf.data, data, dataSize);

    pthread_mutex_lock(&host->sendBufMutex);
    bool qd = RingQueueQueueData(&host->sendBuffer, (char*)&buf, sizeof(QueuedData)+dataSize-MAX_PACKET_SIZE);
    assert(qd);
    pthread_mutex_unlock(&host->sendBufMutex);
}

bool RingQueueInitialize(RingQueue* dq, size_t size)
{
    dq->startAddr = (char*)malloc(size);
    dq->dequeuePointer = dq->startAddr;
    dq->queuePointer = dq->startAddr;
    dq->state = RingQueueState::Empty;
    dq->size = size;
    return dq->startAddr != NULL;
}

void RingQueueFreeMemory(RingQueue* dq)
{
    assert(dq->startAddr != NULL);
    free(dq->startAddr);
}

// sets to empty state
void RingQueueReset(RingQueue* dq)
{
    dq->dequeuePointer = dq->startAddr;
    dq->queuePointer = dq->startAddr;
    dq->state = RingQueueState::Empty;
}

// a reset with zeroing
void RingQueueZeroMemory(RingQueue* dq)
{
    memset(dq->startAddr, 0, dq->size);
    RingQueueReset(dq);
}

// queues data to the buffer and returns true on success
bool RingQueueQueueData(RingQueue* dq, const void* data, size_t size)
{
    if (size <= 0)
        return false;
    char* endaddr = dq->startAddr + dq->size;
    char* pointer = dq->queuePointer;
    char* next = (pointer + sizeof(size_t) + size);
    if (next >= endaddr) // back to start if not enough room
    {
        if (pointer + sizeof(size_t) <= endaddr)
            *(size_t*)pointer = 0;				// write 0, so the reader knows we went back to 0

        pointer = dq->startAddr;
        next = (pointer + sizeof(size_t) + size);

        if (dq->dequeuePointer == pointer)
        {
            return false;
        }
    }
    if ((pointer < dq->dequeuePointer && next >= dq->dequeuePointer) /*|| // buffer overflow would happen                                                                                                                             (pointer >= dq->dequeuePointer && next <= dq->dequeuePointer)*/)
    {
        dq->state = RingQueueState::Full;
        return false;
    }
    *(size_t*)pointer = size;
    pointer += sizeof(size_t);
    memcpy(pointer, data, size);
    pointer += size;
    dq->queuePointer = pointer;
    if (dq->state == RingQueueState::Empty)
    {
        dq->state = RingQueueState::None;
    }
    return true;
}

// dequeues data from the buffer and returns number of bytes read
size_t RingQueueDequeueData(RingQueue* dq, void* data, unsigned int size)
{
    if (dq->state == RingQueueState::Empty) // if its empty then its empty...
    {
        return 0;
    }
    char* pointer = dq->dequeuePointer;
    size_t cursize;
    if ((pointer + sizeof(size_t)) <= (dq->startAddr + dq->size)) // make sure we don't read from outside of the buffer
    {
        cursize = *(size_t*)pointer; // read the size
        if (cursize == 0) // size can only be 0 if the producer went bant back to start
        {
            pointer = dq->startAddr;
            cursize = *(size_t*)pointer; // read the size
        }
    }
    else // we would have read from outside the buffer bounds, back to start
    {
        pointer = dq->startAddr;
        cursize = *(size_t*)pointer; // read the size
    }
    assert(cursize > 0);
    if (cursize > size) // the buffer we were told to put the data was too small
    {
        return 0;
    }
    pointer += sizeof(size_t);
    memcpy(data, pointer, cursize);
    dq->dequeuePointer = pointer + cursize;
    if (dq->queuePointer == dq->dequeuePointer)
        dq->state = RingQueueState::Empty;
    return cursize > 0;
}

// does the same as dequeuedata, but doesnt remove the data from queue
size_t RingQueuePeekData(RingQueue* dq, void* data, unsigned int size)
{
    if (dq->state == RingQueueState::Empty) // if its empty then its empty...
    {
        return 0;
    }
    char* pointer = dq->dequeuePointer;
    size_t cursize;
    if ((pointer + sizeof(size_t)) <= (dq->startAddr + dq->size)) // make sure we don't read from outside of the buffer
    {
        cursize = *(size_t*)pointer; // read the size
        if (cursize == 0) // size can only be 0 if the producer went bant back to start
        {
            pointer = dq->startAddr;
            cursize = *(size_t*)pointer; // read the size
        }
    }
    else // we would have read from outside the buffer bounds, back to start
    {
        pointer = dq->startAddr;
        cursize = *(size_t*)pointer; // read the size
    }
    assert(cursize > 0);
    if (cursize > size) // the buffer we were told to put the data was too small
    {
        return 0;
    }
    pointer += sizeof(size_t);
    memcpy(data, pointer, cursize);
    return cursize > 0;
}

RingQueueState RingQueueGetState(RingQueue* dq)
{
    RingQueueState state;
    state = dq->state;
    return state;
}

bool RingQueueDequeue(RingQueue* dq)
{
    if (dq->state == RingQueueState::Empty) // if its empty then its empty...
    {
        return 0;
    }
    char* pointer = dq->dequeuePointer;
    size_t cursize;
    if ((pointer + sizeof(size_t)) <= (dq->startAddr + dq->size)) // make sure we don't read from outside of the buffer
    {
        cursize = *(size_t*)pointer; // read the size
        if (cursize == 0) // size can only be 0 if the producer went bant back to start
        {
            pointer = dq->startAddr;
            cursize = *(size_t*)pointer; // read the size
        }
    }
    else // we would have read from outside the buffer bounds, back to start
    {
        pointer = dq->startAddr;
        cursize = *(size_t*)pointer; // read the size
    }
    assert(cursize > 0);
    pointer += sizeof(size_t);
    dq->dequeuePointer = pointer + cursize;
    if (dq->queuePointer == dq->dequeuePointer)
    dq->state = RingQueueState::Empty;
    return cursize > 0;
}
//#endif // TNET_IMPLEMENTATION
#endif // TNET_H
