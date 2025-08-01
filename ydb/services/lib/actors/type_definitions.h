#pragma once

#include "ydb/core/persqueue/utils.h"
#include <ydb/library/persqueue/topic_parser/topic_parser.h>

#include <ydb/library/actors/core/actor.h>

#include <util/generic/hash.h>
#include <util/generic/map.h>
#include <util/generic/maybe.h>
#include <util/generic/vector.h>

namespace NKikimr::NGRpcProxy {

struct TPartitionInfo {
    ui64 TabletId;
};

struct TTopicInitInfo {
    NPersQueue::TTopicConverterPtr TopicNameConverter;
    ui64 TabletID;
    TString CloudId;
    TString DbId;
    TString DbPath;
    bool IsServerless = false;
    TString FolderId;
    NKikimrPQ::TPQTabletConfig::EMeteringMode MeteringMode;
    THashMap<ui32, TPartitionInfo> Partitions;
    std::shared_ptr<NPQ::TPartitionGraph> PartitionGraph;
};

using TTopicInitInfoMap = THashMap<TString, TTopicInitInfo>;

struct TTopicHolder {
    using TPtr = std::shared_ptr<TTopicHolder>;

    ui64 TabletID = 0;
    TActorId PipeClient;
    bool ACLRequestInfly = false;
    TString CloudId;
    TString DbId;
    TString DbPath;
    bool IsServerless;
    TString FolderId;
    NKikimrPQ::TPQTabletConfig::EMeteringMode MeteringMode;
    NPersQueue::TDiscoveryConverterPtr DiscoveryConverter;
    NPersQueue::TTopicConverterPtr FullConverter;
    TMaybe<TString> CdcStreamPath;

    TVector<ui32> Groups;
    THashMap<ui32, TPartitionInfo> Partitions;
    std::shared_ptr<NPQ::TPartitionGraph> PartitionGraph;

    TTopicHolder() {
    }

    explicit TTopicHolder(const TTopicInitInfo& info) {
        TabletID = info.TabletID;
        ACLRequestInfly = false;
        CloudId = info.CloudId;
        DbId = info.DbId;
        DbPath = info.DbPath;
        IsServerless = info.IsServerless;
        FolderId = info.FolderId;
        MeteringMode = info.MeteringMode;
        FullConverter = info.TopicNameConverter;
        Partitions = info.Partitions;
        PartitionGraph = info.PartitionGraph;
    }

    inline static TTopicHolder::TPtr FromTopicInfo(const TTopicInitInfo& info) {
        return std::make_shared<TTopicHolder>(info);
    }
};

} // namespace NKikimr::NGRpcProxy
