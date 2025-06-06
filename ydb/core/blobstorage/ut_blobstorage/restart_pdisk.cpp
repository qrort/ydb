#include <ydb/core/blobstorage/base/blobstorage_events.h>
#include <ydb/core/blobstorage/nodewarden/node_warden_impl.h>
#include <ydb/core/blobstorage/ut_blobstorage/lib/env.h>

Y_UNIT_TEST_SUITE(BSCRestartPDisk) {

    Y_UNIT_TEST(RestartNotAllowed) {
        TEnvironmentSetup env({
            .NodeCount = 10,
            .Erasure = TBlobStorageGroupType::Erasure4Plus2Block
        });

        std::unordered_map<TPDiskId, ui64> diskGuids;

        {
            env.CreateBoxAndPool(1, 10);

            env.Sim(TDuration::Seconds(30));

            auto config = env.FetchBaseConfig();

            for (const NKikimrBlobStorage::TBaseConfig::TPDisk& pdisk : config.GetPDisk()) {
                TPDiskId diskId(pdisk.GetNodeId(), pdisk.GetPDiskId());

                diskGuids[diskId] = pdisk.GetGuid();
            }

            env.Sim(TDuration::Seconds(30));
        }

        int i = 0;
        auto it = diskGuids.begin();

        for (; it != diskGuids.end(); it++, i++) {
            auto& diskId =  it->first;

            NKikimrBlobStorage::TConfigRequest request;
            request.SetIgnoreDegradedGroupsChecks(true);

            NKikimrBlobStorage::TRestartPDisk* cmd = request.AddCommand()->MutableRestartPDisk();
            auto pdiskId = cmd->MutableTargetPDiskId();
            pdiskId->SetNodeId(diskId.NodeId);
            pdiskId->SetPDiskId(diskId.PDiskId);

            auto response = env.Invoke(request);

            if (i < 2) {
                // Two disks can be restarted.
                UNIT_ASSERT_C(response.GetSuccess(), response.GetErrorDescription());
            } else {
                // Restarting third disk will not be allowed.
                UNIT_ASSERT_C(!response.GetSuccess(), "Restart should've been prohibited");

                UNIT_ASSERT_STRING_CONTAINS(response.GetErrorDescription(), "Disintegrated");
                break;
            }
        }
    }

    auto GetGroupVDisks(TEnvironmentSetup& env) {
        struct TVDisk {
            ui32 NodeId;
            ui32 PDiskId;
            ui32 VSlotId;
            TVDiskID VDiskId;
        };

        std::vector<TVDisk> vdisks;

        auto config = env.FetchBaseConfig();

        auto& group = config.get_idx_group(0);
        
        for (auto& vslot : config.GetVSlot()) {
            if (group.GetGroupId() == vslot.GetGroupId()) {
                auto slotId = vslot.GetVSlotId();
                auto nodeId = slotId.GetNodeId();
                auto pdiskId = slotId.GetPDiskId();
                auto vdiskId = TVDiskID(group.GetGroupId(), group.GetGroupGeneration(), vslot.GetFailRealmIdx(), vslot.GetFailDomainIdx(), vslot.GetVDiskIdx());
                vdisks.push_back({nodeId, pdiskId, slotId.GetVSlotId(), vdiskId});
            }
        }

        return vdisks;
    }

    Y_UNIT_TEST(RestartBrokenDiskInBrokenGroup) {
        TEnvironmentSetup env({
            .NodeCount = 8,
            .Erasure = TBlobStorageGroupType::Erasure4Plus2Block
        });

        env.UpdateSettings(false, false);
        env.CreateBoxAndPool(1, 1);
        env.Sim(TDuration::Seconds(30));

        auto vdisks = GetGroupVDisks(env);

        // Making all vdisks bad, group is disintegrated
        const TActorId sender = env.Runtime->AllocateEdgeActor(env.Settings.ControllerNodeId, __FILE__, __LINE__);
        for (auto& pdisk : env.PDiskActors) {
            env.Runtime->WrapInActorContext(sender, [&] () {
                env.Runtime->Send(new IEventHandle(EvBecomeError, 0, pdisk, sender, nullptr, 0));
            });
        }

        env.Sim(TDuration::Minutes(1));

        // Restarting the owner of an already broken disk in a broken group must be allowed
        auto& [targetNodeId, targetPDiskId, unused1, unused2] = vdisks[0];

        NKikimrBlobStorage::TConfigRequest request;

        NKikimrBlobStorage::TRestartPDisk* cmd = request.AddCommand()->MutableRestartPDisk();
        auto pdiskId = cmd->MutableTargetPDiskId();
        pdiskId->SetNodeId(targetNodeId);
        pdiskId->SetPDiskId(targetPDiskId);

        auto response = env.Invoke(request);
        UNIT_ASSERT_C(response.GetSuccess(), response.GetErrorDescription());
    }

    Y_UNIT_TEST(RestartGoodDiskInBrokenGroupNotAllowed) {
        TEnvironmentSetup env({
            .NodeCount = 8,
            .Erasure = TBlobStorageGroupType::Erasure4Plus2Block
        });

        env.UpdateSettings(false, false);
        env.CreateBoxAndPool(1, 1);
        env.Sim(TDuration::Seconds(30));
    
        // Making all but one vdisks bad, group is disintegrated
        const TActorId sender = env.Runtime->AllocateEdgeActor(env.Settings.ControllerNodeId, __FILE__, __LINE__);
        for (size_t i = 0; i < env.PDiskActors.size() - 1; i++) {
            env.Runtime->WrapInActorContext(sender, [&] () {
                env.Runtime->Send(new IEventHandle(EvBecomeError, 0, env.PDiskActors[i], sender, nullptr, 0));
            });
        }

        env.Sim(TDuration::Minutes(1));

        ui32 targetNodeId = 0;
        ui32 targetPDiskId = 0;

        for (auto& [k, v] : env.PDiskMockStates) {
            if (v.Get()->GetStateErrorReason().empty()) {
                targetNodeId = k.first;
                targetPDiskId = k.second;
            }
        }

        // However restarting the owner of a single good disk must be prohibited
        NKikimrBlobStorage::TConfigRequest request;

        NKikimrBlobStorage::TRestartPDisk* cmd = request.AddCommand()->MutableRestartPDisk();
        auto pdiskId = cmd->MutableTargetPDiskId();
        pdiskId->SetNodeId(targetNodeId);
        pdiskId->SetPDiskId(targetPDiskId);

        auto response = env.Invoke(request);

        UNIT_ASSERT_C(!response.GetSuccess(), "Restart should've been prohibited");
        UNIT_ASSERT_STRING_CONTAINS(response.GetErrorDescription(), "Disintegrated");
    }

    Y_UNIT_TEST(RestartOneByOne) {
        TEnvironmentSetup env({
            .NodeCount = 10,
            .Erasure = TBlobStorageGroupType::Erasure4Plus2Block
        });

        std::unordered_map<TPDiskId, ui64> diskGuids;

        {
            env.CreateBoxAndPool(1, 10);

            env.Sim(TDuration::Seconds(30));

            auto config = env.FetchBaseConfig();

            for (const NKikimrBlobStorage::TBaseConfig::TPDisk& pdisk : config.GetPDisk()) {
                TPDiskId diskId(pdisk.GetNodeId(), pdisk.GetPDiskId());

                diskGuids[diskId] = pdisk.GetGuid();
            }

            env.Sim(TDuration::Seconds(30));
        }

        int i = 0;
        auto it = diskGuids.begin();

        for (; it != diskGuids.end(); it++, i++) {
            auto& diskId =  it->first;

            NKikimrBlobStorage::TConfigRequest request;

            NKikimrBlobStorage::TRestartPDisk* cmd = request.AddCommand()->MutableRestartPDisk();
            auto pdiskId = cmd->MutableTargetPDiskId();
            pdiskId->SetNodeId(diskId.NodeId);
            pdiskId->SetPDiskId(diskId.PDiskId);

            auto response = env.Invoke(request);

            UNIT_ASSERT_C(response.GetSuccess(), response.GetErrorDescription());

            // Wait for VSlot to become ready after PDisk restart.
            env.Sim(TDuration::Seconds(30));
        }
    }

    Y_UNIT_TEST(RestartOneByOneWithReconnects) {
        TEnvironmentSetup env({
            .NodeCount = 10,
            .Erasure = TBlobStorageGroupType::Erasure4Plus2Block
        });

        std::unordered_map<TPDiskId, ui64> diskGuids;

        {
            env.CreateBoxAndPool(1, 10);

            env.Sim(TDuration::Seconds(30));

            auto config = env.FetchBaseConfig();

            for (const NKikimrBlobStorage::TBaseConfig::TPDisk& pdisk : config.GetPDisk()) {
                TPDiskId diskId(pdisk.GetNodeId(), pdisk.GetPDiskId());

                diskGuids[diskId] = pdisk.GetGuid();
            }

            env.Sim(TDuration::Seconds(30));
        }

        int i = 0;
        auto it = diskGuids.begin();

        auto invokeWithReconnect = [&](const NKikimrBlobStorage::TConfigRequest& request, ui32 nodeId) {
            const TActorId self = env.Runtime->AllocateEdgeActor(env.Settings.ControllerNodeId, __FILE__, __LINE__);
            auto ev = std::make_unique<TEvBlobStorage::TEvControllerConfigRequest>();
            ev->Record.MutableRequest()->CopyFrom(request);
            env.Runtime->SendToPipe(env.TabletId, self, ev.release(), 0, TTestActorSystem::GetPipeConfigWithRetries());

            env.Runtime->FilterFunction = [&](ui32 /*nodeId*/, std::unique_ptr<IEventHandle> &witnessedEvent) {
                if (self == witnessedEvent->GetRecipientRewrite()) {
                    // During reconnect edge actor receives unhandleable events, so drop them.
                    return false;
                }
                return true;
            };

            TInstant barrier = env.Runtime->GetClock() + TDuration::Seconds(30);

            // Wait until BSC receives restart request.
            bool gotPdiskRestart = false;
            env.Runtime->Sim([&] { return env.Runtime->GetClock() <= barrier && !gotPdiskRestart; }, [&](IEventHandle &witnessedEvent) {
                if (witnessedEvent.GetTypeRewrite() == TEvBlobStorage::TEvControllerConfigRequest::EventType) {
                    auto *cfgRequest = witnessedEvent.Get<TEvBlobStorage::TEvControllerConfigRequest>();
                    if (cfgRequest) {
                        const auto &commands = cfgRequest->Record.GetRequest().GetCommand();
                        UNIT_ASSERT_EQUAL(1, commands.size());
                        
                        const auto &command = commands[0];
                        UNIT_ASSERT_EQUAL(NKikimrBlobStorage::TConfigRequest::TCommand::kRestartPDisk, command.GetCommandCase());

                        const auto &restartPdiskCmd = command.GetRestartPDisk();
                        Y_UNUSED(restartPdiskCmd);

                        gotPdiskRestart = true;
                    }
                }
            });

            // Break connection between node warden and BSC.
            TActorId nodeWardenActorId = env.Runtime->GetNode(nodeId)->ActorSystem->LookupLocalService(MakeBlobStorageNodeWardenID(nodeId));
            NStorage::TNodeWarden *warden = dynamic_cast<NStorage::TNodeWarden*>(env.Runtime->GetActor(nodeWardenActorId));
            
            env.Runtime->WrapInActorContext(nodeWardenActorId, [&] {
                warden->OnPipeError();
            });

            // Wait for node warden reconnect to BSC.
            bool gotServiceSetUpdate = false;
            env.Runtime->Sim([&] { return env.Runtime->GetClock() <= barrier && !gotServiceSetUpdate; }, [&](IEventHandle &witnessedEvent) {
                if (witnessedEvent.GetTypeRewrite() == TEvBlobStorage::TEvControllerNodeServiceSetUpdate::EventType) {
                    auto *serviceSetUpdate = witnessedEvent.Get<TEvBlobStorage::TEvControllerNodeServiceSetUpdate>();
                    if (serviceSetUpdate) {
                        const auto &pdisks = serviceSetUpdate->Record.GetServiceSet().GetPDisks();
                        const auto &pdisk = pdisks[0];
                        UNIT_ASSERT_EQUAL(NKikimrBlobStorage::RESTART, pdisk.GetEntityStatus());
                        gotServiceSetUpdate = true;
                    }
                }
            });

            UNIT_ASSERT(gotServiceSetUpdate);

            // Wait until pdisk restarts and node warden sends "pdisk restarted" to BSC.
            bool gotPdiskReport = false;
            env.Runtime->Sim([&] { return env.Runtime->GetClock() <= barrier && !gotPdiskReport; }, [&](IEventHandle &witnessedEvent) {
                if (witnessedEvent.GetTypeRewrite() == TEvBlobStorage::TEvControllerNodeReport::EventType) {
                    auto *nodeReport = witnessedEvent.Get<TEvBlobStorage::TEvControllerNodeReport>();
                    if (nodeReport) {
                        const auto &pdisks = nodeReport->Record.GetPDiskReports();
                        const auto &pdisk = pdisks[0];
                        UNIT_ASSERT(pdisk.GetPhase() == NKikimrBlobStorage::TEvControllerNodeReport_EPDiskPhase_PD_RESTARTED);
                        gotPdiskReport = true;
                    }
                }
            });

            UNIT_ASSERT(gotPdiskReport);

            env.Runtime->FilterFunction = nullptr;
        };

        for (; it != diskGuids.end(); it++, i++) {
            auto& diskId =  it->first;

            NKikimrBlobStorage::TConfigRequest request;

            NKikimrBlobStorage::TRestartPDisk* cmd = request.AddCommand()->MutableRestartPDisk();
            auto pdiskId = cmd->MutableTargetPDiskId();
            pdiskId->SetNodeId(diskId.NodeId);
            pdiskId->SetPDiskId(diskId.PDiskId);

            invokeWithReconnect(request, diskId.NodeId);

            // Wait for VSlot to become ready after PDisk restart.
            env.Sim(TDuration::Seconds(30));
        }
    }
}
