/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Client.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "Client.h"

#include <chrono>
#include <thread>
#include <boost/filesystem.hpp>
#include <libdevcore/Log.h>
#include <libdevcore/StructuredLogger.h>
#include <libp2p/Host.h>
#include "Defaults.h"
#include "Executive.h"
#include "EthereumHost.h"
using namespace std;
using namespace dev;
using namespace dev::eth;
using namespace p2p;

VersionChecker::VersionChecker(string const& _dbPath):
	m_path(_dbPath.size() ? _dbPath : Defaults::dbPath())
{
	bytes statusBytes = contents(m_path + "/status");
	RLP status(statusBytes);
	try
	{
		auto protocolVersion = (unsigned)status[0];
		auto minorProtocolVersion = (unsigned)status[1];
		auto databaseVersion = (unsigned)status[2];
		m_action =
			protocolVersion != eth::c_protocolVersion || databaseVersion != c_databaseVersion ?
				WithExisting::Kill
			: minorProtocolVersion != eth::c_minorProtocolVersion ?
				WithExisting::Verify
			:
				WithExisting::Trust;
	}
	catch (...)
	{
		m_action = WithExisting::Kill;
	}
}

void VersionChecker::setOk()
{
	if (m_action != WithExisting::Trust)
	{
		try
		{
			boost::filesystem::create_directory(m_path);
		}
		catch (...)
		{
			cwarn << "Unhandled exception! Failed to create directory: " << m_path << "\n" << boost::current_exception_diagnostic_information();
		}
		writeFile(m_path + "/status", rlpList(eth::c_protocolVersion, eth::c_minorProtocolVersion, c_databaseVersion));
	}
}

void BasicGasPricer::update(BlockChain const& _bc)
{
	unsigned c = 0;
	h256 p = _bc.currentHash();
	m_gasPerBlock = _bc.info(p).gasLimit;

	map<u256, unsigned> dist;
	unsigned total = 0;
	while (c < 1000 && p)
	{
		BlockInfo bi = _bc.info(p);
		if (bi.transactionsRoot != EmptyTrie)
		{
			auto bb = _bc.block(p);
			RLP r(bb);
			BlockReceipts brs(_bc.receipts(bi.hash()));
			for (unsigned i = 0; i < r[1].size(); ++i)
			{
				auto gu = brs.receipts[i].gasUsed();
				dist[Transaction(r[1][i].data(), CheckTransaction::None).gasPrice()] += (unsigned)brs.receipts[i].gasUsed();
				total += (unsigned)gu;
			}
		}
		p = bi.parentHash;
		++c;
	}
	if (total > 0)
	{
		unsigned t = 0;
		unsigned q = 1;
		m_octiles[0] = dist.begin()->first;
		for (auto const& i: dist)
		{
			for (; t <= total * q / 8 && t + i.second > total * q / 8; ++q)
				m_octiles[q] = i.first;
			if (q > 7)
				break;
		}
		m_octiles[8] = dist.rbegin()->first;
	}
}

Client::Client(p2p::Host* _extNet, std::string const& _dbPath, WithExisting _forceAction, u256 _networkId, int _miners):
	Worker("eth"),
	m_vc(_dbPath),
	m_bc(_dbPath, max(m_vc.action(), _forceAction), [](unsigned d, unsigned t){ cerr << "REVISING BLOCKCHAIN: Processed " << d << " of " << t << "...\r"; }),
	m_gp(new TrivialGasPricer),
	m_stateDB(State::openDB(_dbPath, max(m_vc.action(), _forceAction))),
	m_preMine(m_stateDB, BaseState::CanonGenesis),
	m_postMine(m_stateDB)
{
	m_gp->update(m_bc);

	m_host = _extNet->registerCapability(new EthereumHost(m_bc, m_tq, m_bq, _networkId));

	if (_miners > -1)
		setMiningThreads(_miners);
	else
		setMiningThreads();
	if (_dbPath.size())
		Defaults::setDBPath(_dbPath);
	m_vc.setOk();
	doWork();

	startWorking();
}

Client::Client(p2p::Host* _extNet, std::shared_ptr<GasPricer> _gp, std::string const& _dbPath, WithExisting _forceAction, u256 _networkId, int _miners):
	Worker("eth"),
	m_vc(_dbPath),
	m_bc(_dbPath, max(m_vc.action(), _forceAction), [](unsigned d, unsigned t){ cerr << "REVISING BLOCKCHAIN: Processed " << d << " of " << t << "...\r"; }),
	m_gp(_gp),
	m_stateDB(State::openDB(_dbPath, max(m_vc.action(), _forceAction))),
	m_preMine(m_stateDB),
	m_postMine(m_stateDB)
{
	m_gp->update(m_bc);

	m_host = _extNet->registerCapability(new EthereumHost(m_bc, m_tq, m_bq, _networkId));

	if (_miners > -1)
		setMiningThreads(_miners);
	else
		setMiningThreads();
	if (_dbPath.size())
		Defaults::setDBPath(_dbPath);
	m_vc.setOk();
	doWork();

	startWorking();
}

Client::~Client()
{
	stopWorking();
}

void Client::setNetworkId(u256 _n)
{
	if (auto h = m_host.lock())
		h->setNetworkId(_n);
}

DownloadMan const* Client::downloadMan() const
{
	if (auto h = m_host.lock())
		return &(h->downloadMan());
	return nullptr;
}

bool Client::isSyncing() const
{
	if (auto h = m_host.lock())
		return h->isSyncing();
	return false;
}

void Client::doneWorking()
{
	// Synchronise the state according to the head of the block chain.
	// TODO: currently it contains keys for *all* blocks. Make it remove old ones.
	WriteGuard l(x_stateDB);
	m_preMine.sync(m_bc);
	m_postMine = m_preMine;
}

void Client::killChain()
{
	bool wasMining = isMining();
	if (wasMining)
		stopMining();
	stopWorking();

	m_tq.clear();
	m_bq.clear();
	m_localMiners.clear();
	m_preMine = State();
	m_postMine = State();

	{
		WriteGuard l(x_stateDB);
		m_stateDB = OverlayDB();
		m_stateDB = State::openDB(Defaults::dbPath(), WithExisting::Kill);
	}
	m_bc.reopen(Defaults::dbPath(), WithExisting::Kill);

	m_preMine = State(m_stateDB);
	m_postMine = State(m_stateDB);

	if (auto h = m_host.lock())
		h->reset();

	doWork();

	setMiningThreads(0);

	startWorking();
	if (wasMining)
		startMining();
}

void Client::clearPending()
{
	h256Set changeds;
	{
		WriteGuard l(x_stateDB);
		if (!m_postMine.pending().size())
			return;
//		for (unsigned i = 0; i < m_postMine.pending().size(); ++i)
//			appendFromNewPending(m_postMine.logBloom(i), changeds);
		changeds.insert(PendingChangedFilter);
		m_tq.clear();
		m_postMine = m_preMine;
	}

	{
		ReadGuard l(x_localMiners);
		for (auto& m: m_localMiners)
			m.noteStateChange();
	}

	noteChanged(changeds);
}

template <class T>
static string filtersToString(T const& _fs)
{
	stringstream ret;
	ret << "{";
	unsigned i = 0;
	for (h256 const& f: _fs)
		ret << (i++ ? ", " : "") << (f == PendingChangedFilter ? "pending" : f == ChainChangedFilter ? "chain" : f.abridged());
	ret << "}";
	return ret.str();
}

void Client::noteChanged(h256Set const& _filters)
{
	Guard l(x_filtersWatches);
	if (_filters.size())
		cnote << "noteChanged(" << filtersToString(_filters) << ")";
	// accrue all changes left in each filter into the watches.
	for (auto& w: m_watches)
		if (_filters.count(w.second.id))
		{
			cwatch << "!!!" << w.first << (m_filters.count(w.second.id) ? w.second.id.abridged() : w.second.id == PendingChangedFilter ? "pending" : w.second.id == ChainChangedFilter ? "chain" : "???");
			if (m_filters.count(w.second.id))	// Normal filtering watch
				w.second.changes += m_filters.at(w.second.id).changes;
			else								// Special ('pending'/'latest') watch
				w.second.changes.push_back(LocalisedLogEntry(SpecialLogEntry, 0));
		}
	// clear the filters now.
	for (auto& i: m_filters)
		i.second.changes.clear();
}

void Client::appendFromNewPending(TransactionReceipt const& _receipt, h256Set& io_changed, h256 _transactionHash)
{
	Guard l(x_filtersWatches);
	for (pair<h256 const, InstalledFilter>& i: m_filters)
		if (i.second.filter.envelops(RelativeBlock::Pending, m_bc.number() + 1))
		{
			// acceptable number.
			auto m = i.second.filter.matches(_receipt);
			if (m.size())
			{
				// filter catches them
				for (LogEntry const& l: m)
					i.second.changes.push_back(LocalisedLogEntry(l, m_bc.number() + 1, _transactionHash));
				io_changed.insert(i.first);
			}
		}
}

void Client::appendFromNewBlock(h256 const& _block, h256Set& io_changed)
{
	// TODO: more precise check on whether the txs match.
	auto d = m_bc.info(_block);
	auto br = m_bc.receipts(_block);

	Guard l(x_filtersWatches);
	for (pair<h256 const, InstalledFilter>& i: m_filters)
		if (i.second.filter.envelops(RelativeBlock::Latest, d.number) && i.second.filter.matches(d.logBloom))
			// acceptable number & looks like block may contain a matching log entry.
			for (size_t j = 0; j < br.receipts.size(); j++)
			{
				auto tr = br.receipts[j];
				auto m = i.second.filter.matches(tr);
				if (m.size())
				{
					auto transactionHash = transaction(d.hash(), j).sha3();
					// filter catches them
					for (LogEntry const& l: m)
						i.second.changes.push_back(LocalisedLogEntry(l, (unsigned)d.number, transactionHash));
					io_changed.insert(i.first);
				}
			}
}

void Client::setForceMining(bool _enable)
{
	 m_forceMining = _enable;
	 ReadGuard l(x_localMiners);
	 for (auto& m: m_localMiners)
		 m.noteStateChange();
}

void Client::setMiningThreads(unsigned _threads)
{
	stopMining();
	auto t = _threads ? _threads : thread::hardware_concurrency();
#if ETH_ETHASHCL || !ETH_TRUE
	if (m_turboMining)
		t = 1;
#endif
	WriteGuard l(x_localMiners);
	m_localMiners.clear();
	m_localMiners.resize(t);
	unsigned i = 0;
	for (auto& m: m_localMiners)
		m.setup(this, i++);
}

MineProgress Client::miningProgress() const
{
	MineProgress ret;
	ReadGuard l(x_localMiners);
	for (auto& m: m_localMiners)
		ret.combine(m.miningProgress());
	return ret;
}

uint64_t Client::hashrate() const
{
	uint64_t ret = 0;
	ReadGuard l(x_localMiners);
	for (LocalMiner const& m: m_localMiners)
		ret += m.miningProgress().hashes / m.miningProgress().ms;
	return ret / 1000;
}

std::list<MineInfo> Client::miningHistory()
{
	std::list<MineInfo> ret;

	ReadGuard l(x_localMiners);
	if (m_localMiners.empty())
		return ret;
	ret = m_localMiners[0].miningHistory();
	for (unsigned i = 1; i < m_localMiners.size(); ++i)
	{
		auto l = m_localMiners[i].miningHistory();
		auto ri = ret.begin();
		auto li = l.begin();
		for (; ri != ret.end() && li != l.end(); ++ri, ++li)
			ri->combine(*li);
	}
	return ret;
}

void Client::setupState(State& _s)
{
	{
		ReadGuard l(x_stateDB);
		cwork << "SETUP MINE";
		_s = m_postMine;
	}
	if (m_paranoia)
	{
		if (_s.amIJustParanoid(m_bc))
		{
			cnote << "I'm just paranoid. Block is fine.";
			_s.commitToMine(m_bc);
		}
		else
		{
			cwarn << "I'm not just paranoid. Cannot mine. Please file a bug report.";
		}
	}
	else
		_s.commitToMine(m_bc);
}

ExecutionResult Client::call(Address _dest, bytes const& _data, u256 _gas, u256 _value, u256 _gasPrice, Address const& _from)
{
	ExecutionResult ret;
	try
	{
		State temp;
//		cdebug << "Nonce at " << toAddress(_secret) << " pre:" << m_preMine.transactionsFrom(toAddress(_secret)) << " post:" << m_postMine.transactionsFrom(toAddress(_secret));
		{
			ReadGuard l(x_stateDB);
			temp = m_postMine;
			temp.addBalance(_from, _value + _gasPrice * _gas);
		}
		Executive e(temp, LastHashes(), 0);
		if (!e.call(_dest, _dest, _from, _value, _gasPrice, &_data, _gas, _from))
			e.go();
		ret = e.executionResult();
	}
	catch (...)
	{
		// TODO: Some sort of notification of failure.
	}
	return ret;
}

pair<h256, u256> Client::getWork()
{
	Guard l(x_remoteMiner);
	{
		ReadGuard l(x_stateDB);
		m_remoteMiner.update(m_postMine, m_bc);
	}
	return make_pair(m_remoteMiner.workHash(), m_remoteMiner.difficulty());
}

bool Client::submitWork(ProofOfWork::Proof const& _proof)
{
	Guard l(x_remoteMiner);
	return m_remoteMiner.submitWork(_proof);
}

void Client::doWork()
{
	// TODO: Use condition variable rather than polling.

	bool stillGotWork = false;

	cworkin << "WORK";
	h256Set changeds;

	auto maintainMiner = [&](Miner& m)
	{
		if (m.isComplete())
		{
			// TODO: enable a short-circuit option since we mined it. will need to get the end state from the miner.
			auto lm = dynamic_cast<LocalMiner*>(&m);
			h256s hs;
			h256 c;
			if (false && lm && !m_verifyOwnBlocks)
			{
				// TODO: implement
				//m_bc.attemptImport(m_blockData(), m_stateDB, lm->state());
				// TODO: derive hs from lm->state()
			}
			else
			{
				cwork << "CHAIN <== postSTATE";
				WriteGuard l(x_stateDB);
				tie(hs, c) = m_bc.attemptImport(m.blockData(), m_stateDB);
			}
			if (hs.size())
			{
				for (auto const& h: hs)
					if (h != c)
						appendFromNewBlock(h, changeds);
				changeds.insert(ChainChangedFilter);
			}
			for (auto& m: m_localMiners)
				m.noteStateChange();
		}
	};
	{
		ReadGuard l(x_localMiners);
		for (auto& m: m_localMiners)
			maintainMiner(m);
	}
	{
		Guard l(x_remoteMiner);
		maintainMiner(m_remoteMiner);
	}

	// Synchronise state to block chain.
	// This should remove any transactions on our queue that are included within our state.
	// It also guarantees that the state reflects the longest (valid!) chain on the block chain.
	//   This might mean reverting to an earlier state and replaying some blocks, or, (worst-case:
	//   if there are no checkpoints before our fork) reverting to the genesis block and replaying
	//   all blocks.
	// Resynchronise state with block chain & trans
	bool resyncStateNeeded = false;
	{
		WriteGuard l(x_stateDB);
		cwork << "BQ ==> CHAIN ==> STATE";
		OverlayDB db = m_stateDB;
		x_stateDB.unlock();
		h256s fresh;
		h256s dead;
		bool sgw;
		tie(fresh, dead, sgw) = m_bc.sync(m_bq, db, 100);

		// insert transactions that we are declaring the dead part of the chain
		for (auto const& h: dead)
		{
			clog(ClientNote) << "Dead block:" << h.abridged();
			for (auto const& t: m_bc.transactions(h))
			{
				clog(ClientNote) << "Resubmitting transaction " << Transaction(t, CheckTransaction::None);
				m_tq.import(t);
			}
		}

		// remove transactions from m_tq nicely rather than relying on out of date nonce later on.
		for (auto const& h: fresh)
		{
			clog(ClientChat) << "Live block:" << h.abridged();
			for (auto const& th: m_bc.transactionHashes(h))
			{
				clog(ClientNote) << "Safely dropping transaction " << th.abridged();
				m_tq.drop(th);
			}
		}

		stillGotWork = stillGotWork | sgw;
		if (!fresh.empty())
		{
			for (auto i: fresh)
				appendFromNewBlock(i, changeds);
			changeds.insert(ChainChangedFilter);
		}
		x_stateDB.lock();
		if (fresh.size())
			m_stateDB = db;

		cwork << "preSTATE <== CHAIN";
		if (m_preMine.sync(m_bc) || m_postMine.address() != m_preMine.address())
		{
			if (isMining())
				cnote << "New block on chain: Restarting mining operation.";
			m_postMine = m_preMine;
			resyncStateNeeded = true;
			changeds.insert(PendingChangedFilter);
			// TODO: Move transactions pending from m_postMine back to transaction queue.
		}

		// returns TransactionReceipts, once for each transaction.
		cwork << "postSTATE <== TQ";
		TransactionReceipts newPendingReceipts = m_postMine.sync(m_bc, m_tq, *m_gp);
		if (newPendingReceipts.size())
		{
			for (size_t i = 0; i < newPendingReceipts.size(); i++)
				appendFromNewPending(newPendingReceipts[i], changeds, m_postMine.pending()[i].sha3());
			
			changeds.insert(PendingChangedFilter);

			if (isMining())
				cnote << "Additional transaction ready: Restarting mining operation.";
			resyncStateNeeded = true;
			if (auto h = m_host.lock())
				h->noteNewTransactions();
		}
	}

	if (!changeds.empty())
		if (auto h = m_host.lock())
			h->noteNewBlocks();

	if (resyncStateNeeded)
	{
		ReadGuard l(x_localMiners);
		for (auto& m: m_localMiners)
			m.noteStateChange();
	}

	cwork << "noteChanged" << changeds.size() << "items";
	noteChanged(changeds);
	cworkout << "WORK";

	if (!stillGotWork)
		this_thread::sleep_for(chrono::milliseconds(100));

	if (chrono::system_clock::now() - m_lastGarbageCollection > chrono::seconds(5))
	{
		// watches garbage collection
		vector<unsigned> toUninstall;
		{
			Guard l(x_filtersWatches);
			for (auto key: keysOf(m_watches))
				if (m_watches[key].lastPoll != chrono::system_clock::time_point::max() && chrono::system_clock::now() - m_watches[key].lastPoll > chrono::seconds(20))
				{
					toUninstall.push_back(key);
					cnote << "GC: Uninstall" << key << "(" << chrono::duration_cast<chrono::seconds>(chrono::system_clock::now() - m_watches[key].lastPoll).count() << "s old)";
				}
		}
		for (auto i: toUninstall)
			uninstallWatch(i);

		// blockchain GC
		m_bc.garbageCollect();

		m_lastGarbageCollection = chrono::system_clock::now();
	}
}

State Client::asOf(h256 const& _block) const
{
	ReadGuard l(x_stateDB);
	return State(m_stateDB, bc(), _block);
}

void Client::prepareForTransaction()
{
	startWorking();
}

State Client::state(unsigned _txi, h256 _block) const
{
	ReadGuard l(x_stateDB);
	return State(m_stateDB, m_bc, _block).fromPending(_txi);
}

eth::State Client::state(h256 _block) const
{
	ReadGuard l(x_stateDB);
	return State(m_stateDB, m_bc, _block);
}

eth::State Client::state(unsigned _txi) const
{
	ReadGuard l(x_stateDB);
	return m_postMine.fromPending(_txi);
}

void Client::inject(bytesConstRef _rlp)
{
	startWorking();
	
	m_tq.import(_rlp);
}

void Client::flushTransactions()
{
	doWork();
}
