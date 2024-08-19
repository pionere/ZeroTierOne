/*
 * Copyright (c)2013-2021 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2026-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "Bond.hpp"
#include "Switch.hpp"

#include <cmath>
#include <string>

namespace ZeroTier {

static unsigned char s_freeRandomByteCounter = 0;

int Bond::_minReqMonitorInterval = ZT_BOND_FAILOVER_DEFAULT_INTERVAL;
uint8_t Bond::_defaultPolicy = ZT_BOND_POLICY_NONE;

Phy<Bond*>* Bond::_phy;

Mutex Bond::_bonds_m;
Mutex Bond::_links_m;

std::string Bond::_defaultPolicyStr;
std::map<int64_t, SharedPtr<Bond> > Bond::_bonds;
std::map<int64_t, std::string> Bond::_policyTemplateAssignments;
std::map<std::string, SharedPtr<Bond> > Bond::_bondPolicyTemplates;
std::map<std::string, std::vector<SharedPtr<Link> > > Bond::_linkDefinitions;
std::map<std::string, std::map<std::string, SharedPtr<Link> > > Bond::_interfaceToLinkMap;

bool Bond::linkAllowed(std::string& policyAlias, SharedPtr<Link> link)
{
	bool foundInDefinitions = false;
	if (_linkDefinitions.count(policyAlias)) {
		auto it = _linkDefinitions[policyAlias].begin();
		while (it != _linkDefinitions[policyAlias].end()) {
			if (link->ifname() == (*it)->ifname()) {
				foundInDefinitions = true;
				break;
			}
			++it;
		}
	}
	return _linkDefinitions[policyAlias].empty() || foundInDefinitions;
}

void Bond::addCustomLink(std::string& policyAlias, SharedPtr<Link> link)
{
	Mutex::Lock _l(_links_m);
	_linkDefinitions[policyAlias].push_back(link);
	auto search = _interfaceToLinkMap[policyAlias].find(link->ifname());
	if (search == _interfaceToLinkMap[policyAlias].end()) {
		link->setAsUserSpecified(true);
		_interfaceToLinkMap[policyAlias].insert(std::pair<std::string, SharedPtr<Link> >(link->ifname(), link));
	}
}

bool Bond::addCustomPolicy(const SharedPtr<Bond>& newBond)
{
	Mutex::Lock _l(_bonds_m);
	if (! _bondPolicyTemplates.count(newBond->policyAlias())) {
		_bondPolicyTemplates[newBond->policyAlias()] = newBond;
		return true;
	}
	return false;
}

bool Bond::assignBondingPolicyToPeer(int64_t identity, const std::string& policyAlias)
{
	Mutex::Lock _l(_bonds_m);
	if (! _policyTemplateAssignments.count(identity)) {
		_policyTemplateAssignments[identity] = policyAlias;
		return true;
	}
	return false;
}

SharedPtr<Bond> Bond::getBondByPeerId(int64_t identity)
{
	Mutex::Lock _l(_bonds_m);
	return _bonds.count(identity) ? _bonds[identity] : SharedPtr<Bond>();
}

SharedPtr<Bond> Bond::createTransportTriggeredBond(const RuntimeEnvironment* renv, const SharedPtr<Peer>& peer)
{
	Mutex::Lock _l(_bonds_m);
	int64_t identity = peer->identity().address().toInt();
	Bond* bond = nullptr;
	if (! _bonds.count(identity)) {
		std::string policyAlias;
		if (! _policyTemplateAssignments.count(identity)) {
			if (_defaultPolicy) {
				bond = new Bond(renv, _defaultPolicy, peer);
				bond->log("new default bond");
			}
			if (! _defaultPolicy && _defaultPolicyStr.length()) {
				bond = new Bond(renv, _bondPolicyTemplates[_defaultPolicyStr].ptr(), peer);
				bond->log("new default custom bond");
			}
		}
		else {
			if (! _bondPolicyTemplates[_policyTemplateAssignments[identity]]) {
				bond = new Bond(renv, _defaultPolicy, peer);
				bond->log("peer-specific bond, was specified as %s but the bond definition was not found, using default %s", _policyTemplateAssignments[identity].c_str(), getPolicyStrByCode(_defaultPolicy).c_str());
			}
			else {
				bond = new Bond(renv, _bondPolicyTemplates[_policyTemplateAssignments[identity]].ptr(), peer);
				bond->log("new default bond");
			}
		}
	}
	if (bond) {
		_bonds[identity] = bond;
		/**
		 * Determine if user has specified anything that could affect the bonding policy's decisions
		 */
		if (_interfaceToLinkMap.count(bond->policyAlias())) {
			std::map<std::string, SharedPtr<Link> >::iterator it = _interfaceToLinkMap[bond->policyAlias()].begin();
			while (it != _interfaceToLinkMap[bond->policyAlias()].end()) {
				if (it->second->isUserSpecified()) {
					bond->_userHasSpecifiedLinks = true;
				}
				if (it->second->isUserSpecified() && it->second->primary()) {
					bond->_userHasSpecifiedPrimaryLink = true;
				}
				if (it->second->isUserSpecified() && it->second->userHasSpecifiedFailoverInstructions()) {
					bond->_userHasSpecifiedFailoverInstructions = true;
				}
				if (it->second->isUserSpecified() && (it->second->speed() > 0)) {
					bond->_userHasSpecifiedLinkSpeeds = true;
				}
				++it;
			}
		}
		return bond;
	}
	return SharedPtr<Bond>();
}

SharedPtr<Link> Bond::getLinkBySocket(const std::string& policyAlias, uint64_t localSocket)
{
	Mutex::Lock _l(_links_m);
	char ifname[32] = { 0 };   // 256 because interfaces on Windows can potentially be that long
	_phy->getIfName((PhySocket*)((uintptr_t)localSocket), ifname, sizeof(ifname) - 1);
	// fprintf(stderr, "ifname %s\n",ifname);
	std::string ifnameStr(ifname);
	auto search = _interfaceToLinkMap[policyAlias].find(ifnameStr);
	if (search == _interfaceToLinkMap[policyAlias].end()) {
		// If the link wasn't already known, add a new entry
		// fprintf(stderr, "adding new link?? %s\n", ifnameStr.c_str());
		SharedPtr<Link> s = new Link(ifnameStr, 0, 0, true, ZT_BOND_SLAVE_MODE_SPARE, "", 0.0);
		_interfaceToLinkMap[policyAlias].insert(std::pair<std::string, SharedPtr<Link> >(ifnameStr, s));
		return s;
	}
	else {
		return search->second;
	}
}

SharedPtr<Link> Bond::getLinkByName(const std::string& policyAlias, const std::string& ifname)
{
	Mutex::Lock _l(_links_m);
	auto search = _interfaceToLinkMap[policyAlias].find(ifname);
	if (search != _interfaceToLinkMap[policyAlias].end()) {
		return search->second;
	}
	return SharedPtr<Link>();
}

void Bond::processBackgroundTasks(void* tPtr, const int64_t now)
{
	unsigned long _currMinReqMonitorInterval = ZT_BOND_FAILOVER_DEFAULT_INTERVAL;
	Mutex::Lock _l(_bonds_m);
	std::map<int64_t, SharedPtr<Bond> >::iterator bondItr = _bonds.begin();
	while (bondItr != _bonds.end()) {
		// Update Bond Controller's background processing timer
		_currMinReqMonitorInterval = std::min(_currMinReqMonitorInterval, (unsigned long)(bondItr->second->monitorInterval()));
		// Process bond tasks
		bondItr->second->processBackgroundBondTasks(tPtr, now);
		++bondItr;
	}
	_minReqMonitorInterval = std::min(_currMinReqMonitorInterval, (unsigned long)ZT_BOND_FAILOVER_DEFAULT_INTERVAL);
}

Bond::Bond(const RuntimeEnvironment* renv) : RR(renv)
{
}

Bond::Bond(const RuntimeEnvironment* renv, int policy, const SharedPtr<Peer>& peer) : RR(renv), _freeRandomByte((unsigned char)((uintptr_t)this >> 4) ^ ++s_freeRandomByteCounter), _peer(peer), _peerId(_peer->_id.address().toInt())
{
	setBondParameters(policy, SharedPtr<Bond>(), false);
	_policyAlias = getPolicyStrByCode(policy);
}

Bond::Bond(const RuntimeEnvironment* renv, std::string& basePolicy, std::string& policyAlias, const SharedPtr<Peer>& peer) : RR(renv), _policyAlias(policyAlias), _peer(peer)
{
	setBondParameters(getPolicyCodeByStr(basePolicy), SharedPtr<Bond>(), false);
}

Bond::Bond(const RuntimeEnvironment* renv, SharedPtr<Bond> originalBond, const SharedPtr<Peer>& peer)
	: RR(renv)
	, _freeRandomByte((unsigned char)((uintptr_t)this >> 4) ^ ++s_freeRandomByteCounter)
	, _peer(peer)
	, _peerId(_peer->_id.address().toInt())
{
	setBondParameters(originalBond->_policy, originalBond, true);
}

void Bond::nominatePathToBond(const SharedPtr<Path>& path, int64_t now)
{
	char pathStr[64] = { 0 };
	path->address().toString(pathStr);
	Mutex::Lock _l(_paths_m);
	/**
	 * Ensure the link is allowed and the path is not already present
	 */
	if (! RR->bc->linkAllowed(_policyAlias, getLink(path))) {
		return;
	}
	bool alreadyPresent = false;
	for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		// Sanity check
		if (path.ptr() == _paths[i].p.ptr()) {
			alreadyPresent = true;
			break;
		}
	}
	if (! alreadyPresent) {
		/**
		 * Find somewhere to stick it
		 */
		for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
			if (! _paths[i].p) {
				_paths[i].set(now, path);
				/**
				 * Set user preferences and update state variables of other paths on the same link
				 */
				SharedPtr<Link> sl = getLink(_paths[i].p);
				if (sl) {
					// Determine if there are any other paths on this link
					bool bFoundCommonLink = false;
					SharedPtr<Link> commonLink = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
					for (unsigned int j = 0; j < ZT_MAX_PEER_NETWORK_PATHS; ++j) {
						if (_paths[j].p && _paths[j].p.ptr() != _paths[i].p.ptr()) {
							if (RR->bc->getLinkBySocket(_policyAlias, _paths[j].p->localSocket()) == commonLink) {
								bFoundCommonLink = true;
								_paths[j].onlyPathOnLink = false;
							}
						}
					}
					_paths[i].ipvPref = sl->ipvPref();
					_paths[i].mode = sl->mode();
					_paths[i].enabled = sl->enabled();
					_paths[i].onlyPathOnLink = ! bFoundCommonLink;
				}
				log("nominate link %s/%s (now in trial period)", getLink(path)->ifname().c_str(), pathStr);
				break;
			}
		}
	}
	curateBond(now, true);
	estimatePathQuality(now);
}

void Bond::addPathToBond(int nominatedIdx, int bondedIdx)
{
	// Map bonded set to nominated set
	_bondIdxMap[bondedIdx] = nominatedIdx;
	// Tell the bonding layer that we can now use this bond for traffic
	_paths[nominatedIdx].bonded = true;
}

SharedPtr<Path> Bond::getAppropriatePath(int64_t now, int32_t flowId)
{
	Mutex::Lock _l(_paths_m);
	/**
	 * active-backup
	 */
	if (_policy == ZT_BOND_POLICY_ACTIVE_BACKUP) {
		if (_abPathIdx != ZT_MAX_PEER_NETWORK_PATHS && _paths[_abPathIdx].p) {
			return _paths[_abPathIdx].p;
		}
	}
	/**
	 * broadcast
	 */
	if (_policy == ZT_BOND_POLICY_BROADCAST) {
		return SharedPtr<Path>();	// Handled in Switch::_trySend()
	}
	if (! _numBondedPaths) {
		return SharedPtr<Path>();	// No paths assigned to bond yet, cannot balance traffic
	}
	/**
	 * balance-rr
	 */
	if (_policy == ZT_BOND_POLICY_BALANCE_RR) {
		if (! _allowFlowHashing) {
			if (_packetsPerLink == 0) {
				// Randomly select a path
				return _paths[_bondIdxMap[_freeRandomByte % _numBondedPaths]].p;
			}
			if (_rrPacketsSentOnCurrLink < _packetsPerLink) {
				// Continue to use this link
				++_rrPacketsSentOnCurrLink;
				return _paths[_bondIdxMap[_rrIdx]].p;
			}
			// Reset striping counter
			_rrPacketsSentOnCurrLink = 0;
			if (_numBondedPaths == 1 || _rrIdx >= (ZT_MAX_PEER_NETWORK_PATHS-1)) {
				_rrIdx = 0;
			}
			else {
				int _tempIdx = _rrIdx;
				for (int searchCount = 0; searchCount < (_numBondedPaths - 1); searchCount++) {
					_tempIdx = (_tempIdx == (_numBondedPaths - 1)) ? 0 : _tempIdx + 1;
					if (_bondIdxMap[_tempIdx] != ZT_MAX_PEER_NETWORK_PATHS) {
						if (_paths[_bondIdxMap[_tempIdx]].p && _paths[_bondIdxMap[_tempIdx]].eligible) {
							_rrIdx = _tempIdx;
							break;
						}
					}
				}
			}
			if (_paths[_bondIdxMap[_rrIdx]].p) {
				return _paths[_bondIdxMap[_rrIdx]].p;
			}
		}
	}
	/**
	 * balance-xor
	 */
	if (_policy == ZT_BOND_POLICY_BALANCE_XOR || _policy == ZT_BOND_POLICY_BALANCE_AWARE) {
		if (! _allowFlowHashing || flowId == -1) {
			// No specific path required for unclassified traffic, send on anything
			int m_idx = _bondIdxMap[_freeRandomByte % _numBondedPaths];
			return _paths[m_idx].p;
		}
		else if (_allowFlowHashing) {
			Mutex::Lock _l(_flows_m);
			SharedPtr<Flow> flow;
			if (_flows.count(flowId)) {
				flow = _flows[flowId];
				flow->lastActivity = now;
			}
			else {
				unsigned char entropy;
				Utils::getSecureRandom(&entropy, 1);
				flow = createFlow(ZT_MAX_PEER_NETWORK_PATHS, flowId, entropy, now);
			}
			if (flow) {
				return _paths[flow->assignedPath].p;
			}
		}
	}
	return SharedPtr<Path>();
}

void Bond::recordIncomingInvalidPacket(const SharedPtr<Path>& path)
{
	// char pathStr[64] = { 0 }; path->address().toString(pathStr);
	// log("%s (qos) Invalid packet on link %s/%s from peer %llx",
	//	 getLink(path)->ifname().c_str(), pathStr);
	Mutex::Lock _l(_paths_m);
	for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (_paths[i].p == path) {
			_paths[i].packetValiditySamples.push(false);
		}
	}
}

void Bond::recordOutgoingPacket(const SharedPtr<Path>& path, uint64_t packetId, uint16_t payloadLength, const Packet::Verb verb, const int32_t flowId, int64_t now)
{
	_freeRandomByte += (unsigned char)(packetId >> 8);	 // Grab entropy to use in path selection logic
	bool isFrame = (verb == Packet::Packet::VERB_ECHO || verb == Packet::VERB_FRAME || verb == Packet::VERB_EXT_FRAME);
	if (isFrame) {
		// char pathStr[64] = { 0 };
		// path->address().toString(pathStr);
		// int pathIdx = getNominatedPathIdx(path);
		// log("outgoing packet via [%d]", pathIdx);
		// log("outgoing packet via %s/%s", getLink(path)->ifname().c_str(), pathStr);
	}
	bool shouldRecord = (packetId & (ZT_QOS_ACK_DIVISOR - 1) && (verb != Packet::VERB_ACK) && (verb != Packet::VERB_QOS_MEASUREMENT));
	if (isFrame || shouldRecord) {
		Mutex::Lock _l(_paths_m);
		int pathIdx = getNominatedPathIdx(path);
		if (pathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
			return;
		}
		if (isFrame) {
			++(_paths[pathIdx].packetsOut);
			_lastFrame = now;
		}
		if (shouldRecord) {
			//_paths[pathIdx].unackedBytes += payloadLength;
			// Take note that we're expecting a VERB_ACK on this path as of a specific time
			if (_paths[pathIdx].qosStatsOut.size() < ZT_QOS_MAX_OUTSTANDING_RECORDS) {
				_paths[pathIdx].qosStatsOut[packetId] = now;
			}
		}
	}
	if (_allowFlowHashing && (flowId != ZT_QOS_NO_FLOW)) {
		Mutex::Lock _l(_flows_m);
		if (_flows.count(flowId)) {
			_flows[flowId]->bytesOut += payloadLength;
		}
	}
}

void Bond::recordIncomingPacket(const SharedPtr<Path>& path, uint64_t packetId, uint16_t payloadLength, Packet::Verb verb, int32_t flowId, int64_t now)
{
	bool isFrame = (verb == Packet::Packet::VERB_ECHO || verb == Packet::VERB_FRAME || verb == Packet::VERB_EXT_FRAME);
	if (isFrame) {
		// char pathStr[64] = { 0 }; path->address().toString(pathStr);
		// int pathIdx = getNominatedPathIdx(path);
		// log("incoming packet via [%d]      [id=%llx, len=%d, verb=%d, flowId=%x]", pathIdx, packetId, payloadLength, verb, flowId);
		// log("incoming packet via %s/%s (ls=%llx) [id=%llx, len=%d, verb=%d, flowId=%x]", getLink(path)->ifname().c_str(), pathStr, path->localSocket(), packetId, payloadLength, verb, flowId);
	}
	bool shouldRecord = (packetId & (ZT_QOS_ACK_DIVISOR - 1) && (verb != Packet::VERB_ACK) && (verb != Packet::VERB_QOS_MEASUREMENT));
	Mutex::Lock _l(_paths_m);
	int pathIdx = getNominatedPathIdx(path);
	if (pathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
		return;
	}
	// Take note of the time that this previously-dead path received a packet
	if (! _paths[pathIdx].alive) {
		_paths[pathIdx].lastAliveToggle = now;
	}
	if (isFrame || shouldRecord) {
		if (_paths[pathIdx].allowed()) {
			if (isFrame) {
				++(_paths[pathIdx].packetsIn);
				_lastFrame = now;
			}
			if (shouldRecord) {
				_paths[pathIdx].qosStatsIn[packetId] = now;
				++(_paths[pathIdx].packetsReceivedSinceLastQoS);
				_paths[pathIdx].packetValiditySamples.push(true);
			}
		}
	}

	/**
	 * Learn new flows and pro-actively create entries for them in the bond so
	 * that the next time we send a packet out that is part of a flow we know
	 * which path to use.
	 */
	if ((flowId != ZT_QOS_NO_FLOW) && (_policy == ZT_BOND_POLICY_BALANCE_RR || _policy == ZT_BOND_POLICY_BALANCE_XOR || _policy == ZT_BOND_POLICY_BALANCE_AWARE)) {
		Mutex::Lock _l(_flows_m);
		SharedPtr<Flow> flow;
		if (! _flows.count(flowId)) {
			flow = createFlow(pathIdx, flowId, 0, now);
		}
		else {
			flow = _flows[flowId];
		}
		if (flow) {
			flow->bytesIn += payloadLength;
		}
	}
}

void Bond::receivedQoS(const SharedPtr<Path>& path, int64_t now, int count, uint64_t* rx_id, uint16_t* rx_ts)
{
	Mutex::Lock _l(_paths_m);
	int pathIdx = getNominatedPathIdx(path);
	if (pathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
		return;
	}
	// char pathStr[64] = { 0 }; path->address().toString(pathStr);
	// log("received QoS packet (sampling %d frames) via %s/%s", count, getLink(path)->ifname().c_str(), pathStr);
	// Look up egress times and compute latency values for each record
	std::map<uint64_t, uint64_t>::iterator it;
	for (int j = 0; j < count; j++) {
		it = _paths[pathIdx].qosStatsOut.find(rx_id[j]);
		if (it != _paths[pathIdx].qosStatsOut.end()) {
			_paths[pathIdx].latencySamples.push(((uint16_t)(now - it->second) - rx_ts[j]) / 2);
			_paths[pathIdx].qosStatsOut.erase(it);
		}
	}
	_paths[pathIdx].qosRecordSize.push(count);
}

int32_t Bond::generateQoSPacket(int pathIdx, int64_t now, char* qosBuffer)
{
	int32_t len = 0;
	std::map<uint64_t, uint64_t>::iterator it = _paths[pathIdx].qosStatsIn.begin();
	int i = 0;
	int numRecords = std::min(_paths[pathIdx].packetsReceivedSinceLastQoS, ZT_QOS_TABLE_SIZE);
	while (i < numRecords && it != _paths[pathIdx].qosStatsIn.end()) {
		uint64_t id = it->first;
		memcpy(qosBuffer, &id, sizeof(uint64_t));
		qosBuffer += sizeof(uint64_t);
		uint16_t holdingTime = (uint16_t)(now - it->second);
		memcpy(qosBuffer, &holdingTime, sizeof(uint16_t));
		qosBuffer += sizeof(uint16_t);
		len += sizeof(uint64_t) + sizeof(uint16_t);
		_paths[pathIdx].qosStatsIn.erase(it++);
		++i;
	}
	return len;
}

bool Bond::assignFlowToBondedPath(SharedPtr<Flow>& flow, int64_t now)
{
	char curPathStr[64] = { 0 };
	unsigned int idx = ZT_MAX_PEER_NETWORK_PATHS;
	if (_policy == ZT_BOND_POLICY_BALANCE_XOR) {
		idx = abs((int)(flow->id % (_numBondedPaths)));
		SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[_bondIdxMap[idx]].p->localSocket());
		_paths[_bondIdxMap[idx]].p->address().toString(curPathStr);
		flow->assignPath(_bondIdxMap[idx], now);
		++(_paths[_bondIdxMap[idx]].assignedFlowCount);
	}
	if (_policy == ZT_BOND_POLICY_BALANCE_AWARE) {
		unsigned char entropy;
		Utils::getSecureRandom(&entropy, 1);
		if (_totalBondUnderload) {
			entropy %= _totalBondUnderload;
		}
		if (! _numBondedPaths) {
			log("unable to assign flow %x (bond has no links)\n", flow->id);
			return false;
		}
		/* Since there may be scenarios where a path is removed before we can re-estimate
		relative qualities (and thus allocations) we need to down-modulate the entropy
		value that we use to randomly assign among the surviving paths, otherwise we risk
		not being able to find a path to assign this flow to. */
		int totalIncompleteAllocation = 0;
		for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
			if (_paths[i].p && _paths[i].bonded) {
				totalIncompleteAllocation += _paths[i].allocation;
			}
		}
		entropy %= totalIncompleteAllocation;
		for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
			if (_paths[i].p && _paths[i].bonded) {
				SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
				_paths[i].p->address().toString(curPathStr);
				uint8_t probabilitySegment = (_totalBondUnderload > 0) ? _paths[i].affinity : _paths[i].allocation;
				if (entropy <= probabilitySegment) {
					idx = i;
					break;
				}
				entropy -= probabilitySegment;
			}
		}
		if (idx < ZT_MAX_PEER_NETWORK_PATHS) {
			flow->assignPath(idx, now);
			++(_paths[idx].assignedFlowCount);
		}
		else {
			log("unable to assign out-flow %x (unknown reason)", flow->id);
			return false;
		}
	}
	if (_policy == ZT_BOND_POLICY_ACTIVE_BACKUP) {
		if (_abPathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
			log("unable to assign out-flow %x (no active backup link)", flow->id);
		}
		flow->assignPath(_abPathIdx, now);
	}
	_paths[flow->assignedPath].p->address().toString(curPathStr);
	SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[flow->assignedPath].p->localSocket());
	log("assign out-flow %x to link %s/%s (%6d / %lu flows)", flow->id, link->ifname().c_str(), curPathStr, _paths[flow->assignedPath].assignedFlowCount, (unsigned long)_flows.size());
	return true;
}

SharedPtr<Bond::Flow> Bond::createFlow(int pathIdx, int32_t flowId, unsigned char entropy, int64_t now)
{
	char curPathStr[64] = { 0 };
	if (! _numBondedPaths) {
		log("unable to assign flow %x (bond has no links)\n", flowId);
		return SharedPtr<Flow>();
	}
	if (_flows.size() >= ZT_FLOW_MAX_COUNT) {
		log("forget oldest flow (max flows reached: %d)\n", ZT_FLOW_MAX_COUNT);
		forgetFlowsWhenNecessary(0, true, now);
	}
	SharedPtr<Flow> flow = new Flow(flowId, now);
	_flows[flowId] = flow;
	/**
	 * Add a flow with a given Path already provided. This is the case when a packet
	 * is received on a path but no flow exists, in this case we simply assign the path
	 * that the remote peer chose for us.
	 */
	if (pathIdx != ZT_MAX_PEER_NETWORK_PATHS) {
		flow->assignPath(pathIdx, now);
		_paths[pathIdx].p->address().toString(curPathStr);
		_paths[pathIdx].assignedFlowCount++;
		SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[flow->assignedPath].p->localSocket());
		log("assign in-flow %x to link %s/%s (%6d / %lu)", flow->id, link->ifname().c_str(), curPathStr, _paths[pathIdx].assignedFlowCount, (unsigned long)_flows.size());
	}
	/**
	 * Add a flow when no path was provided. This means that it is an outgoing packet
	 * and that it is up to the local peer to decide how to load-balance its transmission.
	 */
	else {
		assignFlowToBondedPath(flow, now);
	}
	return flow;
}

void Bond::forgetFlowsWhenNecessary(uint64_t age, bool oldest, int64_t now)
{
	std::map<int32_t, SharedPtr<Flow> >::iterator it = _flows.begin();
	std::map<int32_t, SharedPtr<Flow> >::iterator oldestFlow = _flows.end();
	SharedPtr<Flow> expiredFlow;
	if (age) {	 // Remove by specific age
		while (it != _flows.end()) {
			if (it->second->age(now) > age) {
				log("forget flow %x (age %llu) (%6d / %lu)", it->first, (unsigned long long)it->second->age(now), _paths[it->second->assignedPath].assignedFlowCount, (unsigned long)(_flows.size() - 1));
				_paths[it->second->assignedPath].assignedFlowCount--;
				it = _flows.erase(it);
			}
			else {
				++it;
			}
		}
	}
	else if (oldest) {	 // Remove single oldest by natural expiration
		uint64_t maxAge = 0;
		while (it != _flows.end()) {
			if (it->second->age(now) > maxAge) {
				maxAge = (now - it->second->age(now));
				oldestFlow = it;
			}
			++it;
		}
		if (oldestFlow != _flows.end()) {
			log("forget oldest flow %x (age %llu) (total flows: %lu)", oldestFlow->first, (unsigned long long)oldestFlow->second->age(now), (unsigned long)(_flows.size() - 1));
			_paths[oldestFlow->second->assignedPath].assignedFlowCount--;
			_flows.erase(oldestFlow);
		}
	}
}

void Bond::processIncomingPathNegotiationRequest(uint64_t now, SharedPtr<Path>& path, int16_t remoteUtility)
{
	char pathStr[64] = { 0 };
	if (_abLinkSelectMethod != ZT_BOND_RESELECTION_POLICY_OPTIMIZE) {
		return;
	}
	Mutex::Lock _l(_paths_m);
	int pathIdx = getNominatedPathIdx(path);
	if (pathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
		return;
	}
	_paths[pathIdx].p->address().toString(pathStr);
	if (! _lastPathNegotiationCheck) {
		return;
	}
	SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[pathIdx].p->localSocket());
	if (remoteUtility > _localUtility) {
		_paths[pathIdx].p->address().toString(pathStr);
		log("peer suggests alternate link %s/%s, remote utility (%d) greater than local utility (%d), switching to suggested link\n", link->ifname().c_str(), pathStr, remoteUtility, _localUtility);
		negotiatedPathIdx = pathIdx;
	}
	if (remoteUtility < _localUtility) {
		log("peer suggests alternate link %s/%s, remote utility (%d) less than local utility (%d), not switching\n", link->ifname().c_str(), pathStr, remoteUtility, _localUtility);
	}
	if (remoteUtility == _localUtility) {
		log("peer suggests alternate link %s/%s, remote utility (%d) equal to local utility (%d)\n", link->ifname().c_str(), pathStr, remoteUtility, _localUtility);
		if (_peer->_id.address().toInt() > RR->node->identity().address().toInt()) {
			log("agree with peer to use alternate link %s/%s\n", link->ifname().c_str(), pathStr);
			negotiatedPathIdx = pathIdx;
		}
		else {
			log("ignore petition from peer to use alternate link %s/%s\n", link->ifname().c_str(), pathStr);
		}
	}
}

void Bond::pathNegotiationCheck(void* tPtr, int64_t now)
{
	char pathStr[64] = { 0 };
	int maxInPathIdx = ZT_MAX_PEER_NETWORK_PATHS;
	int maxOutPathIdx = ZT_MAX_PEER_NETWORK_PATHS;
	uint64_t maxInCount = 0;
	uint64_t maxOutCount = 0;
	for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (! _paths[i].p) {
			continue;
		}
		if (_paths[i].packetsIn > maxInCount) {
			maxInCount = _paths[i].packetsIn;
			maxInPathIdx = i;
		}
		if (_paths[i].packetsOut > maxOutCount) {
			maxOutCount = _paths[i].packetsOut;
			maxOutPathIdx = i;
		}
		_paths[i].resetPacketCounts();
	}
	bool _peerLinksSynchronized = ((maxInPathIdx != ZT_MAX_PEER_NETWORK_PATHS) && (maxOutPathIdx != ZT_MAX_PEER_NETWORK_PATHS) && (maxInPathIdx != maxOutPathIdx)) ? false : true;
	/**
	 * Determine utility and attempt to petition remote peer to switch to our chosen path
	 */
	if (! _peerLinksSynchronized) {
		_localUtility = _paths[maxOutPathIdx].failoverScore - _paths[maxInPathIdx].failoverScore;
		if (_paths[maxOutPathIdx].negotiated) {
			_localUtility -= ZT_BOND_FAILOVER_HANDICAP_NEGOTIATED;
		}
		if ((now - _lastSentPathNegotiationRequest) > ZT_PATH_NEGOTIATION_CUTOFF_TIME) {
			// fprintf(stderr, "BT: (sync) it's been long enough, sending more requests.\n");
			_numSentPathNegotiationRequests = 0;
		}
		if (_numSentPathNegotiationRequests < ZT_PATH_NEGOTIATION_TRY_COUNT) {
			if (_localUtility >= 0) {
				// fprintf(stderr, "BT: (sync) paths appear to be out of sync (utility=%d)\n", _localUtility);
				sendPATH_NEGOTIATION_REQUEST(tPtr, _paths[maxOutPathIdx].p);
				++_numSentPathNegotiationRequests;
				_lastSentPathNegotiationRequest = now;
				_paths[maxOutPathIdx].p->address().toString(pathStr);
				SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[maxOutPathIdx].p->localSocket());
				// fprintf(stderr, "sending request to use %s on %s, ls=%llx, utility=%d\n", pathStr, link->ifname().c_str(), _paths[maxOutPathIdx].p->localSocket(), _localUtility);
			}
		}
		/**
		 * Give up negotiating and consider switching
		 */
		else if ((now - _lastSentPathNegotiationRequest) > (2 * ZT_BOND_OPTIMIZE_INTERVAL)) {
			if (_localUtility == 0) {
				// There's no loss to us, just switch without sending a another request
				// fprintf(stderr, "BT: (sync) giving up, switching to remote peer's path.\n");
				negotiatedPathIdx = maxInPathIdx;
			}
		}
	}
}

void Bond::sendPATH_NEGOTIATION_REQUEST(void* tPtr, int pathIdx)
{
	char pathStr[64] = { 0 };
	_paths[pathIdx].p->address().toString(pathStr);
	log("send link negotiation request to peer via link %s/%s, local utility is %d", getLink(_paths[pathIdx].p)->ifname().c_str(), pathStr, _localUtility);
	if (_abLinkSelectMethod != ZT_BOND_RESELECTION_POLICY_OPTIMIZE) {
		return;
	}
	Packet outp(_peer->_id.address(), RR->identity.address(), Packet::VERB_PATH_NEGOTIATION_REQUEST);
	outp.append<int16_t>(_localUtility);
	if (_paths[pathIdx].p->address()) {
		outp.armor(_peer->key(), false, _peer->aesKeysIfSupported());
		RR->node->putPacket(tPtr, _paths[pathIdx].p->localSocket(), _paths[pathIdx].p->address(), outp.data(), outp.size());
	}
}

void Bond::sendQOS_MEASUREMENT(void* tPtr, int pathIdx, int64_t localSocket, const InetAddress& atAddress, int64_t now)
{
	char pathStr[64] = { 0 };
	_paths[pathIdx].p->address().toString(pathStr);
	int64_t _now = RR->node->now();
	Packet outp(_peer->_id.address(), RR->identity.address(), Packet::VERB_QOS_MEASUREMENT);
	char qosData[ZT_QOS_MAX_PACKET_SIZE];
	int16_t len = generateQoSPacket(pathIdx, _now, qosData);
	_overheadBytes += len;
	if (len) {
		outp.append(qosData, len);
		if (atAddress) {
			outp.armor(_peer->key(), false, _peer->aesKeysIfSupported());
			RR->node->putPacket(tPtr, localSocket, atAddress, outp.data(), outp.size());
		}
		else {
			RR->sw->send(tPtr, outp, false);
		}
		_paths[pathIdx].packetsReceivedSinceLastQoS = 0;
		_paths[pathIdx].lastQoSMeasurement = now;
	}
	// log("send QOS via link %s/%s (len=%d)", getLink(_paths[pathIdx].p)->ifname().c_str(), pathStr, len);
}

void Bond::processBackgroundBondTasks(void* tPtr, int64_t now)
{
	if (! _peer->_localMultipathSupported || (now - _lastBackgroundTaskCheck) < ZT_BOND_BACKGROUND_TASK_MIN_INTERVAL) {
		return;
	}
	_lastBackgroundTaskCheck = now;
	Mutex::Lock _l(_paths_m);

	curateBond(now, false);
	if ((now - _lastQualityEstimation) > _qualityEstimationInterval) {
		_lastQualityEstimation = now;
		estimatePathQuality(now);
	}
	dumpInfo(now, false);

	// Send ambient monitoring traffic
	for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (_paths[i].p && _paths[i].allowed()) {
			// ECHO (this is our bond's heartbeat)
			if ((_monitorInterval > 0) && ((now - _paths[i].p->_lastOut) >= _monitorInterval)) {
				if ((_peer->remoteVersionProtocol() >= 5) && (! ((_peer->remoteVersionMajor() == 1) && (_peer->remoteVersionMinor() == 1) && (_peer->remoteVersionRevision() == 0)))) {
					Packet outp(_peer->address(), RR->identity.address(), Packet::VERB_ECHO);
					outp.armor(_peer->key(), true, _peer->aesKeysIfSupported());
					RR->node->expectReplyTo(outp.packetId());
					RR->node->putPacket(tPtr, _paths[i].p->localSocket(), _paths[i].p->address(), outp.data(), outp.size());
					_overheadBytes += outp.size();
					char pathStr[64] = { 0 };
					_paths[i].p->address().toString(pathStr);
					// log("send HELLO via link %s/%s (len=%d)", getLink(_paths[i].p)->ifname().c_str(), pathStr, outp.size());
				}
			}
			// QOS
			if (_paths[i].needsToSendQoS(now, _qosSendInterval)) {
				sendQOS_MEASUREMENT(tPtr, i, _paths[i].p->localSocket(), _paths[i].p->address(), now);
			}
		}
	}
	// Perform periodic background tasks unique to each bonding policy
	switch (_policy) {
		case ZT_BOND_POLICY_ACTIVE_BACKUP:
			processActiveBackupTasks(tPtr, now);
			break;
		case ZT_BOND_POLICY_BROADCAST:
			break;
		case ZT_BOND_POLICY_BALANCE_RR:
		case ZT_BOND_POLICY_BALANCE_XOR:
		case ZT_BOND_POLICY_BALANCE_AWARE:
			processBalanceTasks(now);
			break;
		default:
			break;
	}
	// Check whether or not a path negotiation needs to be performed
	if (((now - _lastPathNegotiationCheck) > ZT_BOND_OPTIMIZE_INTERVAL) && _allowPathNegotiation) {
		_lastPathNegotiationCheck = now;
		pathNegotiationCheck(tPtr, now);
	}
}

void Bond::curateBond(int64_t now, bool rebuildBond)
{
	char pathStr[64] = { 0 };
	uint8_t tmpNumAliveLinks = 0;
	uint8_t tmpNumTotalLinks = 0;
	/**
	 * Update path state variables. State variables are used so that critical
	 * blocks that perform fast packet processing won't need to make as many
	 * function calls or computations.
	 */
	for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (! _paths[i].p) {
			continue;
		}
		tmpNumTotalLinks++;
		if (_paths[i].eligible) {
			tmpNumAliveLinks++;
		}

		/**
		 * Determine alive-ness
		 */
		_paths[i].alive = (now - _paths[i].p->_lastIn) < _failoverInterval;

		/**
		 * Determine current eligibility
		 */
		bool currEligibility = false;
		// Simple RX age (driven by packets of any type and gratuitous VERB_HELLOs)
		bool acceptableAge = _paths[i].p->age(now) < (_failoverInterval + _downDelay);
		// Whether we've waited long enough since the link last came online
		bool satisfiedUpDelay = (now - _paths[i].lastAliveToggle) >= _upDelay;
		// Whether this path is still in its trial period
		bool inTrial = (now - _paths[i].whenNominated) < ZT_BOND_OPTIMIZE_INTERVAL;
		// if (includeRefractoryPeriod && _paths[i].refractoryPeriod) {
		// As long as the refractory period value has not fully drained this path is not eligible
		//	currEligibility = false;
		//}
		currEligibility = _paths[i].allowed() && ((acceptableAge && satisfiedUpDelay) || inTrial);
		// log("[%d] allowed=%d, acceptableAge=%d, satisfiedUpDelay=%d, inTrial=%d ==== %d", i, _paths[i].allowed(), acceptableAge, satisfiedUpDelay, inTrial, currEligibility);

		/**
		 * Note eligibility state change (if any) and take appropriate action
		 */
		if (currEligibility != _paths[i].eligible) {
			_paths[i].p->address().toString(pathStr);
			if (currEligibility == 0) {
				log("link %s/%s is no longer eligible", getLink(_paths[i].p)->ifname().c_str(), pathStr);
			}
			if (currEligibility == 1) {
				log("link %s/%s is eligible", getLink(_paths[i].p)->ifname().c_str(), pathStr);
			}
			dumpPathStatus(now, i);
			if (currEligibility) {
				rebuildBond = true;
			}
			if (! currEligibility) {
				_paths[i].adjustRefractoryPeriod(now, _defaultPathRefractoryPeriod, ! currEligibility);
				if (_paths[i].bonded) {
					_paths[i].bonded = false;
					if (_allowFlowHashing) {
						_paths[i].p->address().toString(pathStr);
						log("link %s/%s was bonded, flow reallocation will occur soon", getLink(_paths[i].p)->ifname().c_str(), pathStr);
						rebuildBond = true;
						_paths[i].shouldReallocateFlows = _paths[i].bonded;
					}
				}
			}
		}
		if (currEligibility) {
			_paths[i].adjustRefractoryPeriod(now, _defaultPathRefractoryPeriod, false);
		}
		_paths[i].eligible = currEligibility;
	}

	/**
	 * Determine health status to report to user
	 */
	_numAliveLinks = tmpNumAliveLinks;
	_numTotalLinks = tmpNumTotalLinks;
	bool tmpHealthStatus = true;

	if (_policy == ZT_BOND_POLICY_ACTIVE_BACKUP) {
		if (_numAliveLinks < 2) {
			// Considered healthy if there is at least one backup link
			tmpHealthStatus = false;
		}
	}
	if (_policy == ZT_BOND_POLICY_BROADCAST) {
		if (_numAliveLinks < 1) {
			// Considered healthy if we're able to send frames at all
			tmpHealthStatus = false;
		}
	}
	if ((_policy == ZT_BOND_POLICY_BALANCE_RR) || (_policy == ZT_BOND_POLICY_BALANCE_XOR) || (_policy == ZT_BOND_POLICY_BALANCE_AWARE)) {
		if (_numAliveLinks < _numTotalLinks) {
			tmpHealthStatus = false;
		}
	}
	if (tmpHealthStatus != _isHealthy) {
		std::string healthStatusStr;
		if (tmpHealthStatus == true) {
			healthStatusStr = "HEALTHY";
		}
		else {
			healthStatusStr = "DEGRADED";
		}
		log("bond is in a %s state (links: %d/%d)", healthStatusStr.c_str(), _numAliveLinks, _numTotalLinks);
		dumpInfo(now, true);
	}

	_isHealthy = tmpHealthStatus;

	/**
	 * Curate the set of paths that are part of the bond proper. Select a set of paths
	 * per logical link according to eligibility and user-specified constraints.
	 */

	if ((_policy == ZT_BOND_POLICY_BALANCE_RR) || (_policy == ZT_BOND_POLICY_BALANCE_XOR) || (_policy == ZT_BOND_POLICY_BALANCE_AWARE)) {
		if (! _numBondedPaths) {
			rebuildBond = true;
		}
		if (rebuildBond) {
			log("rebuilding bond");
			// TODO: Obey blacklisting
			int updatedBondedPathCount = 0;
			// Build map associating paths with local physical links. Will be selected from in next step
			std::map<SharedPtr<Link>, std::vector<int> > linkMap;
			for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
				if (_paths[i].p) {
					SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
					linkMap[link].push_back(i);
				}
			}
			// Re-form bond from link<->path map
			std::map<SharedPtr<Link>, std::vector<int> >::iterator it = linkMap.begin();
			while (it != linkMap.end()) {
				SharedPtr<Link> link = it->first;
				int ipvPref = link->ipvPref();

				// If user has no address type preference, then use every path we find on a link
				if (ipvPref == 0) {
					for (int j = 0; j < it->second.size(); j++) {
						int idx = it->second.at(j);
						if (! _paths[idx].p || ! _paths[idx].allowed()) {
							continue;
						}
						addPathToBond(idx, updatedBondedPathCount);
						++updatedBondedPathCount;
						_paths[idx].p->address().toString(pathStr);
						log("add %s/%s (no user addr preference)", link->ifname().c_str(), pathStr);
					}
				}
				// If the user prefers to only use one address type (IPv4 or IPv6)
				if (ipvPref == 4 || ipvPref == 6) {
					for (int j = 0; j < it->second.size(); j++) {
						int idx = it->second.at(j);
						if (! _paths[idx].p) {
							continue;
						}
						if (! _paths[idx].allowed()) {
							_paths[idx].p->address().toString(pathStr);
							log("did not add %s/%s (user addr preference %d)", link->ifname().c_str(), pathStr, ipvPref);
							continue;
						}
						if (! _paths[idx].eligible) {
							continue;
						}
						addPathToBond(idx, updatedBondedPathCount);
						++updatedBondedPathCount;
						_paths[idx].p->address().toString(pathStr);
						log("add path %s/%s (user addr preference %d)", link->ifname().c_str(), pathStr, ipvPref);
					}
				}
				// If the users prefers one address type to another, try to find at least
				// one path of that type before considering others.
				if (ipvPref == 46 || ipvPref == 64) {
					bool foundPreferredPath = false;
					// Search for preferred paths
					for (int j = 0; j < it->second.size(); j++) {
						int idx = it->second.at(j);
						if (! _paths[idx].p || ! _paths[idx].eligible) {
							continue;
						}
						if (_paths[idx].preferred() && _paths[idx].allowed()) {
							addPathToBond(idx, updatedBondedPathCount);
							++updatedBondedPathCount;
							_paths[idx].p->address().toString(pathStr);
							log("add %s/%s (user addr preference %d)", link->ifname().c_str(), pathStr, ipvPref);
							foundPreferredPath = true;
						}
					}
					// Unable to find a path that matches user preference, settle for another address type
					if (! foundPreferredPath) {
						log("did not find first-choice path type on link %s (user preference %d)", link->ifname().c_str(), ipvPref);
						for (int j = 0; j < it->second.size(); j++) {
							int idx = it->second.at(j);
							if (! _paths[idx].p || ! _paths[idx].eligible) {
								continue;
							}
							addPathToBond(idx, updatedBondedPathCount);
							++updatedBondedPathCount;
							_paths[idx].p->address().toString(pathStr);
							log("add %s/%s (user addr preference %d)", link->ifname().c_str(), pathStr, ipvPref);
							foundPreferredPath = true;
						}
					}
				}
				++it;	// Next link
			}
			_numBondedPaths = updatedBondedPathCount;
			if (_policy == ZT_BOND_POLICY_BALANCE_RR) {
				// Cause a RR reset since the current index might no longer be valid
				_rrPacketsSentOnCurrLink = _packetsPerLink;
			}
		}
	}
}

void Bond::estimatePathQuality(int64_t now)
{
	uint32_t totUserSpecifiedLinkSpeed = 0;
	if (_numBondedPaths) {	 // Compute relative user-specified speeds of links
		for (unsigned int i = 0; i < _numBondedPaths; ++i) {
			SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
			if (_paths[i].p && _paths[i].allowed()) {
				totUserSpecifiedLinkSpeed += link->speed();
			}
		}
		for (unsigned int i = 0; i < _numBondedPaths; ++i) {
			SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
			if (_paths[i].p && _paths[i].allowed()) {
				link->setRelativeSpeed((uint8_t)round(((float)link->speed() / (float)totUserSpecifiedLinkSpeed) * 255));
			}
		}
	}

	float lat[ZT_MAX_PEER_NETWORK_PATHS] = { 0 };
	float pdv[ZT_MAX_PEER_NETWORK_PATHS] = { 0 };
	float plr[ZT_MAX_PEER_NETWORK_PATHS] = { 0 };
	float per[ZT_MAX_PEER_NETWORK_PATHS] = { 0 };

	float maxLAT = 0;
	float maxPDV = 0;
	float maxPLR = 0;
	float maxPER = 0;

	float quality[ZT_MAX_PEER_NETWORK_PATHS] = { 0 };
	uint8_t alloc[ZT_MAX_PEER_NETWORK_PATHS] = { 0 };

	float totQuality = 0.0f;

	// Compute initial summary statistics
	for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (! _paths[i].p || ! _paths[i].allowed()) {
			continue;
		}
		// Compute/Smooth average of real-world observations
		_paths[i].latencyMean = _paths[i].latencySamples.mean();
		_paths[i].latencyVariance = _paths[i].latencySamples.stddev();
		_paths[i].packetErrorRatio = 1.0 - (_paths[i].packetValiditySamples.count() ? _paths[i].packetValiditySamples.mean() : 1.0);

		if (userHasSpecifiedLinkSpeeds()) {
			// Use user-reported metrics
			SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
			if (link) {
				_paths[i].throughputMean = link->speed();
				_paths[i].throughputVariance = 0;
			}
		}
		// Drain unacknowledged QoS records
		std::map<uint64_t, uint64_t>::iterator it = _paths[i].qosStatsOut.begin();
		uint64_t currentLostRecords = 0;
		while (it != _paths[i].qosStatsOut.end()) {
			int qosRecordTimeout = 5000;   //_paths[i].p->monitorInterval() * ZT_BOND_QOS_ACK_INTERVAL_MULTIPLIER * 8;
			if ((now - it->second) >= qosRecordTimeout) {
				// Packet was lost
				it = _paths[i].qosStatsOut.erase(it);
				++currentLostRecords;
			}
			else {
				++it;
			}
		}

		quality[i] = 0;
		totQuality = 0;
		// Normalize raw observations according to sane limits and/or user specified values
		lat[i] = 1.0 / expf(4 * Utils::normalize(_paths[i].latencyMean, 0, _maxAcceptableLatency, 0, 1));
		pdv[i] = 1.0 / expf(4 * Utils::normalize(_paths[i].latencyVariance, 0, _maxAcceptablePacketDelayVariance, 0, 1));
		plr[i] = 1.0 / expf(4 * Utils::normalize(_paths[i].packetLossRatio, 0, _maxAcceptablePacketLossRatio, 0, 1));
		per[i] = 1.0 / expf(4 * Utils::normalize(_paths[i].packetErrorRatio, 0, _maxAcceptablePacketErrorRatio, 0, 1));
		// Record bond-wide maximums to determine relative values
		maxLAT = lat[i] > maxLAT ? lat[i] : maxLAT;
		maxPDV = pdv[i] > maxPDV ? pdv[i] : maxPDV;
		maxPLR = plr[i] > maxPLR ? plr[i] : maxPLR;
		maxPER = per[i] > maxPER ? per[i] : maxPER;
	}
	// Convert metrics to relative quantities and apply contribution weights
	for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (_paths[i].p && _paths[i].bonded) {
			quality[i] += ((maxLAT > 0.0f ? lat[i] / maxLAT : 0.0f) * _qw[ZT_QOS_LAT_IDX]);
			quality[i] += ((maxPDV > 0.0f ? pdv[i] / maxPDV : 0.0f) * _qw[ZT_QOS_PDV_IDX]);
			quality[i] += ((maxPLR > 0.0f ? plr[i] / maxPLR : 0.0f) * _qw[ZT_QOS_PLR_IDX]);
			quality[i] += ((maxPER > 0.0f ? per[i] / maxPER : 0.0f) * _qw[ZT_QOS_PER_IDX]);
			totQuality += quality[i];
		}
	}
	// Normalize to 8-bit allocation values
	for (unsigned int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (_paths[i].p && _paths[i].bonded) {
			alloc[i] = (uint8_t)(std::ceil((quality[i] / totQuality) * (float)255));
			_paths[i].allocation = alloc[i];
		}
	}
}

void Bond::processBalanceTasks(int64_t now)
{
	char pathStr[64] = { 0 };

	if (_allowFlowHashing) {
		/**
		 * Clean up and reset flows if necessary
		 */
		if ((now - _lastFlowExpirationCheck) > ZT_PEER_PATH_EXPIRATION) {
			Mutex::Lock _l(_flows_m);
			forgetFlowsWhenNecessary(ZT_PEER_PATH_EXPIRATION, false, now);
			std::map<int32_t, SharedPtr<Flow> >::iterator it = _flows.begin();
			while (it != _flows.end()) {
				it->second->resetByteCounts();
				++it;
			}
			_lastFlowExpirationCheck = now;
		}
		/**
		 * Re-allocate flows from dead paths
		 */
		if (_policy == ZT_BOND_POLICY_BALANCE_XOR || _policy == ZT_BOND_POLICY_BALANCE_AWARE) {
			Mutex::Lock _l(_flows_m);
			for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
				if (! _paths[i].p) {
					continue;
				}
				if (! _paths[i].eligible && _paths[i].shouldReallocateFlows) {
					_paths[i].p->address().toString(pathStr);
					log("reallocate flows from dead link %s/%s", getLink(_paths[i].p)->ifname().c_str(), pathStr);
					std::map<int32_t, SharedPtr<Flow> >::iterator flow_it = _flows.begin();
					while (flow_it != _flows.end()) {
						if (_paths[flow_it->second->assignedPath].p == _paths[i].p) {
							if (assignFlowToBondedPath(flow_it->second, now)) {
								_paths[i].assignedFlowCount--;
							}
						}
						++flow_it;
					}
					_paths[i].shouldReallocateFlows = false;
				}
			}
		}
		/**
		 * Re-allocate flows from under-performing
		 * NOTE: This could be part of the above block but was kept separate for clarity.
		 */
		if (_policy == ZT_BOND_POLICY_BALANCE_AWARE) {
			int totalAllocation = 0;
			for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
				if (! _paths[i].p) {
					continue;
				}
				if (_paths[i].p && _paths[i].bonded && _paths[i].eligible) {
					totalAllocation += _paths[i].allocation;
				}
			}
			unsigned char minimumAllocationValue = (uint8_t)(0.33 * ((float)totalAllocation / (float)_numBondedPaths));

			Mutex::Lock _l(_flows_m);
			for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
				if (! _paths[i].p) {
					continue;
				}
				if (_paths[i].p && _paths[i].bonded && _paths[i].eligible && (_paths[i].allocation < minimumAllocationValue) && _paths[i].assignedFlowCount) {
					_paths[i].p->address().toString(pathStr);
					log("reallocate flows from under-performing link %s/%s\n", getLink(_paths[i].p)->ifname().c_str(), pathStr);
					std::map<int32_t, SharedPtr<Flow> >::iterator flow_it = _flows.begin();
					while (flow_it != _flows.end()) {
						if (flow_it->second->assignedPath == _paths[i].p) {
							if (assignFlowToBondedPath(flow_it->second, now)) {
								_paths[i].assignedFlowCount--;
							}
						}
						++flow_it;
					}
					_paths[i].shouldReallocateFlows = false;
				}
			}
		}
	}
}

void Bond::dequeueNextActiveBackupPath(uint64_t now)
{
	if (_abFailoverQueue.empty()) {
		return;
	}
	_abPathIdx = _abFailoverQueue.front();
	_abFailoverQueue.pop_front();
	_lastActiveBackupPathChange = now;
	for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (_paths[i].p) {
			_paths[i].resetPacketCounts();
		}
	}
}

bool Bond::abForciblyRotateLink()
{
	char prevPathStr[64];
	char curPathStr[64];
	if (_policy == ZT_BOND_POLICY_ACTIVE_BACKUP) {
		int prevPathIdx = _abPathIdx;
		_paths[_abPathIdx].p->address().toString(prevPathStr);
		dequeueNextActiveBackupPath(RR->node->now());
		_paths[_abPathIdx].p->address().toString(curPathStr);
		log("forcibly rotate link from %s/%s to %s/%s", getLink(_paths[prevPathIdx].p)->ifname().c_str(), prevPathStr, getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
		return true;
	}
	return false;
}

void Bond::processActiveBackupTasks(void* tPtr, int64_t now)
{
	char pathStr[64] = { 0 };
	char prevPathStr[64];
	char curPathStr[64];
	int prevActiveBackupPathIdx = _abPathIdx;
	int nonPreferredPathIdx;
	bool bFoundPrimaryLink = false;

	/**
	 * Generate periodic status report
	 */
	if ((now - _lastBondStatusLog) > ZT_BOND_STATUS_INTERVAL) {
		_lastBondStatusLog = now;
		if (_abPathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
			log("no active link");
		}
		else if (_paths[_abPathIdx].p) {
			_paths[_abPathIdx].p->address().toString(curPathStr);
			log("active link is %s/%s, failover queue size is %zu", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr, _abFailoverQueue.size());
		}
		if (_abFailoverQueue.empty()) {
			log("failover queue is empty, no longer fault-tolerant");
		}
	}

	/**
	 * Select initial "active" active-backup link
	 */
	if (_abPathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
		/**
		 * [Automatic mode]
		 * The user has not explicitly specified links or their failover schedule,
		 * the bonding policy will now select the first eligible path and set it as
		 * its active backup path, if a substantially better path is detected the bonding
		 * policy will assign it as the new active backup path. If the path fails it will
		 * simply find the next eligible path.
		 */
		if (! userHasSpecifiedLinks()) {
			log("no user-specified links");
			for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
				if (_paths[i].p && _paths[i].eligible) {
					_paths[i].p->address().toString(curPathStr);
					SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
					if (link) {
						log("found eligible link %s/%s", getLink(_paths[i].p)->ifname().c_str(), curPathStr);
						_abPathIdx = i;
						break;
					}
				}
			}
		}
		/**
		 * [Manual mode]
		 * The user has specified links or failover rules that the bonding policy should adhere to.
		 */
		else if (userHasSpecifiedLinks()) {
			if (userHasSpecifiedPrimaryLink()) {
				for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
					if (! _paths[i].p) {
						continue;
					}
					SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
					if (_paths[i].eligible && link->primary()) {
						if (! _paths[i].preferred()) {
							_paths[i].p->address().toString(curPathStr);
							// Found path on primary link, take note in case we don't find a preferred path
							nonPreferredPathIdx = i;
							bFoundPrimaryLink = true;
						}
						if (_paths[i].preferred()) {
							_abPathIdx = i;
							_paths[_abPathIdx].p->address().toString(curPathStr);
							bFoundPrimaryLink = true;
							SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[_abPathIdx].p->localSocket());
							if (link) {
								log("found preferred primary link %s/%s", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
							}
							break;	 // Found preferred path on primary link
						}
					}
				}
				if (bFoundPrimaryLink && nonPreferredPathIdx) {
					log("found non-preferred primary link");
					_abPathIdx = nonPreferredPathIdx;
				}
				if (_abPathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
					log("user-designated primary link is not yet ready");
					// TODO: Should wait for some time (failover interval?) and then switch to spare link
				}
			}
			else if (! userHasSpecifiedPrimaryLink()) {
				log("user did not specify a primary link, select first available link");
				for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
					if (_paths[i].p && _paths[i].eligible) {
						_abPathIdx = i;
						break;
					}
				}
				if (_abPathIdx != ZT_MAX_PEER_NETWORK_PATHS) {
					SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[_abPathIdx].p->localSocket());
					if (link) {
						_paths[_abPathIdx].p->address().toString(curPathStr);
						log("select non-primary link %s/%s", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
					}
				}
			}
		}
	}

	// Short-circuit if we don't have an active link yet
	if (_abPathIdx == ZT_MAX_PEER_NETWORK_PATHS) {
		return;
	}

	// Remove ineligible paths from the failover link queue
	for (std::deque<int>::iterator it(_abFailoverQueue.begin()); it != _abFailoverQueue.end();) {
		if (_paths[(*it)].p && ! _paths[(*it)].eligible) {
			_paths[(*it)].p->address().toString(curPathStr);
			SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[(*it)].p->localSocket());
			it = _abFailoverQueue.erase(it);
			if (link) {
				log("link %s/%s is now ineligible, removing from failover queue (%zu links in queue)", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr, _abFailoverQueue.size());
			}
		}
		else {
			++it;
		}
	}
	/**
	 * Failover instructions were provided by user, build queue according those as well as IPv
	 * preference, disregarding performance.
	 */
	if (userHasSpecifiedFailoverInstructions()) {
		/**
		 * Clear failover scores
		 */
		for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
			if (_paths[i].p) {
				_paths[i].failoverScore = 0;
			}
		}
		// Follow user-specified failover instructions
		for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
			if (! _paths[i].p || ! _paths[i].allowed() || ! _paths[i].eligible) {
				continue;
			}
			SharedPtr<Link> link = RR->bc->getLinkBySocket(_policyAlias, _paths[i].p->localSocket());
			_paths[i].p->address().toString(pathStr);

			int failoverScoreHandicap = _paths[i].failoverScore;
			if (_paths[i].preferred()) {
				failoverScoreHandicap += ZT_BOND_FAILOVER_HANDICAP_PREFERRED;
			}
			if (link->primary()) {
				// If using "optimize" primary re-select mode, ignore user link designations
				failoverScoreHandicap += ZT_BOND_FAILOVER_HANDICAP_PRIMARY;
			}
			if (! _paths[i].failoverScore) {
				// If we didn't inherit a failover score from a "parent" that wants to use this path as a failover
				int newHandicap = failoverScoreHandicap ? failoverScoreHandicap : _paths[i].allocation;
				_paths[i].failoverScore = newHandicap;
			}
			SharedPtr<Link> failoverLink;
			if (link->failoverToLink().length()) {
				failoverLink = RR->bc->getLinkByName(_policyAlias, link->failoverToLink());
			}
			if (failoverLink) {
				for (int j = 0; j < ZT_MAX_PEER_NETWORK_PATHS; j++) {
					if (_paths[j].p && getLink(_paths[j].p) == failoverLink.ptr()) {
						_paths[j].p->address().toString(pathStr);
						int inheritedHandicap = failoverScoreHandicap - 10;
						int newHandicap = _paths[j].failoverScore > inheritedHandicap ? _paths[j].failoverScore : inheritedHandicap;
						if (! _paths[j].preferred()) {
							newHandicap--;
						}
						_paths[j].failoverScore = newHandicap;
					}
				}
			}
			if (_paths[i].p.ptr() != _paths[_abPathIdx].p.ptr()) {
				bool bFoundPathInQueue = false;
				for (std::deque<int>::iterator it(_abFailoverQueue.begin()); it != _abFailoverQueue.end(); ++it) {
					if (_paths[i].p.ptr() == _paths[(*it)].p.ptr()) {
						bFoundPathInQueue = true;
					}
				}
				if (! bFoundPathInQueue) {
					_abFailoverQueue.push_front(i);
					_paths[i].p->address().toString(curPathStr);
					log("add link %s/%s to failover queue (%zu links in queue)", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr, _abFailoverQueue.size());
					addPathToBond(0, i);
				}
			}
		}
	}
	/**
	 * No failover instructions provided by user, build queue according to performance
	 * and IPv preference.
	 */
	else if (! userHasSpecifiedFailoverInstructions()) {
		for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
			if (! _paths[i].p || ! _paths[i].allowed() || ! _paths[i].eligible) {
				continue;
			}
			int failoverScoreHandicap = 0;
			if (_paths[i].preferred()) {
				failoverScoreHandicap = ZT_BOND_FAILOVER_HANDICAP_PREFERRED;
			}
			if (! _paths[i].eligible) {
				failoverScoreHandicap = -10000;
			}
			if (getLink(_paths[i].p)->primary() && _abLinkSelectMethod != ZT_BOND_RESELECTION_POLICY_OPTIMIZE) {
				// If using "optimize" primary re-select mode, ignore user link designations
				failoverScoreHandicap = ZT_BOND_FAILOVER_HANDICAP_PRIMARY;
			}
			if (_paths[i].p.ptr() == _paths[negotiatedPathIdx].p.ptr()) {
				_paths[i].negotiated = true;
				failoverScoreHandicap = ZT_BOND_FAILOVER_HANDICAP_NEGOTIATED;
			}
			else {
				_paths[i].negotiated = false;
			}
			_paths[i].failoverScore = _paths[i].allocation + failoverScoreHandicap;
			if (_paths[i].p.ptr() != _paths[_abPathIdx].p.ptr()) {
				bool bFoundPathInQueue = false;
				for (std::deque<int>::iterator it(_abFailoverQueue.begin()); it != _abFailoverQueue.end(); ++it) {
					if (_paths[i].p.ptr() == _paths[(*it)].p.ptr()) {
						bFoundPathInQueue = true;
					}
				}
				if (! bFoundPathInQueue) {
					_abFailoverQueue.push_front(i);
					_paths[i].p->address().toString(curPathStr);
					log("add link %s/%s to failover queue (%zu links in queue)", getLink(_paths[i].p)->ifname().c_str(), curPathStr, _abFailoverQueue.size());
					addPathToBond(0, i);
				}
			}
		}
	}
	// Sort queue based on performance
	if (! _abFailoverQueue.empty()) {
		for (int i = 0; i < _abFailoverQueue.size(); i++) {
			int value_to_insert = _abFailoverQueue[i];
			int hole_position = i;
			while (hole_position > 0 && (_abFailoverQueue[hole_position - 1] > value_to_insert)) {
				_abFailoverQueue[hole_position] = _abFailoverQueue[hole_position - 1];
				hole_position = hole_position - 1;
			}
			_abFailoverQueue[hole_position] = value_to_insert;
		}
	}

	/**
	 * Short-circuit if we have no queued paths
	 */
	if (_abFailoverQueue.empty()) {
		return;
	}

	/**
	 * Fulfill primary re-select obligations
	 */
	if (_paths[_abPathIdx].p && ! _paths[_abPathIdx].eligible) {   // Implicit ZT_BOND_RESELECTION_POLICY_FAILURE
		_paths[_abPathIdx].p->address().toString(curPathStr);
		log("link %s/%s has failed, select link from failover queue (%zu links in queue)", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr, _abFailoverQueue.size());
		if (! _abFailoverQueue.empty()) {
			dequeueNextActiveBackupPath(now);
			_paths[_abPathIdx].p->address().toString(curPathStr);
			log("active link switched to %s/%s", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
		}
		else {
			log("failover queue is empty, no links to choose from");
		}
	}
	/**
	 * Detect change to prevent flopping during later optimization step.
	 */
	if (prevActiveBackupPathIdx != _abPathIdx) {
		_lastActiveBackupPathChange = now;
	}
	if (_abLinkSelectMethod == ZT_BOND_RESELECTION_POLICY_ALWAYS) {
		if (_paths[_abPathIdx].p && ! getLink(_paths[_abPathIdx].p)->primary() && getLink(_paths[_abFailoverQueue.front()].p)->primary()) {
			dequeueNextActiveBackupPath(now);
			_paths[_abPathIdx].p->address().toString(curPathStr);
			log("switch back to available primary link %s/%s (select: always)", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
		}
	}
	if (_abLinkSelectMethod == ZT_BOND_RESELECTION_POLICY_BETTER) {
		if (_paths[_abPathIdx].p && ! getLink(_paths[_abPathIdx].p)->primary()) {
			// Active backup has switched to "better" primary link according to re-select policy.
			if (getLink(_paths[_abFailoverQueue.front()].p)->primary() && (_paths[_abFailoverQueue.front()].failoverScore > _paths[_abPathIdx].failoverScore)) {
				dequeueNextActiveBackupPath(now);
				_paths[_abPathIdx].p->address().toString(curPathStr);
				log("switch back to user-defined primary link %s/%s (select: better)", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
			}
		}
	}
	if (_abLinkSelectMethod == ZT_BOND_RESELECTION_POLICY_OPTIMIZE && ! _abFailoverQueue.empty()) {
		/**
		 * Implement link negotiation that was previously-decided
		 */
		if (_paths[_abFailoverQueue.front()].negotiated) {
			dequeueNextActiveBackupPath(now);
			_paths[_abPathIdx].p->address().toString(prevPathStr);
			_lastPathNegotiationCheck = now;
			_paths[_abPathIdx].p->address().toString(curPathStr);
			log("switch negotiated link %s/%s (select: optimize)", getLink(_paths[_abPathIdx].p)->ifname().c_str(), curPathStr);
		}
		else {
			// Try to find a better path and automatically switch to it -- not too often, though.
			if ((now - _lastActiveBackupPathChange) > ZT_BOND_OPTIMIZE_INTERVAL) {
				if (! _abFailoverQueue.empty()) {
					int newFScore = _paths[_abFailoverQueue.front()].failoverScore;
					int prevFScore = _paths[_abPathIdx].failoverScore;
					// Establish a minimum switch threshold to prevent flapping
					int failoverScoreDifference = _paths[_abFailoverQueue.front()].failoverScore - _paths[_abPathIdx].failoverScore;
					int thresholdQuantity = (int)(ZT_BOND_ACTIVE_BACKUP_OPTIMIZE_MIN_THRESHOLD * (float)_paths[_abPathIdx].allocation);
					if ((failoverScoreDifference > 0) && (failoverScoreDifference > thresholdQuantity)) {
						SharedPtr<Path> oldPath = _paths[_abPathIdx].p;
						_paths[_abPathIdx].p->address().toString(prevPathStr);
						dequeueNextActiveBackupPath(now);
						_paths[_abPathIdx].p->address().toString(curPathStr);
						log("ab",
							"switch from %s/%s (score: %d) to better link %s/%s (score: %d) for peer %llx (select: optimize)",
							getLink(oldPath)->ifname().c_str(),
							prevPathStr,
							prevFScore,
							getLink(_paths[_abPathIdx].p)->ifname().c_str(),
							curPathStr,
							newFScore,
							_peerId);
					}
				}
			}
		}
	}
}

void Bond::setBondParameters(int policy, SharedPtr<Bond> templateBond, bool useTemplate)
{
	// Sanity check for policy

	_defaultPolicy = (_defaultPolicy <= ZT_BOND_POLICY_NONE || _defaultPolicy > ZT_BOND_POLICY_BALANCE_AWARE) ? ZT_BOND_POLICY_NONE : _defaultPolicy;
	_policy = (policy <= ZT_BOND_POLICY_NONE || policy > ZT_BOND_POLICY_BALANCE_AWARE) ? ZT_BOND_POLICY_NONE : _defaultPolicy;

	// Flows

	_lastFlowExpirationCheck = 0;
	_lastFlowRebalance = 0;
	_allowFlowHashing = false;

	// Path negotiation

	_lastSentPathNegotiationRequest = 0;
	_lastPathNegotiationCheck = 0;
	_allowPathNegotiation = false;
	_pathNegotiationCutoffCount = 0;
	_lastPathNegotiationReceived = 0;
	_localUtility = 0;

	// QOS Verb (and related checks)

	_qosCutoffCount = 0;
	_lastQoSRateCheck = 0;
	_lastQualityEstimation = 0;

	// User preferences which may override the default bonding algorithm's behavior

	_userHasSpecifiedPrimaryLink = false;
	_userHasSpecifiedFailoverInstructions = false;
	_userHasSpecifiedLinkSpeeds = 0;

	// Bond status

	_lastBondStatusLog = 0;
	_lastSummaryDump = 0;
	_isHealthy = false;
	_numAliveLinks = 0;
	_numTotalLinks = 0;
	_numBondedPaths = 0;

	// active-backup

	_lastActiveBackupPathChange = 0;
	_abPathIdx = ZT_MAX_PEER_NETWORK_PATHS;

	// rr

	_rrPacketsSentOnCurrLink = 0;
	_rrIdx = 0;

	// General parameters

	_downDelay = 0;
	_upDelay = 0;
	_monitorInterval = 0;

	// (Sane?) limits

	_maxAcceptableLatency = 100;
	_maxAcceptablePacketDelayVariance = 50;
	_maxAcceptablePacketLossRatio = 0.10f;
	_maxAcceptablePacketErrorRatio = 0.10f;

	// General timers

	_lastFrame = 0;
	_lastBackgroundTaskCheck = 0;

	// balance-aware

	_totalBondUnderload = 0;

	_overheadBytes = 0;

	/**
	 * Policy-specific defaults
	 */
	switch (_policy) {
		case ZT_BOND_POLICY_ACTIVE_BACKUP:
			_abLinkSelectMethod = ZT_BOND_RESELECTION_POLICY_OPTIMIZE;
			break;
		case ZT_BOND_POLICY_BROADCAST:
			_downDelay = 30000;
			_upDelay = 0;
			break;
		case ZT_BOND_POLICY_BALANCE_RR:
			_packetsPerLink = 64;
			break;
		case ZT_BOND_POLICY_BALANCE_XOR:
			_allowFlowHashing = true;
			break;
		case ZT_BOND_POLICY_BALANCE_AWARE:
			_allowFlowHashing = true;
			break;
		default:
			break;
	}

	_qw[ZT_QOS_LAT_IDX] = 0.3f;
	_qw[ZT_QOS_LTM_IDX] = 0.1f;
	_qw[ZT_QOS_PDV_IDX] = 0.3f;
	_qw[ZT_QOS_PLR_IDX] = 0.1f;
	_qw[ZT_QOS_PER_IDX] = 0.1f;
	_qw[ZT_QOS_SCP_IDX] = 0.1f;

	_failoverInterval = ZT_BOND_FAILOVER_DEFAULT_INTERVAL;

	/* If a user has specified custom parameters for this bonding policy, overlay them onto the defaults */
	if (useTemplate) {
		_policyAlias = templateBond->_policyAlias;
		_failoverInterval = templateBond->_failoverInterval >= ZT_BOND_FAILOVER_MIN_INTERVAL ? templateBond->_failoverInterval : ZT_BOND_FAILOVER_MIN_INTERVAL;
		_downDelay = templateBond->_downDelay;
		_upDelay = templateBond->_upDelay;
		_abLinkSelectMethod = templateBond->_abLinkSelectMethod;
		memcpy(_qw, templateBond->_qw, ZT_QOS_WEIGHT_SIZE * sizeof(float));
	}

	// Timer geometry

	_monitorInterval = _failoverInterval / ZT_BOND_ECHOS_PER_FAILOVER_INTERVAL;
	_qualityEstimationInterval = _failoverInterval * 2;
	_qosSendInterval = _failoverInterval * 2;
	_qosCutoffCount = 0;
	_defaultPathRefractoryPeriod = 8000;
}

void Bond::setUserQualityWeights(float weights[], int len)
{
	if (len == ZT_QOS_WEIGHT_SIZE) {
		float weightTotal = 0.0;
		for (unsigned int i = 0; i < ZT_QOS_WEIGHT_SIZE; ++i) {
			weightTotal += weights[i];
		}
		if (weightTotal > 0.99 && weightTotal < 1.01) {
			memcpy(_qw, weights, len * sizeof(float));
		}
	}
}

SharedPtr<Link> Bond::getLink(const SharedPtr<Path>& path)
{
	return RR->bc->getLinkBySocket(_policyAlias, path->localSocket());
}

void Bond::dumpPathStatus(int64_t now, int pathIdx)
{
	char pathStr[64] = { 0 };
	_paths[pathIdx].p->address().toString(pathStr);
	log("path status: [%2d] alive:%d, eli:%d, bonded:%d, flows:%6d, lat:%10.3f, jitter:%10.3f, error:%6.4f, loss:%6.4f, age:%llu alloc:%d--- (%s/%s)",
		pathIdx,
		_paths[pathIdx].alive,
		_paths[pathIdx].eligible,
		_paths[pathIdx].bonded,
		_paths[pathIdx].assignedFlowCount,
		_paths[pathIdx].latencyMean,
		_paths[pathIdx].latencyVariance,
		_paths[pathIdx].packetErrorRatio,
		_paths[pathIdx].packetLossRatio,
		(unsigned long long)_paths[pathIdx].p->age(now),
		_paths[pathIdx].allocation,
		getLink(_paths[pathIdx].p)->ifname().c_str(),
		pathStr);
}

void Bond::dumpInfo(int64_t now, bool force)
{
	uint64_t timeSinceLastDump = now - _lastSummaryDump;
	if (! force && timeSinceLastDump < ZT_BOND_STATUS_INTERVAL) {
		return;
	}
	_lastSummaryDump = now;
	float overhead = (_overheadBytes / (timeSinceLastDump / 1000.0f) / 1000.0f);
	_overheadBytes = 0;
	log("bond status: bp: %d, fi: %d, mi: %d, ud: %d, dd: %d, flows: %lu, ambient: %f KB/s", _policy, _failoverInterval, _monitorInterval, _upDelay, _downDelay, (unsigned long)_flows.size(), overhead);
	for (int i = 0; i < ZT_MAX_PEER_NETWORK_PATHS; ++i) {
		if (_paths[i].p) {
			dumpPathStatus(now, i);
		}
	}
}

}	// namespace ZeroTier
