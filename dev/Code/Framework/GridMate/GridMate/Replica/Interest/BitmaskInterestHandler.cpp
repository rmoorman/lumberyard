/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#include <GridMate/Replica/Interest/BitmaskInterestHandler.h>

#include <GridMate/Replica/ReplicaChunk.h>
#include <GridMate/Replica/Interpolators.h>
#include <GridMate/Replica/Replica.h>
#include <GridMate/Replica/ReplicaFunctions.h>
#include <GridMate/Replica/ReplicaMgr.h>
#include <GridMate/Replica/RemoteProcedureCall.h>

#include <GridMate/Replica/Interest/InterestManager.h>

namespace GridMate
{
    class BitmaskInterestChunk
        : public ReplicaChunk
    {
    public:
        GM_CLASS_ALLOCATOR(BitmaskInterestChunk);

        // ReplicaChunk
        typedef AZStd::intrusive_ptr<BitmaskInterestChunk> Ptr;
        bool IsReplicaMigratable() override { return false; }
        static const char* GetChunkName() { return "BitmaskInterestChunk"; }
        bool IsBroadcast() { return true; }
        ///////////////////////////////////////////////////////////////////////////


        BitmaskInterestChunk()
            : AddRuleRpc("AddRule")
            , RemoveRuleRpc("RemoveRule")
            , UpdateRuleRpc("UpdateRule")
            , AddRuleForPeerRpc("AddRuleForPeerRpc")
            , m_interestHandler(nullptr)
        {

        }

        void OnReplicaActivate(const ReplicaContext& rc) override
        {
            m_interestHandler = static_cast<BitmaskInterestHandler*>(rc.m_rm->GetUserContext(AZ_CRC("BitmaskInterestHandler", 0x5bf5d75b)));
            AZ_Warning("GridMate", m_interestHandler != nullptr, "No bitmask interest handler in the user context");
            if (m_interestHandler)
            {
                m_interestHandler->OnNewRulesChunk(this, rc.m_peer);
            }
        }

        void OnReplicaDeactivate(const ReplicaContext& rc) override
        {
            if (rc.m_peer && m_interestHandler)
            {
                m_interestHandler->OnDeleteRulesChunk(this, rc.m_peer);
            }
        }

        bool AddRuleFn(RuleNetworkId netId, InterestBitmask bits, const RpcContext& ctx)
        {
            if (IsProxy())
            {
                auto rulePtr = m_interestHandler->CreateRule(ctx.m_sourcePeer);
                rulePtr->Set(bits);
                m_rules.insert(AZStd::make_pair(netId, rulePtr));
            }

            return true;
        }

        bool RemoveRuleFn(RuleNetworkId netId, const RpcContext&)
        {
            if (IsProxy())
            {
                m_rules.erase(netId);
            }

            return true;
        }

        bool UpdateRuleFn(RuleNetworkId netId, InterestBitmask bits, const RpcContext&)
        {
            if (IsProxy())
            {
                auto it = m_rules.find(netId);
                if (it != m_rules.end())
                {
                    it->second->Set(bits);
                }
            }

            return true;
        }

        bool AddRuleForPeerFn(RuleNetworkId netId, PeerId peerId, InterestBitmask bitmask, const RpcContext&)
        {
            BitmaskInterestChunk* peerChunk = m_interestHandler->FindRulesChunkByPeerId(peerId);
            if (peerChunk)
            {
                auto it = peerChunk->m_rules.find(netId);
                if (it == peerChunk->m_rules.end())
                {
                    auto rulePtr = m_interestHandler->CreateRule(peerId);
                    peerChunk->m_rules.insert(AZStd::make_pair(netId, rulePtr));
                    rulePtr->Set(bitmask);
                }
            }
            return false;
        }

        Rpc<RpcArg<RuleNetworkId>, RpcArg<InterestBitmask>>::BindInterface<BitmaskInterestChunk, &BitmaskInterestChunk::AddRuleFn> AddRuleRpc;
        Rpc<RpcArg<RuleNetworkId>>::BindInterface<BitmaskInterestChunk, &BitmaskInterestChunk::RemoveRuleFn> RemoveRuleRpc;
        Rpc<RpcArg<RuleNetworkId>, RpcArg<InterestBitmask>>::BindInterface<BitmaskInterestChunk, &BitmaskInterestChunk::UpdateRuleFn> UpdateRuleRpc;

        Rpc<RpcArg<RuleNetworkId>, RpcArg<PeerId>, RpcArg<InterestBitmask>>::BindInterface<BitmaskInterestChunk, &BitmaskInterestChunk::AddRuleForPeerFn> AddRuleForPeerRpc;

        unordered_map<RuleNetworkId, BitmaskInterestRule::Ptr> m_rules;
        BitmaskInterestHandler* m_interestHandler;
    };
    ///////////////////////////////////////////////////////////////////////////


    /*
    * BitmaskInterest
    */
    BitmaskInterest::BitmaskInterest(BitmaskInterestHandler* handler)
        : m_handler(handler)
        , m_bits(0)
    {
        AZ_Assert(m_handler, "Invalid interest handler");
    }
    ///////////////////////////////////////////////////////////////////////////


    /*
    * BitmaskInterestRule
    */
    void BitmaskInterestRule::Set(InterestBitmask newBitmask)
    {
        m_bits = newBitmask;
        m_handler->UpdateRule(this);
    }

    void BitmaskInterestRule::Destroy()
    {
        m_handler->DestroyRule(this);
    }
    ///////////////////////////////////////////////////////////////////////////


    /*
    * BitmaskInterestAttribute
    */
    void BitmaskInterestAttribute::Set(InterestBitmask newBitmask)
    {
        m_bits = newBitmask;
        m_handler->UpdateAttribute(this);
    }

    void BitmaskInterestAttribute::Destroy()
    {
        m_handler->DestroyAttribute(this);
    }
    ///////////////////////////////////////////////////////////////////////////


    /*
    * BitmaskInterestHandler
    */
    BitmaskInterestHandler::BitmaskInterestHandler()
        : m_im(nullptr)
        , m_rm(nullptr)
        , m_lastRuleNetId(0)
        , m_rulesReplica(nullptr)
    {
        if (!ReplicaChunkDescriptorTable::Get().FindReplicaChunkDescriptor(ReplicaChunkClassId(BitmaskInterestChunk::GetChunkName())))
        {
            ReplicaChunkDescriptorTable::Get().RegisterChunkType<BitmaskInterestChunk>();
        }
    }

    BitmaskInterestRule::Ptr BitmaskInterestHandler::CreateRule(PeerId peerId)
    {
        BitmaskInterestRule* rulePtr = aznew BitmaskInterestRule(this, peerId, GetNewRuleNetId());
        m_rules.insert(rulePtr);

        if (peerId == m_rm->GetLocalPeerId())
        {
            m_rulesReplica->AddRuleRpc(rulePtr->GetNetworkId(), rulePtr->Get());
            m_localRules.insert(rulePtr);
        }

        return rulePtr;
    }

    void BitmaskInterestHandler::FreeRule(BitmaskInterestRule* rule)
    {
        //TODO: should be pool-allocated
        m_rules.erase(rule);
        delete rule;
    }

    void BitmaskInterestHandler::DestroyRule(BitmaskInterestRule* rule)
    {
        if (m_rm && rule->GetPeerId() == m_rm->GetLocalPeerId())
        {
            m_rulesReplica->RemoveRuleRpc(rule->GetNetworkId());
        }

        rule->m_bits = 0;
        m_dirtyRules.insert(rule);
        m_localRules.erase(rule);
    }

    void BitmaskInterestHandler::UpdateRule(BitmaskInterestRule* rule)
    {
        if (m_rm && rule->GetPeerId() == m_rm->GetLocalPeerId())
        {
            m_rulesReplica->UpdateRuleRpc(rule->GetNetworkId(), rule->Get());
        }

        m_dirtyRules.insert(rule);
    }

    BitmaskInterestAttribute::Ptr BitmaskInterestHandler::CreateAttribute(ReplicaId replicaId)
    {
        auto ptr = aznew BitmaskInterestAttribute(this, replicaId);
        m_attrs.insert(ptr);
        return ptr;
    }

    void BitmaskInterestHandler::FreeAttribute(BitmaskInterestAttribute* attrib)
    {
        //TODO: should be pool-allocated
        m_attrs.erase(attrib);
        delete attrib;
    }

    void BitmaskInterestHandler::DestroyAttribute(BitmaskInterestAttribute* attrib)
    {
        attrib->m_bits = 0;
        m_dirtyAttributes.insert(attrib);
    }

    void BitmaskInterestHandler::UpdateAttribute(BitmaskInterestAttribute* attrib)
    {
        m_dirtyAttributes.insert(attrib);
    }

    void BitmaskInterestHandler::OnNewRulesChunk(BitmaskInterestChunk* chunk, ReplicaPeer* peer)
    {
        if (chunk != m_rulesReplica) // non-local
        {
            m_peerChunks.insert(AZStd::make_pair(peer->GetId(), chunk));

            for (auto& rule : m_localRules)
            {
                chunk->AddRuleForPeerRpc(rule->GetNetworkId(), rule->GetPeerId(), rule->Get());
            }
        }
    }

    void BitmaskInterestHandler::OnDeleteRulesChunk(BitmaskInterestChunk* chunk, ReplicaPeer* peer)
    {
        (void)chunk;
        m_peerChunks.erase(peer->GetId());
    }

    RuleNetworkId BitmaskInterestHandler::GetNewRuleNetId()
    {
        ++m_lastRuleNetId;
        return m_rulesReplica->GetReplicaId() | (static_cast<AZ::u64>(m_lastRuleNetId) << 32);
    }

    BitmaskInterestChunk* BitmaskInterestHandler::FindRulesChunkByPeerId(PeerId peerId)
    {
        auto it = m_peerChunks.find(peerId);
        if (it == m_peerChunks.end())
        {
            return nullptr;
        }
        else
        {
            return it->second;
        }
    }

    const InterestMatchResult& BitmaskInterestHandler::GetLastResult()
    {
        return m_resultCache;
    }

    void BitmaskInterestHandler::Update()
    {
        m_resultCache.clear();

        for (BitmaskInterestRule* rule : m_dirtyRules)
        {
            InterestBitmask j = 1;
            for (size_t i = 0; i < k_numGroups; ++i, j <<= 1)
            {
                auto ruleIt = m_ruleGroups[i].find(rule);
                bool isMatch = !!(rule->m_bits & j);
                if (isMatch && ruleIt == m_ruleGroups[i].end())
                {
                    m_ruleGroups[i].insert(rule);
                    
                    // recalculate all the attributes in this bucket
                    for (BitmaskInterestAttribute* attr : m_attrGroups[i])
                    {
                        m_dirtyAttributes.insert(attr);
                    }
                }
                else if (!isMatch && ruleIt != m_ruleGroups[i].end())
                {
                    m_ruleGroups[i].erase(ruleIt);

                    // recalculate all the attributes in this bucket
                    for (BitmaskInterestAttribute* attr : m_attrGroups[i])
                    {
                        m_dirtyAttributes.insert(attr);
                    }
                }
            }

            if (rule->IsDeleted())
            {
                FreeRule(rule);
            }
        }

        m_dirtyRules.clear();

        for (BitmaskInterestAttribute* attr : m_dirtyAttributes)
        {
            InterestBitmask j = 1;
            for (size_t i = 0; i < k_numGroups; ++i, j <<= 1)
            {
                auto attrIt = m_attrGroups[i].find(attr);
                bool isMatch = !!(attr->m_bits & j);
                if (isMatch && attrIt == m_attrGroups[i].end())
                {
                    m_attrGroups[i].insert(attr);
                }
                else if (!isMatch && attrIt != m_attrGroups[i].end())
                {
                    m_attrGroups[i].erase(attrIt);
                }
            }
        }

        for (BitmaskInterestAttribute* attr : m_dirtyAttributes)
        {
            auto repIt = m_resultCache.insert(attr->GetReplicaId());
            
            InterestBitmask j = 1;
            for (size_t i = 0; i < k_numGroups; ++i, j <<= 1)
            {
                if (!!(attr->m_bits & j))
                {
                    for (BitmaskInterestRule* rule : m_ruleGroups[i])
                    {
                        repIt.first->second.insert(rule->GetPeerId());
                    }
                }
            }

            if (attr->IsDeleted())
            {
                FreeAttribute(attr);
            }
        }

        m_dirtyAttributes.clear();
    }
    
    void BitmaskInterestHandler::OnRulesHandlerRegistered(InterestManager* manager)
    {
        AZ_Assert(m_im == nullptr, "Handler is already registered with manager %p (%p)\n", m_im, manager);
        AZ_Assert(m_rulesReplica == nullptr, "Rules replica is already created\n");
        AZ_TracePrintf("GridMate", "Bitmask interest handler is registered\n");
        m_im = manager;
        m_rm = m_im->GetReplicaManager();
        m_rm->RegisterUserContext(AZ_CRC("BitmaskInterestHandler", 0x5bf5d75b), this);

        auto replica = Replica::CreateReplica("BitmaskInterestHandlerRules");
        m_rulesReplica = CreateAndAttachReplicaChunk<BitmaskInterestChunk>(replica);
        m_rm->AddMaster(replica);
    }

    void BitmaskInterestHandler::OnRulesHandlerUnregistered(InterestManager* manager)
    {
        (void)manager;

        AZ_Assert(m_im == manager, "Handler was not registered with manager %p (%p)\n", manager, m_im);
        AZ_TracePrintf("GridMate", "Bitmask interest handler is unregistered\n");

        if (m_rulesReplica)
        {
            m_rulesReplica->m_rules.clear();
            m_rulesReplica->m_interestHandler = nullptr;
        }
            
        for (auto& chunk : m_peerChunks)
        {
            chunk.second->m_rules.clear();
            chunk.second->m_interestHandler = nullptr;
        }

        m_rulesReplica = nullptr;
        m_im = nullptr;
        m_rm->UnregisterUserContext(AZ_CRC("BitmaskInterestHandler", 0x5bf5d75b));
        m_rm = nullptr;

        m_peerChunks.clear();
        m_localRules.clear();

        for (auto& a : m_attrs)
        {
            delete a;
        }

        for (auto& r : m_rules)
        {
            delete r;
        }

        m_dirtyAttributes.clear();
        m_dirtyRules.clear();

        for (auto& group : m_attrGroups)
        {
            group.clear();
        }

        for (auto& group : m_ruleGroups)
        {
            group.clear();
        }

        m_resultCache.clear();
    }
    ///////////////////////////////////////////////////////////////////////////
}
