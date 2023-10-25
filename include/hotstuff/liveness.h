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

#ifndef _HOTSTUFF_LIVENESS_H
#define _HOTSTUFF_LIVENESS_H

#include "salticidae/util.h"
#include "hotstuff/hotstuff.h"
// #include <iostream>
// #include <thread>
// #include <mutex>
// #include <condition_variable>
// std::mutex mtx;
// std::condition_variable cv;

namespace hotstuff {

using salticidae::_1;
using salticidae::_2;

/** Abstraction for liveness gadget (oracle). */
/*pacemaker包含更换领导者等函数*/
class PaceMaker {
    protected:
    HotStuffCore *hsc;
    public:
    bool view_change;/*为0表示fast,为1表示slow*/
    virtual ~PaceMaker() = default;
    /** Initialize the PaceMaker. A derived class should also call the
     * default implementation to set `hsc`. */
    virtual void init(HotStuffCore *_hsc) { hsc = _hsc; }
    /** Get a promise resolved when the pace maker thinks it is a *good* time
     * to issue new commands. When promise is resolved, the replica should
     * propose the command. */
    virtual promise_t beat() = 0;//???
    /** Get the current proposer. */
    virtual ReplicaID get_proposer() = 0;
    /** Select the parent blocks for a new block.
     * @return Parent blocks. The block at index 0 is the direct parent, while
     * the others are uncles/aunts. The returned vector should be non-empty. */
    virtual std::vector<block_t> get_parents() = 0;
    /** Get a promise resolved when the pace maker thinks it is a *good* time
     * to vote for a block. The promise is resolved with the next proposer's ID
     * */
    virtual promise_t beat_resp(ReplicaID last_proposer) = 0;//发生vc
    /** Impeach the current proposer. */
    virtual void impeach() {}
    //virtual void impeach_fast() {}

    virtual void on_consensus(const block_t &) {}
    virtual size_t get_pending_size() = 0;

    virtual block_t get_current_proposal() { }
};

using pacemaker_bt = BoxObj<PaceMaker>;

/** Parent selection implementation for PaceMaker: select the highest tail that
 * follows the current hqc block. */
class PMHighTail: public virtual PaceMaker {
    block_t hqc_tail;
    const int32_t parent_limit;         /**< maximum number of parents */

    //检查a是否是b的祖先
    bool check_ancestry(const block_t &_a, const block_t &_b) {
        block_t b;
        for (b = _b;
            b->get_height() > _a->get_height();
            b = b->get_parents()[0]);
        return b == _a;
    }

    //重新启动异步更新
    void reg_hqc_update() {
        hsc->async_hqc_update().then([this](const block_t &hqc) {
            hqc_tail = hqc;
            for (const auto &tail: hsc->get_tails())
                if (check_ancestry(hqc, tail) && tail->get_height() > hqc_tail->get_height())
                    hqc_tail = tail;
            reg_hqc_update();
        });
    }

    //获取最新的区块
    void reg_proposal() {
        hsc->async_wait_proposal().then([this](const Proposal &prop) {
            hqc_tail = prop.blk;
            reg_proposal();
        });
    }

    void reg_receive_proposal() {
        hsc->async_wait_receive_proposal().then([this](const Proposal &prop) {
            const auto &hqc = hsc->get_hqc();
            const auto &blk = prop.blk;
            if (check_ancestry(hqc, blk) && blk->get_height() > hqc_tail->get_height())
                hqc_tail = blk;
            reg_receive_proposal();
        });//获得区块的时候更新hqc
        //总的更新hqc在两种情况1、启动异步更新，用尾部区块更新hqc；2、获得新区块的时候用新区块更新hqc
    }

    public:
    PMHighTail(int32_t parent_limit): parent_limit(parent_limit) {}
    void init() {
        hqc_tail = hsc->get_genesis();
        reg_hqc_update();
        reg_proposal();
        reg_receive_proposal();
    }//qc更新的起始函数

    std::vector<block_t> get_parents() override {
        const auto &tails = hsc->get_tails();
        std::vector<block_t> parents{hqc_tail};
        // TODO: inclusive block chain
        // auto nparents = tails.size();
        // if (parent_limit > 0)
        //     nparents = std::min(nparents, (size_t)parent_limit);
        // nparents--;
        // /* add the rest of tails as "uncles/aunts" */
        // for (const auto &blk: tails)
        // {
        //     if (blk != hqc_tail)
        //     {
        //         parents.push_back(blk);
        //         if (!--nparents) break;
        //     }
        // }
        return parents;
    }
};

/** Beat implementation for PaceMaker: simply wait for the QC of last proposed
 * block.  PaceMakers derived from this class will beat only when the last
 * block proposed by itself gets its QC. */
class PMWaitQC: public virtual PaceMaker {
    std::queue<promise_t> pending_beats;
    block_t last_proposed;
    bool locked;
    promise_t pm_wait_propose;

    protected:
    void schedule_next() {
        if (!pending_beats.empty())
        {
            if (locked) {
                struct timeval current_time;
                gettimeofday(&current_time, NULL);
                //最新区块没有被提交且流水线队列大小小于区块值同时现在离最后一个区块提交的时间差要大于流水线延迟
                if (hsc->piped_queue.size() < hsc->get_config().async_blocks
                && !hsc->piped_submitted
                && ((current_time.tv_sec - hsc->last_block_time.tv_sec) * 1000000 + current_time.tv_usec - hsc->last_block_time.tv_usec) / 1000 > hsc->get_config().piped_latency) {
                    HOTSTUFF_LOG_PROTO("Extra block");
                    auto pm = pending_beats.front();
                    pending_beats.pop();
                    hsc->piped_submitted = true;
                    pm.resolve(get_proposer());
                    return;
                }

                if (!hsc->piped_queue.empty() && hsc->b_normal_height > 0) {//大于0表明至少已经创建一个新的区块
                    block_t piped_block = hsc->storage->find_blk(hsc->piped_queue.back());//piped_block为队列最后一个区块
                    if ( piped_block->get_height() > hsc->get_config().async_blocks + 10 && hsc->b_normal_height < piped_block->get_height() - (hsc->get_config().async_blocks + 10)//没看懂
                            && ((current_time.tv_sec - hsc->last_block_time.tv_sec) * 1000000 + current_time.tv_usec - hsc->last_block_time.tv_usec) / 1000 > hsc->get_config().piped_latency) {
                        HOTSTUFF_LOG_PROTO("Extra recovery block %d %d", hsc->b_normal_height, piped_block->get_height());

                        auto pm = pending_beats.front();
                        pending_beats.pop();
                        hsc->piped_submitted = true;
                        pm.resolve(get_proposer());
                    }
                }
            } else {
                auto pm = pending_beats.front();
                pending_beats.pop();
                hsc->async_qc_finish(last_proposed)
                        .then([this, pm]() {
                            pm.resolve(get_proposer());
                        });
                locked = true;//进入locked
            }
        }
        else
        {
            std::cout << "not enough client tx" << std::endl;
        }
    }

    void update_last_proposed() {
        pm_wait_propose.reject();
        (pm_wait_propose = hsc->async_wait_proposal()).then(
                [this](const Proposal &prop) {
            last_proposed = prop.blk;
            locked = false;
            schedule_next();//开启下一个日程，接收到beat之后判断是否重新进入锁状态
            update_last_proposed();
        });
    }

    public:

    size_t get_pending_size() override { return pending_beats.size(); }

    void init() {
        last_proposed = hsc->get_genesis();
        locked = false;
        update_last_proposed();
    }

    ReplicaID get_proposer() override {
        return hsc->get_id();
    }

    block_t get_current_proposal() {
        return last_proposed;
    }

    promise_t beat() override {
        promise_t pm;
        pending_beats.push(pm);
        schedule_next();//有两种时机进入日程推进，第一种是在接收到一个beat的时候，第二种是在更新最后一个共识的时候
        return pm;
    }

    promise_t beat_resp(ReplicaID last_proposer) override {
        return promise_t([last_proposer](promise_t &pm) {
            pm.resolve(last_proposer);
        });
    }
};

/** Naive PaceMaker where everyone can be a proposer at any moment. */
struct PaceMakerDummy: public PMHighTail, public PMWaitQC {
    PaceMakerDummy(int32_t parent_limit):
        PMHighTail(parent_limit), PMWaitQC() {}
    void init(HotStuffCore *hsc) override {
        PaceMaker::init(hsc);
        PMHighTail::init();
        PMWaitQC::init();
    }
};

/** PaceMakerDummy with a fixed proposer. */
class PaceMakerDummyFixed: public PaceMakerDummy {
    ReplicaID proposer;

    public:
    PaceMakerDummyFixed(ReplicaID proposer,
                        int32_t parent_limit):
        PaceMakerDummy(parent_limit),
        proposer(proposer) {}

    ReplicaID get_proposer() override {
        return proposer;
    }

    promise_t beat_resp(ReplicaID) override {
        return promise_t([this](promise_t &pm) {
            pm.resolve(proposer);
        });
    }
};

using Net = PeerNetwork<opcode_t>;
/**
 * Simple long-standing round-robin style proposer liveness gadget.
 */
class PMRoundRobinProposer: virtual public PaceMaker {
    
    double base_timeout;
    double exp_timeout;//指数超时时间，用于指数退避
    //double material_timeout;//设置实例化超时时间
    double prop_delay;
    Net pmrr;
    EventContext ec;
    /** QC timer or randomized timeout */
    TimerEvent timer;
    /** the proposer it believes */
    ReplicaID proposer;
    std::unordered_map<ReplicaID, block_t> prop_blk;
    bool rotating;

    /* extra state needed for a proposer */
    std::queue<promise_t> pending_beats;
    block_t last_proposed;
    bool locked;
    promise_t pm_qc_finish;
    promise_t pm_wait_propose;
    promise_t pm_qc_manual;

    void reg_proposal() {
        hsc->async_wait_proposal().then([this](const Proposal &prop) {
            auto &pblk = prop_blk[hsc->get_id()];
            if (!pblk) pblk = prop.blk;
            if (rotating) reg_proposal();
        });
    }

    void reg_receive_proposal() {
        hsc->async_wait_receive_proposal().then([this](const Proposal &prop) {
            auto &pblk = prop_blk[prop.proposer];
            if (!pblk) pblk = prop.blk;
            if (rotating) reg_receive_proposal();
        });
    }

    void proposer_schedule_next() {
        if (!pending_beats.empty() && !locked)
        {
            auto pm = pending_beats.front();
            pending_beats.pop();
            pm_qc_finish.reject();
            (pm_qc_finish = hsc->async_qc_finish(last_proposed))
                .then([this, pm]() {
                    HOTSTUFF_LOG_PROTO("got QC, propose a new block");
                    pm.resolve(proposer);
                });
            locked = true;
        }
    }

    void proposer_update_last_proposed() {
        pm_wait_propose.reject();
        (pm_wait_propose = hsc->async_wait_proposal()).then(
                [this](const Proposal &prop) {
            last_proposed = prop.blk;
            locked = false;
            proposer_schedule_next();
            proposer_update_last_proposed();
        });
    }

    void do_new_consensus(int x, const std::vector<uint256_t> &cmds) {
        block_t blk;
        if(!hsc->material_blk.empty()){
            blk=hsc->material_blk.front();
            hsc->material_blk.erase(hsc->material_blk.begin());
        }//Add an instantiated block, get the instantiation information from the new view message, and then add the instantiated block.
        else blk = hsc->on_propose(cmds, get_parents(), bytearray_t());
        pm_qc_manual.reject();
        (pm_qc_manual = hsc->async_qc_finish(blk))
            .then([this, x]() {
                HOTSTUFF_LOG_PROTO("Pacemaker: got QC for block %d", x);
#ifdef HOTSTUFF_TWO_STEP
                if (x >= 2) return;
#else
                if (x >= 3) 
                {
                    view_change=0;
                    impeach();//fast_view_change
                    return;
                }
#endif
                do_new_consensus(x + 1, std::vector<uint256_t>{});//共识协议运行过程，根据是否宏定义二阶段判断何时结束共识，每收到一个完成的qc就重新启动一次
            });
    }

    /*class MyObject {
public:
    MyObject() : isChanged(false) {}

    void waitForChangeOrTimeout(TimerEvent &timer) {
        // 设置定时器的回调函数
        timer.set_callback([this](TimerEvent &) {
            if (!isChanged) {
                std::cout << "Timeout occurred. isChanged = " << isChanged << std::endl;
            }
        });

        // 启动定时器
        timer.add(5.0);  // 设置超时时间为5秒

        while (!isChanged) {
            // 在等待对象的成员改变时，事件循环继续运行
            uv_run(timer.get_ec().get(), UV_RUN_ONCE);
        }

        // 对象的成员发生改变，停止定时器
        timer.del();
        std::cout << "Wait finished. isChanged = " << isChanged << std::endl;
    }

    void changeOccurred() {
        isChanged = true;
    }

    void rotate() {
        // 处理定时器触发时的操作
        std::cout << "Timer triggered. Rotating..." << std::endl;
    }

private:
    bool isChanged;
};
*/

    void on_exp_timeout(TimerEvent &) {
        if(proposer!=hsc->get_id()){
            NewView nextview=hsc->get_nextview();
            nextview->viewNum=hsc->get_vheight()+1;
            pmrr.send_msg(MsgNewview(nextview),hsc->get_config().get_peer_id(proposer));
        } 
        else if (proposer == hsc->get_id())
        {
            TimerEvent mal_timer=TimerEvent(ec,[this](TimerEvent &){hsc->finish_material=true;});
            mal_timer.add(prop_delay);//material_time=prop_delay
            timer = TimerEvent(ec, [this](TimerEvent &){ 
                rotate(); 
                });
            timer.add(5*prop_delay);//viewtime=5*prop_delay
            while(!hsc->into_newview){
            }
            timer.del();
            while(!hsc->finish_material){}
            mal_timer.del();
            do_new_consensus(0, std::vector<uint256_t>{});
        }
    
        timer = TimerEvent(ec, [this](TimerEvent &){ rotate(); });
        timer.add(prop_delay);
    }
   
    /* role transitions */

    void rotate() {
        reg_proposal();
        reg_receive_proposal();
        prop_blk.clear();
        rotating = true;
        proposer = (proposer + 1) % hsc->get_config().nreplicas;
        HOTSTUFF_LOG_PROTO("Pacemaker: rotate to %d", proposer);//跟换的领导者直接选择为下一个成员
        pm_qc_finish.reject();
        pm_wait_propose.reject();
        pm_qc_manual.reject();
        if(view_change==0){
            if(proposer==hsc->get_id())
                  do_new_consensus(0, std::vector<uint256_t>{});
            return;
        }
        // start timer
        else if(view_change==1)
        timer = TimerEvent(ec, salticidae::generic_bind(&PMRoundRobinProposer::on_exp_timeout, this, _1));/*设置定时事件并开始更换领导人*/
        timer.add(exp_timeout);//添加定时事件,设置超时时间
        exp_timeout *= 2;
    }

    void stop_rotate() {
        timer.del();
        HOTSTUFF_LOG_PROTO("Pacemaker: stop rotation at %d", proposer);
        pm_qc_finish.reject();
        pm_wait_propose.reject();
        pm_qc_manual.reject();
        rotating = false;
        locked = false;
        last_proposed = hsc->get_genesis();
        proposer_update_last_proposed();
        if (proposer == hsc->get_id())
        {
            auto hs = static_cast<hotstuff::HotStuffBase *>(hsc);
            hs->do_elected();
            hs->get_tcall().async_call([this, hs](salticidae::ThreadCall::Handle &) {
                auto &pending = hs->get_decision_waiting();
                if (!pending.size()) return;/*判断现在是否有待办事务*/
                HOTSTUFF_LOG_PROTO("reproposing pending commands");
                std::vector<uint256_t> cmds;
                for (auto &p: pending)
                    cmds.push_back(p.first);
                do_new_consensus(0, cmds);
            });/*发送新领导者上任消息*/
        }
    }

    protected:
    /*重置超时时间*/
    void on_consensus(const block_t &blk) override {
        timer.del();
        exp_timeout = base_timeout;
        if (prop_blk[proposer] == blk) {
            stop_rotate();
        }
    }

    void impeach() override {
        if (rotating) return;
        rotate();
        HOTSTUFF_LOG_INFO("schedule to the proposer");
    }

    // void impeach_fast() override{

    // } 

    public:
    PMRoundRobinProposer(const EventContext &ec,
                        double base_timeout, double prop_delay,const Net::Config &netconfig):
        base_timeout(base_timeout),
        prop_delay(prop_delay),
        ec(ec), proposer(0), rotating(false),pmrr(ec, netconfig){}

    size_t get_pending_size() override { return pending_beats.size(); }

    void init() {
        exp_timeout = base_timeout;
        stop_rotate();
    }

    ReplicaID get_proposer() override {
        return proposer;
    }

    promise_t beat() override {
        if (!rotating && proposer == hsc->get_id())
        {
            promise_t pm;
            pending_beats.push(pm);
            proposer_schedule_next();
            return pm;
        }
        else
            return promise_t([proposer=proposer](promise_t &pm) {
                pm.resolve(proposer);
            });
    }

    promise_t beat_resp(ReplicaID last_proposer) override {
        return promise_t([this](promise_t &pm) {
            pm.resolve(proposer);
        });
    }
};

struct PaceMakerRR: public PMHighTail, public PMRoundRobinProposer {
    PaceMakerRR(EventContext ec, int32_t parent_limit,
                double base_timeout = 1, double prop_delay = 1):
        PMHighTail(parent_limit),
        PMRoundRobinProposer(ec, base_timeout, prop_delay,Net::Config()) {}

    void init(HotStuffCore *hsc) override {
        PaceMaker::init(hsc);
        PMHighTail::init();
        PMRoundRobinProposer::init();
    }
};

}

#endif
