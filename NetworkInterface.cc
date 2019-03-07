/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Niket Agarwal
 *          Tushar Krishna
 */


#include "mem/ruby/network/garnet2.0/NetworkInterface.hh"

#include <cassert>
#include <cmath>

#include "base/cast.hh"
#include "base/stl_helpers.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/garnet2.0/Credit.hh"
#include "mem/ruby/network/garnet2.0/flitBuffer.hh"
#include "mem/ruby/slicc_interface/Message.hh"

using namespace std;
using m5::stl_helpers::deletePointers;

NetworkInterface::NetworkInterface(const Params *p)
    : ClockedObject(p), Consumer(this), m_id(p->id),
      m_virtual_networks(p->virt_nets), m_vc_per_vnet(p->vcs_per_vnet),
      m_num_vcs(m_vc_per_vnet * m_virtual_networks),
      m_deadlock_threshold(p->garnet_deadlock_threshold),
      vc_busy_counter(m_virtual_networks, 0)
     // inputFilename(p->inputFile) 
{
    m_router_id = -1;
    m_vc_round_robin = 0;
    m_ni_out_vcs.resize(m_num_vcs);
    m_ni_out_vcs_enqueue_time.resize(m_num_vcs);
    outCreditQueue = new flitBuffer();

    // instantiating the NI flit buffers
    for (int i = 0; i < m_num_vcs; i++) {
        m_ni_out_vcs[i] = new flitBuffer();
        m_ni_out_vcs_enqueue_time[i] = Cycles(INFINITE_);
    }

    m_vc_allocator.resize(m_virtual_networks); // 1 allocator per vnet
    for (int i = 0; i < m_virtual_networks; i++) {
        m_vc_allocator[i] = 0;
    }

    m_stall_count.resize(m_virtual_networks);

/*	else
	{
		std::string line;
		std::getline(inputFileHandler,line);
		cout<<"first line ="<<line<<endl;	
	}
*/
}

void
NetworkInterface::init()
{
    for (int i = 0; i < m_num_vcs; i++) {
        m_out_vc_state.push_back(new OutVcState(i, m_net_ptr));
    }

        inputFilename = m_net_ptr->inputFilename;

	inputFileHandler.open(inputFilename);

	if(!inputFileHandler.is_open()){
		std::cout<<"error while opening print.txt"<<std::endl;
	}
}

NetworkInterface::~NetworkInterface()
{
    deletePointers(m_out_vc_state);
    deletePointers(m_ni_out_vcs);
    delete outCreditQueue;
    delete outFlitQueue;
}

void
NetworkInterface::addInPort(NetworkLink *in_link,
                              CreditLink *credit_link)
{
    inNetLink = in_link;
    in_link->setLinkConsumer(this);
    outCreditLink = credit_link;
    credit_link->setSourceQueue(outCreditQueue);
}

void
NetworkInterface::addOutPort(NetworkLink *out_link,
                             CreditLink *credit_link,
                             SwitchID router_id)
{
    inCreditLink = credit_link;
    credit_link->setLinkConsumer(this);

    outNetLink = out_link;
    outFlitQueue = new flitBuffer();
    out_link->setSourceQueue(outFlitQueue);

    m_router_id = router_id;
}

void
NetworkInterface::addNode(vector<MessageBuffer *>& in,
                            vector<MessageBuffer *>& out)
{
    inNode_ptr = in;
    outNode_ptr = out;

    for (auto& it : in) {
        if (it != nullptr) {
            it->setConsumer(this);
        }
    }
}

void
NetworkInterface::dequeueCallback()
{
    // An output MessageBuffer has dequeued something this cycle and there
    // is now space to enqueue a stalled message. However, we cannot wake
    // on the same cycle as the dequeue. Schedule a wake at the soonest
    // possible time (next cycle).
    scheduleEventAbsolute(clockEdge(Cycles(1)));
}

void
NetworkInterface::incrementStats(flit *t_flit)
{
    int vnet = t_flit->get_vnet();

    // Latency
    m_net_ptr->increment_received_flits(vnet);
    Cycles network_delay =
        t_flit->get_dequeue_time() - t_flit->get_enqueue_time() - Cycles(1);
    Cycles src_queueing_delay = t_flit->get_src_delay();
    Cycles dest_queueing_delay = (curCycle() - t_flit->get_dequeue_time());
    Cycles queueing_delay = src_queueing_delay + dest_queueing_delay;

    m_net_ptr->increment_flit_network_latency(network_delay, vnet);
    m_net_ptr->increment_flit_queueing_latency(queueing_delay, vnet);

    if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
        m_net_ptr->increment_received_packets(vnet);
        m_net_ptr->increment_packet_network_latency(network_delay, vnet);
        m_net_ptr->increment_packet_queueing_latency(queueing_delay, vnet);
    }

    // Hops
    m_net_ptr->increment_total_hops(t_flit->get_route().hops_traversed);
}

/*
 * The NI wakeup checks whether there are any ready messages in the protocol
 * buffer. If yes, it picks that up, flitisizes it into a number of flits and
 * puts it into an output buffer and schedules the output link. On a wakeup
 * it also checks whether there are flits in the input link. If yes, it picks
 * them up and if the flit is a tail, the NI inserts the corresponding message
 * into the protocol buffer. It also checks for credits being sent by the
 * downstream router.
 */

void
NetworkInterface::wakeup()
{
    DPRINTF(RubyNetwork, "Network Interface %d connected to router %d "
            "woke up at time: %lld\n", m_id, m_router_id, curCycle());

    MsgPtr msg_ptr;
    Tick curTime = clockEdge();

/*****************send ACK packet*****************************/

    for(int vnet = 0; vnet < inNode_ptr.size();++vnet){
	    if(ackQueue.empty() == false){
		    flit * fl = ackQueue.front();
		    int vc = calculateVC(vnet);
		    if(vc != -1){
			    fl -> set_vc(vc);
			    RouteInfo route=fl->get_route();
			    route.vnet = vnet;
			    fl-> set_route(route);
			    fl->set_vnet(vnet);
			    m_ni_out_vcs[vc]->insert(fl);
			    m_ni_out_vcs_enqueue_time[vc]=curCycle();
			    m_out_vc_state[vc]->setState(ACTIVE_,curCycle());
			    ackQueue.erase(ackQueue.begin());
			    m_net_ptr->increment_injected_packets(vnet);
			    m_net_ptr->increment_injected_flits(vnet);

//			    cout<<"send flit ="<<fl->get_ack()<<endl;
		    }
	    }
    }
    if(Retran_Buffer.empty() == false){
	    retransmission pkt;
	    pkt = Retran_Buffer.front();
	    Message *net_msg_ptr = pkt.message.get();
	    int vnet = pkt.route.vnet;
	    int num_flits = (int)ceil ((double) m_net_ptr -> MessageSizeType_to_int(
			    net_msg_ptr -> getMessageSize())/m_net_ptr -> getNiFlitSize());
	    int vc=calculateVC(vnet);
	    if(vc != 1){
		    m_net_ptr -> increment_injected_packets(vnet);
		    for(int i = 0; i<num_flits; i++){
			    m_net_ptr -> increment_injected_flits(vnet);
			    flit *fl = new flit(i,vc,vnet,pkt.route,num_flits,pkt.flit,curCycle(), pkt.protection , RETRAN__);
			    fl->set_src_delay(curCycle() - ticksToCycles(pkt.message->getTime()));
			    m_ni_out_vcs[vc]->insert(fl);
		    }

		    m_ni_out_vcs_enqueue_time[vc] = curCycle();
		    m_out_vc_state[vc] -> setState(ACTIVE_,curCycle());
		    Retran_Buffer.erase(Retran_Buffer.begin());
//		    cout<<"retransmit the packet"<<endl;
	    }
    }	    

    // Checking for messages coming from the protocol
    // can pick up a message/cycle for each virtual net
    for (int vnet = 0; vnet < inNode_ptr.size(); ++vnet) {
        MessageBuffer *b = inNode_ptr[vnet];
        if (b == nullptr) {
            continue;
        }

        if (b->isReady(curTime)) { // Is there a message waiting
            msg_ptr = b->peekMsgPtr();
            if (flitisizeMessage(msg_ptr, vnet)) {
                b->dequeue(curTime);
            }
        }
    }

    scheduleOutputLink();
    checkReschedule();

    // Check if there are flits stalling a virtual channel. Track if a
    // message is enqueued to restrict ejection to one message per cycle.
    bool messageEnqueuedThisCycle = checkStallQueue();

    /*********** Check the incoming flit link **********/
    if (inNetLink->isReady(curCycle())) {
        flit *t_flit = inNetLink->consumeLink();
        int vnet = t_flit->get_vnet();
        t_flit->set_dequeue_time(curCycle());
	
	if(t_flit->get_ack() == ACK__){
		sendCredit(t_flit,true);
		for(int i=0; i < ARQ_Buffer.size();i++){
			if(ARQ_Buffer[i].flit == t_flit -> get_msg_ptr()){
				ARQ_Buffer.erase(ARQ_Buffer.begin()+i);
				//remove packet from the ARQ buffer
			}
		}
		incrementStats(t_flit);
		delete t_flit;
	}
	else if(t_flit->get_ack()==NACK__){
		sendCredit(t_flit,true);
//		cout<<"received NACK"<<end;
		for(int i=0; i< ARQ_Buffer.size();i++){
			if(ARQ_Buffer[i].flit == t_flit->get_msg_ptr()){
				Retran_Buffer.push_back(ARQ_Buffer[i]);
//				cout<<"remove NACK"<<endl;
				//Add packet to the reransmission buffer
			}
		}
		incrementStats(t_flit);
		delete t_flit;
	}

	else{
        // If a tail flit is received, enqueue into the protocol buffers if
        // space is available. Otherwise, exchange non-tail flits for credits.
        if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
            if (!messageEnqueuedThisCycle &&
                outNode_ptr[vnet]->areNSlotsAvailable(1, curTime)) {
                // Space is available. Enqueue to protocol buffer.
       //         outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(), curTime,
       //                                    cyclesToTicks(Cycles(1)));
/**************************Create ACK/NACK Packet***************************/
		NetDest personal_dest;
		NodeID destID=t_flit -> get_route().src_ni;
		for(int m = 0; m < (int) MachineType_NUM; m++){
			if((destID >= MachineType_base_number((MachineType) m   )) &&
			   (destID <  MachineType_base_number((MachineType)(m+1))) ){
				personal_dest.clear();
				personal_dest.add((MachineID){(MachineType)m, (destID - 
					       MachineType_base_number((MachineType)m))});
				break;
			}
		}
			
		RouteInfo route;
		int vc=calculateVC(t_flit->get_vnet());
		route.vnet = t_flit->get_vnet();
		route.net_dest = personal_dest;
		route.src_ni = t_flit -> get_route().dest_ni;
		route.src_router = t_flit -> get_route().dest_router;
		route.dest_ni = t_flit -> get_route().src_ni;
		route.dest_router = t_flit -> get_route().src_router;
		route.hops_traversed = -1;

		m_net_ptr -> increment_injected_packets(route.vnet);

		flit *fl;


		if(t_flit -> get_type()==HEAD_TAIL_){
			fl = new flit(0,vc,route.vnet,route,1,t_flit->get_msg_ptr(),curCycle()+Cycles(1),9 ,ACK__);
			if(t_flit->get_ack() == NORMAL__){
				outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(),curTime,cyclesToTicks(Cycles(1)));
			}
		}
		else if(rand()%100000<=((1-pow(1-error_rate,t_flit->get_route().hops_traversed))*100000)){
			fl = new flit(0,vc,route.vnet,route,1,t_flit->get_msg_ptr(),curCycle()+Cycles(1),9 ,NACK__);
			if(t_flit->get_ack() == NORMAL__){
				outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(), curTime, cyclesToTicks(Cycles(1)));
//				cout<<"packet protection ="<<t_flit->get_protect()<<endl;
			}
//			cout<<"created NACK packet"<<endl;
		}
		else{
			fl = new flit(0,vc,route.vnet,route,1,t_flit->get_msg_ptr(),curCycle()+Cycles(1),9 ,ACK__);
			if(t_flit->get_ack() == NORMAL__){
				outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(),curTime,cyclesToTicks(Cycles(1)));
			}
		}

		fl->set_src_delay(Cycles(1));
		ackQueue.push_back(fl);


/******************************END******************************************/
                // Simply send a credit back since we are not buffering
                // this flit in the NI
                sendCredit(t_flit, true);

                // Update stats and delete flit pointer
                incrementStats(t_flit);
                delete t_flit;
            } else {
                // No space available- Place tail flit in stall queue and set
                // up a callback for when protocol buffer is dequeued. Stat
                // update and flit pointer deletion will occur upon unstall.
                m_stall_queue.push_back(t_flit);
                m_stall_count[vnet]++;

                auto cb = std::bind(&NetworkInterface::dequeueCallback, this);
                outNode_ptr[vnet]->registerDequeueCallback(cb);
            }
        } else {
            // Non-tail flit. Send back a credit but not VC free signal.
            sendCredit(t_flit, false);

            // Update stats and delete flit pointer.
            incrementStats(t_flit);
            delete t_flit;
          }
    
	}
    }

    /****************** Check the incoming credit link *******/

    if (inCreditLink->isReady(curCycle())) {
        Credit *t_credit = (Credit*) inCreditLink->consumeLink();
        m_out_vc_state[t_credit->get_vc()]->increment_credit();
        if (t_credit->is_free_signal()) {
            m_out_vc_state[t_credit->get_vc()]->setState(IDLE_, curCycle());
        }
        delete t_credit;
    }


    // It is possible to enqueue multiple outgoing credit flits if a message
    // was unstalled in the same cycle as a new message arrives. In this
    // case, we should schedule another wakeup to ensure the credit is sent
    // back.
    if (outCreditQueue->getSize() > 0) {
        outCreditLink->scheduleEventAbsolute(clockEdge(Cycles(1)));
    }
}

void
NetworkInterface::sendCredit(flit *t_flit, bool is_free)
{
    Credit *credit_flit = new Credit(t_flit->get_vc(), is_free, curCycle());
    outCreditQueue->insert(credit_flit);
}

bool
NetworkInterface::checkStallQueue()
{
    bool messageEnqueuedThisCycle = false;
    Tick curTime = clockEdge();

    if (!m_stall_queue.empty()) {
        for (auto stallIter = m_stall_queue.begin();
             stallIter != m_stall_queue.end(); ) {
            flit *stallFlit = *stallIter;
            int vnet = stallFlit->get_vnet();

            // If we can now eject to the protocol buffer, send back credits
            if (outNode_ptr[vnet]->areNSlotsAvailable(1, curTime)) {
/****************************Create ack Flit**************************/
		NetDest personal_dest;
		NodeID destID=stallFlit -> get_route().src_ni;
		for(int m = 0; m < (int) MachineType_NUM; m++){
			if((destID >= MachineType_base_number((MachineType) m   )) &&
			   (destID <  MachineType_base_number((MachineType)(m+1))) ){
				personal_dest.clear();
				personal_dest.add((MachineID){(MachineType)m, (destID - 
					       MachineType_base_number((MachineType)m))});
				break;
			}
		}
			
		RouteInfo route;
		int vc=calculateVC(stallFlit->get_vnet());
		route.vnet = stallFlit->get_vnet();
		route.net_dest = personal_dest;
		route.src_ni = stallFlit -> get_route().dest_ni;
		route.src_router = stallFlit -> get_route().dest_router;
		route.dest_ni = stallFlit -> get_route().src_ni;
		route.dest_router = stallFlit -> get_route().src_router;
		route.hops_traversed = -1;

		m_net_ptr -> increment_injected_packets(route.vnet);

		flit *fl;


		if(stallFlit -> get_type()==HEAD_TAIL_){
			fl = new flit(0,vc,route.vnet,route,1,stallFlit->get_msg_ptr(),curCycle()+Cycles(1),9,ACK__);
			if(stallFlit->get_ack() == NORMAL__){
				outNode_ptr[vnet]->enqueue(stallFlit->get_msg_ptr(),curTime,cyclesToTicks(Cycles(1)));
			}
		}
		else if(rand()%100000<=((1-pow(1-error_rate,stallFlit->get_route().hops_traversed))*100000)){
			fl = new flit(0,vc,route.vnet,route,1,stallFlit->get_msg_ptr(),curCycle()+Cycles(1),9,NACK__);
			if(stallFlit->get_ack() == NORMAL__){
				outNode_ptr[vnet]->enqueue(stallFlit->get_msg_ptr(), curTime, cyclesToTicks(Cycles(1)));
			}
		}
		else{
			fl = new flit(0,vc,route.vnet,route,1,stallFlit->get_msg_ptr(),curCycle()+Cycles(1),9,ACK__);
			if(stallFlit->get_ack() == NORMAL__){
				outNode_ptr[vnet]->enqueue(stallFlit->get_msg_ptr(),curTime,cyclesToTicks(Cycles(1)));
			}
		}

		fl->set_src_delay(Cycles(1));
		ackQueue.push_back(fl);

	
      //          outNode_ptr[vnet]->enqueue(stallFlit->get_msg_ptr(), curTime,
      //                                     cyclesToTicks(Cycles(1)));
/**********************************END********************************/
                // Send back a credit with free signal now that the VC is no
                // longer stalled.
                sendCredit(stallFlit, true);

                // Update Stats
                incrementStats(stallFlit);

                // Flit can now safely be deleted and removed from stall queue
                delete stallFlit;
                m_stall_queue.erase(stallIter);
                m_stall_count[vnet]--;

                // If there are no more stalled messages for this vnet, the
                // callback on it's MessageBuffer is not needed.
                if (m_stall_count[vnet] == 0)
                    outNode_ptr[vnet]->unregisterDequeueCallback();

                messageEnqueuedThisCycle = true;
                break;
            } else {
                ++stallIter;
            }
        }
    }

    return messageEnqueuedThisCycle;
}

// Embed the protocol message into flits
bool
NetworkInterface::flitisizeMessage(MsgPtr msg_ptr, int vnet)
{
    Message *net_msg_ptr = msg_ptr.get();
    NetDest net_msg_dest = net_msg_ptr->getDestination();

    retransmission retran_pkt;
    // gets all the destinations associated with this message.
    vector<NodeID> dest_nodes = net_msg_dest.getAllDest();

    // Number of flits is dependent on the link bandwidth available.
    // This is expressed in terms of bytes/cycle or the flit size
    int num_flits = (int) ceil((double) m_net_ptr->MessageSizeType_to_int(net_msg_ptr->getMessageSize())/m_net_ptr->getNiFlitSize());

    string protect;


    if(!std::getline(inputFileHandler , protect)){
	    inputFileHandler.clear();
	    inputFileHandler.seekg(0,ios::beg);
    }
    int protection=9;
    int approx;

    if(std::atoi(protect.c_str())>8){
	    approx = 9;
    }
    else {
	    approx = std::atoi(protect.c_str())+1;
    }


        if(num_flits == 1){
		protection = 9;
	}
	else {
		protection = approx;
	}

    DPRINTF(RubyNetwork, "Packet length = %d\n", num_flits);
//	cout<<"Packet length ="<<num_flits<<endl;
//	cout<<"Protect ="<<protect<<endl;
    // loop to convert all multicast messages into unicast messages
    for (int ctr = 0; ctr < dest_nodes.size(); ctr++) {

        // this will return a free output virtual channel
        int vc = calculateVC(vnet);

        if (vc == -1) {
            return false ;
        }
        MsgPtr new_msg_ptr = msg_ptr->clone();
        NodeID destID = dest_nodes[ctr];

        Message *new_net_msg_ptr = new_msg_ptr.get();
        if (dest_nodes.size() > 1) {
            NetDest personal_dest;
            for (int m = 0; m < (int) MachineType_NUM; m++) {
                if ((destID >= MachineType_base_number((MachineType) m)) &&
                    destID < MachineType_base_number((MachineType) (m+1))) {
                    // calculating the NetDest associated with this destID
                    personal_dest.clear();
                    personal_dest.add((MachineID) {(MachineType) m, (destID -
                        MachineType_base_number((MachineType) m))});
                    new_net_msg_ptr->getDestination() = personal_dest;
                    break;
                }
            }
            net_msg_dest.removeNetDest(personal_dest);
            // removing the destination from the original message to reflect
            // that a message with this particular destination has been
            // flitisized and an output vc is acquired
            net_msg_ptr->getDestination().removeNetDest(personal_dest);
        }

        // Embed Route into the flits
        // NetDest format is used by the routing table
        // Custom routing algorithms just need destID
        RouteInfo route;
        route.vnet = vnet;
        route.net_dest = new_net_msg_ptr->getDestination();
        route.src_ni = m_id;
        route.src_router = m_router_id;
        route.dest_ni = destID;
        route.dest_router = m_net_ptr->get_router_id(destID);

        // initialize hops_traversed to -1
        // so that the first router increments it to 0
        route.hops_traversed = -1;

        m_net_ptr->increment_injected_packets(vnet);
        for (int i = 0; i < num_flits; i++) {
            m_net_ptr->increment_injected_flits(vnet);
            flit *fl = new flit(i, vc, vnet, route, num_flits, new_msg_ptr, curCycle(),protection ,NORMAL__);

            fl->set_src_delay(curCycle() - ticksToCycles(msg_ptr->getTime()));
            m_ni_out_vcs[vc]->insert(fl);
        }

        m_ni_out_vcs_enqueue_time[vc] = curCycle();
        m_out_vc_state[vc]->setState(ACTIVE_, curCycle());

        retran_pkt.message=msg_ptr;
        retran_pkt.flit = new_msg_ptr;
        retran_pkt.route=route;
        retran_pkt.protection = protection;
    }


    ARQ_Buffer.push_back(retran_pkt);
    return true ;
}

// Looking for a free output vc
int
NetworkInterface::calculateVC(int vnet)
{
    for (int i = 0; i < m_vc_per_vnet; i++) {
        int delta = m_vc_allocator[vnet];
        m_vc_allocator[vnet]++;
        if (m_vc_allocator[vnet] == m_vc_per_vnet)
            m_vc_allocator[vnet] = 0;

        if (m_out_vc_state[(vnet*m_vc_per_vnet) + delta]->isInState(
                    IDLE_, curCycle())) {
            vc_busy_counter[vnet] = 0;
            return ((vnet*m_vc_per_vnet) + delta);
        }
    }

    vc_busy_counter[vnet] += 1;
    panic_if(vc_busy_counter[vnet] > m_deadlock_threshold,
        "%s: Possible network deadlock in vnet: %d at time: %llu \n",
        name(), vnet, curTick());

    return -1;
}


/** This function looks at the NI buffers
 *  if some buffer has flits which are ready to traverse the link in the next
 *  cycle, and the downstream output vc associated with this flit has buffers
 *  left, the link is scheduled for the next cycle
 */

void
NetworkInterface::scheduleOutputLink()
{
    int vc = m_vc_round_robin;
    m_vc_round_robin++;
    if (m_vc_round_robin == m_num_vcs)
        m_vc_round_robin = 0;

    for (int i = 0; i < m_num_vcs; i++) {
        vc++;
        if (vc == m_num_vcs)
            vc = 0;

        // model buffer backpressure
        if (m_ni_out_vcs[vc]->isReady(curCycle()) &&
            m_out_vc_state[vc]->has_credit()) {

            bool is_candidate_vc = true;
            int t_vnet = get_vnet(vc);
            int vc_base = t_vnet * m_vc_per_vnet;

            if (m_net_ptr->isVNetOrdered(t_vnet)) {
                for (int vc_offset = 0; vc_offset < m_vc_per_vnet;
                     vc_offset++) {
                    int t_vc = vc_base + vc_offset;
                    if (m_ni_out_vcs[t_vc]->isReady(curCycle())) {
                        if (m_ni_out_vcs_enqueue_time[t_vc] <
                            m_ni_out_vcs_enqueue_time[vc]) {
                            is_candidate_vc = false;
                            break;
                        }
                    }
                }
            }
            if (!is_candidate_vc)
                continue;

            m_out_vc_state[vc]->decrement_credit();
            // Just removing the flit
            flit *t_flit = m_ni_out_vcs[vc]->getTopFlit();
            t_flit->set_time(curCycle() + Cycles(1));
            outFlitQueue->insert(t_flit);
            // schedule the out link
            outNetLink->scheduleEventAbsolute(clockEdge(Cycles(1)));

            if (t_flit->get_type() == TAIL_ ||
               t_flit->get_type() == HEAD_TAIL_) {
                m_ni_out_vcs_enqueue_time[vc] = Cycles(INFINITE_);
            }
            return;
        }
    }
}

int
NetworkInterface::get_vnet(int vc)
{
    for (int i = 0; i < m_virtual_networks; i++) {
        if (vc >= (i*m_vc_per_vnet) && vc < ((i+1)*m_vc_per_vnet)) {
            return i;
        }
    }
    fatal("Could not determine vc");
}


// Wakeup the NI in the next cycle if there are waiting
// messages in the protocol buffer, or waiting flits in the
// output VC buffer
void
NetworkInterface::checkReschedule()
{
    for (const auto& it : inNode_ptr) {
        if (it == nullptr) {
            continue;
        }

        while (it->isReady(clockEdge())) { // Is there a message waiting
            scheduleEvent(Cycles(1));
            return;
        }
    }

    for (int vc = 0; vc < m_num_vcs; vc++) {
        if (m_ni_out_vcs[vc]->isReady(curCycle() + Cycles(1))) {
            scheduleEvent(Cycles(1));
            return;
        }
    }
}

void
NetworkInterface::print(std::ostream& out) const
{
    out << "[Network Interface]";
}

uint32_t
NetworkInterface::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    for (unsigned int i  = 0; i < m_num_vcs; ++i) {
        num_functional_writes += m_ni_out_vcs[i]->functionalWrite(pkt);
    }

    num_functional_writes += outFlitQueue->functionalWrite(pkt);
    return num_functional_writes;
}

NetworkInterface *
GarnetNetworkInterfaceParams::create()
{
    return new NetworkInterface(this);
}
