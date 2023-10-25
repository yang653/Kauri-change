/**
 * Copyright 2018 VMware
 * Copyright 2018 Ted Yin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hotstuff/client.h"
#include"hotstuff/hotstuff.h"
#include "hotstuff/liveness.h"
// #include "../include/hotstuff/client.h"
// #include"../include/hotstuff/hotstuff.h"
// #include "../include/hotstuff/liveness.h"

#include <random>
#include <future>
#include<unordered_map>


//#include "hotstuff.h"

using salticidae::static_pointer_cast;

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN

namespace hotstuff {

const opcode_t MsgPropose::opcode;
MsgPropose::MsgPropose(const Proposal &proposal) { serialized << proposal; }
void MsgPropose::postponed_parse(HotStuffCore *hsc) {
    proposal.hsc = hsc;
    HOTSTUFF_LOG_PROTO("Size of the block: %lld", serialized.size());
    serialized >> proposal;
}

const opcode_t MsgRelay::opcode;
MsgRelay::MsgRelay(const VoteRelay &proposal) { serialized << proposal; }
void MsgRelay::postponed_parse(HotStuffCore *hsc) {//将协议流中的信息流向投票
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgVote::opcode;
MsgVote::MsgVote(const Vote &vote) { serialized << vote; }
void MsgVote::postponed_parse(HotStuffCore *hsc) {
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgReqBlock::opcode;
MsgReqBlock::MsgReqBlock(const std::vector<uint256_t> &blk_hashes) {
    serialized << htole((uint32_t)blk_hashes.size());//hash转移
    for (const auto &h: blk_hashes)
        serialized << h;
}

MsgReqBlock::MsgReqBlock(DataStream &&s) {
    uint32_t size;
    s >> size;
    size = letoh(size);
    blk_hashes.resize(size);
    for (auto &h: blk_hashes) s >> h;
}

const opcode_t MsgRespBlock::opcode;
MsgRespBlock::MsgRespBlock(const std::vector<block_t> &blks) {
    serialized << htole((uint32_t)blks.size());
    for (auto blk: blks) serialized << *blk;
}

void MsgRespBlock::postponed_parse(HotStuffCore *hsc) {
    uint32_t size;
    serialized >> size;
    size = letoh(size);
    blks.resize(size);
    for (auto &blk: blks)
    {
        Block _blk;
        _blk.unserialize(serialized, hsc);
        blk = hsc->storage->add_blk(std::move(_blk), hsc->get_config());
    }
}

const opcode_t MsgNewview::opcode;
MsgNewview::MsgNewview(const NewView &newview) { serialized << newview; }
void MsgNewview::postponed_parse(HotStuffCore *hsc) {
    serialized>>newview;
}

void HotStuffBase::exec_command(uint256_t cmd_hash, commit_cb_t callback) {
    cmd_pending.enqueue(std::make_pair(cmd_hash, callback));
}

void HotStuffBase::on_fetch_blk(const block_t &blk) {
#ifdef HOTSTUFF_BLK_PROFILE
    blk_profiler.get_tx(blk->get_hash());
#endif
    LOG_DEBUG("fetched %.10s", get_hex(blk->get_hash()).c_str());
    part_fetched++;
    fetched++;
    //for (auto cmd: blk->get_cmds()) on_fetch_cmd(cmd);
    const uint256_t &blk_hash = blk->get_hash();
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it != blk_fetch_waiting.end())
    {
        it->second.resolve(blk);
        blk_fetch_waiting.erase(it);
    }
}

bool HotStuffBase::on_deliver_blk(const block_t &blk) {
    HOTSTUFF_LOG_PROTO("Base deliver");

    const uint256_t &blk_hash = blk->get_hash();
    bool valid;
    /* sanity check: all parents must be delivered */
    for (const auto &p: blk->get_parent_hashes())
        assert(storage->is_blk_delivered(p));
    if ((valid = HotStuffCore::on_deliver_blk(blk)))
    {
        LOG_DEBUG("block %.10s delivered",
                get_hex(blk_hash).c_str());
        part_parent_size += blk->get_parent_hashes().size();
        part_delivered++;
        delivered++;
    }
    else
    {
        LOG_WARN("dropping invalid block");
    }

    bool res = true;
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
    {
        auto &pm = it->second;
        if (valid)
        {
            pm.elapsed.stop(false);
            auto sec = pm.elapsed.elapsed_sec;
            part_delivery_time += sec;
            part_delivery_time_min = std::min(part_delivery_time_min, sec);
            part_delivery_time_max = std::max(part_delivery_time_max, sec);

            pm.resolve(blk);
        }
        else
        {
            pm.reject(blk);
            res = false;
        }
        blk_delivery_waiting.erase(it);
    }
    return res;
}

promise_t HotStuffBase::async_fetch_blk(const uint256_t &blk_hash,
                                        const PeerId *replica,
                                        bool fetch_now) {
    if (storage->is_blk_fetched(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm){
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it == blk_fetch_waiting.end())
    {
#ifdef HOTSTUFF_BLK_PROFILE
        blk_profiler.rec_tx(blk_hash, false);
#endif
        it = blk_fetch_waiting.insert(
            std::make_pair(
                blk_hash,
                BlockFetchContext(blk_hash, this))).first;
    }
    if (replica != nullptr)
        it->second.add_replica(*replica, fetch_now);
    return static_cast<promise_t &>(it->second);
}

promise_t HotStuffBase::async_deliver_blk(const uint256_t &blk_hash, const PeerId &replica) {
    if (storage->is_blk_delivered(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm) {
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
        return static_cast<promise_t &>(it->second);
    BlockDeliveryContext pm{[](promise_t){}};
    it = blk_delivery_waiting.insert(std::make_pair(blk_hash, pm)).first;
    /* otherwise the on_deliver_batch will resolve */
    async_fetch_blk(blk_hash, &replica).then([this, replica](block_t blk) {
        /* qc_ref should be fetched */
        std::vector<promise_t> pms;
        const auto &qc = blk->get_qc();
        assert(qc);
        if (blk == get_genesis())
            pms.push_back(promise_t([](promise_t &pm){ pm.resolve(true); }));
        else
            pms.push_back(blk->verify(this, vpool));
        pms.push_back(async_fetch_blk(qc->get_obj_hash(), &replica));
        /* the parents should be delivered */
        for (const auto &phash: blk->get_parent_hashes())
            pms.push_back(async_deliver_blk(phash, replica));
        promise::all(pms).then([this, blk](const promise::values_t values) {
            auto ret = promise::any_cast<bool>(values[0]) && this->on_deliver_blk(blk);
            if (!ret)
                HOTSTUFF_LOG_WARN("verification failed during async delivery");
        });
    });
    return static_cast<promise_t &>(pm);
}

void HotStuffBase::propose_handler(MsgPropose &&msg, const Net::conn_t &conn) {
    const PeerId &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    auto stream = msg.serialized;

    if (!childPeers.empty()) {
        MsgPropose relay = MsgPropose(stream, true);
        for (const PeerId &peerId : childPeers) {
            pn.send_msg(relay, peerId);
        }
    }

    msg.postponed_parse(this);
    auto &prop = msg.proposal;

    /*获取newview消息中的Vrr方便在svc的时候发送给领导者*/
    // NewView it=NVset_t.find(id);
    // if(it!=NVset_t.end()){
    //     it.Vote_Req=prop;
    //     it.viewNum=vheight;
    //     it.id=id;
    // }else{
    //     NewView newview;
    //     newview.Vote_Req=prop;
    //     newview.viewNum=vheight;
    //     newview.id=id;
    //     NVset_t[id]=newview;
    // }
    nextview->id=id;
    //nextview.viewNum=vheight;
    nextview->Vote_Req=&prop;

    block_t blk = prop.blk;
    if (!blk) return;

    promise::all(std::vector<promise_t>{
        async_deliver_blk(blk->get_hash(), peer)
    }).then([this, prop = std::move(prop)]() {
        on_receive_proposal(prop);
    });
}

void HotStuffBase::vote_handler(MsgVote &&msg, const Net::conn_t &conn) {
    struct timeval timeStart,timeEnd;
    gettimeofday(&timeStart, NULL);

    const auto &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    //HOTSTUFF_LOG_PROTO("received vote");

    if (id == pmaker->get_proposer() && !piped_queue.empty() && std::find(piped_queue.begin(), piped_queue.end(), msg.vote.blk_hash) != piped_queue.end()) {
        HOTSTUFF_LOG_PROTO("piped block");
        block_t blk = storage->find_blk(msg.vote.blk_hash);
        if (!blk->delivered) {
            process_block(blk, false);
            HOTSTUFF_LOG_PROTO("Normalized piped block");
        }
    }

    block_t blk = get_potentially_not_delivered_blk(msg.vote.blk_hash);/*如果区块还没有被交付会抛出异常，已经被交付就返回该区块*/

    if (!blk->delivered && blk->self_qc == nullptr) {
        blk->self_qc = create_quorum_cert(blk->get_hash());
        part_cert_bt part = create_part_cert(*priv_key, blk->get_hash());
        blk->self_qc->add_part(config, id, *part);

        std::cout << "create cert: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
    }

    std::cout << "vote handler: " << msg.vote.blk_hash.to_hex() << " " << std::endl;
    //HOTSTUFF_LOG_PROTO("vote handler %d %d", config.nmajority, config.nreplicas);

    if (blk->self_qc->has_n(config.nmajority)) {
        HOTSTUFF_LOG_PROTO("bye vote handler");
        //std::cout << "bye vote handler: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
        /*if (id == get_pace_maker()->get_proposer()) {
            gettimeofday(&timeEnd, NULL);
            long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
            stats[blk->hash] = stats[blk->hash] + usec;
            HOTSTUFF_LOG_PROTO("result: %s, %s ", blk->hash.to_hex().c_str(), std::to_string(stats[blk->parent_hashes[0]]).c_str());
        }*/
        return;
    }

    //auto &vote = msg.vote;
    RcObj<Vote> v(new Vote(std::move(msg.vote)));
    promise::all(std::vector<promise_t>{
        async_deliver_blk(v->blk_hash, peer),
        v->verify(vpool),
    }).then([this, blk, v=std::move(v), timeStart](const promise::values_t values) {
        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN("invalid vote from %d", v->voter);
        auto &cert = blk->self_qc;
        //struct timeval timeEnd;

      if (id != pmaker->get_proposer() ) {

        if (cert->has_n(numberOfChildren + 1)) {
          return;
        }

        cert->add_part(config, v->voter, *v->cert);

        if (!cert->has_n(numberOfChildren + 1)) {
          return;
        }
        std::cout <<  " got enough votes: " << v->blk_hash.to_hex().c_str() <<  std::endl;

        if (!piped_queue.empty()) {

          for (auto hash = std::begin(piped_queue); hash != std::end(piped_queue); ++hash) {
            block_t b = storage->find_blk(*hash);
            if (b->delivered && b->qc->has_n(config.nmajority)) {
              piped_queue.erase(hash);
              HOTSTUFF_LOG_PROTO("Confirm Piped block");
            }
          }

          if (blk->hash == piped_queue.front()){
            piped_queue.pop_front();
            HOTSTUFF_LOG_PROTO("Reset Piped block");
          }
          else {
            HOTSTUFF_LOG_PROTO("Failed resetting piped block, wasn't front!!!");
          }
        }

        cert->compute();
        if (!cert->verify(config)) {
          HOTSTUFF_LOG_PROTO("Error, Invalid Sig!!!");
          return;
        }

        std::cout <<  " send relay message: " << v->blk_hash.to_hex().c_str() <<  std::endl;
        pn.send_msg(MsgRelay(VoteRelay(v->blk_hash, blk->self_qc->clone(), this)), parentPeer);
        return;
      }

      cert->add_part(config, v->voter, *v->cert);
      if (cert != nullptr && cert->get_obj_hash() == blk->get_hash()) {
        if (cert->has_n(config.nmajority)) {
          cert->compute();
          if (id != 0 && !cert->verify(config)) {
            throw std::runtime_error("Invalid Sigs in intermediate signature!");
          }
          update_hqc(blk, cert);
          on_qc_finish(blk);
        }
      }

      /*if (id == get_pace_maker()->get_proposer()) {
            gettimeofday(&timeEnd, NULL);
            long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
            std::cout << usec << " a:a " << stats[blk->hash] << std::endl;
            stats[blk->hash] = stats[blk->hash] + usec;
            std::cout << usec << " b:b " << stats[blk->hash] << std::endl;
        }*/

        /*struct timeval timeEnd;
        gettimeofday(&timeEnd, NULL);

        std::cout << "Vote handling cost partially threaded: "
                  << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
                  << " us to execute."
                  << std::endl;*/

    });

    /*
    gettimeofday(&timeEnd, NULL);

    std::cout << "Vote handling cost: "
              << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
              << " us to execute."
              << std::endl;*/
}

void HotStuffBase::vote_relay_handler(MsgRelay &&msg, const Net::conn_t &conn) {
    struct timeval timeStart, timeEnd;
    gettimeofday(&timeStart, NULL);

    const auto &peer = conn->get_peer_id();
    if (peer.is_null()) return;
    msg.postponed_parse(this);
    //std::cout << "vote relay handler: " << msg.vote.blk_hash.to_hex() << std::endl;
    
    /*领导者节点处理处于区块队列的区块的投票*/
    if (id == pmaker->get_proposer() && !piped_queue.empty() && std::find(piped_queue.begin(), piped_queue.end(), msg.vote.blk_hash) != piped_queue.end()) {
        HOTSTUFF_LOG_PROTO("piped block");
        block_t blk = storage->find_blk(msg.vote.blk_hash);
        if (!blk->delivered) {
            process_block(blk, false);
            HOTSTUFF_LOG_PROTO("Normalized piped block");
        }
    }

    block_t blk = get_potentially_not_delivered_blk(msg.vote.blk_hash);
    if (!blk->delivered && blk->self_qc == nullptr) {
        blk->self_qc = create_quorum_cert(blk->get_hash());
        part_cert_bt part = create_part_cert(*priv_key, blk->get_hash());
        blk->self_qc->add_part(config, id, *part);

        std::cout << "create cert: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
    }

    if (blk->self_qc->has_n(config.nmajority)) {
        std::cout << "bye vote relay handler: " << msg.vote.blk_hash.to_hex() << " " << &blk->self_qc << std::endl;
        if (id == pmaker->get_proposer() && blk->hash == piped_queue.front()) {
            piped_queue.pop_front();
            HOTSTUFF_LOG_PROTO("Reset Piped block");

            if (!rdy_queue.empty()) {
                auto curr_blk = blk;
                bool foundChildren = true;
                while (foundChildren) {
                    foundChildren = false;
                    for (const auto &hash : rdy_queue) {
                        block_t rdy_blk = storage->find_blk(hash);
                        if (rdy_blk->get_parent_hashes()[0] == curr_blk->hash) {
                            HOTSTUFF_LOG_PROTO("Resolved block in rdy queue %s", hash.to_hex().c_str());
                            rdy_queue.erase(std::find(rdy_queue.begin(), rdy_queue.end(), hash));
                            piped_queue.erase(std::find(piped_queue.begin(), piped_queue.end(), hash));

                            update_hqc(rdy_blk, rdy_blk->self_qc);
                            on_qc_finish(rdy_blk);
                            foundChildren = true;
                            curr_blk = rdy_blk;
                            break;
                        }
                    }
                }
            }
        }

        /*if (id == get_pace_maker()->get_proposer()) {
            gettimeofday(&timeEnd, NULL);
            long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
            stats[blk->hash] = stats[blk->hash] + usec;
            HOTSTUFF_LOG_PROTO("result: %s, %s ", blk->hash.to_hex().c_str(), std::to_string(stats[blk->parent_hashes[0]]).c_str());
        }*/
        return;
    }

    std::cout << "vote relay handler: " << msg.vote.blk_hash.to_hex() << " " << std::endl;

    //auto &vote = msg.vote;
    RcObj<VoteRelay> v(new VoteRelay(std::move(msg.vote)));
    promise::all(std::vector<promise_t>{
            async_deliver_blk(v->blk_hash, peer),
            v->cert->verify(config, vpool),
    }).then([this, blk, v=std::move(v), timeStart](const promise::values_t& values) {
        struct timeval timeEnd;

        if (!promise::any_cast<bool>(values[1]))
            LOG_WARN ("invalid vote-relay");
        auto &cert = blk->self_qc;
        std::cout << "got relay and verified" << std::endl;

        if (cert != nullptr && cert->get_obj_hash() == blk->get_hash() && !cert->has_n(config.nmajority)) {
            if (id != pmaker->get_proposer() && cert->has_n(numberOfChildren + 1))
            {
                return;
            }

            cert->merge_quorum(*v->cert);

            std::cout << "merge quorum " << std::endl;
            if (id != pmaker->get_proposer()) {
                if (!cert->has_n(numberOfChildren + 1)) return;
                cert->compute();
                if (!cert->verify(config)) {
                    throw std::runtime_error("Invalid Sigs in intermediate signature!");
                }
                std::cout << "Send Vote Relay: " << v->blk_hash.to_hex() << std::endl;
                pn.send_msg(MsgRelay(VoteRelay(v->blk_hash, cert.get()->clone(), this)), parentPeer);
                return;
            }

            //HOTSTUFF_LOG_PROTO("got %s", std::string(*v).c_str());

            if (!cert->has_n(config.nmajority)) {
                /*if (id == get_pace_maker()->get_proposer()) {
                    gettimeofday(&timeEnd, NULL);
                    long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
                    std::cout << usec << " a:a " << stats[blk->hash] << std::endl;
                    stats[blk->hash] = stats[blk->hash] + usec;
                    std::cout << usec << " b:b " << stats[blk->hash] << std::endl;
                }*/
                return;
            }

            cert->compute();
            if (!cert->verify(config)) {
                HOTSTUFF_LOG_PROTO("Error, Invalid Sig!!!");
                return;
            }

            if (!piped_queue.empty()) {
                if (blk->hash == piped_queue.front()) {
                    piped_queue.pop_front();
                    HOTSTUFF_LOG_PROTO("Reset Piped block");

                    std::cout << "go to town: " << std::endl;

                    update_hqc(blk, cert);
                    on_qc_finish(blk);

                    if (!rdy_queue.empty()) {
                        auto curr_blk = blk;
                        bool foundChildren = true;
                        while (foundChildren) {
                            foundChildren = false;
                            for (const auto &hash : rdy_queue) {
                                block_t rdy_blk = storage->find_blk(hash);
                                if (rdy_blk->get_parent_hashes()[0] == curr_blk->hash) {
                                    HOTSTUFF_LOG_PROTO("Resolved block in rdy queue %s", hash.to_hex().c_str());
                                    rdy_queue.erase(std::find(rdy_queue.begin(), rdy_queue.end(), hash));
                                    piped_queue.erase(std::find(piped_queue.begin(), piped_queue.end(), hash));

                                    update_hqc(rdy_blk, rdy_blk->self_qc);
                                    on_qc_finish(rdy_blk);
                                    foundChildren = true;
                                    curr_blk = rdy_blk;
                                    break;
                                }
                            }
                        }
                    }
                }
                else {
                    auto place = std::find(piped_queue.begin(), piped_queue.end(), blk->hash);
                    if (place != piped_queue.end()) {
                        HOTSTUFF_LOG_PROTO("Failed resetting piped block, wasn't front! Adding to rdy_queue %s", blk->hash.to_hex().c_str());
                        rdy_queue.push_back(blk->hash);

                        // Don't finish this block until the previous one was finished.
                        return;
                    }
                    else {
                        std::cout << "go to town: " << std::endl;

                        update_hqc(blk, cert);
                        on_qc_finish(blk);
                    }
                }
            }
            else
            {
                std::cout << "go to town: " << std::endl;

                update_hqc(blk, cert);
                on_qc_finish(blk);
            }
            
            /*if (id == get_pace_maker()->get_proposer()) {
                gettimeofday(&timeEnd, NULL);
                long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
                stats[blk->hash] = stats[blk->hash] + usec;
                HOTSTUFF_LOG_PROTO("result: %s, %s ", blk->hash.to_hex().c_str(), std::to_string(stats[blk->hash]).c_str());
            }*/

            /*
            struct timeval timeEnd;
            gettimeofday(&timeEnd, NULL);

            std::cout << "Vote relay handling cost partially threaded: "
                      << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
                      << " us to execute."
                      << std::endl;*/
        }
        /*else {
            if (id == get_pace_maker()->get_proposer()) {
                gettimeofday(&timeEnd, NULL);
                long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
                stats[blk->hash] = stats[blk->hash] + usec;
                HOTSTUFF_LOG_PROTO("result: %s, %s ", blk->hash.to_hex().c_str(), std::to_string(stats[blk->parent_hashes[0]]).c_str());
            }
        }*/
    });

    /*gettimeofday(&timeEnd, NULL);

    std::cout << "Vote relay handling cost: "
              << ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec)
              << " us to execute."
              << std::endl;*/
}

void HotStuffBase::req_blk_handler(MsgReqBlock &&msg, const Net::conn_t &conn) {
    const PeerId replica = conn->get_peer_id();
    if (replica.is_null()) return;
    auto &blk_hashes = msg.blk_hashes;
    std::vector<promise_t> pms;
    for (const auto &h: blk_hashes)
        pms.push_back(async_fetch_blk(h, nullptr));
    promise::all(pms).then([replica, this](const promise::values_t values) {
        std::vector<block_t> blks;
        for (auto &v: values)
        {
            auto blk = promise::any_cast<block_t>(v);
            blks.push_back(blk);
        }
        pn.send_msg(MsgRespBlock(blks), replica);
    });
}

void HotStuffBase::resp_blk_handler(MsgRespBlock &&msg, const Net::conn_t &) {
    msg.postponed_parse(this);
    for (const auto &blk: msg.blks)
        if (blk) on_fetch_blk(blk);
}

void HotStuffBase::newview_handler(MsgNewview &&msg, const Net::conn_t &conn)
{
    struct timeval timeStart,timeEnd;/*暂时设置，不知道有什么用*/
    const auto &peer=conn->get_peer_id();
    if(peer.is_null()) return;
    msg.postponed_parse(this);

    /*待定是否需要判断newview消息中的视图号超过当前的最高视图号vheight*/
    if(id == pmaker->get_proposer()&&msg.newview->viewNum>vheight){
        ReplicaID ID=msg.newview->id;
         auto it=NVset_t.find(ID);
        if(it!=NVset_t.end()){
            it->second=msg.newview;
        }else{
            NVset_t[ID]=msg.newview;
        }
        if(NVset_t.size()>=config.nmajority){
            into_newview=true;
            vector<block_t> blks;
            for(const auto& new_view: NVset_t){
                NewView value=new_view.second;
                Proposal newproposal;
                if(value->Vote_Req!=nullptr){
                    newproposal=*(value->Vote_Req);
                }
                blks.push_back(newproposal.blk);
            }
            int heightest=blks[0]->height;
            block_t highblk=blks[0];
            for(int i=1;i<blks.size();i++){
                if(blks[i]->height>heightest){
                    heightest=blks[i]->height;
                    highblk=blks[i];
                }
            }
            quorum_cert_bt QCanc=highblk->qc->clone();
            block_t Banc;
            if(QCanc){
                Banc=storage->find_blk(QCanc->get_obj_hash());
                if(Banc==nullptr){
                    throw std::runtime_error("block referred by qc not fetched");
                }
            }
            std::unordered_map<block_t,int> numvote;
            for(const auto& newview:NVset_t){
                Vote newvote;
                if(newview.second->Vote_Resp!=nullptr){
                    newvote=*(newview.second->Vote_Resp);
                }
                block_t respBlk=storage->find_blk(newvote.blk_hash);
                if(respBlk==nullptr){
                    //throw std::runtime_error("block referred by qc not fetched");
                    continue;
                }
                if(numvote.count(respBlk)>0){
                    numvote[respBlk]+=1;
                }else{
                    numvote[respBlk]=1;
                }
            }
            for(auto num:numvote){
                if(num.second>=config.nmajority){
                    block_t blk=num.first;
                    if(blk->parents[0]==Banc){
                        finish_material=true;//cancel materialtime
                        material_blk.push_back(blk);
                    }
                }
            }
        
        // TimerEvent timer;
        // // timer = TimerEvent(ec, [this](TimerEvent &){ rotate(); });
        // // timer.add(prop_delay);
        // timer=TimerEvent(ec,[this](TimerEvent &){ materialization(); }) 
        // timer.add(material_timeout);
    }
    }
}


bool HotStuffBase::conn_handler(const salticidae::ConnPool::conn_t &conn, bool connected) {
    if (connected)
    {
        auto cert = conn->get_peer_cert();
        //SALTICIDAE_LOG_INFO("%s", salticidae::get_hash(cert->get_der()).to_hex().c_str());
        return (!cert) || valid_tls_certs.count(salticidae::get_hash(cert->get_der()));
    }
    return true;
}

void HotStuffBase::print_stat() const {
    LOG_INFO("===== begin stats =====");
    LOG_INFO("-------- queues -------");
    LOG_INFO("blk_fetch_waiting: %lu", blk_fetch_waiting.size());
    LOG_INFO("blk_delivery_waiting: %lu", blk_delivery_waiting.size());
    LOG_INFO("decision_waiting: %lu", decision_waiting.size());
    LOG_INFO("-------- misc ---------");
    LOG_INFO("fetched: %lu", fetched);
    LOG_INFO("delivered: %lu", delivered);
    LOG_INFO("cmd_cache: %lu", storage->get_cmd_cache_size());
    LOG_INFO("blk_cache: %lu", storage->get_blk_cache_size());
    LOG_INFO("------ misc (10s) -----");
    LOG_INFO("fetched: %lu", part_fetched);
    LOG_INFO("delivered: %lu", part_delivered);
    LOG_INFO("decided: %lu", part_decided);
    LOG_INFO("gened: %lu", part_gened);
    LOG_INFO("avg. parent_size: %.3f",
            part_delivered ? part_parent_size / double(part_delivered) : 0);
    LOG_INFO("delivery time: %.3f avg, %.3f min, %.3f max",
            part_delivered ? part_delivery_time / double(part_delivered) : 0,
            part_delivery_time_min == double_inf ? 0 : part_delivery_time_min,
            part_delivery_time_max);

    part_parent_size = 0;
    part_fetched = 0;
    part_delivered = 0;
    part_decided = 0;
    part_gened = 0;
    part_delivery_time = 0;
    part_delivery_time_min = double_inf;
    part_delivery_time_max = 0;
#ifdef HOTSTUFF_MSG_STAT
    LOG_INFO("--- replica msg. (10s) ---");
    size_t _nsent = 0;
    size_t _nrecv = 0;
    for (const auto &replica: peers)
    {
        try {
            auto conn = pn.get_peer_conn(replica);
            if (conn == nullptr) continue;
            size_t ns = conn->get_nsent();
            size_t nr = conn->get_nrecv();
            size_t nsb = conn->get_nsentb();
            size_t nrb = conn->get_nrecvb();
            conn->clear_msgstat();
            LOG_INFO("%s: %u(%u), %u(%u), %u",
                     get_hex10(replica).c_str(), ns, nsb, nr, nrb, part_fetched_replica[replica]);
            _nsent += ns;
            _nrecv += nr;
            part_fetched_replica[replica] = 0;
        }
        catch (...) { }
    }
    nsent += _nsent;
    nrecv += _nrecv;
    LOG_INFO("sent: %lu", _nsent);
    LOG_INFO("recv: %lu", _nrecv);
    LOG_INFO("--- replica msg. total ---");
    LOG_INFO("sent: %lu", nsent);
    LOG_INFO("recv: %lu", nrecv);
#endif
    LOG_INFO("====== end stats ======");
}

HotStuffBase::HotStuffBase(uint32_t blk_size,
                    ReplicaID rid,
                    privkey_bt &&priv_key,
                    NetAddr listen_addr,
                    pacemaker_bt pmaker,
                    EventContext ec,
                    size_t nworker,
                    const Net::Config &netconfig):
        HotStuffCore(rid, std::move(priv_key)),
        listen_addr(listen_addr),
        blk_size(blk_size),
        ec(ec),
        tcall(ec),
        vpool(ec, nworker),
        pn(ec, netconfig),
        pmaker(std::move(pmaker)),

        fetched(0), delivered(0),
        nsent(0), nrecv(0),
        part_parent_size(0),
        part_fetched(0),
        part_delivered(0),
        part_decided(0),
        part_gened(0),
        part_delivery_time(0),
        part_delivery_time_min(double_inf),
        part_delivery_time_max(0)
{
    /* register the handlers for msg from replicas */
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::propose_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::vote_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::req_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::resp_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::vote_relay_handler, this, _1, _2));
    pn.reg_handler(salticidae::generic_bind(&HotStuffBase::newview_handler, this, _1, _2));
    pn.reg_conn_handler(salticidae::generic_bind(&HotStuffBase::conn_handler, this, _1, _2));
    pn.start();
    pn.listen(listen_addr);
}

void HotStuffBase::do_broadcast_proposal(const Proposal &prop) {
    pn.multicast_msg(MsgPropose(prop), std::vector<PeerId>(childPeers.begin(), childPeers.end()));
}

void HotStuffBase::do_vote(Proposal prop, const Vote &vote) {
    //HOTSTUFF_LOG_PROTO("do vote");

    pmaker->beat_resp(prop.proposer).then([this, vote, prop](ReplicaID proposer) {

        if (proposer == get_id())
        {
            throw HotStuffError("unreachable line");
        }
            /*插入newview消息*/
            // NewView it=NVset_t.find(id);
            // if(it!=NVset_t.end()){
            //     it.Vote_Resp=vote;
            //     it.viewNum=vheight;
            //     it.id=id;
            // }else{
            //     NewView newview;
            //     newview.Vote_Resp=vote;
            //     newview.viewNum=vheight;
            //     newview.id=id;
            //     NVset_t[id]=newview;
            // }
            nextview->id=id;
            //nextview.viewNum=vheight;
            Vote newvote=vote;
            nextview->Vote_Resp=&newvote;

        if (childPeers.empty()) {
            //HOTSTUFF_LOG_PROTO("send vote");
            pn.send_msg(MsgVote(vote), parentPeer);
        } else {
            block_t blk = get_delivered_blk(vote.blk_hash);
            if (blk->self_qc == nullptr)
            {
                //HOTSTUFF_LOG_PROTO("create cert");
                blk->self_qc = create_quorum_cert(prop.blk->get_hash());
                blk->self_qc->add_part(config, vote.voter, *vote.cert);
            }
        }
    });
}

void HotStuffBase::do_consensus(const block_t &blk) {
    pmaker->on_consensus(blk);
}

void HotStuffBase::materialization()
{
}

void HotStuffBase::do_decide(Finality &&fin) {
    part_decided++;
    state_machine_execute(fin);
    auto it = decision_waiting.find(fin.cmd_hash);
    if (it != decision_waiting.end())
    {
        it->second(std::move(fin));
        decision_waiting.erase(it);
    }
}

HotStuffBase::~HotStuffBase() {}

void HotStuffBase::start(std::vector<std::tuple<NetAddr, pubkey_bt, uint256_t>> &&replicas, bool ec_loop) {

    std::set<uint16_t> children;
    auto size = replicas.size();

    childPeers.clear();
    for (size_t i = 0; i < size; i++) {

        auto cert_hash = std::move(std::get<2>(replicas[i]));
        salticidae::PeerId peer{cert_hash};
        valid_tls_certs.insert(cert_hash);
        auto &addr = std::get<0>(replicas[i]);

        HotStuffCore::add_replica(i, peer, std::move(std::get<1>(replicas[i])));
        if (addr != listen_addr) {
            peers.push_back(peer);
            pn.add_peer(peer);
            pn.set_peer_addr(peer, addr);
        }
    }

    size_t fanout = config.fanout;
    auto processesOnLevel = 1;
    bool done = false;

    size_t i = 0;
    /*后面这部分是开始划分树
    *i记录层数，start记录每一层开始的节点数
    */
    while (i < size) { // 0 // 11
        if (done) {
            break;
        }
        const size_t remaining = size - i; // 100 // 89

        const size_t max_fanout = ceil(remaining / processesOnLevel); //100 // 9
        auto curr_fanout = std::min(max_fanout, fanout); //10 // 9

        auto parent_cert_hash = std::move(std::get<2>(replicas[i]));
        salticidae::PeerId parent_peer{parent_cert_hash};

        auto start = i + processesOnLevel; // 11
        for (auto counter = 1; counter <= processesOnLevel; counter++) { // 1 run // 10 runs
            if (done) {
                break;
            }
            for (size_t j = start; j < start + curr_fanout; j++) { // j = 1 -> 10 // j = 11 -> 21 // (i = 11) j = 22 - 32
                if (j >= size) {
                    done = true;
                    break;
                }
                auto cert_hash = std::move(std::get<2>(replicas[j]));
                salticidae::PeerId peer{cert_hash};

                if (id == i) {
                    HOTSTUFF_LOG_PROTO("Adding Child Process: %lld", j);
                    children.insert(j);/*添加子节点序号*/
                    childPeers.insert(peer);
                } else if (id == j) {
                    HOTSTUFF_LOG_PROTO("Setting Parent Process: %lld", i);
                    parentPeer = parent_peer;/*设置父节点值*/
                } else if (childPeers.find(parent_peer) != childPeers.end()) {
                    children.insert(j);
                }
            }
            start += curr_fanout;
            i++;
            parent_cert_hash = std::move(std::get<2>(replicas[i]));
            salticidae::PeerId temp_parent_peer{parent_cert_hash};
            parent_peer = temp_parent_peer;
        } // i = 1
        processesOnLevel = std::min(curr_fanout * processesOnLevel, remaining); // 10
    }

    HOTSTUFF_LOG_PROTO("total children: %d", children.size());
    numberOfChildren = children.size();

    vector<PeerId> newPeers;
    copy(peers.begin(), peers.end(), back_inserter(newPeers));

    std::shuffle(newPeers.begin(), newPeers.end(), std::mt19937(std::random_device()()));
    for (const PeerId& peer : newPeers) {
        pn.conn_peer(peer);
        usleep(10);
    }

    std::cout << " total children: " << children.size() << std::endl;
    numberOfChildren = children.size();

    /* ((n - 1) + 1 - 1) / 3 */
    uint32_t nfaulty = peers.size() / 3;
    if (nfaulty == 0)
        LOG_WARN("too few replicas in the system to tolerate any failure");
    on_init(nfaulty);//初始化
    pmaker->init(this);
    if (ec_loop)
        ec.dispatch();

    cmd_pending_buffer.reserve(blk_size);
    cmd_pending.reg_handler(ec, [this](cmd_queue_t &q) {
        std::pair<uint256_t, commit_cb_t> e;
        while (q.try_dequeue(e))
        {
            ReplicaID proposer = pmaker->get_proposer();
            if (proposer != get_id()) {
                continue;
            }

            if (cmd_pending_buffer.size() < blk_size && final_buffer.empty()) {
                const auto &cmd_hash = e.first;
                auto it = decision_waiting.find(cmd_hash);
                if (it == decision_waiting.end())
                    it = decision_waiting.insert(std::make_pair(cmd_hash, e.second)).first;
                
                e.second(Finality(id, 0, 0, 0, cmd_hash, uint256_t()));
                cmd_pending_buffer.push_back(cmd_hash);
            }
            else {
                e.second(Finality(id, 0, 0, 0, e.first, uint256_t()));
            }

            if (cmd_pending_buffer.size() >= blk_size || !final_buffer.empty()) {
                if (final_buffer.empty())
                {
                    final_buffer = std::move(cmd_pending_buffer);
                }

                beat();
                return true;
            }
        }
        return false;
    });
}

void HotStuffBase::beat() {
    pmaker->beat().then([this](ReplicaID proposer) {
        if (piped_queue.size() > get_config().async_blocks + 1) {
            return;
        }

        HOTSTUFF_LOG_PROTO("Proposing: %d", final_buffer.size());
        if (proposer == get_id()) {
            struct timeval timeStart, timeEnd;
            gettimeofday(&timeStart, NULL);

            auto parents = pmaker->get_parents();

            struct timeval current_time;
            gettimeofday(&current_time, NULL);
            block_t current = pmaker->get_current_proposal();

            if (piped_queue.size() < get_config().async_blocks && current != get_genesis()) {

                if (piped_queue.empty() && ((current_time.tv_sec - last_block_time.tv_sec) * 1000000 + current_time.tv_usec -last_block_time.tv_usec) / 1000 < config.piped_latency) {
                    HOTSTUFF_LOG_PROTO("omitting propose");
                } else {
                    block_t highest = current;
                    for (auto p_hash : piped_queue) {
                        block_t block = storage->find_blk(p_hash);
                        if (block->height > highest->height) {
                            highest = block;
                        }
                    }

                    if (parents[0]->height < highest->height) {
                        parents.insert(parents.begin(), highest);
                    }
                    block_t piped_block = storage->add_blk(new Block(parents, final_buffer,
                                                             hqc.second->clone(), bytearray_t(),
                                                             parents[0]->height + 1,
                                                             current,
                                                             nullptr,
                                                             NVset_t));
                    piped_queue.push_back(piped_block->hash);

                    Proposal prop(id, piped_block, nullptr);
                    HOTSTUFF_LOG_PROTO("propose piped %s", std::string(*piped_block).c_str());
                    /* broadcast to other replicas */
                    gettimeofday(&last_block_time, NULL);
                    do_broadcast_proposal(prop);

                    /*if (id == get_pace_maker()->get_proposer()) {
                        gettimeofday(&timeEnd, NULL);
                        long usec = ((timeEnd.tv_sec - timeStart.tv_sec) * 1000000 + timeEnd.tv_usec - timeStart.tv_usec);
                        stats.insert(std::make_pair(piped_block->hash, usec));
                    }*/
                    piped_submitted = false;
                }
            } else {
                gettimeofday(&last_block_time, NULL);
                on_propose(final_buffer, std::move(parents));
            }
        }
    });
}
}