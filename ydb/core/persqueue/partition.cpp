#include "event_helpers.h"
#include "mirrorer.h"
#include "offload_actor.h"
#include "partition_util.h"
#include "partition_common.h"
#include "partition_log.h"

#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/blobstorage.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/base/path.h>
#include <ydb/core/quoter/public/quoter.h>
#include <ydb/core/persqueue/writer/source_id_encoding.h>
#include <ydb/core/protos/counters_pq.pb.h>
#include <ydb/core/protos/msgbus.pb.h>
#include <ydb/library/persqueue/topic_parser/topic_parser.h>
#include <ydb/public/lib/base/msgbus.h>
#include <library/cpp/html/pcdata/pcdata.h>
#include <library/cpp/monlib/service/pages/templates.h>
#include <library/cpp/time_provider/time_provider.h>
#include <util/folder/path.h>
#include <util/string/escape.h>
#include <util/system/byteorder.h>


namespace NKafka {

    // IsDuplicate is only needed for Kafka protocol deduplication.
    // baseSequence field in Kafka protocol has type int32 and the numbers loop back from the maximum possible value back to 0.
    // I.e. the next seqno after int32max is 0.
    // To decide if we got a duplicate seqno or an out of order seqno,
    // we are comparing the difference between maxSeqNo and seqNo with MAX_SEQNO_DIFFERENCE_UNTIL_OUT_OF_ORDER.
    // The value of MAX_SEQNO_DIFFERENCE_UNTIL_OUT_OF_ORDER is half of the int32 range.
    bool IsDuplicate(ui64 maxSeqNo, ui64 seqNo) {
        if (maxSeqNo < seqNo) {
            maxSeqNo += 1ul << 31;
        }
        return maxSeqNo - seqNo < MAX_SEQNO_DIFFERENCE_UNTIL_OUT_OF_ORDER;
    }

    // InSequence is only needed for Kafka protocol deduplication.
    bool InSequence(ui64 maxSeqNo, ui64 seqNo) {
        return (maxSeqNo + 1 == seqNo) || (maxSeqNo == std::numeric_limits<i32>::max() && seqNo == 0);
    }

    ECheckDeduplicationResult CheckDeduplication(i16 lastEpoch, ui64 lastSeqNo, i16 messageEpoch, ui64 messageSeqNo) {
        if (lastEpoch > messageEpoch) {
            return ECheckDeduplicationResult::INVALID_PRODUCER_EPOCH;
        }

        if (lastEpoch < messageEpoch) {
            if (messageSeqNo == 0) {
                // Only the first ever epoch for a given producerId is allowed to have the first seqNo != 0.
                return ECheckDeduplicationResult::OK;
            }
            return ECheckDeduplicationResult::OUT_OF_ORDER_SEQUENCE_NUMBER;
        }

        if (InSequence(lastSeqNo, messageSeqNo)) {
            return ECheckDeduplicationResult::OK;
        }

        if (IsDuplicate(lastSeqNo, messageSeqNo)) {
            // Kafka sends successful answer in response to requests
            // that exactly match some of the last 5 batches (that may contain multiple records each).

            return ECheckDeduplicationResult::DUPLICATE_SEQUENCE_NUMBER;
        }

        return ECheckDeduplicationResult::OUT_OF_ORDER_SEQUENCE_NUMBER;
    }

    std::pair<NPersQueue::NErrorCode::EErrorCode, TString> MakeDeduplicationError(
        ECheckDeduplicationResult res, const TString& topicName, ui32 partitionId, const TString& sourceId, ui64 poffset,
        i16 lastEpoch, ui64 lastSeqNo, i16 messageEpoch, ui64 messageSeqNo
    ) {
        switch (res) {
        case NKafka::ECheckDeduplicationResult::INVALID_PRODUCER_EPOCH: {
            return {
                NPersQueue::NErrorCode::KAFKA_INVALID_PRODUCER_EPOCH,
                TStringBuilder() << "Epoch of producer " << EscapeC(sourceId) << " at offset " << poffset
                                    << " in " << topicName << "-" << partitionId << " is " << messageEpoch
                                    << ", which is smaller than the last seen epoch " << lastEpoch
            };
        }
        case NKafka::ECheckDeduplicationResult::OUT_OF_ORDER_SEQUENCE_NUMBER: {
            auto message = TStringBuilder() << "Out of order sequence number for producer " << EscapeC(sourceId) << " at offset " << poffset
                                    << " in " << topicName << "-" << partitionId << ": ";
            if (lastEpoch < messageEpoch) {
                message << "for new producer epoch expected seqNo 0, got " << messageSeqNo;
            } else {
                message << messageSeqNo << " (incoming seq. number), " << lastSeqNo << " (current end sequence number)";
            }
            return {NPersQueue::NErrorCode::KAFKA_OUT_OF_ORDER_SEQUENCE_NUMBER, message};
        }
        default:
            return {};
        }
    }
}

namespace {

template <class T>
struct TIsSimpleSharedPtr : std::false_type {
};

template <class U>
struct TIsSimpleSharedPtr<TSimpleSharedPtr<U>> : std::true_type {
};

}

namespace NKikimr::NPQ {

static const TDuration WAKE_TIMEOUT = TDuration::Seconds(5);
static const TDuration UPDATE_AVAIL_SIZE_INTERVAL = TDuration::MilliSeconds(100);
static const TDuration MIN_UPDATE_COUNTERS_DELAY = TDuration::MilliSeconds(300);
static const ui32 MAX_USERS = 1000;
static const ui32 MAX_KEYS = 10000;
static const ui32 MAX_TXS = 1000;
static const ui32 MAX_WRITE_CYCLE_SIZE = 16_MB;

auto GetStepAndTxId(ui64 step, ui64 txId)
{
    return std::make_pair(step, txId);
}

template<class E>
auto GetStepAndTxId(const E& event)
{
    return GetStepAndTxId(event.Step, event.TxId);
}

// SeqnoViolation checks that the message seqno is correct and is used in transaction processing.
// The user may run conflicting transactions and we should block a transaction that tries to write a wrong seqno.
bool SeqnoViolation(TMaybe<i16> lastEpoch, ui64 lastSeqNo, TMaybe<i16> messageEpoch, ui64 messageSeqNo) {
    bool isKafkaRequest = lastEpoch.Defined() && messageEpoch.Defined();
    if (isKafkaRequest) {
        // In Kafka conflicting transactions are not possible if the user follows the protocol.
        return false;
    }

    return messageSeqNo <= lastSeqNo;
}

bool TPartition::LastOffsetHasBeenCommited(const TUserInfoBase& userInfo) const {
    return !IsActive() &&
        (static_cast<ui64>(std::max<i64>(userInfo.Offset, 0)) == BlobEncoder.EndOffset ||
         CompactionBlobEncoder.StartOffset == BlobEncoder.EndOffset);
}

struct TMirrorerInfo {
    TMirrorerInfo(const TActorId& actor, const TTabletCountersBase& baseline)
    : Actor(actor) {
        Baseline.Populate(baseline);
    }

    TActorId Actor;
    TTabletCountersBase Baseline;
};

const TString& TPartition::TopicName() const {
    return TopicConverter->GetClientsideName();
}

TString TPartition::LogPrefix() const {
    TString state;
    if (CurrentStateFunc() == &TThis::StateInit) {
        state = "StateInit";
    } else if (CurrentStateFunc() == &TThis::StateIdle) {
        state = "StateIdle";
    } else {
        state = "Unknown";
    }
    return TStringBuilder() << "[PQ: " << TabletID << ", Partition: " << Partition << ", State: " << state << "] ";
}

bool TPartition::IsActive() const {
    return PartitionConfig == nullptr || PartitionConfig->GetStatus() == NKikimrPQ::ETopicPartitionStatus::Active;
}

bool TPartition::CanWrite() const {
    if (PartitionConfig == nullptr) {
        // Old format without AllPartitions configuration field.
        // It is not split/merge partition.
        return true;
    }
    if (NewPartition && PartitionConfig->ParentPartitionIdsSize() > 0) {
        // A tx of create partition configuration is not commited.
        return false;
    }
    if (PendingPartitionConfig && PendingPartitionConfig->GetStatus() != NKikimrPQ::ETopicPartitionStatus::Active) {
        // Pending configuration tx inactivate this partition.
        return false;
    }

    if (ClosedInternalPartition) {
        return false;
    }

    return IsActive();
}

bool TPartition::CanEnqueue() const {
    if (ClosedInternalPartition) {
        return false;
    }

    return IsActive();
}

ui64 GetOffsetEstimate(const std::deque<TDataKey>& container, TInstant timestamp, ui64 offset) {
    if (container.empty()) {
        return offset;
    }
    auto it = std::lower_bound(container.begin(), container.end(), timestamp,
                    [](const TDataKey& p, const TInstant timestamp) { return timestamp > p.Timestamp; });
    if (it == container.end()) {
        return offset;
    } else {
        return it->Key.GetOffset();
    }
}

void TPartition::ReplyError(const TActorContext& ctx, const ui64 dst, NPersQueue::NErrorCode::EErrorCode errorCode, const TString& error) {
    ReplyPersQueueError(
        dst == 0 ? ctx.SelfID : Tablet, ctx, TabletID, TopicName(), Partition,
        TabletCounters, NKikimrServices::PERSQUEUE, dst, errorCode, error, true
    );
}

void TPartition::ReplyError(const TActorContext& ctx, const ui64 dst, NPersQueue::NErrorCode::EErrorCode errorCode, const TString& error, NWilson::TSpan& span) {
    ReplyError(ctx, dst, errorCode, error);
    span.EndError(error);
}

void TPartition::ReplyPropose(const TActorContext& ctx,
                              const NKikimrPQ::TEvProposeTransaction& event,
                              NKikimrPQ::TEvProposeTransactionResult::EStatus statusCode,
                              NKikimrPQ::TError::EKind kind,
                              const TString& reason)
{
    ctx.Send(ActorIdFromProto(event.GetSourceActor()),
             MakeReplyPropose(event,
                              statusCode,
                              kind, reason).Release());
}

void TPartition::ReplyOk(const TActorContext& ctx, const ui64 dst) {
    ctx.Send(Tablet, MakeReplyOk(dst).Release());
}

void TPartition::ReplyOk(const TActorContext& ctx, const ui64 dst, NWilson::TSpan& span) {
    ReplyOk(ctx, dst);
    span.EndOk();
}

void TPartition::ReplyGetClientOffsetOk(const TActorContext& ctx, const ui64 dst, const i64 offset,
    const TInstant writeTimestamp, const TInstant createTimestamp, bool consumerHasAnyCommits,
    const std::optional<TString>& committedMetadata) {
    ctx.Send(Tablet, MakeReplyGetClientOffsetOk(dst, offset, writeTimestamp, createTimestamp, consumerHasAnyCommits, committedMetadata).Release());
}

NKikimrClient::TKeyValueRequest::EStorageChannel GetChannel(ui32 i) {
    return NKikimrClient::TKeyValueRequest::EStorageChannel(NKikimrClient::TKeyValueRequest::MAIN + i);
}

void AddCheckDiskRequest(TEvKeyValue::TEvRequest *request, ui32 numChannels) {
    for (ui32 i = 0; i < numChannels; ++i) {
        request->Record.AddCmdGetStatus()->SetStorageChannel(GetChannel(i));
    }
}

TPartition::TPartition(ui64 tabletId, const TPartitionId& partition, const TActorId& tablet, ui32 tabletGeneration, const TActorId& blobCache,
                       const NPersQueue::TTopicConverterPtr& topicConverter, TString dcId, bool isServerless,
                       const NKikimrPQ::TPQTabletConfig& tabletConfig, const TTabletCountersBase& counters, bool subDomainOutOfSpace, ui32 numChannels,
                       const TActorId& writeQuoterActorId, bool newPartition, TVector<TTransaction> distrTxs)
    : Initializer(this)
    , TabletID(tabletId)
    , TabletGeneration(tabletGeneration)
    , Partition(partition)
    , TabletConfig(tabletConfig)
    , Counters(counters)
    , TopicConverter(topicConverter)
    , IsLocalDC(TabletConfig.GetLocalDC())
    , DCId(std::move(dcId))
    , PartitionGraph()
    , SourceManager(*this)
    , WriteInflightSize(0)
    , Tablet(tablet)
    , BlobCache(blobCache)
    , DeletedKeys(std::make_shared<std::deque<TString>>())
    , CompactionBlobEncoder(partition, false)
    , BlobEncoder(partition, true)
    , GapSize(0)
    , IsServerless(isServerless)
    , ReadingTimestamp(false)
    , Cookie(ERequestCookie::End)
    , InitDuration(TDuration::Zero())
    , InitDone(false)
    , NewPartition(newPartition)
    , Subscriber(partition, TabletCounters, Tablet)
    , DiskIsFull(false)
    , SubDomainOutOfSpace(subDomainOutOfSpace)
    , HasDataReqNum(0)
    , WriteQuotaTrackerActor(writeQuoterActorId)
    , AvgWriteBytes{{TDuration::Seconds(1), 1000}, {TDuration::Minutes(1), 1000}, {TDuration::Hours(1), 2000}, {TDuration::Days(1), 2000}}
    , AvgReadBytes(TDuration::Minutes(1), 1000)
    , AvgQuotaBytes{{TDuration::Seconds(1), 1000}, {TDuration::Minutes(1), 1000}, {TDuration::Hours(1), 2000}, {TDuration::Days(1), 2000}}
    , ReservedSize(0)
    , Channel(0)
    , NumChannels(numChannels)
    , WriteBufferIsFullCounter(nullptr)
    , WriteLagMs(TDuration::Minutes(1), 100)
    , LastEmittedHeartbeat(TRowVersion::Min())
{

    TabletCounters.Populate(Counters);

    if (!distrTxs.empty()) {
        for (auto& tx : distrTxs) {
            if (!tx.Predicate.Defined()) {
                UserActionAndTransactionEvents.emplace_back(MakeSimpleShared<TTransaction>(std::move(tx)));
            } else if (*tx.Predicate) {
                auto txId = tx.GetTxId();
                auto txPtr = MakeSimpleShared<TTransaction>(std::move(tx));
                BatchingState = ETxBatchingState::Executing;
                if (txId.Defined()) {
                    TransactionsInflight.insert(std::make_pair(*txId, txPtr));
                }
                UserActionAndTxPendingCommit.emplace_back(std::move(txPtr));
            }
        }
    }
}

void TPartition::EmplaceResponse(TMessage&& message, const TActorContext& ctx) {
    const auto now = ctx.Now();
    Responses.emplace_back(
        message.Body,
        std::move(message.Span),
        (now - TInstant::Zero()) - message.QueueTime,
        now
    );
}

ui64 TPartition::UserDataSize() const {
    if (CompactionBlobEncoder.DataKeysBody.size() <= 1) {
        // tiny optimization - we do not meter very small queues up to 16MB
        return 0;
    }

    // We assume that DataKeysBody contains an up-to-date set of blobs, their relevance is
    // maintained by the background process. However, the last block may contain several irrelevant
    // messages. Because of them, we throw out the size of the entire blob.
    auto size = Size();
    auto lastBlobSize = CompactionBlobEncoder.DataKeysBody[0].Size;
    Y_DEBUG_ABORT_UNLESS(size >= lastBlobSize, "Metering data size must be positive");
    return size >= lastBlobSize ? size - lastBlobSize : 0;
}

ui64 TPartition::MeteringDataSize(TInstant now) const {
    if (IsActive() || NKikimrPQ::TPQTabletConfig::METERING_MODE_REQUEST_UNITS == Config.GetMeteringMode()) {
        return UserDataSize();
    } else {
        // We only add the amount of data that is blocked by an important consumer.
        auto expirationTimestamp = now - TDuration::Seconds(Config.GetPartitionConfig().GetLifetimeSeconds()) - WAKE_TIMEOUT;
        ui64 size = BlobEncoder.GetBodySizeBefore(expirationTimestamp);
        return size;
    }
}

ui64 TPartition::ReserveSize() const {
    return IsActive() ? TopicPartitionReserveSize(Config) : 0;
}

ui64 TPartition::StorageSize(const TActorContext&) const {
    return std::max<ui64>(UserDataSize(), ReserveSize());
}

ui64 TPartition::UsedReserveSize(const TActorContext&) const {
    return std::min<ui64>(UserDataSize(), ReserveSize());
}

ui64 TPartition::GetUsedStorage(const TInstant& now) {
    const auto duration = now - LastUsedStorageMeterTimestamp;
    LastUsedStorageMeterTimestamp = now;

    auto dataSize = MeteringDataSize(now);
    auto reservedSize = ReserveSize();
    auto size = dataSize > reservedSize ? dataSize - reservedSize : 0;
    return size * duration.MilliSeconds() / 1000 / 1_MB; // mb*seconds
}

ui64 TPartition::ImportantClientsMinOffset() const {
    ui64 minOffset = BlobEncoder.EndOffset;
    for (const auto& consumer : Config.GetConsumers()) {
        if (!consumer.GetImportant()) {
            continue;
        }

        const TUserInfo* userInfo = UsersInfoStorage->GetIfExists(consumer.GetName());
        ui64 curOffset = CompactionBlobEncoder.StartOffset;
        if (userInfo && userInfo->Offset >= 0) //-1 means no offset
            curOffset = userInfo->Offset;
        minOffset = Min<ui64>(minOffset, curOffset);
    }

    return minOffset;
}

TInstant TPartition::GetEndWriteTimestamp() const {
    return EndWriteTimestamp;
}

void TPartition::HandleWakeup(const TActorContext& ctx) {
    const auto now = ctx.Now();

    FilterDeadlinedWrites(ctx);

    ctx.Schedule(WAKE_TIMEOUT, new TEvents::TEvWakeup());
    ctx.Send(Tablet, new TEvPQ::TEvPartitionCounters(Partition, TabletCounters));

    ui64 usedStorage = GetUsedStorage(now);
    if (usedStorage > 0) {
        ctx.Send(Tablet, new TEvPQ::TEvMetering(EMeteringJson::UsedStorageV1, usedStorage));
    }

    if (ManageWriteTimestampEstimate || !IsActive()) {
        WriteTimestampEstimate = now;
    }

    ReportCounters(ctx);

    ProcessHasDataRequests(ctx);

    for (auto& userInfo : UsersInfoStorage->GetAll()) {
        userInfo.second.UpdateReadingTimeAndState(BlobEncoder.EndOffset, now);
        for (auto& avg : userInfo.second.AvgReadBytes) {
            avg.Update(now);
        }
    }
    WriteBufferIsFullCounter.UpdateWorkingTime(now);

    WriteLagMs.Update(0, now);

    for (auto& avg : AvgWriteBytes) {
        avg.Update(now);
    }
    for (auto& avg : AvgQuotaBytes) {
        avg.Update(now);
    }

    UpdateCompactionCounters();

    TryRunCompaction();
}

void TPartition::AddMetaKey(TEvKeyValue::TEvRequest* request) {
    //Set Start Offset
    auto write = request->Record.AddCmdWrite();
    TKeyPrefix ikey(TKeyPrefix::TypeMeta, Partition);

    NKikimrPQ::TPartitionMeta meta;
    //meta.SetStartOffset(CompactionBlobEncoder.StartOffset);
    //meta.SetEndOffset(Max(BlobEncoder.NewHead.GetNextOffset(), BlobEncoder.EndOffset));
    meta.SetSubDomainOutOfSpace(SubDomainOutOfSpace);
    meta.SetEndWriteTimestamp(PendingWriteTimestamp.MilliSeconds());

    if (IsSupportive()) {
        auto* counterData = meta.MutableCounterData();
        counterData->SetMessagesWrittenGrpc(MsgsWrittenGrpc.Value());
        counterData->SetMessagesWrittenTotal(MsgsWrittenTotal.Value());
        counterData->SetBytesWrittenGrpc(BytesWrittenGrpc.Value());
        counterData->SetBytesWrittenTotal(BytesWrittenTotal.Value());
        counterData->SetBytesWrittenUncompressed(BytesWrittenUncompressed.Value());
        for(const auto& v : MessageSize.GetValues()) {
            counterData->AddMessagesSizes(v);
        }
    }

    TString out;
    Y_PROTOBUF_SUPPRESS_NODISCARD meta.SerializeToString(&out);

    write->SetKey(ikey.Data(), ikey.Size());
    write->SetValue(out.c_str(), out.size());
    write->SetStorageChannel(NKikimrClient::TKeyValueRequest::INLINE);
}

bool TPartition::CleanUp(TEvKeyValue::TEvRequest* request, const TActorContext& ctx) {
    if (IsSupportive()) {
        return false;
    }

    bool haveChanges = CleanUpBlobs(request, ctx);

    PQ_LOG_T("Have " << request->Record.CmdDeleteRangeSize() << " items to delete old stuff");

    haveChanges |= SourceIdStorage.DropOldSourceIds(request, ctx.Now(), CompactionBlobEncoder.StartOffset, Partition,
                                                    Config.GetPartitionConfig());
    if (haveChanges) {
        SourceIdStorage.MarkOwnersForDeletedSourceId(Owners);
    }

    PQ_LOG_T("Have " << request->Record.CmdDeleteRangeSize() << " items to delete all stuff. "
            << "Delete command " << request->ToString());

    return haveChanges;
}

bool TPartition::CleanUpBlobs(TEvKeyValue::TEvRequest *request, const TActorContext& ctx) {
    if (CompactionBlobEncoder.StartOffset == BlobEncoder.EndOffset || CompactionBlobEncoder.DataKeysBody.size() <= 1) {
        return false;
    }
    if (Config.GetEnableCompactification()) {
        return false;
    }
    const auto& partConfig = Config.GetPartitionConfig();

    const TDuration lifetimeLimit{TDuration::Seconds(partConfig.GetLifetimeSeconds())};

    const bool hasStorageLimit = partConfig.HasStorageLimitBytes();
    const auto now = ctx.Now();
    const ui64 importantConsumerMinOffset = ImportantClientsMinOffset();

    bool hasDrop = false;
    while (CompactionBlobEncoder.DataKeysBody.size() > 1) {
        auto& nextKey = CompactionBlobEncoder.DataKeysBody[1].Key;
        if (importantConsumerMinOffset < nextKey.GetOffset()) {
            // The first message in the next blob was not read by an important consumer.
            // We also save the current blob, since not all messages from it could be read.
            break;
        }
        if (importantConsumerMinOffset == nextKey.GetOffset() && nextKey.GetPartNo() != 0) {
            // We save all the blobs that contain parts of the last message read by an important consumer.
            break;
        }

        const auto& firstKey = CompactionBlobEncoder.DataKeysBody.front();
        if (hasStorageLimit) {
            const auto bodySize = CompactionBlobEncoder.BodySize - firstKey.Size;
            if (bodySize < partConfig.GetStorageLimitBytes()) {
                break;
            }
        } else {
            if (now < firstKey.Timestamp + lifetimeLimit) {
                break;
            }
        }

        CompactionBlobEncoder.BodySize -= firstKey.Size;
        CompactionBlobEncoder.DataKeysBody.pop_front();

        if (!GapOffsets.empty() && nextKey.GetOffset() == GapOffsets.front().second) {
            GapSize -= GapOffsets.front().second - GapOffsets.front().first;
            GapOffsets.pop_front();
        }

        hasDrop = true;
    }

    Y_ABORT_UNLESS(!CompactionBlobEncoder.DataKeysBody.empty());

    if (!hasDrop) {
        return false;
    }

    const auto& lastKey = CompactionBlobEncoder.DataKeysBody.front().Key;

    CompactionBlobEncoder.StartOffset = lastKey.GetOffset();
    if (lastKey.GetPartNo() > 0) {
        ++CompactionBlobEncoder.StartOffset;
    }

    Y_UNUSED(request);

    return true;
}

void TPartition::Handle(TEvPQ::TEvMirrorerCounters::TPtr& ev, const TActorContext& /*ctx*/) {
    if (Mirrorer) {
        auto diff = ev->Get()->Counters.MakeDiffForAggr(Mirrorer->Baseline);
        TabletCounters.Populate(*diff.Get());
        ev->Get()->Counters.RememberCurrentStateAsBaseline(Mirrorer->Baseline);
    }
}

void TPartition::DestroyActor(const TActorContext& ctx)
{
    // Reply to all outstanding requests in order to destroy corresponding actors

    NPersQueue::NErrorCode::EErrorCode errorCode;
    TStringBuilder ss;

    if (IsSupportive()) {
        errorCode = NPersQueue::NErrorCode::ERROR;
        ss << "The transaction is completed";
    } else {
        errorCode = NPersQueue::NErrorCode::INITIALIZING;
        ss << "Tablet is restarting, topic '" << TopicName() << "'";
    }

    for (const auto& ev : WaitToChangeOwner) {
        ReplyError(ctx, ev->Cookie, errorCode, ss);
    }

    for (auto& w : PendingRequests) {
        ReplyError(ctx, w.GetCookie(), errorCode, ss);
        w.Span.EndError(static_cast<const TString&>(ss));
    }

    for (const auto& w : Responses) {
        ReplyError(ctx, w.GetCookie(), errorCode, TStringBuilder() << ss << " (WriteResponses)");
    }

    for (const auto& ri : ReadInfo) {
        ReplyError(ctx, ri.second.Destination, errorCode,
            TStringBuilder() << ss << " (ReadInfo) cookie " << ri.first);
    }

    if (Mirrorer) {
        Send(Mirrorer->Actor, new TEvents::TEvPoisonPill());
    }

    if (UsersInfoStorage.Defined()) {
        UsersInfoStorage->Clear(ctx);
    }

    Send(ReadQuotaTrackerActor, new TEvents::TEvPoisonPill());
    if (!IsSupportive()) {
        Send(WriteQuotaTrackerActor, new TEvents::TEvPoisonPill());
    }

    if (OffloadActor) {
        Send(OffloadActor, new TEvents::TEvPoisonPill());
    }

    Die(ctx);
}

void TPartition::Handle(TEvents::TEvPoisonPill::TPtr&, const TActorContext& ctx)
{
    DestroyActor(ctx);
}

bool CheckDiskStatus(const TStorageStatusFlags status) {
    return !status.Check(NKikimrBlobStorage::StatusDiskSpaceYellowStop);
}

void TPartition::InitComplete(const TActorContext& ctx) {
    if (BlobEncoder.StartOffset == BlobEncoder.EndOffset && BlobEncoder.EndOffset == 0) {
        for (auto& [user, info] : UsersInfoStorage->GetAll()) {
            if (info.Offset > 0 && BlobEncoder.StartOffset < (ui64)info.Offset) {
                 BlobEncoder.Head.Offset = BlobEncoder.EndOffset = BlobEncoder.StartOffset = info.Offset;
            }
        }
    }

    PQ_LOG_I("init complete for topic '" << TopicName() << "' partition " << Partition << " generation " << TabletGeneration << " " << ctx.SelfID);

    TStringBuilder ss;
    ss << "SYNC INIT topic " << TopicName() << " partitition " << Partition
       << " so " << BlobEncoder.StartOffset << " endOffset " << BlobEncoder.EndOffset << " Head " << BlobEncoder.Head << "\n";
    for (const auto& s : SourceIdStorage.GetInMemorySourceIds()) {
        ss << "SYNC INIT sourceId " << s.first << " seqNo " << s.second.SeqNo << " offset " << s.second.Offset << "\n";
    }
    for (const auto& h : BlobEncoder.DataKeysBody) {
        ss << "SYNC INIT DATA KEY: " << h.Key.ToString() << " size " << h.Size << "\n";
    }
    for (const auto& h : BlobEncoder.HeadKeys) {
        ss << "SYNC INIT HEAD KEY: " << h.Key.ToString() << " size " << h.Size << "\n";
    }
    PQ_LOG_D(ss);

    CompactionBlobEncoder.CheckHeadConsistency(CompactLevelBorder, TotalLevels, TotalMaxCount);

    Become(&TThis::StateIdle);
    InitDuration = ctx.Now() - CreationTime;
    InitDone = true;
    TabletCounters.Percentile()[COUNTER_LATENCY_PQ_INIT].IncrementFor(InitDuration.MilliSeconds());

    FillReadFromTimestamps(ctx);
    ProcessPendingEvents(ctx);
    ProcessTxsAndUserActs(ctx);

    ctx.Send(ctx.SelfID, new TEvents::TEvWakeup());
    ctx.Send(Tablet, new TEvPQ::TEvInitComplete(Partition));

    for (const auto& s : SourceIdStorage.GetInMemorySourceIds()) {
        PQ_LOG_D("Init complete for topic '" << TopicName() << "' Partition: " << Partition
                    << " SourceId: " << s.first << " SeqNo: " << s.second.SeqNo << " offset: " << s.second.Offset
                    << " MaxOffset: " << BlobEncoder.EndOffset
        );
    }
    ProcessHasDataRequests(ctx);

    InitUserInfoForImportantClients(ctx);

    for (auto& userInfoPair : UsersInfoStorage->GetAll()) {
        Y_ABORT_UNLESS(userInfoPair.second.Offset >= 0);
        ReadTimestampForOffset(userInfoPair.first, userInfoPair.second, ctx);
    }
    if (PartitionCountersLabeled) {
        PartitionCountersLabeled->GetCounters()[METRIC_INIT_TIME] = InitDuration.MilliSeconds();
        PartitionCountersLabeled->GetCounters()[METRIC_LIFE_TIME] = CreationTime.MilliSeconds();
        PartitionCountersLabeled->GetCounters()[METRIC_PARTITIONS] = 1;
        PartitionCountersLabeled->GetCounters()[METRIC_PARTITIONS_TOTAL] = Config.PartitionsSize();
        ctx.Send(Tablet, new TEvPQ::TEvPartitionLabeledCounters(Partition, *PartitionCountersLabeled));
    }
    UpdateUserInfoEndOffset(ctx.Now());

    ScheduleUpdateAvailableSize(ctx);

    if (MirroringEnabled(Config)) {
        CreateMirrorerActor();
    }

    ReportCounters(ctx, true);
}


void TPartition::UpdateUserInfoEndOffset(const TInstant& now) {
    for (auto& userInfo : UsersInfoStorage->GetAll()) {
        userInfo.second.UpdateReadingTimeAndState(BlobEncoder.EndOffset, now);
    }
}

void TPartition::Handle(TEvPQ::TEvChangePartitionConfig::TPtr& ev, const TActorContext& ctx) {
    PushBackDistrTx(ev->Release());

    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(TEvPQ::TEvPipeDisconnected::TPtr& ev, const TActorContext& ctx) {
    const TString& owner = ev->Get()->Owner;
    const TActorId& pipeClient = ev->Get()->PipeClient;

    OwnerPipes.erase(pipeClient);

    auto it = Owners.find(owner);
    if (it == Owners.end() || it->second.PipeClient != pipeClient) // owner session is already dead
        return;
    //TODO: Uncommet when writes will be done via new gRPC protocol
    // msgbus do not reserve bytes right now!!
    // DropOwner will drop reserved bytes and ownership
    if (owner != "default") { //default owner is for old LB protocol, pipe is dead right now after GetOwnership request, and no ReserveBytes done. So, ignore pipe disconnection
        DropOwner(it, ctx);
        ProcessChangeOwnerRequests(ctx);
    }

}

TConsumerSnapshot TPartition::CreateSnapshot(TUserInfo& userInfo) const {
    auto now = TAppData::TimeProvider->Now();

    userInfo.UpdateReadingTimeAndState(BlobEncoder.EndOffset, now);

    TConsumerSnapshot result;
    result.Now = now;

    if (userInfo.Offset >= static_cast<i64>(BlobEncoder.EndOffset)) {
        result.LastCommittedMessage.CreateTimestamp = now;
        result.LastCommittedMessage.WriteTimestamp = now;
    } else if (userInfo.ActualTimestamps) {
        result.LastCommittedMessage.CreateTimestamp = userInfo.CreateTimestamp;
        result.LastCommittedMessage.WriteTimestamp = userInfo.WriteTimestamp;
    } else {
        auto timestamp = GetWriteTimeEstimate(userInfo.Offset);
        result.LastCommittedMessage.CreateTimestamp = timestamp;
        result.LastCommittedMessage.WriteTimestamp = timestamp;
    }

    auto readOffset = userInfo.GetReadOffset();

    result.ReadOffset = readOffset;
    result.LastReadTimestamp = userInfo.ReadTimestamp;

    if (readOffset >= static_cast<i64>(BlobEncoder.EndOffset)) {
        result.LastReadMessage.CreateTimestamp = now;
        result.LastReadMessage.WriteTimestamp = now;
    } else if (userInfo.ReadOffset == -1) {
        result.LastReadMessage = result.LastCommittedMessage;
    } else if (userInfo.ReadWriteTimestamp) {
        result.LastReadMessage.CreateTimestamp = userInfo.ReadCreateTimestamp;
        result.LastReadMessage.WriteTimestamp = userInfo.ReadWriteTimestamp;
    } else {
        auto timestamp = GetWriteTimeEstimate(readOffset);
        result.LastCommittedMessage.CreateTimestamp = timestamp;
        result.LastCommittedMessage.WriteTimestamp = timestamp;
    }

    if (readOffset < (i64)BlobEncoder.EndOffset) {
        result.ReadLag = result.LastReadTimestamp - result.LastReadMessage.WriteTimestamp;
    }
    result.CommitedLag = result.LastCommittedMessage.WriteTimestamp - now;
    result.TotalLag = TDuration::MilliSeconds(userInfo.GetWriteLagMs()) + result.ReadLag + (now - result.LastReadTimestamp);

    return result;
}

void TPartition::Handle(TEvPQ::TEvPartitionStatus::TPtr& ev, const TActorContext& ctx) {
    const auto now = ctx.Now();

    NKikimrPQ::TStatusResponse::TPartResult result;
    result.SetPartition(Partition.InternalPartitionId);
    result.SetGeneration(TabletGeneration);
    result.SetCookie(++PQRBCookie);

    if (DiskIsFull || WaitingForSubDomainQuota(ctx)) {
        result.SetStatus(NKikimrPQ::TStatusResponse::STATUS_DISK_IS_FULL);
    } else if (BlobEncoder.EndOffset - BlobEncoder.StartOffset >= static_cast<ui64>(Config.GetPartitionConfig().GetMaxCountInPartition()) ||
               Size() >= static_cast<ui64>(Config.GetPartitionConfig().GetMaxSizeInPartition())) {
        result.SetStatus(NKikimrPQ::TStatusResponse::STATUS_PARTITION_IS_FULL);
    } else {
        result.SetStatus(NKikimrPQ::TStatusResponse::STATUS_OK);
    }
    result.SetLastInitDurationSeconds(InitDuration.Seconds());
    result.SetCreationTimestamp(CreationTime.Seconds());
    ui64 headGapSize = BlobEncoder.GetHeadGapSize();
    ui32 gapsCount = GapOffsets.size() + (headGapSize ? 1 : 0);
    result.SetGapCount(gapsCount);
    result.SetGapSize(headGapSize + GapSize);

    Y_ABORT_UNLESS(AvgWriteBytes.size() == 4);
    result.SetAvgWriteSpeedPerSec(AvgWriteBytes[0].GetValue());
    result.SetAvgWriteSpeedPerMin(AvgWriteBytes[1].GetValue());
    result.SetAvgWriteSpeedPerHour(AvgWriteBytes[2].GetValue());
    result.SetAvgWriteSpeedPerDay(AvgWriteBytes[3].GetValue());

    Y_ABORT_UNLESS(AvgQuotaBytes.size() == 4);
    result.SetAvgQuotaSpeedPerSec(AvgQuotaBytes[0].GetValue());
    result.SetAvgQuotaSpeedPerMin(AvgQuotaBytes[1].GetValue());
    result.SetAvgQuotaSpeedPerHour(AvgQuotaBytes[2].GetValue());
    result.SetAvgQuotaSpeedPerDay(AvgQuotaBytes[3].GetValue());

    result.SetSourceIdCount(SourceIdStorage.GetInMemorySourceIds().size());
    result.SetSourceIdRetentionPeriodSec((now - SourceIdStorage.MinAvailableTimestamp(now)).Seconds());

    result.SetWriteBytesQuota(TotalPartitionWriteSpeed);

    TVector<ui64> resSpeed;
    resSpeed.resize(4);
    ui64 maxQuota = 0;
    bool filterConsumers = !ev->Get()->Consumers.empty();
    TSet<TString> requiredConsumers(ev->Get()->Consumers.begin(), ev->Get()->Consumers.end());
    for (auto& userInfoPair : UsersInfoStorage->GetAll()) {
        auto& userInfo = userInfoPair.second;
        auto& clientId = ev->Get()->ClientId;
        bool consumerShouldBeProcessed = filterConsumers
            ? requiredConsumers.contains(userInfo.User)
            : clientId.empty() || clientId == userInfo.User;
        if (consumerShouldBeProcessed) {
            Y_ABORT_UNLESS(userInfo.AvgReadBytes.size() == 4);
            for (ui32 i = 0; i < 4; ++i) {
                resSpeed[i] += userInfo.AvgReadBytes[i].GetValue();
            }
            maxQuota += userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_BYTES].Get();
        }
        if (filterConsumers) {
            if (requiredConsumers.contains(userInfo.User)) {
                auto* clientInfo = result.AddConsumerResult();
                clientInfo->SetConsumer(userInfo.User);
                clientInfo->set_errorcode(NPersQueue::NErrorCode::EErrorCode::OK);
                clientInfo->SetCommitedOffset(userInfo.Offset);
                if (userInfo.CommittedMetadata.has_value()) {
                    clientInfo->SetCommittedMetadata(*userInfo.CommittedMetadata);
                }
                requiredConsumers.extract(userInfo.User);
            }
            continue;
        }

        if (clientId == userInfo.User) { //fill lags
            NKikimrPQ::TClientInfo* clientInfo = result.MutableLagsInfo();
            clientInfo->SetClientId(userInfo.User);

            auto snapshot = CreateSnapshot(userInfo);

            auto write = clientInfo->MutableWritePosition();
            write->SetOffset(userInfo.Offset);
            write->SetWriteTimestamp(snapshot.LastCommittedMessage.WriteTimestamp.MilliSeconds());
            write->SetCreateTimestamp(snapshot.LastCommittedMessage.CreateTimestamp.MilliSeconds());
            write->SetSize(GetSizeLag(userInfo.Offset));

            auto readOffset = userInfo.GetReadOffset();

            auto read = clientInfo->MutableReadPosition();
            read->SetOffset(readOffset);
            read->SetWriteTimestamp(snapshot.LastReadMessage.WriteTimestamp.MilliSeconds());
            read->SetCreateTimestamp(snapshot.LastReadMessage.CreateTimestamp.MilliSeconds());
            read->SetSize(GetSizeLag(readOffset));

            clientInfo->SetLastReadTimestampMs(snapshot.LastReadTimestamp.MilliSeconds());
            clientInfo->SetCommitedLagMs(snapshot.CommitedLag.MilliSeconds());
            if (IsActive() || readOffset < (i64)BlobEncoder.EndOffset) {
                clientInfo->SetReadLagMs(snapshot.ReadLag.MilliSeconds());
                clientInfo->SetWriteLagMs(userInfo.GetWriteLagMs());
                clientInfo->SetTotalLagMs(snapshot.TotalLag.MilliSeconds());
            } else {
                clientInfo->SetReadLagMs(0);
                clientInfo->SetWriteLagMs(0);
                clientInfo->SetTotalLagMs(0);
            }
        }

        if (ev->Get()->GetStatForAllConsumers) { //fill lags
            auto snapshot = CreateSnapshot(userInfo);

            auto* clientInfo = result.AddConsumerResult();
            clientInfo->SetConsumer(userInfo.User);
            clientInfo->SetLastReadTimestampMs(userInfo.GetReadTimestamp().MilliSeconds());
            clientInfo->SetCommitedLagMs(snapshot.CommitedLag.MilliSeconds());

            auto readOffset = userInfo.GetReadOffset();
            if (IsActive() || readOffset < (i64)BlobEncoder.EndOffset) {
                clientInfo->SetReadLagMs(snapshot.ReadLag.MilliSeconds());
                clientInfo->SetWriteLagMs(userInfo.GetWriteLagMs());
            } else {
                clientInfo->SetReadLagMs(0);
                clientInfo->SetWriteLagMs(0);
            }

            clientInfo->SetAvgReadSpeedPerMin(userInfo.AvgReadBytes[1].GetValue());
            clientInfo->SetAvgReadSpeedPerHour(userInfo.AvgReadBytes[2].GetValue());
            clientInfo->SetAvgReadSpeedPerDay(userInfo.AvgReadBytes[3].GetValue());

            clientInfo->SetReadingFinished(LastOffsetHasBeenCommited(userInfo));
        }
    }

    result.SetStartOffset(BlobEncoder.StartOffset);
    result.SetEndOffset(BlobEncoder.EndOffset);

    if (filterConsumers) {
        for (TString consumer : requiredConsumers) {
            auto* clientInfo = result.AddConsumerResult();
            clientInfo->SetConsumer(consumer);
            clientInfo->set_errorcode(NPersQueue::NErrorCode::EErrorCode::SCHEMA_ERROR);
        }
    } else {
        result.SetAvgReadSpeedPerSec(resSpeed[0]);
        result.SetAvgReadSpeedPerMin(resSpeed[1]);
        result.SetAvgReadSpeedPerHour(resSpeed[2]);
        result.SetAvgReadSpeedPerDay(resSpeed[3]);

        result.SetReadBytesQuota(maxQuota);

        result.SetPartitionSize(UserDataSize());
        result.SetUsedReserveSize(UsedReserveSize(ctx));

        result.SetLastWriteTimestampMs(WriteTimestamp.MilliSeconds());
        result.SetWriteLagMs(WriteLagMs.GetValue());

        *result.MutableErrors() = {Errors.begin(), Errors.end()};

        PQ_LOG_D("Topic PartitionStatus PartitionSize: " << result.GetPartitionSize()
                    << " UsedReserveSize: " << result.GetUsedReserveSize()
                    << " ReserveSize: " << ReserveSize()
                    << " PartitionConfig" << Config.GetPartitionConfig();
        );
    }

    UpdateCounters(ctx);
    if (PartitionCountersLabeled) {
        auto* ac = result.MutableAggregatedCounters();
        for (ui32 i = 0; i < PartitionCountersLabeled->GetCounters().Size(); ++i) {
            ac->AddValues(PartitionCountersLabeled->GetCounters()[i].Get());
        }
        for (auto& userInfoPair : UsersInfoStorage->GetAll()) {
            auto& userInfo = userInfoPair.second;
            if (!userInfo.LabeledCounters)
                continue;
            if (userInfoPair.first != CLIENTID_WITHOUT_CONSUMER && !userInfo.HasReadRule && !userInfo.Important)
                continue;
            auto* cac = ac->AddConsumerAggregatedCounters();
            cac->SetConsumer(userInfo.User);
            for (ui32 i = 0; i < userInfo.LabeledCounters->GetCounters().Size(); ++i) {
                cac->AddValues(userInfo.LabeledCounters->GetCounters()[i].Get());
            }
        }
    }
    result.SetScaleStatus(SplitMergeEnabled(TabletConfig) ? ScaleStatus : NKikimrPQ::EScaleStatus::NORMAL);
    ctx.Send(ev->Get()->Sender, new TEvPQ::TEvPartitionStatusResponse(result, Partition));
}

void TPartition::HandleOnInit(TEvPQ::TEvPartitionStatus::TPtr& ev, const TActorContext& ctx) {
    NKikimrPQ::TStatusResponse::TPartResult result;
    result.SetPartition(Partition.InternalPartitionId);
    result.SetStatus(NKikimrPQ::TStatusResponse::STATUS_INITIALIZING);
    result.SetLastInitDurationSeconds((ctx.Now() - CreationTime).Seconds());
    result.SetCreationTimestamp(CreationTime.Seconds());
    ctx.Send(ev->Get()->Sender, new TEvPQ::TEvPartitionStatusResponse(result, Partition));
}


void TPartition::Handle(TEvPQ::TEvGetPartitionClientInfo::TPtr& ev, const TActorContext& ctx) {
    THolder<TEvPersQueue::TEvPartitionClientInfoResponse> response = MakeHolder<TEvPersQueue::TEvPartitionClientInfoResponse>();
    NKikimrPQ::TClientInfoResponse& result(response->Record);
    result.SetPartition(Partition.InternalPartitionId);
    result.SetStartOffset(BlobEncoder.StartOffset);
    result.SetEndOffset(BlobEncoder.EndOffset);
    result.SetResponseTimestamp(ctx.Now().MilliSeconds());
    for (auto& pr : UsersInfoStorage->GetAll()) {
        auto snapshot = CreateSnapshot(pr.second);

        TUserInfo& userInfo(pr.second);
        NKikimrPQ::TClientInfo& clientInfo = *result.AddClientInfo();
        clientInfo.SetClientId(pr.first);

        auto& write = *clientInfo.MutableWritePosition();
        write.SetOffset(userInfo.Offset);
        write.SetWriteTimestamp(snapshot.LastCommittedMessage.WriteTimestamp.MilliSeconds());
        write.SetCreateTimestamp(snapshot.LastCommittedMessage.CreateTimestamp.MilliSeconds());
        write.SetSize(GetSizeLag(userInfo.Offset));

        auto& read = *clientInfo.MutableReadPosition();
        read.SetOffset(userInfo.GetReadOffset());
        read.SetWriteTimestamp(snapshot.LastReadMessage.WriteTimestamp.MilliSeconds());
        read.SetCreateTimestamp(snapshot.LastReadMessage.CreateTimestamp.MilliSeconds());
        read.SetSize(GetSizeLag(userInfo.GetReadOffset()));
    }
    ctx.Send(ev->Get()->Sender, response.Release(), 0, ev->Cookie);
}

void TPartition::Handle(TEvPersQueue::TEvReportPartitionError::TPtr& ev, const TActorContext& ctx) {
    LogAndCollectError(ev->Get()->Record, ctx);
}

void TPartition::LogAndCollectError(const NKikimrPQ::TStatusResponse::TErrorMessage& error, const TActorContext& ctx) {
    if (Errors.size() == MAX_ERRORS_COUNT_TO_STORE) {
        Errors.pop_front();
    }
    Errors.push_back(error);
    LOG_ERROR_S(ctx, error.GetService(), error.GetMessage());
}

void TPartition::LogAndCollectError(NKikimrServices::EServiceKikimr service, const TString& msg, const TActorContext& ctx) {
    NKikimrPQ::TStatusResponse::TErrorMessage error;
    error.SetTimestamp(ctx.Now().Seconds());
    error.SetService(service);
    error.SetMessage(TStringBuilder() << "topic '" << TopicName() << "' partition " << Partition << " got error: " << msg);
    LogAndCollectError(error, ctx);
}

const TPartitionBlobEncoder& TPartition::GetBlobEncoder(ui64 offset) const
{
    if (BlobEncoder.DataKeysBody.empty()) {
        return CompactionBlobEncoder;
    }

    const auto required = std::make_tuple(offset, 0);
    const auto& key = BlobEncoder.DataKeysBody.front().Key;
    const auto fastWriteStart = std::make_tuple(key.GetOffset(), key.GetPartNo());

    if (required < fastWriteStart) {
        return CompactionBlobEncoder;
    }

    return BlobEncoder;
}

const std::deque<TDataKey>& GetContainer(const TPartitionBlobEncoder& zone, ui64 offset)
{
    return zone.PositionInBody(offset, 0) ? zone.DataKeysBody : zone.HeadKeys;
}

//zero means no such record
TInstant TPartition::GetWriteTimeEstimate(ui64 offset) const {
    if (offset < CompactionBlobEncoder.StartOffset) {
        offset = CompactionBlobEncoder.StartOffset;
    }
    if (offset >= BlobEncoder.EndOffset) {
        return TInstant::Zero();
    }

    const std::deque<TDataKey>& container =
        GetContainer(GetBlobEncoder(offset), offset);
    Y_ABORT_UNLESS(!container.empty());

    auto it = std::upper_bound(container.begin(), container.end(), offset,
                    [](const ui64 offset, const TDataKey& p) {
                        return offset < p.Key.GetOffset() ||
                                        offset == p.Key.GetOffset() && p.Key.GetPartNo() > 0;
                    });
    // Always greater
    Y_ABORT_UNLESS(it != container.begin(),
             "Tablet %lu StartOffset %lu, HeadOffset %lu, offset %lu, containter size %lu, first-elem: %s",
             TabletID, BlobEncoder.StartOffset, BlobEncoder.Head.Offset, offset, container.size(),
             container.front().Key.ToString().data());
    Y_ABORT_UNLESS(it == container.end() ||
                   offset < it->Key.GetOffset() ||
                   it->Key.GetOffset() == offset && it->Key.GetPartNo() > 0);

    --it;
    if (it != container.begin())
        --it;

    return it->Timestamp;
}


void TPartition::Handle(TEvPQ::TEvUpdateWriteTimestamp::TPtr& ev, const TActorContext& ctx) {
    TInstant timestamp = TInstant::MilliSeconds(ev->Get()->WriteTimestamp);
    if (WriteTimestampEstimate > timestamp) {
        ReplyError(ctx, ev->Get()->Cookie, NPersQueue::NErrorCode::BAD_REQUEST,
            TStringBuilder() << "too big timestamp: " << timestamp << " known " << WriteTimestampEstimate);
        return;
    }
    WriteTimestampEstimate = timestamp;
    ReplyOk(ctx, ev->Get()->Cookie);
}

void TPartition::Handle(TEvPersQueue::TEvProposeTransaction::TPtr& ev, const TActorContext& ctx)
{
    const NKikimrPQ::TEvProposeTransaction& event = ev->Get()->GetRecord();
    Y_ABORT_UNLESS(event.GetTxBodyCase() == NKikimrPQ::TEvProposeTransaction::kData);
    Y_ABORT_UNLESS(event.HasData());
    const NKikimrPQ::TDataTransaction& txBody = event.GetData();

    if (!txBody.GetImmediate()) {
        ReplyPropose(ctx,
                     event,
                     NKikimrPQ::TEvProposeTransactionResult::ABORTED,
                     NKikimrPQ::TError::INTERNAL,
                     "immediate transaction is expected");
        return;
    }

    if (ImmediateTxCount >= MAX_TXS) {
        ReplyPropose(ctx,
                     event,
                     NKikimrPQ::TEvProposeTransactionResult::OVERLOADED,
                     NKikimrPQ::TError::INTERNAL,
                     "the allowed number of transactions has been exceeded");
        return;
    }
    AddImmediateTx(ev->Release());
    ProcessTxsAndUserActs(ctx);
}

template <class T>
void TPartition::ProcessPendingEvent(TAutoPtr<TEventHandle<T>>& ev, const TActorContext& ctx)
{
    if (PendingEvents.empty()) {
        // Optimization: if the queue is empty, you can process the message immediately
        ProcessPendingEvent(std::unique_ptr<T>(ev->Release().Release()), ctx);
    } else {
        // We need to keep the order in which the messages arrived
        AddPendingEvent(ev);
        ProcessPendingEvents(ctx);
    }
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvProposePartitionConfig> ev, const TActorContext& ctx)
{
    PushBackDistrTx(ev.release());

    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(TEvPQ::TEvProposePartitionConfig::TPtr& ev, const TActorContext& ctx)
{
    PQ_LOG_D("Handle TEvPQ::TEvProposePartitionConfig" <<
             " Step " << ev->Get()->Step <<
             ", TxId " << ev->Get()->TxId);

    ProcessPendingEvent(ev, ctx);
}

template <class T>
void TPartition::AddPendingEvent(TAutoPtr<TEventHandle<T>>& ev)
{
    std::unique_ptr<T> p(ev->Release().Release());
    PendingEvents.emplace_back(std::move(p));
}

void TPartition::HandleOnInit(TEvPQ::TEvTxCalcPredicate::TPtr& ev, const TActorContext&)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvTxCalcPredicate");

    AddPendingEvent(ev);
}

void TPartition::HandleOnInit(TEvPQ::TEvTxCommit::TPtr& ev, const TActorContext&)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvTxCommit");

    AddPendingEvent(ev);
}

void TPartition::HandleOnInit(TEvPQ::TEvTxRollback::TPtr& ev, const TActorContext&)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvTxRollback");

    AddPendingEvent(ev);
}

void TPartition::HandleOnInit(TEvPQ::TEvProposePartitionConfig::TPtr& ev, const TActorContext&)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvProposePartitionConfig");

    AddPendingEvent(ev);
}

void TPartition::HandleOnInit(TEvPQ::TEvGetWriteInfoRequest::TPtr& ev, const TActorContext& /* ctx */)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvGetWriteInfoRequest");

    Y_ABORT_UNLESS(IsSupportive());

    ev->Get()->OriginalPartition = ev->Sender;
    AddPendingEvent(ev);
}

void TPartition::HandleOnInit(TEvPQ::TEvGetWriteInfoResponse::TPtr& ev, const TActorContext& /* ctx */)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvGetWriteInfoResponse");

    Y_ABORT_UNLESS(!IsSupportive());

    AddPendingEvent(ev);
}

void TPartition::HandleOnInit(TEvPQ::TEvGetWriteInfoError::TPtr& ev, const TActorContext& /* ctx */)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvGetWriteInfoError");

    Y_ABORT_UNLESS(!IsSupportive());

    AddPendingEvent(ev);
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvTxCalcPredicate> ev, const TActorContext& ctx)
{
    if (PlanStep.Defined() && TxId.Defined()) {
        if (GetStepAndTxId(*ev) < GetStepAndTxId(*PlanStep, *TxId)) {
            Send(Tablet,
                 MakeHolder<TEvPQ::TEvTxCalcPredicateResult>(ev->Step,
                                                             ev->TxId,
                                                             Partition,
                                                             Nothing()).Release());
            return;
        }
    }

    PushBackDistrTx(ev.release());

    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(TEvPQ::TEvTxCalcPredicate::TPtr& ev, const TActorContext& ctx)
{
    PQ_LOG_D("Handle TEvPQ::TEvTxCalcPredicate" <<
             " Step " << ev->Get()->Step <<
             ", TxId " << ev->Get()->TxId);

    ProcessPendingEvent(ev, ctx);
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvTxCommit> ev, const TActorContext& ctx)
{
    if (PlanStep.Defined() && TxId.Defined()) {
        if (GetStepAndTxId(*ev) < GetStepAndTxId(*PlanStep, *TxId)) {
            PQ_LOG_D("Send TEvTxCommitDone" <<
                     " Step " << ev->Step <<
                     ", TxId " << ev->TxId);
            ctx.Send(Tablet, MakeCommitDone(ev->Step, ev->TxId).Release());
            return;
        }
    }

    auto txIter = TransactionsInflight.begin();
    if (ChangeConfig) {
        Y_ABORT_UNLESS(TransactionsInflight.size() == 1,
                       "PQ: %" PRIu64 ", Partition: %" PRIu32 ", Step: %" PRIu64 ", TxId: %" PRIu64,
                       TabletID, Partition.OriginalPartitionId,
                       ev->Step, ev->TxId);
        PendingExplicitMessageGroups = ev->ExplicitMessageGroups;
    } else {
        Y_ABORT_UNLESS(!TransactionsInflight.empty(),
                       "PQ: %" PRIu64 ", Partition: %" PRIu32 ", Step: %" PRIu64 ", TxId: %" PRIu64,
                       TabletID, Partition.OriginalPartitionId,
                       ev->Step, ev->TxId);
        txIter = TransactionsInflight.find(ev->TxId);
        Y_ABORT_UNLESS(!txIter.IsEnd(),
                       "PQ: %" PRIu64 ", Partition: %" PRIu32 ", Step: %" PRIu64 ", TxId: %" PRIu64,
                       TabletID, Partition.OriginalPartitionId,
                       ev->Step, ev->TxId);
    }
    Y_ABORT_UNLESS(txIter->second->State == ECommitState::Pending);

    txIter->second->State = ECommitState::Committed;
    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(TEvPQ::TEvTxCommit::TPtr& ev, const TActorContext& ctx)
{
    PQ_LOG_D("Handle TEvPQ::TEvTxCommit" <<
             " Step " << ev->Get()->Step <<
             ", TxId " << ev->Get()->TxId);

    ProcessPendingEvent(ev, ctx);
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvTxRollback> ev, const TActorContext& ctx)
{
    if (PlanStep.Defined() && TxId.Defined()) {
        if (GetStepAndTxId(*ev) < GetStepAndTxId(*PlanStep, *TxId)) {
            PQ_LOG_D("Rollback for" <<
                     " Step " << ev->Step <<
                     ", TxId " << ev->TxId);
            return;
        }
    }

    auto txIter = TransactionsInflight.begin();
    if (ChangeConfig) {
        Y_ABORT_UNLESS(TransactionsInflight.size() == 1,
                       "PQ: %" PRIu64 ", Partition: %" PRIu32,
                       TabletID, Partition.OriginalPartitionId);
    } else {
        Y_ABORT_UNLESS(!TransactionsInflight.empty(),
                       "PQ: %" PRIu64 ", Partition: %" PRIu32,
                       TabletID, Partition.OriginalPartitionId);
        txIter = TransactionsInflight.find(ev->TxId);
        Y_ABORT_UNLESS(!txIter.IsEnd(),
                       "PQ: %" PRIu64 ", Partition: %" PRIu32,
                       TabletID, Partition.OriginalPartitionId);
    }
    Y_ABORT_UNLESS(txIter->second->State == ECommitState::Pending);

    txIter->second->State = ECommitState::Aborted;
    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(TEvPQ::TEvTxRollback::TPtr& ev, const TActorContext& ctx)
{
    ProcessPendingEvent(ev, ctx);
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvGetWriteInfoRequest> ev, const TActorContext& ctx)
{
    TActorId originalPartition = ev->OriginalPartition;
    Y_ABORT_UNLESS(originalPartition != TActorId());

    if (ClosedInternalPartition || WaitingForPreviousBlobQuota() || (CurrentStateFunc() != &TThis::StateIdle)) {
        PQ_LOG_D("Send TEvPQ::TEvGetWriteInfoError");
        auto* response = new TEvPQ::TEvGetWriteInfoError(Partition.InternalPartitionId,
                                                         "Write info requested while writes are not complete");
        ctx.Send(originalPartition, response);
        ClosedInternalPartition = true;
        return;
    }
    ClosedInternalPartition = true;
    auto response = new TEvPQ::TEvGetWriteInfoResponse();
    response->Cookie = Partition.InternalPartitionId;
    response->BodyKeys = std::move(CompactionBlobEncoder.DataKeysBody);
    std::move(CompactionBlobEncoder.HeadKeys.begin(), CompactionBlobEncoder.HeadKeys.end(),
              std::back_inserter(response->BodyKeys));
    std::move(BlobEncoder.DataKeysBody.begin(), BlobEncoder.DataKeysBody.end(),
              std::back_inserter(response->BodyKeys));
    response->SrcIdInfo = std::move(SourceIdStorage.ExtractInMemorySourceIds());

    response->BytesWrittenGrpc = BytesWrittenGrpc.Value();
    response->BytesWrittenUncompressed = BytesWrittenUncompressed.Value();
    response->BytesWrittenTotal = BytesWrittenTotal.Value();
    response->MessagesWrittenTotal = MsgsWrittenTotal.Value();
    response->MessagesWrittenGrpc = MsgsWrittenGrpc.Value();
    response->MessagesSizes = std::move(MessageSize.GetValues());
    response->InputLags = std::move(SupportivePartitionTimeLag);

    PQ_LOG_D("Send TEvPQ::TEvGetWriteInfoResponse");
    ctx.Send(originalPartition, response);
}

void TPartition::Handle(TEvPQ::TEvGetWriteInfoRequest::TPtr& ev, const TActorContext& ctx) {
    PQ_LOG_D("Handle TEvPQ::TEvGetWriteInfoRequest");

    ev->Get()->OriginalPartition = ev->Sender;

    ProcessPendingEvent(ev, ctx);
}

void TPartition::WriteInfoResponseHandler(
        const TActorId& sender,
        TGetWriteInfoResp&& ev,
        const TActorContext& ctx
) {
    auto txIter = WriteInfosToTx.find(sender);
    Y_ABORT_UNLESS(!txIter.IsEnd());

    auto& tx = (*txIter->second);

    std::visit(TOverloaded{
        [&tx](TAutoPtr<TEvPQ::TEvGetWriteInfoResponse>& msg) {
            tx.WriteInfo.Reset(msg.Release());
        },
        [&tx](TAutoPtr<TEvPQ::TEvGetWriteInfoError>& err) {
            tx.Predicate = false;
            tx.WriteInfoApplied = true;
            tx.Message = err->Message;
        }
    }, ev);

    WriteInfosToTx.erase(txIter);
    ProcessTxsAndUserActs(ctx);
}

TPartition::EProcessResult TPartition::ApplyWriteInfoResponse(TTransaction& tx) {
    bool isImmediate = (tx.ProposeTransaction != nullptr);
    Y_ABORT_UNLESS(tx.WriteInfo);
    Y_ABORT_UNLESS(!tx.WriteInfoApplied);
    if (!tx.Predicate.GetOrElse(true)) {
        return EProcessResult::Continue;
    }

    if (!CanWrite()) {
        tx.Predicate = false;
        tx.Message = TStringBuilder() << "Partition " << Partition << " is inactive. Writing is not possible";
        tx.WriteInfoApplied = true;
        return EProcessResult::Continue;
    }

    auto& srcIdInfo = tx.WriteInfo->SrcIdInfo;

    EProcessResult ret = EProcessResult::Continue;
    const auto& knownSourceIds = SourceIdStorage.GetInMemorySourceIds();
    THashSet<TString> txSourceIds;
    for (auto& s : srcIdInfo) {
        if (TxAffectedSourcesIds.contains(s.first)) {
            ret = EProcessResult::Blocked;
            break;
        }
        if (isImmediate) {
            WriteAffectedSourcesIds.insert(s.first);
        } else {
            if (WriteAffectedSourcesIds.contains(s.first)) {
                ret = EProcessResult::Blocked;
                break;
            }
            txSourceIds.insert(s.first);
        }

        if (auto inFlightIter = TxInflightMaxSeqNoPerSourceId.find(s.first); !inFlightIter.IsEnd()) {
            if (SeqnoViolation(inFlightIter->second.KafkaProducerEpoch, inFlightIter->second.SeqNo, s.second.ProducerEpoch, s.second.MinSeqNo)) {
                tx.Predicate = false;
                tx.Message = TStringBuilder() << "MinSeqNo violation failure on " << s.first;
                tx.WriteInfoApplied = true;
                break;
            }
        }

        if (auto existing = knownSourceIds.find(s.first); !existing.IsEnd()) {
            if (SeqnoViolation(existing->second.ProducerEpoch, existing->second.SeqNo, s.second.ProducerEpoch, s.second.MinSeqNo)) {
                tx.Predicate = false;
                tx.Message = TStringBuilder() << "MinSeqNo violation failure on " << s.first;
                tx.WriteInfoApplied = true;
                break;
            }
        }
    }
    if (ret == EProcessResult::Continue && tx.Predicate.GetOrElse(true)) {
        TxAffectedSourcesIds.insert(txSourceIds.begin(), txSourceIds.end());

        tx.WriteInfoApplied = true;
        WriteKeysSizeEstimate += tx.WriteInfo->BodyKeys.size();
        WriteKeysSizeEstimate += tx.WriteInfo->SrcIdInfo.size();
    }

    return ret;
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvGetWriteInfoResponse> ev, const TActorContext& ctx)
{
    const auto sender = ev->SupportivePartition;
    WriteInfoResponseHandler(sender, ev.release(), ctx);
}

void TPartition::Handle(TEvPQ::TEvGetWriteInfoResponse::TPtr& ev, const TActorContext& ctx) {
    PQ_LOG_D("Handle TEvPQ::TEvGetWriteInfoResponse");

    ev->Get()->SupportivePartition = ev->Sender;

    ProcessPendingEvent(ev, ctx);
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvGetWriteInfoError> ev, const TActorContext& ctx)
{
    const auto sender = ev->SupportivePartition;
    WriteInfoResponseHandler(sender, ev.release(), ctx);
}

void TPartition::Handle(TEvPQ::TEvGetWriteInfoError::TPtr& ev, const TActorContext& ctx) {
    PQ_LOG_D("Handle TEvPQ::TEvGetWriteInfoError " <<
             "Cookie " << ev->Get()->Cookie <<
             ", Message " << ev->Get()->Message);

    ev->Get()->SupportivePartition = ev->Sender;

    ProcessPendingEvent(ev, ctx);
}

void TPartition::ReplyToProposeOrPredicate(TSimpleSharedPtr<TTransaction>& tx, bool isPredicate) {

    if (isPredicate) {
        auto insRes = TransactionsInflight.emplace(tx->Tx->TxId, tx);
        Y_ABORT_UNLESS(insRes.second);
        Send(Tablet,
             MakeHolder<TEvPQ::TEvTxCalcPredicateResult>(tx->Tx->Step,
                                                         tx->Tx->TxId,
                                                         Partition,
                                                         *tx->Predicate).Release());
    } else {
        auto insRes = TransactionsInflight.emplace(tx->ProposeConfig->TxId, tx);
        Y_ABORT_UNLESS(insRes.second);
        auto result = MakeHolder<TEvPQ::TEvProposePartitionConfigResult>(tx->ProposeConfig->Step,
                                                                tx->ProposeConfig->TxId,
                                                                Partition);

        result->Data.SetPartitionId(Partition.OriginalPartitionId);
        for (auto& [id, v] : SourceIdStorage.GetInMemorySourceIds()) {
            if (v.Explicit) {
                auto* m = result->Data.AddMessageGroup();
                m->SetId(id);
                m->SetSeqNo(v.SeqNo);
            }
        }
        Send(Tablet, result.Release());
    }
}

void TPartition::Handle(TEvPQ::TEvGetMaxSeqNoRequest::TPtr& ev, const TActorContext& ctx) {
    auto response = MakeHolder<TEvPQ::TEvProxyResponse>(ev->Get()->Cookie);
    NKikimrClient::TResponse& resp = *response->Response;

    resp.SetStatus(NMsgBusProxy::MSTATUS_OK);
    resp.SetErrorCode(NPersQueue::NErrorCode::OK);

    auto& result = *resp.MutablePartitionResponse()->MutableCmdGetMaxSeqNoResult();
    for (const auto& sourceId : ev->Get()->SourceIds) {
        auto& protoInfo = *result.AddSourceIdInfo();
        protoInfo.SetSourceId(sourceId);

        auto info = SourceManager.Get(sourceId);

        Y_ABORT_UNLESS(info.Offset <= (ui64)Max<i64>(), "Offset is too big: %" PRIu64, info.Offset);
        Y_ABORT_UNLESS(info.SeqNo <= (ui64)Max<i64>(), "SeqNo is too big: %" PRIu64, info.SeqNo);

        protoInfo.SetSeqNo(info.SeqNo);
        protoInfo.SetOffset(info.Offset);
        protoInfo.SetWriteTimestampMS(info.WriteTimestamp.MilliSeconds());
        protoInfo.SetExplicit(info.Explicit);
        protoInfo.SetState(TSourceIdInfo::ConvertState(info.State));
    }

    ctx.Send(Tablet, response.Release());
}

void TPartition::OnReadComplete(TReadInfo& info,
                                TUserInfo* userInfo,
                                const TEvPQ::TEvBlobResponse* blobResponse,
                                const TActorContext& ctx)
{
    TReadAnswer answer = info.FormAnswer(
        ctx, blobResponse, BlobEncoder.StartOffset, BlobEncoder.EndOffset, Partition, userInfo,
        info.Destination, GetSizeLag(info.Offset), Tablet, Config.GetMeteringMode(), IsActive(),
        GetResultPostProcessor<NKikimrClient::TCmdReadResult>(info.User)
    );
    const auto& resp = dynamic_cast<TEvPQ::TEvProxyResponse*>(answer.Event.Get())->Response;

    if (blobResponse && HasError(*blobResponse)) {
        if (info.IsSubscription) {
            TabletCounters.Cumulative()[COUNTER_PQ_READ_SUBSCRIPTION_ERROR].Increment(1);
        }
        TabletCounters.Cumulative()[COUNTER_PQ_READ_ERROR].Increment(1);
        TabletCounters.Percentile()[COUNTER_LATENCY_PQ_READ_ERROR].IncrementFor((ctx.Now() - info.Timestamp).MilliSeconds());
    } else {
        if (info.IsSubscription) {
            TabletCounters.Cumulative()[COUNTER_PQ_READ_SUBSCRIPTION_OK].Increment(1);
        }

        if (blobResponse) {
            TabletCounters.Cumulative()[COUNTER_PQ_READ_OK].Increment(1);
            TabletCounters.Percentile()[COUNTER_LATENCY_PQ_READ_OK].IncrementFor((ctx.Now() - info.Timestamp).MilliSeconds());
        } else {
            TabletCounters.Cumulative()[COUNTER_PQ_READ_HEAD_ONLY_OK].Increment(1);
            TabletCounters.Percentile()[COUNTER_LATENCY_PQ_READ_HEAD_ONLY].IncrementFor((ctx.Now() - info.Timestamp).MilliSeconds());
        }

        TabletCounters.Cumulative()[COUNTER_PQ_READ_BYTES].Increment(resp->ByteSize());
    }

    ctx.Send(info.Destination != 0 ? Tablet : ctx.SelfID, answer.Event.Release());

    OnReadRequestFinished(info.Destination, answer.Size, info.User, ctx);
}

void TPartition::Handle(TEvPQ::TEvBlobResponse::TPtr& ev, const TActorContext& ctx) {
    const ui64 cookie = ev->Get()->GetCookie();
    if (cookie == ERequestCookie::ReadBlobsForCompaction) {
        BlobsForCompactionWereRead(ev->Get()->GetBlobs());
        return;
    }
    auto it = ReadInfo.find(cookie);

    // If there is no such cookie, then read was canceled.
    // For example, it can be after consumer deletion
    if (it == ReadInfo.end()) {
        return;
    }

    TReadInfo info = std::move(it->second);
    ReadInfo.erase(it);

    auto* userInfo = UsersInfoStorage->GetIfExists(info.User);
    if (!userInfo) {
        ReplyError(ctx, info.Destination,  NPersQueue::NErrorCode::BAD_REQUEST, GetConsumerDeletedMessage(info.User));
        OnReadRequestFinished(info.Destination, 0, info.User, ctx);
    }

    OnReadComplete(info, userInfo, ev->Get(), ctx);
}

void TPartition::Handle(TEvPQ::TEvError::TPtr& ev, const TActorContext& ctx) {
    ReadingTimestamp = false;
    auto userInfo = UsersInfoStorage->GetIfExists(ReadingForUser);
    if (!userInfo || userInfo->ReadRuleGeneration != ReadingForUserReadRuleGeneration) {
        ProcessTimestampRead(ctx);
        return;
    }
    Y_ABORT_UNLESS(userInfo->ReadScheduled);
    Y_ABORT_UNLESS(ReadingForUser != "");

    PQ_LOG_ERROR("Topic '" << TopicName() << "' partition " << Partition
            << " user " << ReadingForUser << " readTimeStamp error: " << ev->Get()->Error
    );

    UpdateUserInfoTimestamp.push_back(std::make_pair(ReadingForUser, ReadingForUserReadRuleGeneration));

    ProcessTimestampRead(ctx);
}

void TPartition::CheckHeadConsistency() const
{
    BlobEncoder.CheckHeadConsistency(CompactLevelBorder, TotalLevels, TotalMaxCount);
}

ui64 TPartition::GetSizeLag(i64 offset) {
    return BlobEncoder.GetSizeLag(offset);
}


bool TPartition::UpdateCounters(const TActorContext& ctx, bool force) {
    if (!PartitionCountersLabeled) {
        return false;
    }

    const auto now = ctx.Now();
    if ((now - LastCountersUpdate < MIN_UPDATE_COUNTERS_DELAY) && !force)
        return false;

    LastCountersUpdate = now;

    // per client counters
    for (auto& userInfoPair : UsersInfoStorage->GetAll()) {
        auto& userInfo = userInfoPair.second;
        if (!userInfo.LabeledCounters)
            continue;
        if (userInfoPair.first != CLIENTID_WITHOUT_CONSUMER && !userInfo.HasReadRule && !userInfo.Important)
            continue;
        bool haveChanges = false;
        auto snapshot = CreateSnapshot(userInfo);
        auto ts = snapshot.LastCommittedMessage.WriteTimestamp.MilliSeconds();
        if (ts < MIN_TIMESTAMP_MS) ts = Max<i64>();
        if (userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_WRITE_TIME].Get() != ts) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_WRITE_TIME].Set(ts);
        }
        ts = snapshot.LastCommittedMessage.CreateTimestamp.MilliSeconds();
        if (ts < MIN_TIMESTAMP_MS) ts = Max<i64>();
        if (userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_CREATE_TIME].Get() != ts) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_CREATE_TIME].Set(ts);
        }
        auto readWriteTimestamp = snapshot.LastReadMessage.WriteTimestamp;
        if (userInfo.LabeledCounters->GetCounters()[METRIC_READ_WRITE_TIME].Get() != readWriteTimestamp.MilliSeconds()) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_WRITE_TIME].Set(readWriteTimestamp.MilliSeconds());
        }

        if (userInfo.LabeledCounters->GetCounters()[METRIC_READ_TOTAL_TIME].Get() != snapshot.TotalLag.MilliSeconds()) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_TOTAL_TIME].Set(snapshot.TotalLag.MilliSeconds());
        }

        ts = snapshot.LastReadTimestamp.MilliSeconds();
        if (userInfo.LabeledCounters->GetCounters()[METRIC_LAST_READ_TIME].Get() != ts) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_LAST_READ_TIME].Set(ts);
        }

        ui64 timeLag = userInfo.GetWriteLagMs();
        if (userInfo.LabeledCounters->GetCounters()[METRIC_WRITE_TIME_LAG].Get() != timeLag) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_WRITE_TIME_LAG].Set(timeLag);
        }

        if (userInfo.LabeledCounters->GetCounters()[METRIC_READ_TIME_LAG].Get() != snapshot.ReadLag.MilliSeconds()) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_TIME_LAG].Set(snapshot.ReadLag.MilliSeconds());
        }

        if (userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_MESSAGE_LAG].Get() != BlobEncoder.EndOffset - userInfo.Offset) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_MESSAGE_LAG].Set(BlobEncoder.EndOffset - userInfo.Offset);
        }

        auto readMessageLag = BlobEncoder.EndOffset - snapshot.ReadOffset;
        if (userInfo.LabeledCounters->GetCounters()[METRIC_READ_MESSAGE_LAG].Get() != readMessageLag) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_MESSAGE_LAG].Set(readMessageLag);
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_TOTAL_MESSAGE_LAG].Set(readMessageLag);
        }

        ui64 sizeLag = GetSizeLag(userInfo.Offset);
        if (userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_SIZE_LAG].Get() != sizeLag) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_COMMIT_SIZE_LAG].Set(sizeLag);
        }

        ui64 sizeLagRead = GetSizeLag(userInfo.ReadOffset);
        if (userInfo.LabeledCounters->GetCounters()[METRIC_READ_SIZE_LAG].Get() != sizeLagRead) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_SIZE_LAG].Set(sizeLagRead);
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_TOTAL_SIZE_LAG].Set(sizeLag);
        }

        if (userInfo.LabeledCounters->GetCounters()[METRIC_USER_PARTITIONS].Get() == 0) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_USER_PARTITIONS].Set(1);
        }

        ui64 readOffsetRewindSum = userInfo.ReadOffsetRewindSum;
        if (readOffsetRewindSum != userInfo.LabeledCounters->GetCounters()[METRIC_READ_OFFSET_REWIND_SUM].Get()) {
            haveChanges = true;
            userInfo.LabeledCounters->GetCounters()[METRIC_READ_OFFSET_REWIND_SUM].Set(readOffsetRewindSum);
        }

        ui32 id = METRIC_TOTAL_READ_SPEED_1;
        for (ui32 i = 0; i < userInfo.AvgReadBytes.size(); ++i) {
            ui64 avg = userInfo.AvgReadBytes[i].GetValue();
            if (avg != userInfo.LabeledCounters->GetCounters()[id].Get()) {
                haveChanges = true;
                userInfo.LabeledCounters->GetCounters()[id].Set(avg); //total
                userInfo.LabeledCounters->GetCounters()[id + 1].Set(avg); //max
            }
            id += 2;
        }
        Y_ABORT_UNLESS(id == METRIC_MAX_READ_SPEED_4 + 1);
        if (userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_BYTES].Get()) {
            ui64 quotaUsage = ui64(userInfo.AvgReadBytes[1].GetValue()) * 1000000 / userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_BYTES].Get() / 60;
            if (quotaUsage != userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_USAGE].Get()) {
                haveChanges = true;
                userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_USAGE].Set(quotaUsage);
            }
        }

        if (userInfoPair.first == CLIENTID_WITHOUT_CONSUMER ) {
            PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_NO_CONSUMER_BYTES].Set(userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_BYTES].Get());
            PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_NO_CONSUMER_USAGE].Set(userInfo.LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_USAGE].Get());
        }

        if (haveChanges) {
            ctx.Send(Tablet, new TEvPQ::TEvPartitionLabeledCounters(Partition, *userInfo.LabeledCounters));
        }
    }

    bool haveChanges = false;
    if (SourceIdStorage.GetInMemorySourceIds().size() != PartitionCountersLabeled->GetCounters()[METRIC_MAX_NUM_SIDS].Get()) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_MAX_NUM_SIDS].Set(SourceIdStorage.GetInMemorySourceIds().size());
        PartitionCountersLabeled->GetCounters()[METRIC_NUM_SIDS].Set(SourceIdStorage.GetInMemorySourceIds().size());
    }

    TDuration lifetimeNow = ctx.Now() - SourceIdStorage.MinAvailableTimestamp(ctx.Now());
    if (lifetimeNow.MilliSeconds() != PartitionCountersLabeled->GetCounters()[METRIC_MIN_SID_LIFETIME].Get()) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_MIN_SID_LIFETIME].Set(lifetimeNow.MilliSeconds());
    }

    const ui64 headGapSize = BlobEncoder.GetHeadGapSize();
    const ui64 gapSize = GapSize + headGapSize;
    if (gapSize != PartitionCountersLabeled->GetCounters()[METRIC_GAPS_SIZE].Get()) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_MAX_GAPS_SIZE].Set(gapSize);
        PartitionCountersLabeled->GetCounters()[METRIC_GAPS_SIZE].Set(gapSize);
    }

    const ui32 gapsCount = GapOffsets.size() + (headGapSize ? 1 : 0);
    if (gapsCount != PartitionCountersLabeled->GetCounters()[METRIC_GAPS_COUNT].Get()) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_MAX_GAPS_COUNT].Set(gapsCount);
        PartitionCountersLabeled->GetCounters()[METRIC_GAPS_COUNT].Set(gapsCount);
    }

    if (TotalPartitionWriteSpeed != PartitionCountersLabeled->GetCounters()[METRIC_WRITE_QUOTA_BYTES].Get()) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_WRITE_QUOTA_BYTES].Set(TotalPartitionWriteSpeed);
    }

    ui32 id = METRIC_TOTAL_WRITE_SPEED_1;
    for (ui32 i = 0; i < AvgWriteBytes.size(); ++i) {
        ui64 avg = AvgWriteBytes[i].GetValue();
        if (avg != PartitionCountersLabeled->GetCounters()[id].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[id].Set(avg); //total
            PartitionCountersLabeled->GetCounters()[id + 1].Set(avg); //max
        }
        id += 2;
    }
    Y_ABORT_UNLESS(id == METRIC_MAX_WRITE_SPEED_4 + 1);


    id = METRIC_TOTAL_QUOTA_SPEED_1;
    for (ui32 i = 0; i < AvgQuotaBytes.size(); ++i) {
        ui64 avg = AvgQuotaBytes[i].GetValue();
        if (avg != PartitionCountersLabeled->GetCounters()[id].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[id].Set(avg); //total
            PartitionCountersLabeled->GetCounters()[id + 1].Set(avg); //max
        }
        id += 2;
    }
    Y_ABORT_UNLESS(id == METRIC_MAX_QUOTA_SPEED_4 + 1);

    if (TotalPartitionWriteSpeed) {
        ui64 quotaUsage = ui64(AvgQuotaBytes[1].GetValue()) * 1000000 / TotalPartitionWriteSpeed / 60;
        if (quotaUsage != PartitionCountersLabeled->GetCounters()[METRIC_WRITE_QUOTA_USAGE].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[METRIC_WRITE_QUOTA_USAGE].Set(quotaUsage);
        }
    }

    ui64 storageSize = StorageSize(ctx);
    if (storageSize != PartitionCountersLabeled->GetCounters()[METRIC_TOTAL_PART_SIZE].Get()) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_MAX_PART_SIZE].Set(storageSize);
        PartitionCountersLabeled->GetCounters()[METRIC_TOTAL_PART_SIZE].Set(storageSize);
    }

    if (NKikimrPQ::TPQTabletConfig::METERING_MODE_RESERVED_CAPACITY == Config.GetMeteringMode()) {
        ui64 reserveSize = ReserveSize();
        if (reserveSize != PartitionCountersLabeled->GetCounters()[METRIC_RESERVE_LIMIT_BYTES].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[METRIC_RESERVE_LIMIT_BYTES].Set(reserveSize);
        }

        ui64 reserveUsed = UsedReserveSize(ctx);
        if (reserveUsed != PartitionCountersLabeled->GetCounters()[METRIC_RESERVE_USED_BYTES].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[METRIC_RESERVE_USED_BYTES].Set(reserveUsed);
        }
    }

    ui64 ts = (WriteTimestamp.MilliSeconds() < MIN_TIMESTAMP_MS) ? Max<i64>() : WriteTimestamp.MilliSeconds();
    if (PartitionCountersLabeled->GetCounters()[METRIC_LAST_WRITE_TIME].Get() != ts) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_LAST_WRITE_TIME].Set(ts);
    }

    ui64 timeLag = WriteLagMs.GetValue();
    if (PartitionCountersLabeled->GetCounters()[METRIC_WRITE_TIME_LAG_MS].Get() != timeLag) {
        haveChanges = true;
        PartitionCountersLabeled->GetCounters()[METRIC_WRITE_TIME_LAG_MS].Set(timeLag);
    }

    if (PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_BYTES].Get()) {
        ui64 quotaUsage = ui64(AvgReadBytes.GetValue()) * 1000000 / PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_BYTES].Get() / 60;
        if (quotaUsage != PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_USAGE].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_USAGE].Set(quotaUsage);
        }
    }

    if (PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_NO_CONSUMER_BYTES].Get()) {
        ui64 quotaUsage = ui64(AvgReadBytes.GetValue()) * 1000000 / PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_BYTES].Get() / 60;
        if (quotaUsage != PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_USAGE].Get()) {
            haveChanges = true;
            PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_USAGE].Set(quotaUsage);
        }
    }
    return haveChanges;
}

void TPartition::ReportCounters(const TActorContext& ctx, bool force) {
    if (UpdateCounters(ctx, force)) {
        ctx.Send(Tablet, new TEvPQ::TEvPartitionLabeledCounters(Partition, *PartitionCountersLabeled));
    }
}

void TPartition::Handle(NReadQuoterEvents::TEvQuotaUpdated::TPtr& ev, const TActorContext&) {
    for (auto& [consumerStr, quota] : ev->Get()->UpdatedConsumerQuotas) {
        const TUserInfo* userInfo = UsersInfoStorage->GetIfExists(consumerStr);
        if (userInfo) {
            userInfo->LabeledCounters->GetCounters()[METRIC_READ_QUOTA_PER_CONSUMER_BYTES].Set(quota);
        }
    }
    if (PartitionCountersLabeled)
        PartitionCountersLabeled->GetCounters()[METRIC_READ_QUOTA_PARTITION_TOTAL_BYTES].Set(ev->Get()->UpdatedTotalPartitionReadQuota);
}

void TPartition::Handle(TEvKeyValue::TEvResponse::TPtr& ev, const TActorContext& ctx) {
    auto& response = ev->Get()->Record;

    //check correctness of response
    if (response.GetStatus() != NMsgBusProxy::MSTATUS_OK) {
        PQ_LOG_ERROR("OnWrite topic '" << TopicName() << "' partition " << Partition
                << " commands are not processed at all, reason: " << response.DebugString());
        ctx.Send(Tablet, new TEvents::TEvPoisonPill());
        //TODO: if status is DISK IS FULL, is global status MSTATUS_OK? it will be good if it is true
        return;
    }
    if (response.DeleteRangeResultSize()) {
        for (ui32 i = 0; i < response.DeleteRangeResultSize(); ++i) {
            if (response.GetDeleteRangeResult(i).GetStatus() != NKikimrProto::OK) {
                PQ_LOG_ERROR("OnWrite topic '" << TopicName() << "' partition " << Partition << " delete range error");
                //TODO: if disk is full, could this be ok? delete must be ok, of course
                ctx.Send(Tablet, new TEvents::TEvPoisonPill());
                return;
            }
        }
    }

    if (response.WriteResultSize()) {
        bool diskIsOk = true;
        for (ui32 i = 0; i < response.WriteResultSize(); ++i) {
            if (response.GetWriteResult(i).GetStatus() != NKikimrProto::OK) {
                PQ_LOG_ERROR("OnWrite  topic '" << TopicName() << "' partition " << Partition << " write error");
                ctx.Send(Tablet, new TEvents::TEvPoisonPill());
                return;
            }
            diskIsOk = diskIsOk && CheckDiskStatus(response.GetWriteResult(i).GetStatusFlags());
        }
        DiskIsFull = !diskIsOk;
    }
    bool diskIsOk = true;
    for (ui32 i = 0; i < response.GetStatusResultSize(); ++i) {
        auto& res = response.GetGetStatusResult(i);
        if (res.GetStatus() != NKikimrProto::OK) {
            PQ_LOG_ERROR("OnWrite  topic '" << TopicName() << "' partition " << Partition
                    << " are not processed at all, got KV error in CmdGetStatus " << res.GetStatus());
            ctx.Send(Tablet, new TEvents::TEvPoisonPill());
            return;
        }
        diskIsOk = diskIsOk && CheckDiskStatus(res.GetStatusFlags());
    }
    if (response.GetStatusResultSize()) {
        DiskIsFull = !diskIsOk;
    }
    const auto writeDuration = ctx.Now() - WriteStartTime;
    const auto minWriteLatency = TDuration::MilliSeconds(AppData(ctx)->PQConfig.GetMinWriteLatencyMs());

    if (response.HasCookie() && (response.GetCookie() == ERequestCookie::WriteBlobsForCompaction)) {
        BlobsForCompactionWereWrite();
        return;
    }

    if (writeDuration > minWriteLatency) {
        OnHandleWriteResponse(ctx);
    } else {
        ctx.Schedule(minWriteLatency - writeDuration, new TEvPQ::TEvHandleWriteResponse(response.GetCookie()));
    }
}


void TPartition::PushBackDistrTx(TSimpleSharedPtr<TEvPQ::TEvTxCalcPredicate> event)
{
    UserActionAndTransactionEvents.emplace_back(MakeSimpleShared<TTransaction>(std::move(event)));
    RequestWriteInfoIfRequired();
}

void TPartition::RequestWriteInfoIfRequired()
{
    auto tx = std::get<1>(UserActionAndTransactionEvents.back().Event);
    auto supportId = tx->SupportivePartitionActor;
    if (supportId) {
        Send(supportId, new TEvPQ::TEvGetWriteInfoRequest());
        WriteInfosToTx.insert(std::make_pair(supportId, tx));
    }
}


void TPartition::PushBackDistrTx(TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> event)
{
    UserActionAndTransactionEvents.emplace_back(MakeSimpleShared<TTransaction>(std::move(event), true));
}

void TPartition::PushFrontDistrTx(TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> event)
{
    UserActionAndTransactionEvents.emplace_front(MakeSimpleShared<TTransaction>(std::move(event), false));
}

void TPartition::PushBackDistrTx(TSimpleSharedPtr<TEvPQ::TEvProposePartitionConfig> event)
{
    UserActionAndTransactionEvents.emplace_back(MakeSimpleShared<TTransaction>(std::move(event)));
}

void TPartition::AddImmediateTx(TSimpleSharedPtr<TEvPersQueue::TEvProposeTransaction> tx)
{
    UserActionAndTransactionEvents.emplace_back(MakeSimpleShared<TTransaction>(std::move(tx)));
    ++ImmediateTxCount;
    RequestWriteInfoIfRequired();
}

void TPartition::AddUserAct(TSimpleSharedPtr<TEvPQ::TEvSetClientInfo> act)
{
    TString clientId = act->ClientId;
    UserActionAndTransactionEvents.emplace_back(std::move(act));
    ++UserActCount[clientId];
}

void TPartition::RemoveUserAct(const TString& consumerId)
{
    auto p = UserActCount.find(consumerId);
    Y_ABORT_UNLESS(p != UserActCount.end());

    Y_ABORT_UNLESS(p->second > 0);
    if (!--p->second) {
        UserActCount.erase(p);
    }
}

size_t TPartition::GetUserActCount(const TString& consumer) const
{
    if (auto i = UserActCount.find(consumer); i != UserActCount.end()) {
        return i->second;
    } else {
        return 0;
    }
}

void TPartition::ProcessTxsAndUserActs(const TActorContext& ctx)
{
    if (KVWriteInProgress) {
        return;
    }
    if (DeletePartitionState == DELETION_INITED) {
        if (!PersistRequest) {
            PersistRequest = MakeHolder<TEvKeyValue::TEvRequest>();
        }
        ScheduleNegativeReplies();
        ScheduleDeletePartitionDone();

        AddCmdDeleteRangeForAllKeys(*PersistRequest);

        ctx.Send(BlobCache, PersistRequest.Release(), 0, 0, PersistRequestSpan.GetTraceId());
        PersistRequest = nullptr;
        CurrentPersistRequestSpan = std::move(PersistRequestSpan);
        PersistRequestSpan = NWilson::TSpan();
        DeletePartitionState = DELETION_IN_PROCESS;
        KVWriteInProgress = true;

        return;
    }
    while (true) {
        if (BatchingState == ETxBatchingState::PreProcessing) {
            ContinueProcessTxsAndUserActs(ctx);
        }
        if (BatchingState == ETxBatchingState::PreProcessing) {
            return; // Still preprocessing - waiting for something;
        }

        // Preprocessing complete;
        if (CurrentBatchSize > 0) {
            Send(SelfId(), new TEvPQ::TEvTxBatchComplete(CurrentBatchSize));
        }
        CurrentBatchSize = 0;

        if (UserActionAndTxPendingCommit.empty()) {
            // Processing stopped and nothing to commit - finalize
            BatchingState = ETxBatchingState::Finishing;
        } else {
            // Process commit queue
            ProcessCommitQueue();
        }
        if (!UserActionAndTxPendingCommit.empty()) {
            // Still pending for come commits
            return;
        }
        // Commit queue processing complete. Now can either swith to persist or continue preprocessing;
        if (BatchingState == ETxBatchingState::Finishing) { // Persist required;
            RunPersist();
            return;
        }
        BatchingState = ETxBatchingState::PreProcessing;
    }
}

void TPartition::ContinueProcessTxsAndUserActs(const TActorContext&)
{
    Y_ABORT_UNLESS(!KVWriteInProgress);

    if (WriteCycleSizeEstimate >= MAX_WRITE_CYCLE_SIZE || WriteKeysSizeEstimate >= MAX_KEYS) {
        BatchingState = ETxBatchingState::Finishing;
        return;
    }
    auto visitor = [this](auto& event) {
        return this->PreProcessUserActionOrTransaction(event);
    };
    while (BatchingState == ETxBatchingState::PreProcessing && !UserActionAndTransactionEvents.empty()) {
        if (ChangingConfig) {
            BatchingState = ETxBatchingState::Finishing;
            break;
        }
        auto& front = UserActionAndTransactionEvents.front();
        if (TMessage* msg = std::get_if<TMessage>(&front.Event); msg && msg->WaitPreviousWriteSpan) {
            msg->WaitPreviousWriteSpan.End();
        }
        switch (std::visit(visitor, front.Event)) {
            case EProcessResult::Continue:
                MoveUserActOrTxToCommitState();
                FirstEvent = false;
                break;
            case EProcessResult::ContinueDrop:
                UserActionAndTransactionEvents.pop_front();
                break;
            case EProcessResult::Break:
                MoveUserActOrTxToCommitState();
                BatchingState = ETxBatchingState::Finishing;
                FirstEvent = false;
                break;
            case EProcessResult::Blocked:
                BatchingState = ETxBatchingState::Executing;
                return;
            case EProcessResult::NotReady:
                return;
        }
        CurrentBatchSize += 1;
    }
    if (UserActionAndTransactionEvents.empty()) {
        BatchingState = ETxBatchingState::Executing;
        return;
    }

}

void TPartition::MoveUserActOrTxToCommitState() {
    auto& front = UserActionAndTransactionEvents.front();
    UserActionAndTxPendingCommit.push_back(std::move(front));
    UserActionAndTransactionEvents.pop_front();
}

void TPartition::ProcessCommitQueue() {
    CurrentBatchSize = 0;

    Y_ABORT_UNLESS(!KVWriteInProgress);
    if (!PersistRequest) {
        PersistRequest = MakeHolder<TEvKeyValue::TEvRequest>();
    }
    auto visitor = [this, request = PersistRequest.Get()](auto& event) {
        return this->ExecUserActionOrTransaction(event, request);
    };
    while (!UserActionAndTxPendingCommit.empty()) {
        auto& front = UserActionAndTxPendingCommit.front();
        auto state = ECommitState::Committed;
        if (auto* tx = get_if<TSimpleSharedPtr<TTransaction>>(&front.Event)) {
            state = tx->Get()->State;
        }
        switch (state) {
            case ECommitState::Pending:
                return;
            case ECommitState::Aborted:
                break;
            case ECommitState::Committed:
                break;
        }
        auto event = std::move(front.Event);
        UserActionAndTxPendingCommit.pop_front();
        std::visit(visitor, event);
    }
    if (UserActionAndTxPendingCommit.empty()) {
        TxAffectedConsumers.clear();
        TxAffectedSourcesIds.clear();
        Y_ABORT_UNLESS(UserActionAndTxPendingCommit.empty());
        TransactionsInflight.clear();
    }
}

ui64 TPartition::NextReadCookie()
{
    if (Cookie == Max<ui64>()) {
        Cookie = ERequestCookie::End;
    }
    return Cookie++;
}

void TPartition::RunPersist() {
    TransactionsInflight.clear();

    Y_ABORT_UNLESS(UserActionAndTxPendingCommit.empty());
    const auto& ctx = ActorContext();
    const auto now = ctx.Now();
    if (!PersistRequest) {
        PersistRequest = MakeHolder<TEvKeyValue::TEvRequest>();
    }

    if (ManageWriteTimestampEstimate) {
        WriteTimestampEstimate = now;
    }

    HaveDrop = CleanUp(PersistRequest.Get(), ctx);
    bool haveChanges = HaveDrop;
    if (DiskIsFull) {
        AddCheckDiskRequest(PersistRequest.Get(), NumChannels);
        haveChanges = true;
    }

    ProcessReserveRequests(ctx);

    WriteStartTime = TActivationContext::Now();

    if (HaveWriteMsg) {
        if (!DiskIsFull) {
            EndAppendHeadWithNewWrites(ctx);
            EndProcessWrites(PersistRequest.Get(), ctx);
        }
        EndHandleRequests(PersistRequest.Get(), ctx);
        //haveChanges = true;
    }

    if (TryAddDeleteHeadKeysToPersistRequest()) {
        haveChanges = true;
    }

    if (haveChanges || TxIdHasChanged || !AffectedUsers.empty() || ChangeConfig) {
        WriteCycleStartTime = now;
        WriteStartTime = now;
        TopicQuotaWaitTimeForCurrentBlob = TDuration::Zero();
        PartitionQuotaWaitTimeForCurrentBlob = TDuration::Zero();
        WritesTotal.Inc();
        AddMetaKey(PersistRequest.Get());
        HaveWriteMsg = true;

        AddCmdWriteTxMeta(PersistRequest->Record);
        AddCmdWriteUserInfos(PersistRequest->Record);
        AddCmdWriteConfig(PersistRequest->Record);
    }
    if (PersistRequest->Record.CmdDeleteRangeSize() || PersistRequest->Record.CmdWriteSize() || PersistRequest->Record.CmdRenameSize()) {
        // Apply counters
        for (const auto& writeInfo : WriteInfosApplied) {
            // writeTimeLag
            if (InputTimeLag && writeInfo->InputLags) {
                writeInfo->InputLags->UpdateTimestamp(ctx.Now().MilliSeconds());
                for (const auto& values : writeInfo->InputLags->GetValues()) {
                    if (values.second)
                        InputTimeLag->IncFor(std::ceil(values.first), values.second);
                }
            }
            //MessageSize
            auto i = 0u;
            for (auto range : MessageSize.GetRanges()) {
                if (i >= writeInfo->MessagesSizes.size()) {
                    break;
                }
                MessageSize.IncFor(range, writeInfo->MessagesSizes[i++]);
            }

            // Bytes Written
            BytesWrittenTotal.Inc(writeInfo->BytesWrittenTotal);
            BytesWrittenGrpc.Inc(writeInfo->BytesWrittenGrpc);
            BytesWrittenUncompressed.Inc(writeInfo->BytesWrittenUncompressed);
            // Messages written
            MsgsWrittenTotal.Inc(writeInfo->MessagesWrittenTotal);
            MsgsWrittenGrpc.Inc(writeInfo->MessagesWrittenTotal);

            WriteNewSizeFromSupportivePartitions += writeInfo->BytesWrittenTotal;
        }
        WriteInfosApplied.clear();
        //Done with counters.

        // for debugging purposes
        //DumpKeyValueRequest(PersistRequest->Record);

        PersistRequestSpan.Attribute("bytes", static_cast<i64>(PersistRequest->Record.ByteSizeLong()));
        ctx.Send(HaveWriteMsg ? BlobCache : Tablet, PersistRequest.Release(), 0, 0, PersistRequestSpan.GetTraceId());
        CurrentPersistRequestSpan = std::move(PersistRequestSpan);
        PersistRequestSpan = NWilson::TSpan();
        KVWriteInProgress = true;
    } else {
        OnProcessTxsAndUserActsWriteComplete(ActorContext());
        AnswerCurrentWrites(ctx);
        AnswerCurrentReplies(ctx);
        HaveWriteMsg = false;
    }
    PersistRequest = nullptr;
}

bool TPartition::TryAddDeleteHeadKeysToPersistRequest()
{
    bool haveChanges = !DeletedKeys->empty();

    while (!DeletedKeys->empty()) {
        auto& k = DeletedKeys->back();

        auto* cmd = PersistRequest->Record.AddCmdDeleteRange();
        auto* range = cmd->MutableRange();

        range->SetFrom(k.data(), k.size());
        range->SetIncludeFrom(true);
        range->SetTo(k.data(), k.size());
        range->SetIncludeTo(true);

        DeletedKeys->pop_back();
    }

    return haveChanges;
}

//void TPartition::DumpKeyValueRequest(const NKikimrClient::TKeyValueRequest& request)
//{
//    DBGTRACE_LOG("=== DumpKeyValueRequest ===");
//    DBGTRACE_LOG("--- delete ----------------");
//    for (size_t i = 0; i < request.CmdDeleteRangeSize(); ++i) {
//        const auto& cmd = request.GetCmdDeleteRange(i);
//        const auto& range = cmd.GetRange();
//        Y_UNUSED(range);
//        DBGTRACE_LOG((range.GetIncludeFrom() ? '[' : '(') << range.GetFrom() <<
//                     ", " <<
//                     range.GetTo() << (range.GetIncludeTo() ? ']' : ')'));
//    }
//    DBGTRACE_LOG("--- write -----------------");
//    for (size_t i = 0; i < request.CmdWriteSize(); ++i) {
//        const auto& cmd = request.GetCmdWrite(i);
//        Y_UNUSED(cmd);
//        DBGTRACE_LOG(cmd.GetKey());
//    }
//    DBGTRACE_LOG("--- rename ----------------");
//    for (size_t i = 0; i < request.CmdRenameSize(); ++i) {
//        const auto& cmd = request.GetCmdRename(i);
//        Y_UNUSED(cmd);
//        DBGTRACE_LOG(cmd.GetOldKey() << ", " << cmd.GetNewKey());
//    }
//    DBGTRACE_LOG("===========================");
//}

//void TPartition::DumpZones(const char* file, unsigned line) const
//{
//    DBGTRACE("TPartition::DumpZones");
//
//    if (file) {
//        Y_UNUSED(line);
//        DBGTRACE_LOG(file << "(" << line << ")");
//    }
//
//    DBGTRACE_LOG("=== DumpPartitionZones ===");
//    DBGTRACE_LOG("--- Compaction -----------");
//    CompactionBlobEncoder.Dump();
//    DBGTRACE_LOG("--- FastWrite ------------");
//    BlobEncoder.Dump();
//    DBGTRACE_LOG("==========================");
//}

TBlobKeyTokenPtr TPartition::MakeBlobKeyToken(const TString& key)
{
    // The number of links counts is `std::shared_ptr'. It is possible to set its own destructor,
    // which adds the key to the queue for deletion before freeing the memory.
    auto ptr = std::make_unique<TBlobKeyToken>(key);

    auto deleter = [keys = DeletedKeys](TBlobKeyToken* token) {
        if (token->NeedDelete) {
            keys->emplace_back(std::move(token->Key));
        }
        delete token;
    };

    return {ptr.release(), std::move(deleter)};
}

void TPartition::AnswerCurrentReplies(const TActorContext& ctx)
{
    for (auto& [actor, reply] : Replies) {
        ctx.Send(actor, reply.release());
    }
    Replies.clear();
}

TPartition::EProcessResult TPartition::PreProcessUserActionOrTransaction(TSimpleSharedPtr<TTransaction>& t)
{
    auto result = EProcessResult::Continue;
    if (t->SupportivePartitionActor && !t->WriteInfo && !t->WriteInfoApplied) { // Pending for write info
        return EProcessResult::NotReady;
    }
    if (t->WriteInfo && !t->WriteInfoApplied) { //Recieved write info but not applied
        result = ApplyWriteInfoResponse(*t);
        if (!t->WriteInfoApplied) { // Tried to apply write info but couldn't - TX must be blocked.
            Y_ABORT_UNLESS(result != EProcessResult::Continue);
            return result;
        }
    }
    if (t->ProposeTransaction) { // Immediate TX
        if (!t->Predicate.GetOrElse(true)) {
            t->State = ECommitState::Aborted;
            return EProcessResult::Continue;
        }
        t->Predicate.ConstructInPlace(true);
        return PreProcessImmediateTx(t->ProposeTransaction->GetRecord());

    } else if (t->Tx) { // Distributed TX
        if (t->Predicate.Defined()) { // Predicate defined - either failed previously or Tx created with predicate defined.
            ReplyToProposeOrPredicate(t, true);
            return EProcessResult::Continue;
        }
        result = BeginTransaction(*t->Tx, t->Predicate);
        if (t->Predicate.Defined()) {
            ReplyToProposeOrPredicate(t, true);
        }
        return result;
    } else if (t->ProposeConfig) {
        if (!FirstEvent) {
            return EProcessResult::Blocked;
        }
        t->Predicate = BeginTransaction(*t->ProposeConfig);
        ChangingConfig = true;
        PendingPartitionConfig = GetPartitionConfig(t->ProposeConfig->Config);
        //Y_VERIFY_DEBUG_S(PendingPartitionConfig, "Partition " << Partition << " config not found");
        ReplyToProposeOrPredicate(t, false);
        return EProcessResult::Break;
    } else {
        Y_ABORT_UNLESS(t->ChangeConfig);

        Y_ABORT_UNLESS(!ChangeConfig && !ChangingConfig);
        if (!FirstEvent) {
            return EProcessResult::Blocked;
        }
        ChangingConfig = true;
        // Should remove this and add some id to TEvChangeConfig if we want to batch change of configs
        t->State = ECommitState::Committed;
        return EProcessResult::Break;
    }
    Y_ABORT();
    return result;
}

bool TPartition::ExecUserActionOrTransaction(TSimpleSharedPtr<TTransaction>& t, TEvKeyValue::TEvRequest*)
{
    if (t->ProposeTransaction) {
        ExecImmediateTx(*t);
        return true;
    }
    switch(t->State) {
        case ECommitState::Pending:
            return false;
        case ECommitState::Aborted:
            RollbackTransaction(t);
            return true;
        case ECommitState::Committed:
            break;
    }
    const auto& ctx = ActorContext();
    if (t->ChangeConfig) {
        Y_ABORT_UNLESS(!ChangeConfig);
        Y_ABORT_UNLESS(ChangingConfig);
        ChangeConfig = t->ChangeConfig;
        SendChangeConfigReply = t->SendReply;
        BeginChangePartitionConfig(ChangeConfig->Config, ctx);
    } else if (t->ProposeConfig) {
        Y_ABORT_UNLESS(ChangingConfig);
        ChangeConfig = MakeSimpleShared<TEvPQ::TEvChangePartitionConfig>(TopicConverter,
                                                                         t->ProposeConfig->Config);
        PendingPartitionConfig = GetPartitionConfig(ChangeConfig->Config);
        SendChangeConfigReply = false;
    }
    CommitTransaction(t);
    return true;
}

TPartition::EProcessResult TPartition::BeginTransaction(const TEvPQ::TEvTxCalcPredicate& tx, TMaybe<bool>& predicateOut)
{
    if (tx.ForcePredicateFalse) {
        predicateOut = false;
        return EProcessResult::Continue;
    }

    THashSet<TString> consumers;
    bool result = true;
    for (auto& operation : tx.Operations) {
        const TString& consumer = operation.GetConsumer();
        if (TxAffectedConsumers.contains(consumer)) {
            return EProcessResult::Blocked;
        }
        if (SetOffsetAffectedConsumers.contains(consumer)) {
            return EProcessResult::Blocked;
        }

        if (AffectedUsers.contains(consumer) && !GetPendingUserIfExists(consumer)) {
            PQ_LOG_D("Partition " << Partition <<
                    " Consumer '" << consumer << "' has been removed");
            result = false;
            break;
        }

        if (!UsersInfoStorage->GetIfExists(consumer)) {
            PQ_LOG_D("Partition " << Partition <<
                        " Unknown consumer '" << consumer << "'");
            result = false;
            break;
        }

        bool isAffectedConsumer = AffectedUsers.contains(consumer);
        TUserInfoBase& userInfo = GetOrCreatePendingUser(consumer);

        if (operation.GetOnlyCheckCommitedToFinish()) {
            if (IsActive() || static_cast<ui64>(userInfo.Offset) != BlobEncoder.EndOffset) {
               result = false;
            }
        } else if (!operation.GetReadSessionId().empty() && operation.GetReadSessionId() != userInfo.Session) {
            if (IsActive() || operation.GetCommitOffsetsEnd() < BlobEncoder.EndOffset || userInfo.Offset != i64(BlobEncoder.EndOffset)) {
                PQ_LOG_D("Partition " << Partition <<
                    " Consumer '" << consumer << "'" <<
                    " Bad request (session already dead) " <<
                    " RequestSessionId '" << operation.GetReadSessionId() <<
                    " CurrentSessionId '" << userInfo.Session <<
                    "'");
                result = false;
            }
        } else {
            if (!operation.GetForceCommit() && operation.GetCommitOffsetsBegin() > operation.GetCommitOffsetsEnd()) {
                PQ_LOG_D("Partition " << Partition <<
                            " Consumer '" << consumer << "'" <<
                            " Bad request (invalid range) " <<
                            " Begin " << operation.GetCommitOffsetsBegin() <<
                            " End " << operation.GetCommitOffsetsEnd());
                result = false;
            } else if (!operation.GetForceCommit() && userInfo.Offset != (i64)operation.GetCommitOffsetsBegin()) {
                PQ_LOG_D("Partition " << Partition <<
                            " Consumer '" << consumer << "'" <<
                            " Bad request (gap) " <<
                            " Offset " << userInfo.Offset <<
                            " Begin " << operation.GetCommitOffsetsBegin());
                result = false;
            } else if (!operation.GetForceCommit() && operation.GetCommitOffsetsEnd() > BlobEncoder.EndOffset) {
                PQ_LOG_D("Partition " << Partition <<
                            " Consumer '" << consumer << "'" <<
                            " Bad request (behind the last offset) " <<
                            " EndOffset " << BlobEncoder.EndOffset <<
                            " End " << operation.GetCommitOffsetsEnd());
                result = false;
            }

            if (!result) {
                if (!isAffectedConsumer) {
                    AffectedUsers.erase(consumer);
                }
                break;
            }
            consumers.insert(consumer);
        }
    }

    if (result) {
        TxAffectedConsumers.insert(consumers.begin(), consumers.end());
    }
    predicateOut = result;
    return EProcessResult::Continue;
}

bool TPartition::BeginTransaction(const TEvPQ::TEvProposePartitionConfig& event)
{
    ChangeConfig =
        MakeSimpleShared<TEvPQ::TEvChangePartitionConfig>(TopicConverter,
                                                          event.Config);
    PendingPartitionConfig = GetPartitionConfig(ChangeConfig->Config);

    SendChangeConfigReply = false;
    return true;
}

void TPartition::CommitWriteOperations(TTransaction& t)
{
    PQ_LOG_D("TPartition::CommitWriteOperations TxId: " << t.GetTxId());

    Y_ABORT_UNLESS(PersistRequest);
    Y_ABORT_UNLESS(!BlobEncoder.PartitionedBlob.IsInited());

    if (!t.WriteInfo) {
        return;
    }
    for (const auto& s : t.WriteInfo->SrcIdInfo) {
        auto pair = TSeqNoProducerEpoch{s.second.SeqNo, s.second.ProducerEpoch};
        auto [iter, ins] = TxInflightMaxSeqNoPerSourceId.emplace(s.first, pair);
        if (!ins) {
            bool ok = !SeqnoViolation(iter->second.KafkaProducerEpoch, iter->second.SeqNo, s.second.ProducerEpoch, s.second.SeqNo);
            Y_ABORT_UNLESS(ok);
            iter->second = pair;
        }
    }
    const auto& ctx = ActorContext();

    if (!HaveWriteMsg) {
        BeginHandleRequests(PersistRequest.Get(), ctx);
        if (!DiskIsFull) {
            BeginProcessWrites(ctx);
            BeginAppendHeadWithNewWrites(ctx);
        }
        HaveWriteMsg = true;
    }

    PQ_LOG_D("Head=" << BlobEncoder.Head << ", NewHead=" << BlobEncoder.NewHead);

    auto oldHeadOffset = BlobEncoder.NewHead.Offset;

    if (!t.WriteInfo->BodyKeys.empty()) {
        bool needCompactHead =
            (Parameters->FirstCommitWriteOperations ? BlobEncoder.Head : BlobEncoder.NewHead).PackedSize != 0;

        BlobEncoder.NewPartitionedBlob(Partition,
                                       BlobEncoder.NewHead.Offset,
                                       "", // SourceId
                                       0,  // SeqNo
                                       0,  // TotalParts
                                       0,  // TotalSize
                                       Parameters->HeadCleared,  // headCleared
                                       needCompactHead,          // needCompactHead
                                       MaxBlobSize);

        for (auto& k : t.WriteInfo->BodyKeys) {
            PQ_LOG_D("add key " << k.Key.ToString());
            auto write = BlobEncoder.PartitionedBlob.Add(k.Key, k.Size);
            if (write && !write->Value.empty()) {
                AddCmdWrite(write, PersistRequest.Get(), ctx);
                BlobEncoder.CompactedKeys.emplace_back(write->Key, write->Value.size());
            }
            Parameters->CurOffset += k.Key.GetCount();
            // The key does not need to be deleted, as it will be renamed
            k.BlobKeyToken->NeedDelete = false;
        }

        PQ_LOG_D("PartitionedBlob.GetFormedBlobs().size=" << BlobEncoder.PartitionedBlob.GetFormedBlobs().size());
        if (const auto& formedBlobs = BlobEncoder.PartitionedBlob.GetFormedBlobs(); !formedBlobs.empty()) {
            ui32 curWrites = RenameTmpCmdWrites(PersistRequest.Get());
            RenameFormedBlobs(formedBlobs,
                              *Parameters,
                              curWrites,
                              PersistRequest.Get(),
                              BlobEncoder,
                              ctx);
        }

        BlobEncoder.ClearPartitionedBlob(Partition, MaxBlobSize);

        BlobEncoder.NewHead.Clear();
        BlobEncoder.NewHead.Offset = Parameters->CurOffset;
    }

    //if (!t.WriteInfo->BlobsFromHead.empty()) {
    //    auto& first = t.WriteInfo->BlobsFromHead.front();
    //    // In one operation, a partition can write blocks of several transactions. Some of them can be broken down
    //    // into parts. We need to take this division into account.
    //    BlobEncoder.NewHead.PartNo += first.GetPartNo();

    //    Parameters->HeadCleared = Parameters->HeadCleared || !t.WriteInfo->BodyKeys.empty();

    //    BlobEncoder.NewPartitionedBlob(Partition,
    //                                BlobEncoder.NewHead.Offset,
    //                                first.SourceId,
    //                                first.SeqNo,
    //                                first.GetTotalParts(),
    //                                first.GetTotalSize(),
    //                                Parameters->HeadCleared, // headCleared
    //                                false,                   // needCompactHead
    //                                MaxBlobSize,
    //                                first.GetPartNo());

    //    for (auto& blob : t.WriteInfo->BlobsFromHead) {
    //        TWriteMsg msg{Max<ui64>(), Nothing(), TEvPQ::TEvWrite::TMsg{
    //            .SourceId = blob.SourceId,
    //            .SeqNo = blob.SeqNo,
    //            .PartNo = (ui16)(blob.PartData ? blob.PartData->PartNo : 0),
    //            .TotalParts = (ui16)(blob.PartData ? blob.PartData->TotalParts : 1),
    //            .TotalSize = (ui32)(blob.PartData ? blob.PartData->TotalSize : blob.UncompressedSize),
    //            .CreateTimestamp = blob.CreateTimestamp.MilliSeconds(),
    //            .ReceiveTimestamp = blob.CreateTimestamp.MilliSeconds(),
    //            .DisableDeduplication = false,
    //            .WriteTimestamp = blob.WriteTimestamp.MilliSeconds(),
    //            .Data = blob.Data,
    //            .UncompressedSize = blob.UncompressedSize,
    //            .PartitionKey = blob.PartitionKey,
    //            .ExplicitHashKey = blob.ExplicitHashKey,
    //            .External = false,
    //            .IgnoreQuotaDeadline = true,
    //            .HeartbeatVersion = std::nullopt,
    //        }, std::nullopt};
    //        msg.Internal = true;

    //        WriteInflightSize += msg.Msg.Data.size();
    //        ExecRequest(msg, *Parameters, PersistRequest.Get());
    //    }
    //}
    for (const auto& [srcId, info] : t.WriteInfo->SrcIdInfo) {
        auto& sourceIdBatch = Parameters->SourceIdBatch;
        auto sourceId = sourceIdBatch.GetSource(srcId);
        sourceId.Update(info.SeqNo, info.Offset + oldHeadOffset, CurrentTimestamp, info.ProducerEpoch);
        auto& persistInfo = TxSourceIdForPostPersist[srcId];
        persistInfo.SeqNo = info.SeqNo;
        persistInfo.Offset = info.Offset + oldHeadOffset;
        persistInfo.KafkaProducerEpoch = info.ProducerEpoch;
    }

    Parameters->FirstCommitWriteOperations = false;

    WriteInfosApplied.emplace_back(std::move(t.WriteInfo));
}

void TPartition::CommitTransaction(TSimpleSharedPtr<TTransaction>& t)
{
    const auto& ctx = ActorContext();
    if (t->Tx) {
        Y_ABORT_UNLESS(t->Predicate.Defined() && *t->Predicate);

        for (auto& operation : t->Tx->Operations) {
            if (operation.GetOnlyCheckCommitedToFinish()) {
                continue;
            }

            TUserInfoBase& userInfo = GetOrCreatePendingUser(operation.GetConsumer());

            if (!operation.GetForceCommit()) {
                Y_ABORT_UNLESS(userInfo.Offset == (i64)operation.GetCommitOffsetsBegin());
            }

            if ((i64)operation.GetCommitOffsetsEnd() < userInfo.Offset && !operation.GetReadSessionId().empty()) {
                continue; // this is stale request, answer ok for it
            }

            if (operation.GetCommitOffsetsEnd() <= CompactionBlobEncoder.StartOffset) {
                userInfo.AnyCommits = false;
                userInfo.Offset = CompactionBlobEncoder.StartOffset;
            } else if (operation.GetCommitOffsetsEnd() > BlobEncoder.EndOffset) {
                userInfo.AnyCommits = true;
                userInfo.Offset = BlobEncoder.EndOffset;
            } else {
                userInfo.AnyCommits = true;
                userInfo.Offset = operation.GetCommitOffsetsEnd();
            }

            if (operation.GetKillReadSession()) {
                userInfo.Session = "";
                userInfo.PartitionSessionId = 0;
                userInfo.Generation = 0;
                userInfo.Step = 0;
                userInfo.PipeClient = {};
            }
        }

        CommitWriteOperations(*t);
        ChangePlanStepAndTxId(t->Tx->Step, t->Tx->TxId);
        ScheduleReplyCommitDone(t->Tx->Step, t->Tx->TxId);
    } else if (t->ProposeConfig) {
        Y_ABORT_UNLESS(t->Predicate.Defined() && *t->Predicate);

        BeginChangePartitionConfig(t->ProposeConfig->Config, ctx);
        ExecChangePartitionConfig();
        ChangePlanStepAndTxId(t->ProposeConfig->Step, t->ProposeConfig->TxId);

        ScheduleReplyCommitDone(t->ProposeConfig->Step, t->ProposeConfig->TxId);
    } else {
        Y_ABORT_UNLESS(t->ChangeConfig);
        ExecChangePartitionConfig();
    }
}

void TPartition::RollbackTransaction(TSimpleSharedPtr<TTransaction>& t)
{
    auto stepAndId = GetStepAndTxId(*t->Tx);

    auto txIter = TransactionsInflight.find(stepAndId.second);
    Y_ABORT_UNLESS(!txIter.IsEnd());

    if (t->Tx) {
        Y_ABORT_UNLESS(t->Predicate.Defined());
        ChangePlanStepAndTxId(t->Tx->Step, t->Tx->TxId);
    } else if (t->ProposeConfig) {
        Y_ABORT_UNLESS(t->Predicate.Defined());
        ChangingConfig = false;
        ChangePlanStepAndTxId(t->ProposeConfig->Step, t->ProposeConfig->TxId);
    } else {
        Y_ABORT_UNLESS(t->ChangeConfig);
        ChangeConfig = nullptr;
        ChangingConfig = false;
    }
}

void TPartition::BeginChangePartitionConfig(const NKikimrPQ::TPQTabletConfig& config,
                                            const TActorContext& ctx)
{
    TSet<TString> hasReadRule;
    for (auto& [consumer, info] : UsersInfoStorage->GetAll()) {
        hasReadRule.insert(consumer);
    }

    TSet<TString> important;
    for (const auto& consumer : config.GetConsumers()) {
        if (consumer.GetImportant()) {
            important.insert(consumer.GetName());
        }
    }

    for (auto& consumer : config.GetConsumers()) {
        auto& userInfo = GetOrCreatePendingUser(consumer.GetName(), 0);

        TInstant ts = TInstant::MilliSeconds(consumer.GetReadFromTimestampsMs());
        if (!ts) {
            ts += TDuration::MilliSeconds(1);
        }
        userInfo.ReadFromTimestamp = ts;
        userInfo.Important = important.contains(consumer.GetName());

        ui64 rrGen = consumer.GetGeneration();
        if (userInfo.ReadRuleGeneration != rrGen) {
            auto act = MakeHolder<TEvPQ::TEvSetClientInfo>(0, consumer.GetName(), 0, "", 0, 0, 0, TActorId{},
                                        TEvPQ::TEvSetClientInfo::ESCI_INIT_READ_RULE, rrGen);

            auto res = PreProcessUserAct(*act, ctx);
            ChangeConfigActs.emplace_back(std::move(act));

            Y_ABORT_UNLESS(res == EProcessResult::Continue);
        }
        hasReadRule.erase(consumer.GetName());
    }

    for (auto& consumer : hasReadRule) {
        GetOrCreatePendingUser(consumer);
        auto act = MakeHolder<TEvPQ::TEvSetClientInfo>(0, consumer, 0, "", 0, 0, 0, TActorId{},
                                    TEvPQ::TEvSetClientInfo::ESCI_DROP_READ_RULE, 0);

        auto res = PreProcessUserAct(*act, ctx);
        Y_ABORT_UNLESS(res == EProcessResult::Continue);
        ChangeConfigActs.emplace_back(std::move(act));
    }
}

void TPartition::ExecChangePartitionConfig() {
    for (auto& act : ChangeConfigActs) {
        auto& userInfo = GetOrCreatePendingUser(act->ClientId);
        EmulatePostProcessUserAct(*act, userInfo, ActorContext());
    }
}

void TPartition::OnProcessTxsAndUserActsWriteComplete(const TActorContext& ctx) {
    FirstEvent = true;
    TxAffectedConsumers.clear();
    TxAffectedSourcesIds.clear();
    WriteAffectedSourcesIds.clear();
    SetOffsetAffectedConsumers.clear();
    BatchingState = ETxBatchingState::PreProcessing;
    WriteCycleSizeEstimate = 0;
    WriteKeysSizeEstimate = 0;

    if (ChangeConfig) {
        EndChangePartitionConfig(std::move(ChangeConfig->Config),
                                 PendingExplicitMessageGroups,
                                 ChangeConfig->TopicConverter,
                                 ctx);
        PendingExplicitMessageGroups.reset();
    }

    for (auto& user : AffectedUsers) {
        if (auto* actual = GetPendingUserIfExists(user)) {
            TUserInfo& userInfo = UsersInfoStorage->GetOrCreate(user, ctx);
            bool offsetHasChanged = (userInfo.Offset != actual->Offset);

            userInfo.Session = actual->Session;
            userInfo.Generation = actual->Generation;
            userInfo.Step = actual->Step;
            userInfo.Offset = actual->Offset;
            userInfo.CommittedMetadata = actual->CommittedMetadata;
            if (userInfo.Offset <= (i64)CompactionBlobEncoder.StartOffset) {
                userInfo.AnyCommits = false;
            }
            userInfo.ReadRuleGeneration = actual->ReadRuleGeneration;
            userInfo.ReadFromTimestamp = actual->ReadFromTimestamp;
            userInfo.HasReadRule = true;

            if (userInfo.Important != actual->Important) {
                if (userInfo.LabeledCounters) {
                    ScheduleDropPartitionLabeledCounters(userInfo.LabeledCounters->GetGroup());
                }
                userInfo.SetImportant(actual->Important);
            }
            if (userInfo.Important && userInfo.Offset < (i64)CompactionBlobEncoder.StartOffset) {
                userInfo.Offset = CompactionBlobEncoder.StartOffset;
            }

            if (offsetHasChanged && !userInfo.UpdateTimestampFromCache()) {
                userInfo.ActualTimestamps = false;
                ReadTimestampForOffset(user, userInfo, ctx);
            } else {
                TabletCounters.Cumulative()[COUNTER_PQ_WRITE_TIMESTAMP_CACHE_HIT].Increment(1);
            }

            if (LastOffsetHasBeenCommited(userInfo)) {
                SendReadingFinished(user);
            }
        } else if (user != CLIENTID_WITHOUT_CONSUMER) {
            auto ui = UsersInfoStorage->GetIfExists(user);
            if (ui && ui->LabeledCounters) {
                ScheduleDropPartitionLabeledCounters(ui->LabeledCounters->GetGroup());
            }

            UsersInfoStorage->Remove(user, ctx);

            // Finish all ongoing reads
            std::unordered_set<ui64> readCookies;
            for (auto& [cookie, info] : ReadInfo) {
                if (info.User == user) {
                    readCookies.insert(cookie);
                    ReplyError(ctx, info.Destination,  NPersQueue::NErrorCode::BAD_REQUEST, GetConsumerDeletedMessage(user));
                    OnReadRequestFinished(info.Destination, 0, user, ctx);
                }
            }
            for (ui64 cookie : readCookies) {
                ReadInfo.erase(cookie);
            }

            Send(ReadQuotaTrackerActor, new TEvPQ::TEvConsumerRemoved(user));
        }
    }

    ChangeConfigActs.clear();
    AnswerCurrentReplies(ctx);

    PendingUsersInfo.clear();
    AffectedUsers.clear();

    TxIdHasChanged = false;

    if (ChangeConfig) {
        ReportCounters(ctx, true);
        ChangeConfig = nullptr;
        PendingPartitionConfig = nullptr;
    }
    ChangingConfig = false;
    BatchingState = ETxBatchingState::PreProcessing;
}

void TPartition::EndChangePartitionConfig(NKikimrPQ::TPQTabletConfig&& config,
                                          const TEvPQ::TMessageGroupsPtr& explicitMessageGroups,
                                          NPersQueue::TTopicConverterPtr topicConverter,
                                          const TActorContext& ctx)
{
    Config = std::move(config);
    PartitionConfig = GetPartitionConfig(Config);
    PartitionGraph = MakePartitionGraph(Config);

    if (explicitMessageGroups) {
        for (const auto& [id, group] : *explicitMessageGroups) {
            NPQ::TPartitionKeyRange keyRange = group.KeyRange;
            TSourceIdInfo sourceId(group.SeqNo, 0, ctx.Now(), std::move(keyRange), false);
            SourceIdStorage.RegisterSourceIdInfo(id, std::move(sourceId), true);
        }
    }

    TopicConverter = topicConverter;
    NewPartition = false;

    Y_ABORT_UNLESS(Config.GetPartitionConfig().GetTotalPartitions() > 0);

    if (Config.GetPartitionStrategy().GetScaleThresholdSeconds() != SplitMergeAvgWriteBytes->GetDuration().Seconds()) {
        InitSplitMergeSlidingWindow();
    }

    Send(ReadQuotaTrackerActor, new TEvPQ::TEvChangePartitionConfig(TopicConverter, Config));
    Send(WriteQuotaTrackerActor, new TEvPQ::TEvChangePartitionConfig(TopicConverter, Config));
    TotalPartitionWriteSpeed = config.GetPartitionConfig().GetWriteSpeedInBytesPerSecond();

    if (MirroringEnabled(Config)) {
        if (Mirrorer) {
            ctx.Send(Mirrorer->Actor, new TEvPQ::TEvChangePartitionConfig(TopicConverter,
                                                                          Config));
        } else {
            CreateMirrorerActor();
        }
    } else {
        if (Mirrorer) {
            ctx.Send(Mirrorer->Actor, new TEvents::TEvPoisonPill());
            Mirrorer.Reset();
        }
    }

    if (SendChangeConfigReply) {
        SchedulePartitionConfigChanged();
    }

    if (Config.HasOffloadConfig() && !OffloadActor && !IsSupportive()) {
        OffloadActor = Register(CreateOffloadActor(Tablet, TabletID, Partition, Config.GetOffloadConfig()));
    } else if (!Config.HasOffloadConfig() && OffloadActor) {
        Send(OffloadActor, new TEvents::TEvPoisonPill());
        OffloadActor = {};
    }
}


TString TPartition::GetKeyConfig() const
{
    return Sprintf("_config_%u", Partition.InternalPartitionId);
}

void TPartition::ChangePlanStepAndTxId(ui64 step, ui64 txId)
{
    PlanStep = step;
    TxId = txId;
    TxIdHasChanged = true;
}

TPartition::EProcessResult TPartition::PreProcessImmediateTx(const NKikimrPQ::TEvProposeTransaction& tx)
{
    if (AffectedUsers.size() >= MAX_USERS) {
        return EProcessResult::Blocked;
    }
    Y_ABORT_UNLESS(tx.GetTxBodyCase() == NKikimrPQ::TEvProposeTransaction::kData);
    Y_ABORT_UNLESS(tx.HasData());
    THashSet<TString> consumers;
    for (auto& operation : tx.GetData().GetOperations()) {
        if (!operation.HasCommitOffsetsBegin() || !operation.HasCommitOffsetsEnd() || !operation.HasConsumer()) {
            continue; //Write operation - handled separately via WriteInfo
        }

        Y_ABORT_UNLESS(operation.GetCommitOffsetsBegin() <= (ui64)Max<i64>(), "Unexpected begin offset: %" PRIu64, operation.GetCommitOffsetsBegin());
        Y_ABORT_UNLESS(operation.GetCommitOffsetsEnd() <= (ui64)Max<i64>(), "Unexpected end offset: %" PRIu64, operation.GetCommitOffsetsEnd());

        const TString& user = operation.GetConsumer();
        if (TxAffectedConsumers.contains(user)) {
            return EProcessResult::Blocked;
        }
        if (!PendingUsersInfo.contains(user) && AffectedUsers.contains(user)) {
            ScheduleReplyPropose(tx,
                                 NKikimrPQ::TEvProposeTransactionResult::ABORTED,
                                 NKikimrPQ::TError::BAD_REQUEST,
                                 "the consumer has been deleted");
            return EProcessResult::ContinueDrop;
        }
        if (operation.GetCommitOffsetsBegin() > operation.GetCommitOffsetsEnd()) {
            ScheduleReplyPropose(tx,
                                 NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST,
                                 NKikimrPQ::TError::BAD_REQUEST,
                                 "incorrect offset range (begin > end)");
            return EProcessResult::ContinueDrop;
        }

        consumers.insert(user);
    }
    SetOffsetAffectedConsumers.insert(consumers.begin(), consumers.end());
    WriteKeysSizeEstimate += consumers.size();
    return EProcessResult::Continue;
}

void TPartition::ExecImmediateTx(TTransaction& t)
{
    --ImmediateTxCount;
    const auto& record = t.ProposeTransaction->GetRecord();
    Y_ABORT_UNLESS(record.GetTxBodyCase() == NKikimrPQ::TEvProposeTransaction::kData);
    Y_ABORT_UNLESS(record.HasData());


    //ToDo - check, this probably wouldn't work any longer.
    if (!t.Predicate.GetRef()) {
        ScheduleReplyPropose(record,
                             NKikimrPQ::TEvProposeTransactionResult::ABORTED,
                             NKikimrPQ::TError::BAD_REQUEST,
                             t.Message);
        return;
    }
    for (const auto& operation : record.GetData().GetOperations()) {
        if (operation.GetOnlyCheckCommitedToFinish()) {
            continue;
        }

        if (!operation.HasCommitOffsetsBegin() || !operation.HasCommitOffsetsEnd() || !operation.HasConsumer()) {
            continue; //Write operation - handled separately via WriteInfo
        }

        Y_ABORT_UNLESS(operation.GetCommitOffsetsBegin() <= (ui64)Max<i64>(), "Unexpected begin offset: %" PRIu64, operation.GetCommitOffsetsBegin());
        Y_ABORT_UNLESS(operation.GetCommitOffsetsEnd() <= (ui64)Max<i64>(), "Unexpected end offset: %" PRIu64, operation.GetCommitOffsetsEnd());

        const TString& user = operation.GetConsumer();
        if (!PendingUsersInfo.contains(user) && AffectedUsers.contains(user)) {
            ScheduleReplyPropose(record,
                                 NKikimrPQ::TEvProposeTransactionResult::ABORTED,
                                 NKikimrPQ::TError::BAD_REQUEST,
                                 "the consumer has been deleted");
            return;
        }
        TUserInfoBase& pendingUserInfo = GetOrCreatePendingUser(user);

        if (!operation.GetForceCommit() && operation.GetCommitOffsetsBegin() > operation.GetCommitOffsetsEnd()) {
            ScheduleReplyPropose(record,
                                 NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST,
                                 NKikimrPQ::TError::BAD_REQUEST,
                                 "incorrect offset range (begin > end)");
            return;
        }

        if (!operation.GetForceCommit() && pendingUserInfo.Offset != (i64)operation.GetCommitOffsetsBegin()) {
            ScheduleReplyPropose(record,
                                 NKikimrPQ::TEvProposeTransactionResult::ABORTED,
                                 NKikimrPQ::TError::BAD_REQUEST,
                                 "incorrect offset range (gap)");
            return;
        }

        if (!operation.GetForceCommit() && operation.GetCommitOffsetsEnd() > BlobEncoder.EndOffset) {
            ScheduleReplyPropose(record,
                                 NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST,
                                 NKikimrPQ::TError::BAD_REQUEST,
                                 "incorrect offset range (commit to the future)");
            return;
        }

        if (!operation.GetReadSessionId().empty() && operation.GetReadSessionId() != pendingUserInfo.Session) {
            if (IsActive() || operation.GetCommitOffsetsEnd() < BlobEncoder.EndOffset || pendingUserInfo.Offset != i64(BlobEncoder.EndOffset)) {
                ScheduleReplyPropose(record,
                            NKikimrPQ::TEvProposeTransactionResult::BAD_REQUEST,
                            NKikimrPQ::TError::BAD_REQUEST,
                            "session already dead");
                return;
            }
        }

        if ((i64)operation.GetCommitOffsetsEnd() < pendingUserInfo.Offset && !operation.GetReadSessionId().empty()) {
            continue; // this is stale request, answer ok for it
        }

        pendingUserInfo.Offset = operation.GetCommitOffsetsEnd();
    }
    CommitWriteOperations(t);

    ScheduleReplyPropose(record,
                         NKikimrPQ::TEvProposeTransactionResult::COMPLETE,
                         NKikimrPQ::TError::OK,
                         "");

    ScheduleTransactionCompleted(record);
    return;
}

TPartition::EProcessResult TPartition::PreProcessUserActionOrTransaction(TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>& act)
{
    if (AffectedUsers.size() >= MAX_USERS) {
        return EProcessResult::Blocked;
    }
    return PreProcessUserAct(*act, ActorContext());
}

bool TPartition::ExecUserActionOrTransaction(
        TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>& event, TEvKeyValue::TEvRequest*
) {
    CommitUserAct(*event);
    return true;
}

TPartition::EProcessResult TPartition::PreProcessUserActionOrTransaction(TMessage& msg)
{
    if (WriteCycleSize >= MAX_WRITE_CYCLE_SIZE) {
        return EProcessResult::Blocked;
    }

    auto result = EProcessResult::Continue;
    if (msg.IsWrite()) {
        result = PreProcessRequest(msg.GetWrite());
    } else if (msg.IsRegisterMessageGroup()) {
        result = PreProcessRequest(msg.GetRegisterMessageGroup());
    } else if (msg.IsDeregisterMessageGroup()) {
        result = PreProcessRequest(msg.GetDeregisterMessageGroup());
    } else if (msg.IsSplitMessageGroup()) {
        result = PreProcessRequest(msg.GetSplitMessageGroup());
    } else {
        Y_ABORT_UNLESS(msg.IsOwnership());
    }
    return result;
}

bool TPartition::ExecUserActionOrTransaction(TMessage& msg, TEvKeyValue::TEvRequest* request)
{
    const auto& ctx = ActorContext();
    if (!HaveWriteMsg) {
        BeginHandleRequests(request, ctx);
        if (!DiskIsFull) {
            BeginProcessWrites(ctx);
            BeginAppendHeadWithNewWrites(ctx);
        }
        HaveWriteMsg = true;
    }

    bool doReply = true;
    if (msg.IsWrite()) {
        doReply = ExecRequest(msg.GetWrite(), *Parameters, request);
    } else if (msg.IsRegisterMessageGroup()) {
        ExecRequest(msg.GetRegisterMessageGroup(), *Parameters);
    } else if (msg.IsDeregisterMessageGroup()) {
        ExecRequest(msg.GetDeregisterMessageGroup(), *Parameters);
    } else if (msg.IsSplitMessageGroup()) {
        ExecRequest(msg.GetSplitMessageGroup(), *Parameters);
    } else {
        Y_ABORT_UNLESS(msg.IsOwnership());
    }
    if (doReply) {
        EmplaceResponse(std::move(msg), ctx);
    }
    return true;
}

TPartition::EProcessResult TPartition::PreProcessUserAct(
        TEvPQ::TEvSetClientInfo& act, const TActorContext&
) {
    Y_ABORT_UNLESS(!KVWriteInProgress);

    const TString& user = act.ClientId;
    if (act.Type == TEvPQ::TEvSetClientInfo::ESCI_OFFSET) {
        if (TxAffectedConsumers.contains(user)) {
            return EProcessResult::Blocked;
        }
    }
    WriteKeysSizeEstimate += 1;
    SetOffsetAffectedConsumers.insert(user);
    return EProcessResult::Continue;
}

void TPartition::CommitUserAct(TEvPQ::TEvSetClientInfo& act) {
    const bool strictCommitOffset = (act.Type == TEvPQ::TEvSetClientInfo::ESCI_OFFSET && act.Strict);
    const TString& user = act.ClientId;
    RemoveUserAct(user);
    const auto& ctx = ActorContext();
    if (!PendingUsersInfo.contains(user) && AffectedUsers.contains(user)) {
        switch (act.Type) {
        case TEvPQ::TEvSetClientInfo::ESCI_INIT_READ_RULE:
            break;
        case TEvPQ::TEvSetClientInfo::ESCI_DROP_READ_RULE:
            return;
        default:
            ScheduleReplyError(act.Cookie,
                               NPersQueue::NErrorCode::WRONG_COOKIE,
                               "request to deleted read rule");
            return;
        }
    }
    auto& userInfo = GetOrCreatePendingUser(user);

    if (act.Type == TEvPQ::TEvSetClientInfo::ESCI_DROP_READ_RULE) {
        PQ_LOG_D("Topic '" << TopicName() << "' partition " << Partition
                    << " user " << user << " drop request");

        EmulatePostProcessUserAct(act, userInfo, ctx);

        return;
    }

    if ( //this is retry of current request, answer ok
            act.Type == TEvPQ::TEvSetClientInfo::ESCI_CREATE_SESSION
            && act.SessionId == userInfo.Session
            && act.Generation == userInfo.Generation
            && act.Step == userInfo.Step
    ) {
        auto* ui = UsersInfoStorage->GetIfExists(userInfo.User);
        auto ts = ui ? GetTime(*ui, userInfo.Offset) : std::make_pair<TInstant, TInstant>(TInstant::Zero(), TInstant::Zero());

        userInfo.PipeClient = act.PipeClient;
        ScheduleReplyGetClientOffsetOk(act.Cookie,
                                       userInfo.Offset,
                                       ts.first, ts.second, ui->AnyCommits);

        return;
    }

    if (act.Type != TEvPQ::TEvSetClientInfo::ESCI_CREATE_SESSION && act.Type != TEvPQ::TEvSetClientInfo::ESCI_INIT_READ_RULE
            && !act.SessionId.empty() && userInfo.Session != act.SessionId //request to wrong session
            && (act.Type != TEvPQ::TEvSetClientInfo::ESCI_DROP_SESSION || !userInfo.Session.empty()) //but allow DropSession request when session is already dropped - for idempotence
            || (act.ClientId != CLIENTID_WITHOUT_CONSUMER && act.Type == TEvPQ::TEvSetClientInfo::ESCI_CREATE_SESSION && !userInfo.Session.empty()
                 && (act.Generation < userInfo.Generation || act.Generation == userInfo.Generation && act.Step <= userInfo.Step))) { //old generation request
        TabletCounters.Cumulative()[COUNTER_PQ_SET_CLIENT_OFFSET_ERROR].Increment(1);

        ScheduleReplyError(act.Cookie,
                           NPersQueue::NErrorCode::WRONG_COOKIE,
                           TStringBuilder() << "set offset in already dead session " << act.SessionId << " actual is " << userInfo.Session);

        return;
    }

    if (!act.SessionId.empty() && act.Type == TEvPQ::TEvSetClientInfo::ESCI_OFFSET && (i64)act.Offset <= userInfo.Offset) { //this is stale request, answer ok for it
        ScheduleReplyOk(act.Cookie);
        return;
    }

    if (strictCommitOffset && act.Offset < CompactionBlobEncoder.StartOffset) {
        // strict commit to past, reply error
        TabletCounters.Cumulative()[COUNTER_PQ_SET_CLIENT_OFFSET_ERROR].Increment(1);
        ScheduleReplyError(act.Cookie,
                           NPersQueue::NErrorCode::SET_OFFSET_ERROR_COMMIT_TO_PAST,
                           TStringBuilder() << "set offset " <<  act.Offset << " to past for consumer " << act.ClientId << " actual start offset is " << BlobEncoder.StartOffset);

        return;
    }

    //request in correct session - make it

    ui64 offset = (act.Type == TEvPQ::TEvSetClientInfo::ESCI_OFFSET ? act.Offset : userInfo.Offset);
    ui64 readRuleGeneration = userInfo.ReadRuleGeneration;

    if (act.Type == TEvPQ::TEvSetClientInfo::ESCI_INIT_READ_RULE) {
        readRuleGeneration = act.ReadRuleGeneration;
        offset = 0;
        PQ_LOG_D("Topic '" << TopicName() << "' partition " << Partition
                    << " user " << act.ClientId << " reinit request with generation " << readRuleGeneration
        );
    }

    Y_ABORT_UNLESS(offset <= (ui64)Max<i64>(), "Offset is too big: %" PRIu64, offset);

    if (offset > BlobEncoder.EndOffset) {
        if (strictCommitOffset) {
            TabletCounters.Cumulative()[COUNTER_PQ_SET_CLIENT_OFFSET_ERROR].Increment(1);
            ScheduleReplyError(act.Cookie,
                            NPersQueue::NErrorCode::SET_OFFSET_ERROR_COMMIT_TO_FUTURE,
                            TStringBuilder() << "strict commit can't set offset " <<  act.Offset << " to future, consumer " << act.ClientId << ", actual end offset is " << BlobEncoder.EndOffset);

            return;
        }
        PQ_LOG_W("commit to future - topic " << TopicName() << " partition " << Partition
                << " client " << act.ClientId << " EndOffset " << BlobEncoder.EndOffset << " offset " << offset
        );
        act.Offset = BlobEncoder.EndOffset;
/*
        TODO:
        TabletCounters.Cumulative()[COUNTER_PQ_SET_CLIENT_OFFSET_ERROR].Increment(1);
        ReplyError(ctx, ev->Cookie, NPersQueue::NErrorCode::SET_OFFSET_ERROR_COMMIT_TO_FUTURE,
            TStringBuilder() << "can't commit to future. Offset " << offset << " EndOffset " << EndOffset);
        userInfo.UserActrs.pop_front();
        continue;
*/
    }

    if (!IsActive() && act.Type == TEvPQ::TEvSetClientInfo::ESCI_OFFSET && static_cast<i64>(BlobEncoder.EndOffset) == userInfo.Offset && offset < BlobEncoder.EndOffset) {
        TabletCounters.Cumulative()[COUNTER_PQ_SET_CLIENT_OFFSET_ERROR].Increment(1);
        ScheduleReplyError(act.Cookie,
                           NPersQueue::NErrorCode::SET_OFFSET_ERROR_COMMIT_TO_PAST,
                           TStringBuilder() << "set offset " <<  act.Offset << " to past for consumer " << act.ClientId << " for inactive partition");

        return;
    }

    return EmulatePostProcessUserAct(act, userInfo, ActorContext());
}

void TPartition::EmulatePostProcessUserAct(const TEvPQ::TEvSetClientInfo& act,
                                           TUserInfoBase& userInfo,
                                           const TActorContext&)
{
    const TString& user = act.ClientId;
    ui64 offset = act.Offset;
    const std::optional<TString>& committedMetadata = act.CommittedMetadata ? act.CommittedMetadata : userInfo.CommittedMetadata;
    const TString& session = act.SessionId;
    ui32 generation = act.Generation;
    ui32 step = act.Step;
    const ui64 readRuleGeneration = act.ReadRuleGeneration;

    bool createSession = act.Type == TEvPQ::TEvSetClientInfo::ESCI_CREATE_SESSION;
    bool dropSession = act.Type == TEvPQ::TEvSetClientInfo::ESCI_DROP_SESSION;
    bool commitNotFromReadSession = (act.Type == TEvPQ::TEvSetClientInfo::ESCI_OFFSET && act.SessionId.empty());

    if (act.Type == TEvPQ::TEvSetClientInfo::ESCI_DROP_READ_RULE) {
        userInfo.ReadRuleGeneration = 0;
        userInfo.Session = "";
        userInfo.Generation = userInfo.Step = 0;
        userInfo.Offset = 0;
        userInfo.AnyCommits = false;

        PQ_LOG_D("Topic '" << TopicName() << "' partition " << Partition << " user " << user
                    << " drop done"
        );
        PendingUsersInfo.erase(user);
    } else if (act.Type == TEvPQ::TEvSetClientInfo::ESCI_INIT_READ_RULE) {
        PQ_LOG_D("Topic '" << TopicName() << "' partition " << Partition << " user " << user
                    << " reinit with generation " << readRuleGeneration << " done"
        );

        userInfo.ReadRuleGeneration = readRuleGeneration;
        userInfo.Session = "";
        userInfo.PartitionSessionId = 0;
        userInfo.Generation = userInfo.Step = 0;
        userInfo.Offset = 0;
        userInfo.AnyCommits = false;

        if (userInfo.Important) {
            userInfo.Offset = CompactionBlobEncoder.StartOffset;
        }
    } else {
        if (createSession || dropSession) {
            offset = userInfo.Offset;

            auto *ui = UsersInfoStorage->GetIfExists(userInfo.User);
            auto ts = ui ? GetTime(*ui, userInfo.Offset) : std::make_pair<TInstant, TInstant>(TInstant::Zero(), TInstant::Zero());

            ScheduleReplyGetClientOffsetOk(act.Cookie,
                                           offset,
                                           ts.first, ts.second, ui ? ui->AnyCommits : false);
        } else {
            ScheduleReplyOk(act.Cookie);
        }

        if (createSession) {
            userInfo.Session = session;
            userInfo.Generation = generation;
            userInfo.Step = step;
            userInfo.PipeClient = act.PipeClient;
            userInfo.PartitionSessionId = act.PartitionSessionId;
        } else if ((dropSession && act.PipeClient == userInfo.PipeClient) || commitNotFromReadSession) {
            userInfo.Session = "";
            userInfo.PartitionSessionId = 0;
            userInfo.Generation = 0;
            userInfo.Step = 0;
            userInfo.PipeClient = {};
        }

        Y_ABORT_UNLESS(offset <= (ui64)Max<i64>(), "Unexpected Offset: %" PRIu64, offset);
        PQ_LOG_D("Topic '" << TopicName() << "' partition " << Partition << " user " << user
                    << (createSession || dropSession ? " session" : " offset")
                    << " is set to " << offset << " (startOffset " << BlobEncoder.StartOffset << ") session " << session
        );

        userInfo.Offset = offset;
        userInfo.CommittedMetadata = committedMetadata;
        userInfo.AnyCommits = userInfo.Offset > (i64)BlobEncoder.StartOffset;

        if (LastOffsetHasBeenCommited(userInfo)) {
            SendReadingFinished(user);
        }

        auto counter = createSession ? COUNTER_PQ_CREATE_SESSION_OK : (dropSession ? COUNTER_PQ_DELETE_SESSION_OK : COUNTER_PQ_SET_CLIENT_OFFSET_OK);
        TabletCounters.Cumulative()[counter].Increment(1);
    }
}

void TPartition::ScheduleReplyOk(const ui64 dst)
{
    Replies.emplace_back(Tablet,
                         MakeReplyOk(dst).Release());
}

void TPartition::ScheduleReplyGetClientOffsetOk(const ui64 dst,
                                                const i64 offset,
                                                const TInstant writeTimestamp,
                                                const TInstant createTimestamp,
                                                bool consumerHasAnyCommits,
                                                const std::optional<TString>& committedMetadata)
{
    Replies.emplace_back(Tablet,
                         MakeReplyGetClientOffsetOk(dst,
                                                    offset,
                                                    writeTimestamp,
                                                    createTimestamp,
                                                    consumerHasAnyCommits, committedMetadata).Release());

}

void TPartition::ScheduleReplyError(const ui64 dst,
                                    NPersQueue::NErrorCode::EErrorCode errorCode,
                                    const TString& error)
{
    Replies.emplace_back(Tablet,
                         MakeReplyError(dst,
                                        errorCode,
                                        error).Release());
}

void TPartition::ScheduleReplyPropose(const NKikimrPQ::TEvProposeTransaction& event,
                                      NKikimrPQ::TEvProposeTransactionResult::EStatus statusCode,
                                      NKikimrPQ::TError::EKind kind,
                                      const TString& reason)
{
    PQ_LOG_D("schedule TEvPersQueue::TEvProposeTransactionResult(" <<
             NKikimrPQ::TEvProposeTransactionResult_EStatus_Name(statusCode) <<
             ")" <<
             ", reason=" << reason);
    Replies.emplace_back(ActorIdFromProto(event.GetSourceActor()),
                         MakeReplyPropose(event,
                                          statusCode,
                                          kind, reason).Release());
}

void TPartition::ScheduleReplyCommitDone(ui64 step, ui64 txId)
{
    Replies.emplace_back(Tablet,
                         MakeCommitDone(step, txId).Release());
}

void TPartition::ScheduleDropPartitionLabeledCounters(const TString& group)
{
    Replies.emplace_back(Tablet,
                         MakeHolder<TEvPQ::TEvPartitionLabeledCountersDrop>(Partition, group).Release());
}

void TPartition::SchedulePartitionConfigChanged()
{
    Replies.emplace_back(Tablet,
                         MakeHolder<TEvPQ::TEvPartitionConfigChanged>(Partition).Release());
}

void TPartition::ScheduleDeletePartitionDone()
{
    Replies.emplace_back(Tablet,
                         MakeHolder<TEvPQ::TEvDeletePartitionDone>(Partition).Release());
}

void TPartition::AddCmdDeleteRange(NKikimrClient::TKeyValueRequest& request,
                                   const TKeyPrefix& ikey, const TKeyPrefix& ikeyDeprecated)
{
    auto del = request.AddCmdDeleteRange();
    auto range = del->MutableRange();
    range->SetFrom(ikey.Data(), ikey.Size());
    range->SetTo(ikey.Data(), ikey.Size());
    range->SetIncludeFrom(true);
    range->SetIncludeTo(true);

    del = request.AddCmdDeleteRange();
    range = del->MutableRange();
    range->SetFrom(ikeyDeprecated.Data(), ikeyDeprecated.Size());
    range->SetTo(ikeyDeprecated.Data(), ikeyDeprecated.Size());
    range->SetIncludeFrom(true);
    range->SetIncludeTo(true);
}

void TPartition::AddCmdWrite(NKikimrClient::TKeyValueRequest& request,
                             const TKeyPrefix& ikey, const TKeyPrefix& ikeyDeprecated,
                             ui64 offset, ui32 gen, ui32 step, const TString& session,
                             ui64 readOffsetRewindSum,
                             ui64 readRuleGeneration,
                             bool anyCommits, const std::optional<TString>& committedMetadata)
{
    TBuffer idata;
    {
        NKikimrPQ::TUserInfo userData;
        userData.SetOffset(offset);
        userData.SetGeneration(gen);
        userData.SetStep(step);
        userData.SetSession(session);
        userData.SetOffsetRewindSum(readOffsetRewindSum);
        userData.SetReadRuleGeneration(readRuleGeneration);
        userData.SetAnyCommits(anyCommits);
        if (committedMetadata.has_value()) {
            userData.SetCommittedMetadata(*committedMetadata);
        }

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD userData.SerializeToString(&out);

        idata.Append(out.c_str(), out.size());
    }

    auto write = request.AddCmdWrite();
    write->SetKey(ikey.Data(), ikey.Size());
    write->SetValue(idata.Data(), idata.Size());
    write->SetStorageChannel(NKikimrClient::TKeyValueRequest::INLINE);

    TBuffer idataDeprecated = NDeprecatedUserData::Serialize(offset, gen, step, session);

    write = request.AddCmdWrite();
    write->SetKey(ikeyDeprecated.Data(), ikeyDeprecated.Size());
    write->SetValue(idataDeprecated.Data(), idataDeprecated.Size());
    write->SetStorageChannel(NKikimrClient::TKeyValueRequest::INLINE);
}

void TPartition::AddCmdWriteTxMeta(NKikimrClient::TKeyValueRequest& request)
{
    if (!TxIdHasChanged) {
        return;
    }

    Y_ABORT_UNLESS(PlanStep.Defined());
    Y_ABORT_UNLESS(TxId.Defined());

    TKeyPrefix ikey(TKeyPrefix::TypeTxMeta, Partition);

    NKikimrPQ::TPartitionTxMeta meta;
    meta.SetPlanStep(*PlanStep);
    meta.SetTxId(*TxId);

    TString out;
    Y_PROTOBUF_SUPPRESS_NODISCARD meta.SerializeToString(&out);

    auto write = request.AddCmdWrite();
    write->SetKey(ikey.Data(), ikey.Size());
    write->SetValue(out.c_str(), out.size());
    write->SetStorageChannel(NKikimrClient::TKeyValueRequest::INLINE);
}

void TPartition::AddCmdWriteUserInfos(NKikimrClient::TKeyValueRequest& request)
{
    for (auto& user : AffectedUsers) {
        TKeyPrefix ikey(TKeyPrefix::TypeInfo, Partition, TKeyPrefix::MarkUser);
        ikey.Append(user.c_str(), user.size());
        TKeyPrefix ikeyDeprecated(TKeyPrefix::TypeInfo, Partition, TKeyPrefix::MarkUserDeprecated);
        ikeyDeprecated.Append(user.c_str(), user.size());

        if (TUserInfoBase* userInfo = GetPendingUserIfExists(user)) {
            auto *ui = UsersInfoStorage->GetIfExists(user);

            AddCmdWrite(request,
                        ikey, ikeyDeprecated,
                        userInfo->Offset, userInfo->Generation, userInfo->Step,
                        userInfo->Session,
                        ui ? ui->ReadOffsetRewindSum : 0,
                        userInfo->ReadRuleGeneration,
                        userInfo->AnyCommits, userInfo->CommittedMetadata);
        } else {
            AddCmdDeleteRange(request,
                              ikey, ikeyDeprecated);
        }
    }
}

void TPartition::AddCmdWriteConfig(NKikimrClient::TKeyValueRequest& request)
{
    if (!ChangeConfig) {
        return;
    }

    TString key = GetKeyConfig();

    TString data;
    Y_ABORT_UNLESS(ChangeConfig->Config.SerializeToString(&data));

    auto write = request.AddCmdWrite();
    write->SetKey(key.data(), key.size());
    write->SetValue(data.data(), data.size());
    write->SetStorageChannel(NKikimrClient::TKeyValueRequest::INLINE);
}

TUserInfoBase& TPartition::GetOrCreatePendingUser(const TString& user,
                                                  TMaybe<ui64> readRuleGeneration)
{
    TUserInfoBase* userInfo = nullptr;
    auto pendingUserIt = PendingUsersInfo.find(user);
    if (pendingUserIt == PendingUsersInfo.end()) {
        auto userIt = UsersInfoStorage->GetIfExists(user);
        auto [newPendingUserIt, _] = PendingUsersInfo.emplace(user, UsersInfoStorage->CreateUserInfo(user, readRuleGeneration));

        if (userIt) {
            newPendingUserIt->second.Session = userIt->Session;
            newPendingUserIt->second.PartitionSessionId = userIt->PartitionSessionId;
            newPendingUserIt->second.PipeClient = userIt->PipeClient;

            newPendingUserIt->second.Generation = userIt->Generation;
            newPendingUserIt->second.Step = userIt->Step;
            newPendingUserIt->second.Offset = userIt->Offset;
            newPendingUserIt->second.CommittedMetadata = userIt->CommittedMetadata;
            newPendingUserIt->second.ReadRuleGeneration = userIt->ReadRuleGeneration;
            newPendingUserIt->second.Important = userIt->Important;
            newPendingUserIt->second.ReadFromTimestamp = userIt->ReadFromTimestamp;
        }

        userInfo = &newPendingUserIt->second;
    } else {
        userInfo = &pendingUserIt->second;
    }
    AffectedUsers.insert(user);

    return *userInfo;
}

TUserInfoBase* TPartition::GetPendingUserIfExists(const TString& user)
{
    if (auto i = PendingUsersInfo.find(user); i != PendingUsersInfo.end()) {
        return &i->second;
    }

    return nullptr;
}

THolder<TEvPQ::TEvProxyResponse> TPartition::MakeReplyOk(const ui64 dst)
{
    auto response = MakeHolder<TEvPQ::TEvProxyResponse>(dst);
    NKikimrClient::TResponse& resp = *response->Response;

    resp.SetStatus(NMsgBusProxy::MSTATUS_OK);
    resp.SetErrorCode(NPersQueue::NErrorCode::OK);

    return response;
}

THolder<TEvPQ::TEvProxyResponse> TPartition::MakeReplyGetClientOffsetOk(const ui64 dst,
                                                                        const i64 offset,
                                                                        const TInstant writeTimestamp,
                                                                        const TInstant createTimestamp,
                                                                        bool consumerHasAnyCommits,
                                                                        const std::optional<TString>& committedMetadata)
{
    auto response = MakeHolder<TEvPQ::TEvProxyResponse>(dst);
    NKikimrClient::TResponse& resp = *response->Response;

    resp.SetStatus(NMsgBusProxy::MSTATUS_OK);
    resp.SetErrorCode(NPersQueue::NErrorCode::OK);

    auto user = resp.MutablePartitionResponse()->MutableCmdGetClientOffsetResult();
    if (offset > -1)
        user->SetOffset(offset);

    if (committedMetadata.has_value()) {
        user->SetCommittedMetadata(*committedMetadata);
    }
    if (writeTimestamp)
        user->SetWriteTimestampMS(writeTimestamp.MilliSeconds());
    if (createTimestamp) {
        Y_ABORT_UNLESS(writeTimestamp);
        user->SetCreateTimestampMS(createTimestamp.MilliSeconds());
    }
    user->SetEndOffset(BlobEncoder.EndOffset);
    user->SetWriteTimestampEstimateMS(WriteTimestampEstimate.MilliSeconds());
    if (IsActive() || (offset > -1 && offset < (i64)BlobEncoder.EndOffset)) {
        user->SetSizeLag(GetSizeLag(offset));
    } else {
        user->SetSizeLag(0);
    }
    user->SetClientHasAnyCommits(consumerHasAnyCommits);
    return response;
}
THolder<TEvPQ::TEvError> TPartition::MakeReplyError(const ui64 dst,
                                                    NPersQueue::NErrorCode::EErrorCode errorCode,
                                                    const TString& error)
{
    //
    // FIXME(abcdef): в ReplyPersQueueError есть дополнительные действия
    //
    return MakeHolder<TEvPQ::TEvError>(errorCode, error, dst);
}

THolder<TEvPersQueue::TEvProposeTransactionResult> TPartition::MakeReplyPropose(const NKikimrPQ::TEvProposeTransaction& event,
                                                                                NKikimrPQ::TEvProposeTransactionResult::EStatus statusCode,
                                                                                NKikimrPQ::TError::EKind kind,
                                                                                const TString& reason)
{
    auto response = MakeHolder<TEvPersQueue::TEvProposeTransactionResult>();

    response->Record.SetOrigin(TabletID);
    response->Record.SetStatus(statusCode);
    response->Record.SetTxId(event.GetTxId());

    if (kind != NKikimrPQ::TError::OK) {
        auto* error = response->Record.MutableErrors()->Add();
        error->SetKind(kind);
        error->SetReason(reason);
    }

    return response;
}

THolder<TEvPQ::TEvTxCommitDone> TPartition::MakeCommitDone(ui64 step, ui64 txId)
{
    return MakeHolder<TEvPQ::TEvTxCommitDone>(step, txId, Partition);
}

void TPartition::ScheduleUpdateAvailableSize(const TActorContext& ctx) {
    ctx.Schedule(UPDATE_AVAIL_SIZE_INTERVAL, new TEvPQ::TEvUpdateAvailableSize());
}

void TPartition::ClearOldHead(const ui64 offset, const ui16 partNo) {
    for (auto it = BlobEncoder.HeadKeys.rbegin(); it != BlobEncoder.HeadKeys.rend(); ++it) {
        if (it->Key.GetOffset() > offset || it->Key.GetOffset() == offset && it->Key.GetPartNo() >= partNo) {
            // The repackaged blocks will be deleted after writing.
            DefferedKeysForDeletion.push_back(std::move(it->BlobKeyToken));
        } else {
            break;
        }
    }
}

ui32 TPartition::NextChannel(bool isHead, ui32 blobSize) {

    if (isHead) {
        ui32 i = 0;
        for (ui32 j = 1; j < TotalChannelWritesByHead.size(); ++j) {
            if (TotalChannelWritesByHead[j] < TotalChannelWritesByHead[i])
                i = j;
        }
        TotalChannelWritesByHead[i] += blobSize;

        return i;
    };

    ui32 res = Channel;
    Channel = (Channel + 1) % NumChannels;

    return res;
}

void TPartition::Handle(TEvPQ::TEvApproveWriteQuota::TPtr& ev, const TActorContext& ctx) {
    const ui64 cookie = ev->Get()->Cookie;
    PQ_LOG_D("Got quota." <<
            " Topic: \"" << TopicName() << "\"." <<
            " Partition: " << Partition << ": Cookie: " << cookie
    );

    // Search for proper request
    Y_ABORT_UNLESS(TopicQuotaRequestCookie == cookie);
    ConsumeBlobQuota();
    TopicQuotaRequestCookie = 0;
    for (auto& r : QuotaWaitingRequests) {
        r.WaitQuotaSpan.End();
    }

    RemoveMessagesToQueue(QuotaWaitingRequests);

    // Metrics
    TopicQuotaWaitTimeForCurrentBlob = ev->Get()->AccountQuotaWaitTime;
    PartitionQuotaWaitTimeForCurrentBlob = ev->Get()->PartitionQuotaWaitTime;
    if (TopicWriteQuotaWaitCounter) {
        TopicWriteQuotaWaitCounter->IncFor(TopicQuotaWaitTimeForCurrentBlob.MilliSeconds());
    }

    if (NeedDeletePartition) {
        // deferred TEvPQ::TEvDeletePartition
        DeletePartitionState = DELETION_INITED;
    } else {
        RequestBlobQuota();
    }

    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(NReadQuoterEvents::TEvQuotaCountersUpdated::TPtr& ev, const TActorContext&) {
    if (ev->Get()->ForWriteQuota) {
        PQ_LOG_ALERT("Got TEvQuotaCountersUpdated for write counters, this is unexpected. Event ignored");
        return;
    } else if (PartitionCountersLabeled) {
        PartitionCountersLabeled->GetCounters()[METRIC_READ_INFLIGHT_LIMIT_THROTTLED].Set(ev->Get()->AvgInflightLimitThrottledMicroseconds);
    }
}

size_t TPartition::GetQuotaRequestSize(const TEvKeyValue::TEvRequest& request) {
    if (AppData()->PQConfig.GetQuotingConfig().GetTopicWriteQuotaEntityToLimit() ==
        NKikimrPQ::TPQConfig::TQuotingConfig::USER_PAYLOAD_SIZE) {
        return WriteNewSize;
    } else {
        return std::accumulate(request.Record.GetCmdWrite().begin(), request.Record.GetCmdWrite().end(), 0ul,
                               [](size_t sum, const auto& el) { return sum + el.GetValue().size(); });
    }
}

void TPartition::CreateMirrorerActor() {
    Mirrorer = MakeHolder<TMirrorerInfo>(
        Register(new TMirrorer(Tablet, SelfId(), TopicConverter, Partition.InternalPartitionId, IsLocalDC, BlobEncoder.EndOffset, Config.GetPartitionConfig().GetMirrorFrom(), TabletCounters)),
        TabletCounters
    );
}

bool IsQuotingEnabled(const NKikimrPQ::TPQConfig& pqConfig,
                      bool isLocalDC)
{
    const auto& quotingConfig = pqConfig.GetQuotingConfig();
    return isLocalDC && quotingConfig.GetEnableQuoting() && !pqConfig.GetTopicsAreFirstClassCitizen();
}

bool TPartition::IsQuotingEnabled() const
{
    return NPQ::IsQuotingEnabled(AppData()->PQConfig,
                                 IsLocalDC);
}

void TPartition::Handle(TEvPQ::TEvSubDomainStatus::TPtr& ev, const TActorContext& ctx)
{
    const TEvPQ::TEvSubDomainStatus& event = *ev->Get();

    bool statusChanged = SubDomainOutOfSpace != event.SubDomainOutOfSpace();
    SubDomainOutOfSpace = event.SubDomainOutOfSpace();

    if (statusChanged) {
        PQ_LOG_I("SubDomainOutOfSpace was changed." <<
            " Topic: \"" << TopicName() << "\"." <<
            " Partition: " << Partition << "." <<
            " SubDomainOutOfSpace: " << SubDomainOutOfSpace
        );

        if (!SubDomainOutOfSpace) {
            ProcessTxsAndUserActs(ctx);
        }
    }
}

void TPartition::Handle(TEvPQ::TEvCheckPartitionStatusRequest::TPtr& ev, const TActorContext&) {
    auto& record = ev->Get()->Record;

    if (Partition.InternalPartitionId != record.GetPartition()) {
        PQ_LOG_I("TEvCheckPartitionStatusRequest for wrong partition " << record.GetPartition() << "." <<
            " Topic: \"" << TopicName() << "\"." <<
            " Partition: " << Partition << "."
        );
        return;
    }

    auto response = MakeHolder<TEvPQ::TEvCheckPartitionStatusResponse>();
    response->Record.SetStatus(PartitionConfig ? PartitionConfig->GetStatus() : NKikimrPQ::ETopicPartitionStatus::Active);

    if (record.HasSourceId()) {
        auto sit = SourceIdStorage.GetInMemorySourceIds().find(NSourceIdEncoding::EncodeSimple(record.GetSourceId()));
        if (sit != SourceIdStorage.GetInMemorySourceIds().end()) {
            response->Record.SetSeqNo(sit->second.SeqNo);
        }
    }

    Send(ev->Sender, response.Release());
}

void TPartition::HandleOnInit(TEvPQ::TEvDeletePartition::TPtr& ev, const TActorContext&)
{
    PQ_LOG_D("HandleOnInit TEvPQ::TEvDeletePartition");

    Y_ABORT_UNLESS(IsSupportive());

    AddPendingEvent(ev);
}

template <>
void TPartition::ProcessPendingEvent(std::unique_ptr<TEvPQ::TEvDeletePartition> ev, const TActorContext& ctx)
{
    Y_UNUSED(ev);

    Y_ABORT_UNLESS(IsSupportive());
    Y_ABORT_UNLESS(DeletePartitionState == DELETION_NOT_INITED);

    NeedDeletePartition = true;

    if (TopicQuotaRequestCookie != 0) {
        // wait for TEvPQ::TEvApproveWriteQuota
        return;
    }

    DeletePartitionState = DELETION_INITED;

    ProcessTxsAndUserActs(ctx);
}

void TPartition::Handle(TEvPQ::TEvDeletePartition::TPtr& ev, const TActorContext& ctx)
{
    PQ_LOG_D("Handle TEvPQ::TEvDeletePartition");

    ProcessPendingEvent(ev, ctx);
}

void TPartition::ScheduleNegativeReplies()
{
    auto processQueue = [&](std::deque<TUserActionAndTransactionEvent>& queue) {
        for (auto& event : queue) {
            std::visit(TOverloaded{
                [this](TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>& v) {
                    ScheduleNegativeReply(*v);
                },
                [this](TSimpleSharedPtr<TTransaction>& v) {
                    if (v->ProposeTransaction) {
                        ScheduleNegativeReply(*v->ProposeTransaction);
                    } else {
                        ScheduleNegativeReply(*v);
                    }
                },
                [this](TMessage& v) {
                    ScheduleNegativeReply(v);
                }
            }, event.Event);
        }
        queue.clear();
    };

    processQueue(UserActionAndTransactionEvents);
    processQueue(UserActionAndTxPendingCommit);
}

void TPartition::AddCmdDeleteRangeForAllKeys(TEvKeyValue::TEvRequest& request)
{
    NPQ::AddCmdDeleteRange(request, TKeyPrefix::TypeInfo, Partition);
    NPQ::AddCmdDeleteRange(request, TKeyPrefix::TypeData, Partition);
    NPQ::AddCmdDeleteRange(request, TKeyPrefix::TypeTmpData, Partition);
    NPQ::AddCmdDeleteRange(request, TKeyPrefix::TypeMeta, Partition);
    NPQ::AddCmdDeleteRange(request, TKeyPrefix::TypeTxMeta, Partition);
}

void TPartition::ScheduleNegativeReply(const TEvPQ::TEvSetClientInfo&)
{
    Y_ABORT("The supportive partition does not accept read operations");
}

void TPartition::ScheduleNegativeReply(const TEvPersQueue::TEvProposeTransaction&)
{
    Y_ABORT("The supportive partition does not accept immediate transactions");
}

void TPartition::ScheduleNegativeReply(const TTransaction&)
{
    Y_ABORT("The supportive partition does not accept distribute transactions");
}

void TPartition::ScheduleNegativeReply(const TMessage& msg)
{
    ScheduleReplyError(msg.GetCookie(), NPersQueue::NErrorCode::ERROR, "The transaction is completed");
}

void TPartition::ScheduleTransactionCompleted(const NKikimrPQ::TEvProposeTransaction& tx)
{
    Y_ABORT_UNLESS(tx.GetTxBodyCase() == NKikimrPQ::TEvProposeTransaction::kData);
    Y_ABORT_UNLESS(tx.HasData());

    TMaybe<TWriteId> writeId;
    if (tx.GetData().HasWriteId()) {
        writeId = GetWriteId(tx.GetData());
    }

    Replies.emplace_back(Tablet,
                         MakeHolder<TEvPQ::TEvTransactionCompleted>(writeId).Release());
}

void TPartition::ProcessPendingEvents(const TActorContext& ctx)
{
    PQ_LOG_D("Process pending events. Count " << PendingEvents.size());

    while (!PendingEvents.empty()) {
        auto ev = std::move(PendingEvents.front());
        PendingEvents.pop_front();

        auto visitor = [this, &ctx](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            ProcessPendingEvent(std::forward<T>(v), ctx);
        };

        std::visit(visitor, std::move(ev));
    }
}

const NKikimrPQ::TPQTabletConfig::TPartition* TPartition::GetPartitionConfig(const NKikimrPQ::TPQTabletConfig& config)
{
    return NPQ::GetPartitionConfig(config, Partition.OriginalPartitionId);
}

bool TPartition::IsSupportive() const
{
    return Partition.IsSupportivePartition();
}

void TPartition::AttachPersistRequestSpan(NWilson::TSpan& span)
{
    if (span) {
        if (!PersistRequestSpan) {
            PersistRequestSpan = NWilson::TSpan(TWilsonTopic::TopicDetailed, NWilson::TTraceId::NewTraceId(span.GetTraceId().GetVerbosity(), Max<ui32>()), "Topic.Partition.PersistRequest");
        }
        span.Link(PersistRequestSpan.GetTraceId());
    }
}

} // namespace NKikimr::NPQ
