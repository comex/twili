//
// Twili - Homebrew debug monitor for the Nintendo Switch
// Copyright (C) 2018 misson20000 <xenotoad@xenotoad.net>
//
// This file is part of Twili.
//
// Twili is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Twili is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Twili.  If not, see <http://www.gnu.org/licenses/>.
//

#include "GdbStub.hpp"

#include<functional>

#include "common/Logger.hpp"
#include "common/ResultError.hpp"

namespace twili {
namespace twib {
namespace tool {
namespace gdb {

GdbStub::GdbStub(ITwibDeviceInterface &itdi) :
	itdi(itdi),
	connection(
		platform::File(STDIN_FILENO, false),
		platform::File(STDOUT_FILENO, false)),
	logic(*this),
	loop(logic),
	xfer_libraries(*this, &GdbStub::XferReadLibraries),
	xfer_memory_map(*this, &GdbStub::XferReadMemoryMap) {
	AddGettableQuery(Query(*this, "Supported", &GdbStub::QueryGetSupported, false));
	AddGettableQuery(Query(*this, "C", &GdbStub::QueryGetCurrentThread, false));
	AddGettableQuery(Query(*this, "fThreadInfo", &GdbStub::QueryGetFThreadInfo, false));
	AddGettableQuery(Query(*this, "sThreadInfo", &GdbStub::QueryGetSThreadInfo, false));
	AddGettableQuery(Query(*this, "ThreadExtraInfo", &GdbStub::QueryGetThreadExtraInfo, false, ','));
	AddGettableQuery(Query(*this, "Offsets", &GdbStub::QueryGetOffsets, false));
	AddGettableQuery(Query(*this, "Rcmd", &GdbStub::QueryGetRemoteCommand, false, ','));
	AddGettableQuery(Query(*this, "Xfer", &GdbStub::QueryXfer, false));
	AddSettableQuery(Query(*this, "StartNoAckMode", &GdbStub::QuerySetStartNoAckMode));
	AddSettableQuery(Query(*this, "ThreadEvents", &GdbStub::QuerySetThreadEvents));
	AddMultiletterHandler("Attach", &GdbStub::HandleVAttach);
	AddMultiletterHandler("Cont?", &GdbStub::HandleVContQuery);
	AddMultiletterHandler("Cont", &GdbStub::HandleVCont);
	AddXferObject("libraries", xfer_libraries);
	AddXferObject("memory-map", xfer_memory_map);
	InitBreakpointRegs();
}

GdbStub::~GdbStub() {
	ClearBreakpointRegs();
	loop.Destroy();
}

void GdbStub::Run() {
	std::unique_lock<std::mutex> lock(connection.mutex);
	loop.Begin();
	while(!connection.error_flag) {
		connection.error_condvar.wait(lock);
	}
}

GdbStub::Query::Query(GdbStub &stub, std::string field, void (GdbStub::*visitor)(util::Buffer&), bool should_advertise, char separator) :
	stub(stub),
	field(field),
	visitor(visitor),
	should_advertise(should_advertise),
	separator(separator) {
}

void GdbStub::AddFeature(std::string feature) {
	features.push_back(feature);
}

void GdbStub::AddGettableQuery(Query query) {
	gettable_queries.emplace(query.field, query);

	if(query.should_advertise) {
		features.push_back(std::string("q") + query.field + "+");
	}
}

void GdbStub::AddSettableQuery(Query query) {
	settable_queries.emplace(query.field, query);

	if(query.should_advertise) {
		features.push_back(std::string("Q") + query.field + "+");
	}
}

void GdbStub::AddMultiletterHandler(std::string name, void (GdbStub::*handler)(util::Buffer&)) {
	multiletter_handlers.emplace(name, handler);
}

void GdbStub::AddXferObject(std::string name, XferObject &object) {
	xfer_objects.emplace(name, object);
	if(object.AdvertiseRead()) {
		features.push_back(std::string("qXfer:") + name + ":read+");
	}
	if(object.AdvertiseWrite()) {
		features.push_back(std::string("qXfer:") + name + ":write+");
	}
}

void GdbStub::ReadThreadId(util::Buffer &packet, int64_t &pid, int64_t &thread_id) {
	pid = current_thread ? current_thread->process.pid : 0;
	
	char ch;
	if(packet.ReadAvailable() && packet.Read()[0] == 'p') { // peek
		packet.MarkRead(1); // consume
		if(packet.ReadAvailable() && packet.Read()[0] == '-') { // all processes
			pid = -1;
			packet.MarkRead(1); // consume
		} else {
			uint64_t dec_pid;
			GdbConnection::DecodeWithSeparator(dec_pid, '.', packet);
			pid = dec_pid;
		}
	}

	if(packet.ReadAvailable() && packet.Read()[0] == '-') { // all threads
		thread_id = -1;
		packet.MarkRead(1); // consume
	} else {
		uint64_t dec_thread_id;
		GdbConnection::Decode(dec_thread_id, packet);
		thread_id = dec_thread_id;
	}
}

void GdbStub::HandleGeneralGetQuery(util::Buffer &packet) {
	std::string field;
	char ch;
	std::unordered_map<std::string, Query>::iterator i = gettable_queries.end();
	while(packet.Read(ch)) {
		if(i != gettable_queries.end() && ch == i->second.separator) {
			break;
		}
		field.push_back(ch);
		i = gettable_queries.find(field);
	}
	LogMessage(Debug, "got get query for '%s'", field.c_str());

	if(i != gettable_queries.end()) {
		std::invoke(i->second.visitor, this, packet);
	} else {
		LogMessage(Info, "unsupported query: '%s'", field.c_str());
		connection.RespondEmpty();
	}
}

void GdbStub::HandleGeneralSetQuery(util::Buffer &packet) {
	std::string field;
	char ch;
	std::unordered_map<std::string, Query>::iterator i = settable_queries.end();
	while(packet.Read(ch)) {
		if(i != settable_queries.end() && ch == i->second.separator) {
			break;
		}
		field.push_back(ch);
		i = settable_queries.find(field);
	}
	LogMessage(Debug, "got set query for '%s'", field.c_str());

	if(i != settable_queries.end()) {
		std::invoke(i->second.visitor, this, packet);
	} else {
		LogMessage(Info, "unsupported query: '%s'", field.c_str());
		connection.RespondEmpty();
	}
}

void GdbStub::HandleIsThreadAlive(util::Buffer &packet) {
	connection.RespondOk();
}

void GdbStub::HandleMultiletterPacket(util::Buffer &packet) {
	std::string title;
	char ch;
	while(packet.Read(ch) && ch != ';') {
		title.push_back(ch);
	}
	LogMessage(Debug, "got v'%s'", title.c_str());

	auto i = multiletter_handlers.find(title);
	if(i != multiletter_handlers.end()) {
		std::invoke(i->second, this, packet);
	} else {
		LogMessage(Info, "unsupported v: '%s'", title.c_str());
		connection.RespondEmpty();
	}
}

void GdbStub::HandleGetStopReason() {
	util::Buffer buf;
	buf.Write(stop_reason);
	connection.Respond(buf);
}

void GdbStub::HandleDetach(util::Buffer &packet) {
	char ch;
	if(packet.Read(ch)) {
		if(ch != ';') {
			LogMessage(Warning, "invalid detach packet");
			connection.RespondError(1);
			return;
		}
		
		uint64_t pid;
		GdbConnection::Decode(pid, packet);
		LogMessage(Debug, "detaching from 0x%x", pid);
		if(current_thread->process.pid == pid) {
			current_thread = nullptr;
		}
		get_thread_info.valid = false;
		attached_processes.erase(pid);
		ClearBreakpointRegs(pid);
	} else { // detach all
		LogMessage(Debug, "detaching from all");
		current_thread = nullptr;
		get_thread_info.valid = false;
		attached_processes.clear();
		ClearBreakpointRegs();
	}
	stop_reason = "W00";
	connection.RespondOk();
}

void GdbStub::HandleReadGeneralRegisters() {
	if(current_thread == nullptr) {
		LogMessage(Warning, "attempt to read registers with no selected thread");
		connection.RespondError(1);
		return;
	}

	try {
		util::Buffer response;
		std::vector<uint64_t> registers = current_thread->GetRegisters();
		GdbConnection::Encode((uint8_t*) registers.data(), 268, response);
		GdbConnection::Encode((uint8_t*) registers.data() + 272, 520, response);
		std::string str;
		size_t sz = response.ReadAvailable();
		response.Read(str, sz);
		response.MarkRead(-sz);
		LogMessage(Debug, "responding with '%s'", str.c_str());
		connection.Respond(response);
	} catch(ResultError &e) {
		LogMessage(Debug, "failed to read registers: 0x%x", e.code);
		connection.RespondError(e.code);
	}
}

void GdbStub::HandleWriteGeneralRegisters(util::Buffer &packet) {
	if(current_thread == nullptr) {
		LogMessage(Warning, "attempt to write registers with no selected thread");
		connection.RespondError(1);
	}

	std::vector<uint8_t> registers_binary;
	GdbConnection::Decode(registers_binary, packet);

	std::vector<uint64_t> registers(99);
	memcpy(registers.data(), registers_binary.data(), 268);
	memcpy(registers.data() + 272/8, registers_binary.data() + 268, 520);
	
	try {
		current_thread->SetRegisters(registers);
		connection.RespondOk();
	} catch(ResultError &e) {
		LogMessage(Debug, "failed to write registers: 0x%x", e.code);
		connection.RespondError(e.code);
	}
}

void GdbStub::HandleSetCurrentThread(util::Buffer &packet) {
	if(packet.ReadAvailable() < 2) {
		LogMessage(Warning, "invalid thread id");
		connection.RespondError(1);
		return;
	}

	char op;
	if(!packet.Read(op)) {
		LogMessage(Warning, "invalid H packet");
		connection.RespondError(1);
		return;
	}

	int64_t pid, thread_id;
	ReadThreadId(packet, pid, thread_id);

	Process *proc = nullptr;
	if(pid == 0) {
		if(attached_processes.begin() != attached_processes.end()) {
			proc = &attached_processes.begin()->second;
		} else {
			LogMessage(Debug, "no attached processes");
			connection.RespondError(1);
			return;
		}
	} else {
		auto i = attached_processes.find(pid);
		if(i == attached_processes.end()) {
			LogMessage(Error, "no such process with pid 0x%x", pid);
			connection.RespondError(1);
			return;
		}
		proc = &i->second;
	}

	if(thread_id == 0) {
		// pick the first thread
		current_thread = &proc->threads.begin()->second;
	} else {
		auto j = proc->threads.find(thread_id);
		if(j == proc->threads.end()) {
			LogMessage(Debug, "no such thread with tid 0x%x", thread_id);
			connection.RespondError(1);
			return;
		}
		
		current_thread = &j->second;
	}
	
	LogMessage(Debug, "selected thread for '%c': pid 0x%lx tid 0x%lx", op, proc->pid, current_thread->thread_id);
	connection.RespondOk();
}

void GdbStub::HandleReadMemory(util::Buffer &packet) {
	uint64_t address, size;
	GdbConnection::DecodeWithSeparator(address, ',', packet);
	GdbConnection::Decode(size, packet);

	if(!current_thread) {
		LogMessage(Warning, "attempted to read without selected thread");
		connection.RespondError(1);
		return;
	}

	LogMessage(Debug, "reading 0x%lx bytes from 0x%lx", size, address);

	try {
		util::Buffer response;
		std::vector<uint8_t> mem = current_thread->process.debugger.ReadMemory(address, size);
		GdbConnection::Encode(mem.data(), mem.size(), response);
		connection.Respond(response);
	} catch(ResultError &e) {
		connection.RespondError(e.code);
	}
}

void GdbStub::HandleWriteMemory(util::Buffer &packet) {
	uint64_t address, size;
	GdbConnection::DecodeWithSeparator(address, ',', packet);
	GdbConnection::DecodeWithSeparator(size, ':', packet);

	if(!current_thread) {
		LogMessage(Warning, "attempted to write without selected thread");
		connection.RespondError(1);
		return;
	}

	LogMessage(Debug, "writing 0x%lx bytes to 0x%lx", size, address);

	std::vector<uint8_t> bytes;
	GdbConnection::Decode(bytes, packet);
	if(bytes.size() != size) {
		LogMessage(Error, "size mismatch (0x%lx != 0x%lx)", bytes.size(), size);
		connection.RespondError(1);
		return;
	}

	LogMessage(Debug, "  %lx", *(uint32_t*) bytes.data());
	
	try {
		util::Buffer response;
		current_thread->process.debugger.WriteMemory(address, bytes);
		connection.RespondOk();
	} catch(ResultError &e) {
		connection.RespondError(e.code);
	}
}

void GdbStub::InitBreakpointRegs() {
	HWBreakpoint bp;
	for(int i = 0; i < 4; i++) {
		bp.id = i;
		InstallBreakpoint(bp);
		break_bps.push_back(bp);
	}
	for(int i = 0; i < 2; i++) {
		bp.id = 4 + i;
		InstallBreakpoint(bp);
		contextid_bps.push_back(bp);
	}
	for(int i = 0; i < 4; i++) {
		bp.id = 0x10 + i;
		InstallBreakpoint(bp);
		watch_bps.push_back(bp);
	}
}

void GdbStub::ClearBreakpointRegs(std::optional<uint64_t> pid) {
	ClearBreakpointRegsIn(break_bps, pid);
	ClearBreakpointRegsIn(watch_bps, pid);
	ClearBreakpointRegsIn(contextid_bps, pid);
}

void GdbStub::ClearBreakpointRegsIn(std::vector<HWBreakpoint> &bps, std::optional<uint64_t> pid) {
	for (HWBreakpoint &bp : bps) {
		if (!pid.has_value() || bp.pid == *pid) {
			bp.type = HWBreakpoint::Type::Disabled;
			InstallBreakpoint(bp);
		}
	}
}

void GdbStub::HandleBreakpointCommand(util::Buffer &packet, BreakpointCommand command) {
	uint64_t type_raw;
	HWBreakpoint match;
	GdbConnection::DecodeWithSeparator(type_raw, ',', packet);
	GdbConnection::DecodeWithSeparator(match.address, ',', packet);
	GdbConnection::DecodeWithSeparator(match.size, ';', packet);

	if(packet.ReadAvailable()) {
		// gdb tried to send stub-side conditions or commands; not supported
		connection.RespondError(1);
	}

	if (!current_thread) {
		LogMessage(Warning, "attempted breakpoint command without selected thread");
		connection.RespondError(1);
		return;
	}
	match.pid = current_thread->process.pid;

	switch(type_raw) {
	case 1:
		match.type = HWBreakpoint::Type::Break;
		break;
	case 2:
		match.type = HWBreakpoint::Type::WatchW;
		break;
	case 3:
		match.type = HWBreakpoint::Type::WatchR;
		break;
	case 4:
		match.type = HWBreakpoint::Type::WatchRW;
		break;
	case 0: // software breakpoint
	default:
		// "A remote target shall return an empty string for an unrecognized
		// breakpoint or watchpoint packet type."
		connection.RespondEmpty();
		return;
	}

	switch (command) {
	case BreakpointCommand::Clear:
		HandleClearBreakpoint(match);
		return;
	case BreakpointCommand::Set:
		HandleSetBreakpoint(match);
		return;
	}
}

void GdbStub::HandleSetBreakpoint(HWBreakpoint &match) {
	HWBreakpoint match_context_id_bp;
	match_context_id_bp.type = HWBreakpoint::Type::ContextID;
	match_context_id_bp.pid = match.pid;

	std::optional<HWBreakpoint *> context_id_bp = FindOrSetBreakpoint(match_context_id_bp);
	if (!context_id_bp) {
		LogMessage(Error, "Cannot set %s: out of slots for context ID breakpoint", match.TypeStr());
		connection.RespondError(1);
		return;
	}

	match.linked_contextid_bp_id = (*context_id_bp)->id;

	if (!match.ValidateAndComputeRegs()) {
		LogMessage(Error, "Cannot set %s: invalid address/size (0x%llx, 0x%llx)",
			match.TypeStr(), (long long)match.address, (long long)match.size);
		connection.RespondError(1);
	} else if (!FindOrSetBreakpoint(match)) {
		LogMessage(Error, "Cannot set %s: out of slots", match.TypeStr());
		connection.RespondError(1);
	} else {
		connection.RespondOk();
	}

	CheckContextIDRefcount(**context_id_bp);
}

void GdbStub::HandleClearBreakpoint(const HWBreakpoint &match) {
	for(HWBreakpoint &bw : BreakpointRegsForType(match.type)) {
		if(bw.Matches(match)) {
			if(bw.HasLinkedContextIDBreakpoint()) {
				HWBreakpoint &link = contextid_bps[bw.linked_contextid_bp_id - 4];
				link.refcount--;
				CheckContextIDRefcount(link);
			}
			bw.type = HWBreakpoint::Type::Disabled;
			InstallBreakpoint(bw);
		}
	}
	connection.RespondOk();
}

std::vector<GdbStub::HWBreakpoint> &GdbStub::BreakpointRegsForType(HWBreakpoint::Type type) {
	switch (type) {
	case HWBreakpoint::Type::Break:
		return break_bps;
	case HWBreakpoint::Type::WatchR:
	case HWBreakpoint::Type::WatchW:
	case HWBreakpoint::Type::WatchRW:
		return watch_bps;
	case HWBreakpoint::Type::ContextID:
		return contextid_bps;
	case HWBreakpoint::Type::Disabled:
		throw std::runtime_error("BreakpointRegsForType: invalid type");
	}
}

std::optional<GdbStub::HWBreakpoint *> GdbStub::FindOrSetBreakpoint(const HWBreakpoint &match) {
	std::vector<HWBreakpoint> &regs = BreakpointRegsForType(match.type);
	for(HWBreakpoint &bw : regs) {
		if(bw.Matches(match)) {
			return &bw;
		}
	}
	for(HWBreakpoint &bw : regs) {
		if (bw.type == HWBreakpoint::Type::Disabled) {
			bw.SetTo(match);
			InstallBreakpoint(bw);
			if(match.HasLinkedContextIDBreakpoint()) {
				contextid_bps[match.linked_contextid_bp_id - 4].refcount++;
			}
			return &bw;
		}
	}
	return std::nullopt;
}

void GdbStub::CheckContextIDRefcount(HWBreakpoint &bw) {
	if(bw.refcount == 0) {
		bw.type = HWBreakpoint::Type::Disabled;
		InstallBreakpoint(bw);
	}
}

void GdbStub::InstallBreakpoint(HWBreakpoint &bw) {
	if (!bw.ValidateAndComputeRegs()) {
		throw std::runtime_error("InstallBreakpoint: invalid breakpoint");
	}
	if(bw.type != HWBreakpoint::Type::ContextID) {
		LogMessage(Debug, "SetHardwareBreakPoint(%x, %x, %lx)", bw.id, bw.cr, bw.vr);
		itdi.SetHardwareBreakPoint(bw.id, bw.cr, bw.vr);
	} else {
		auto p = attached_processes.find(bw.pid);
		if (p == attached_processes.end()) {
			throw std::runtime_error("InstallBreakpoint: no such pid");
		}
		LogMessage(Debug, "SetHardwareBreakPointContextIDR(%x, %x, pid=%lx)", bw.id, bw.cr, bw.pid);
		p->second.debugger.SetHardwareBreakPointContextIDR(bw.id, bw.cr);
	}
}

bool GdbStub::HWBreakpoint::ValidateAndComputeRegs() {
	switch(type) {
	case Type::Disabled:
		vr = 0;
		cr = 0;
		return true;

	case Type::Break: {
		if(size != 4 || (address & 3) || linked_contextid_bp_id < 0) {
			return false;
		}
		vr = address;
		uint32_t bt = 1; // type = linked instruction address match
		uint32_t lbn = (uint32_t)linked_contextid_bp_id;
		uint32_t bas = 0xf;
		uint32_t e = 1; // enabled
		cr = bt << 20 | lbn << 16 | bas << 5 | e << 0;
		return true;
	}

	case Type::WatchR:
	case Type::WatchW:
	case Type::WatchRW: {
		if(size <= 8) {
			// Any size <= 8 can be implemented with the BAS field as long as
			// the start and end are within the same u64.
			if((address & 7) + size > 8) {
				return false;
			}
			vr = address & ~7;
			uint32_t bas = ((1 << size) - 1) << (address & 7);
			cr = bas << 5;
		} else if (size <= 0x80000000) {
			// Larger watches up to 2GB can be implemented with MASK if the
			// size is a power of 2 and the address is aligned to that size.
			if((size & (size - 1)) != 0 ||
				(address & (size - 1)) != 0) {
				return false;
			}
			uint32_t mask = 0;
			for(uint64_t test = 1; test != size; test *= 2) {
				mask++;
			}
			cr = mask << 24;
		}
		if(linked_contextid_bp_id < 0) {
			return false;
		}
		uint32_t lbn = (uint32_t)linked_contextid_bp_id;
		uint32_t lsc =
			(type != Type::WatchW ? 1 : 0) | // load
			(type != Type::WatchR ? 2 : 0); // store
		uint32_t e = 1; // enabled
		cr |= lbn << 16 | lsc << 3 | e << 0;
		return true;
	}

	case Type::ContextID: {
		uint32_t bt = 3; // type = linked ContextID match
		uint32_t bas = 0xf;
		uint32_t e = 1; // enabled
		cr = bt << 20 | bas << 5 | e << 0;
		return true;
	}
	}
}

const char *GdbStub::HWBreakpoint::TypeStr() const {
	return type == Type::Break ? "hardware breakpoint" : "watchpoint";
}

bool GdbStub::HWBreakpoint::HasLinkedContextIDBreakpoint() const {
	return type != Type::Disabled && type != Type::ContextID;
}

bool GdbStub::HWBreakpoint::Matches(const HWBreakpoint &other) const {
	return type == other.type && pid == other.pid &&
		address == other.address && size == other.size;
}

void GdbStub::HWBreakpoint::SetTo(const HWBreakpoint &other) {
	int8_t my_id = id;
	*this = other;
	id = my_id;
}

void GdbStub::HandleVAttach(util::Buffer &packet) {
	uint64_t pid = 0;
	char ch;
	while(packet.Read(ch)) {
		pid<<= 4;
		pid|= GdbConnection::DecodeHexNybble(ch);
	}
	LogMessage(Debug, "decoded PID: 0x%lx", pid);

	if(attached_processes.find(pid) != attached_processes.end()) {
		connection.RespondError(1);
		return;
	}

	if(!multiprocess_enabled && attached_processes.size() > 0) {
		LogMessage(Error, "Already debugging a process! Make sure multiprocess extensions are enabled!");
		connection.RespondError(1);
		return;
	}

	auto r = attached_processes.emplace(pid, Process(pid, itdi.OpenActiveDebugger(pid)));
	
	r.first->second.IngestEvents(*this);

	if(r.first->second.threads.empty()) {
		// try to start it with pm:dmnt
		try {
			r.first->second.debugger.LaunchDebugProcess();
		} catch(ResultError &e) {
			LogMessage(Warning, "attached to process with no threads, and LaunchDebugProcess failed: 0x%lx", e.code);
		}
		r.first->second.IngestEvents(*this);
	}
	
	// ok
	HandleGetStopReason();
}

void GdbStub::HandleVContQuery(util::Buffer &packet) {
	util::Buffer response;
	response.Write(std::string("vCont;c;C"));
	connection.Respond(response);
}

void GdbStub::HandleVCont(util::Buffer &packet) {
	char ch;
	util::Buffer action_buffer;
	util::Buffer thread_id_buffer;
	bool reading_action = true;
	bool read_success = true;

	struct Action {
		enum class Type {
			Invalid,
			Continue,
			Step
		} type = Type::Invalid;
	};

	std::map<uint64_t, std::map<uint64_t, Action>> process_actions;
	
	while(read_success) {
		read_success = packet.Read(ch);
		if(!read_success || ch == ';') {
			if(!action_buffer.Read(ch)) {
				LogMessage(Warning, "invalid vCont action: too small");
				connection.RespondError(1);
				return;
			}
			
			Action action;
			switch(ch) {
			case 'C':
				LogMessage(Warning, "vCont 'C' action not well supported");
				// fall-through
			case 'c':
				action.type = Action::Type::Continue;
				break;
			case 'S':
				LogMessage(Warning, "vCont 'S' action not well supported");
				// fall-through
			case 's':
				action.type = Action::Type::Step;
				break;
			default:
				LogMessage(Warning, "unsupported vCont action: %c", ch);
			}

			if(action.type != Action::Type::Invalid) {
				int64_t pid = -1;
				int64_t thread_id = -1;
				if(thread_id_buffer.ReadAvailable()) {
					ReadThreadId(thread_id_buffer, pid, thread_id);
				}
				LogMessage(Debug, "vCont %ld, %ld action %c\n", pid, thread_id, ch);
				if(pid == -1) {
					for(auto &p : attached_processes) {
						std::map<uint64_t, Action> &thread_actions = process_actions.insert({p.first, {}}).first->second;
						for(auto &t : p.second.threads) {
							thread_actions.insert({t.first, action});
						}
					}
				} else {
					auto p = attached_processes.find(pid);
					if(p != attached_processes.end()) {
						std::map<uint64_t, Action> &thread_actions = process_actions.insert({p->first, {}}).first->second;
						if(thread_id == -1) {
							for(auto &t : p->second.threads) {
								thread_actions.insert({t.first, action});
							}
						} else {
							auto t = p->second.threads.find(thread_id);
							if(t != p->second.threads.end()) {
								thread_actions.insert({t->first, action});
							}
						}
					}
				}
			}
			
			reading_action = true;
			action_buffer.Clear();
			thread_id_buffer.Clear();
			continue;
		}
		if(ch == ':') {
			reading_action = false;
			continue;
		}
		if(reading_action) {
			action_buffer.Write(ch);
		} else {
			thread_id_buffer.Write(ch);
		}
	}

	run_state = RunState::Running;

	for(auto p : process_actions) {
		auto p_i = attached_processes.find(p.first);
		if(p_i == attached_processes.end()) {
			LogMessage(Warning, "no such process: 0x%lx", p.first);
			continue;
		}
		Process &proc = p_i->second;
		LogMessage(Debug, "ingesting process events before continue...");
		if(proc.IngestEvents(*this)) {
			LogMessage(Debug, "  stopped");
			RequestStop();
			return;
		}

		proc.running_thread_ids.clear();
		for(auto &t : p.second) {
			auto t_i = proc.threads.find(t.first);
			if(t_i == proc.threads.end()) {
				LogMessage(Warning, "no such thread: 0x%lx", t.first);
				continue;
			}
			bool step = t.second.type == Action::Type::Step;
			proc.running_thread_ids.push_back(ITwibDebugger::ThreadToContinue {
				t.first, step
			});
		}
		LogMessage(Debug, "continuing process");
		for(auto &t : proc.running_thread_ids) {
			LogMessage(Debug, "  tid 0x%lx", t.thread_id);
		}
		proc.debugger.ContinueDebugEvent(7, proc.running_thread_ids);
		proc.running = true;
	}

	xfer_libraries.InvalidateCache();
	xfer_memory_map.InvalidateCache();

	LogMessage(Debug, "reached end of vCont");
}

void GdbStub::QueryGetSupported(util::Buffer &packet) {
	util::Buffer response;

	while(packet.ReadAvailable()) {
		char ch;
		std::string feature;
		while(packet.Read(ch) && ch != ';') {
			feature.push_back(ch);
		}

		if(feature == "multiprocess+") {
			multiprocess_enabled = true;
		}
		
		LogMessage(Debug, "gdb advertises feature: '%s'", feature.c_str());
	}

	LogMessage(Debug, "gdb multiprocess: %s", multiprocess_enabled ? "true" : "false");

	bool is_first = true;
	for(std::string &feature : features) {
		if(!is_first) {
			response.Write(';');
		} else {
			is_first = false;
		}
		
		response.Write(feature);
	}
	
	connection.Respond(response);
}

void GdbStub::QueryGetCurrentThread(util::Buffer &packet) {
	util::Buffer response;
	if(multiprocess_enabled) {
		response.Write('p');
		GdbConnection::Encode(current_thread ? current_thread->process.pid : 0, 0, response);
		response.Write('.');
	}
	GdbConnection::Encode(current_thread ? current_thread->thread_id : 0, 0, response);
	connection.Respond(response);
}

void GdbStub::QueryGetFThreadInfo(util::Buffer &packet) {
	get_thread_info.process_iterator = attached_processes.begin();
	get_thread_info.thread_iterator = get_thread_info.process_iterator->second.threads.begin();
	get_thread_info.valid = true;
	QueryGetSThreadInfo(packet);
}

void GdbStub::QueryGetSThreadInfo(util::Buffer &packet) {
	util::Buffer response;
	if(!get_thread_info.valid) {
		LogMessage(Warning, "get_thread_info iterators invalidated");
		connection.RespondError(1);
		return;
	}
	
	bool has_written = false;
	for(;
			get_thread_info.process_iterator != attached_processes.end();
			get_thread_info.process_iterator++,
				get_thread_info.thread_iterator = get_thread_info.process_iterator->second.threads.begin()) {
		for(;
				get_thread_info.thread_iterator != get_thread_info.process_iterator->second.threads.end();
				get_thread_info.thread_iterator++) {
			Thread &thread = get_thread_info.thread_iterator->second;
			if(has_written) {
				response.Write(',');
			} else {
				response.Write('m');
			}
			if(multiprocess_enabled) {
				response.Write('p');
				GdbConnection::Encode(thread.process.pid, 0, response);
				response.Write('.');
			}
			GdbConnection::Encode(thread.thread_id, 0, response);
			has_written = true;
		}
	}
	if(!has_written) {
		get_thread_info.valid = false;
		response.Write('l'); // end of list
	}
	connection.Respond(response);
}

void GdbStub::QueryGetThreadExtraInfo(util::Buffer &packet) {
	int64_t pid, thread_id;
	ReadThreadId(packet, pid, thread_id);

	auto i = attached_processes.find(pid);
	if(i == attached_processes.end()) {
		LogMessage(Debug, "no such process with pid 0x%x", pid);
		connection.RespondError(1);
		return;
	}

	Process &p = i->second;
	
	auto j = p.threads.find(thread_id);
	if(j == p.threads.end()) {
		LogMessage(Debug, "no such thread with tid 0x%x", thread_id);
		connection.RespondError(1);
		return;
	}

	Thread &t = j->second;

	std::string extra_info;

	try {
		std::vector<uint8_t> tls_ctx_ptr_u8 = p.debugger.ReadMemory(t.tls_addr + 0x1f8, 8);
		uint64_t tls_ctx_addr = *(uint64_t*) tls_ctx_ptr_u8.data();
		std::vector<uint8_t> name_ptr_u8 = p.debugger.ReadMemory(tls_ctx_addr + 0x1a8, 8);
		uint64_t name_addr = *(uint64_t*) name_ptr_u8.data();
	
		if(name_addr != 0) {
			std::vector<uint8_t> name = p.debugger.ReadMemory(name_addr, 0x40);
			for(size_t i = 0; i < name.size(); i++) {
				if(name[i] == 0) {
					break;
				}
				extra_info.push_back(name[i]);
				if(i == name.size()-1) {
					name_addr+= name.size();
					name = p.debugger.ReadMemory(name_addr, 0x40);
					i = 0;
				}
			}
			extra_info = (char*) name.data();
		}
	} catch(ResultError &e) {
		LogMessage(Warning, "caught 0x%x reading thread name", e.code);
	}

	if(extra_info.empty()) {
		extra_info = "?";
	}
	
	util::Buffer response;
	GdbConnection::Encode(extra_info, response);
	connection.Respond(response);
}

void GdbStub::QueryGetOffsets(util::Buffer &packet) {
	if(current_thread == nullptr) {
		connection.RespondError(1);
		return;
	}
	
	uint64_t addr = current_thread->process.debugger.GetTargetEntry();
	
	util::Buffer response;
	response.Write("TextSeg=");
	GdbConnection::Encode(addr, 8, response);
	connection.Respond(response);
}

void GdbStub::QueryGetRemoteCommand(util::Buffer &packet) {
	util::Buffer message;
	GdbConnection::Decode(message, packet);
	
	std::string command;
	char ch;
	while(message.Read(ch) && ch != ' ') {
		command.push_back(ch);
	}

	std::stringstream response;
	try {
		if(command == "help") {
			response << "Available commands:" << std::endl;
			response << "  - wait application" << std::endl;
			response << "  - wait title <title id>" << std::endl;
		} else if(command == "wait") {
			std::string wait_for;
			while(message.Read(ch) && ch != ' ') {
				wait_for.push_back(ch);
			}
			if(wait_for == "application") {
				if(message.ReadAvailable()) {
					response << "Syntax error: expected end of input" << std::endl;
				} else {
					uint64_t pid = itdi.WaitToDebugApplication();
					response << "PID: 0x" << std::hex << pid << std::endl;
				}
			} else {
				std::string tid_str;
				while(message.Read(ch) && ch != ' ') {
					tid_str.push_back(ch);
				}
				if(message.ReadAvailable()) {
					response << "Syntax error: expected end of input" << std::endl;
				} else {
					uint64_t tid = std::stoull(tid_str, 0, 16);
					uint64_t pid = itdi.WaitToDebugTitle(tid);
					response << "PID: 0x" << std::hex << pid << std::endl;
				}
			}
		} else {
			response << "Unknown command '" << command << "'" << std::endl;
		}
	} catch(ResultError &e) {
		response = std::stringstream();
		response << "Target error: 0x" << std::hex << e.code << std::endl;
	} catch(std::invalid_argument &e) {
		response = std::stringstream();
		response << "Invalid argument." << std::endl;
	}
	util::Buffer response_buffer;
	GdbConnection::Encode(response.str(), response_buffer);
	connection.Respond(response_buffer);
}

void GdbStub::QueryXfer(util::Buffer &packet) {
	std::string object_name;
	std::string op;
	
	char ch;
	while(packet.Read(ch) && ch != ':') {
		object_name.push_back(ch);
	}

	auto i = xfer_objects.find(object_name);
	if(i == xfer_objects.end()) {
		connection.RespondEmpty();
		return;
	}

	while(packet.Read(ch) && ch != ':') {
		op.push_back(ch);
	}

	if(op == "read") {
		std::string annex;
		uint64_t offset;
		uint64_t length;
		while(packet.Read(ch) && ch != ':') {
			annex.push_back(ch);
		}
		GdbConnection::DecodeWithSeparator(offset, ',', packet);
		GdbConnection::Decode(length, packet);

		i->second.Read(annex, offset, length);
	} else if(op == "write") {
		std::string annex;
		uint64_t offset;
		while(packet.Read(ch) && ch != ':') {
			annex.push_back(ch);
		}
		GdbConnection::DecodeWithSeparator(offset, ':', packet);

		i->second.Write(annex, offset, packet);
	} else {
		connection.RespondEmpty();
	}
}

void GdbStub::QuerySetStartNoAckMode(util::Buffer &packet) {
	connection.StartNoAckMode();
	connection.RespondOk();
}

void GdbStub::QuerySetThreadEvents(util::Buffer &packet) {
	char c;
	if(!packet.Read(c)) {
		connection.RespondError(1);
		return;
	}
	if(c == '0') {
		thread_events_enabled = false;
	} else if(c == '1') {
		thread_events_enabled = true;
	} else {
		connection.RespondError(1);
		return;
	}
	connection.RespondOk();
}

bool GdbStub::Process::IngestEvents(GdbStub &stub) {
	std::unique_lock<std::mutex> lock(async_wait_state->mutex);
	if(!async_wait_state->has_events) {
		return false;
	}

	std::optional<nx::DebugEvent> event;

	bool was_running = running;
	
	char style = 'T';
	int signal = 0;
	uint64_t thread_id = 0;
	util::Buffer stop_info;
	std::optional<uint64_t> watchpoint_hit_addr = std::nullopt;
	bool stopped = false;
	
	while(!stopped && (event = debugger.GetDebugEvent())) {
		LogMessage(Debug, "got event: %d thread_id: %llx", event->event_type, event->thread_id);

		running = false;
		
		thread_id = event->thread_id;
		
		switch(event->event_type) {
		case nx::DebugEvent::EventType::AttachProcess: {
			break; }
		case nx::DebugEvent::EventType::AttachThread: {
			thread_id = event->attach_thread.thread_id;
			LogMessage(Debug, "  attaching new thread: 0x%x", thread_id);
			running_thread_ids.push_back(ITwibDebugger::ThreadToContinue {
				thread_id, false
			}); // autocontinue
			auto r = threads.emplace(thread_id, Thread(*this, thread_id, event->attach_thread.tls_pointer));

			stub.get_thread_info.valid = false;
			
			if(stub.thread_events_enabled) {
				signal = 5;
				stop_info.Write("create");
				stopped = true;
			}
			break; }
		case nx::DebugEvent::EventType::ExitProcess: {
			LogMessage(Warning, "process exited");
			style = 'W';
			signal = 0;
			stopped = true;
			break; }
		case nx::DebugEvent::EventType::ExitThread: {
			LogMessage(Warning, "thread exited");

			auto i = threads.find(thread_id);
			if(i != threads.end()) {
				if(stub.current_thread == &i->second) {
					stub.current_thread = nullptr;
				}
				threads.erase(i);
				running_thread_ids.erase(
					std::remove_if(
						running_thread_ids.begin(),
						running_thread_ids.end(),
						[&](const ITwibDebugger::ThreadToContinue &t) {
							return t.thread_id == thread_id;
						}), running_thread_ids.end());
				stub.get_thread_info.valid = false;
			} else {
				LogMessage(Warning, "  no such thread 0x%x", thread_id);
			}
			
			if(stub.thread_events_enabled) {
				style = 'w';
				signal = 0;
				stopped = true;
			}
			break; }
		case nx::DebugEvent::EventType::Exception: {
			LogMessage(Warning, "hit exception");
			stopped = true;
			switch(event->exception.exception_type) {
			case nx::DebugEvent::ExceptionType::Trap:
				LogMessage(Warning, "trap");
				signal = 5; // SIGTRAP
				break;
			case nx::DebugEvent::ExceptionType::InstructionAbort:
				LogMessage(Warning, "instruction abort");
				signal = 145; // EXC_BAD_ACCESS
				break;
			case nx::DebugEvent::ExceptionType::DataAbortMisc:
				LogMessage(Warning, "data abort misc");
				signal = 11; // SIGSEGV
				break;
			case nx::DebugEvent::ExceptionType::PcSpAlignmentFault:
				LogMessage(Warning, "pc sp alignment fault");
				signal = 145; // EXC_BAD_ACCESS
				break;
			case nx::DebugEvent::ExceptionType::DebuggerAttached:
				LogMessage(Warning, "debugger attached");
				signal = 0; // no signal
				break;
			case nx::DebugEvent::ExceptionType::BreakPoint:
				LogMessage(Warning, "breakpoint");
				signal = 5; // SIGTRAP
				if(event->exception.breakpoint.is_watchpoint) {
					watchpoint_hit_addr = event->exception.fault_register;
				}
				break;
			case nx::DebugEvent::ExceptionType::UserBreak:
				LogMessage(Warning, "user break");
				signal = 149; // EXC_SOFTWARE
				break;
			case nx::DebugEvent::ExceptionType::DebuggerBreak:
				LogMessage(Warning, "debugger break");
				signal = 2; // SIGINT
				break;
			case nx::DebugEvent::ExceptionType::BadSvcId:
				LogMessage(Warning, "bad svc id");
				signal = 12; // SIGSYS
				break;
			case nx::DebugEvent::ExceptionType::SError:
				LogMessage(Warning, "SError");
				signal = 10; // SIGBUS
				break;
			}
			break; }
		}
	}

	if(stopped) {
		util::Buffer stop_reason;
		if(style == 'T') { // signal
			stop_reason.Write('T');
			GdbConnection::Encode(signal, 1, stop_reason);

			if(thread_id) {
				stop_reason.Write("thread:p");
				GdbConnection::Encode(pid, 0, stop_reason);
				stop_reason.Write('.');
				GdbConnection::Encode(thread_id, 0, stop_reason);
				stop_reason.Write(';');

				auto i = threads.find(thread_id);
				if(i != threads.end()) {
					stub.current_thread = &i->second;
				} else {
					stub.current_thread = nullptr;
				}
			}
			if(watchpoint_hit_addr) {
				stop_reason.Write("watch:");
				GdbConnection::Encode(*watchpoint_hit_addr, 8, stop_reason);
				stop_reason.Write(';');
			}
		} else if(style == 'W') { // process exit
			stop_reason.Write('W');
			GdbConnection::Encode(signal, 1, stop_reason);
		} else if(style == 'w') { // thread exit
			stop_reason.Write('w');
			GdbConnection::Encode(signal, 1, stop_reason);
			stop_reason.Write(";p");
			GdbConnection::Encode(pid, 0, stop_reason);
			stop_reason.Write('.');
			GdbConnection::Encode(thread_id, 0, stop_reason);
		} else {
			LogMessage(Warning, "invalid stop reason style: '%c'", style);
			stop_reason.Write("T05");
		}
		stub.stop_reason = stop_reason.GetString();
		LogMessage(Debug, "set stop reason: \"%s\"", stub.stop_reason.c_str());
	}

	debugger.AsyncWait(
		[&stub, async_wait_state=async_wait_state](uint32_t r) {
			// TODO: ref stub
			std::unique_lock<std::mutex> lock(async_wait_state->mutex);
			if(r == 0) {
				LogMessage(Debug, "process got event signal");
				async_wait_state->has_events = true;
				stub.loop.GetNotifier().Notify();
			} else {
				LogMessage(Error, "process got error signal");
				async_wait_state->has_events = true;
			}
		});
	async_wait_state->has_events = false;

	if(was_running && !running && !stopped) { // if we're not running but we should be...
		LogMessage(Debug, "got debug events but didn't stop, so continuing...");
		debugger.ContinueDebugEvent(7, running_thread_ids);
		running = true;
	}

	return stopped;
}

std::string GdbStub::Process::BuildLibraryList() {
	std::stringstream ss;
	ss << "<library-list>" << std::endl;
	util::Buffer build_id_buffer;
	std::vector<nx::LoadedModuleInfo> nsos = debugger.GetNsoInfos();
	for(size_t i = 0; i < nsos.size(); i++) {
		// skip main
		//if(nsos.size() == 1) { continue; } // standalone main
		//if(nsos.size() >= 2 && i == 1) { continue; } // rtld, main, subsdks, etc.

		nx::LoadedModuleInfo &info = nsos[i];
		
		build_id_buffer.Clear();
		GdbConnection::Encode(info.build_id, sizeof(info.build_id), build_id_buffer);
		std::string build_id = build_id_buffer.GetString();

		ss << "  <library";
		ss << " name=\"" << build_id << "\"";
		ss << " build_id=\"" << build_id << "\"";
		ss << " type=\"nso\"";
		ss << ">" << std::endl;
		ss << "    <segment address=\"0x" << std::hex << info.base_addr << "\" />" << std::endl;
		ss << "  </library>" << std::endl;
	}
	/*
	for(nx::LoadedModuleInfo &info : debugger.GetNroInfos()) {
		build_id_buffer.Clear();
		GdbConnection::Encode(info.build_id, sizeof(info.build_id), build_id_buffer);
		std::string build_id = build_id_buffer.GetString();

		ss << "  <library";
		ss << " name=\"" << build_id << "\"";
		ss << " build_id=\"" << build_id << "\"";
		ss << " type=\"nro\"";
		ss << ">" << std::endl;
		ss << "    <segment address=\"0x" << std::hex << info.base_addr << "\" />" << std::endl;
		ss << "  </library>" << std::endl;
		}*/
	ss << "</library-list>" << std::endl;

	return ss.str();
}

std::string GdbStub::Process::BuildMemoryMap() {
	LogMessage(Info, "BuildMemoryMap");
	std::stringstream ss;
	ss << "<memory-map>" << std::endl;

	// Group memory into chunks having the same writability, even if other
	// attributes (that we don't report) are different.
	// This reduces the number of memory regions we have to report (which is
	// already rather high).

	struct Chunk {
		bool writable;
		uint64_t start_addr;
		uint64_t end_addr;
	};
	std::optional<Chunk> chunk;
	auto flush = [&]() {
		ss << "<memory "
			"type=\"" << (chunk->writable ? "ram" : "ram") << "\" " // XXX
			"start=\"0x" << std::hex << chunk->start_addr << "\" "
			"length=\"0x" << std::hex << (chunk->end_addr - chunk->start_addr) << "\" "
			"/>" << std::endl;
		chunk = std::nullopt;
	};
	for(nx::MemoryInfo mi : debugger.QueryAllMemory()) {
		bool writable = mi.permission & 2;
		if(chunk &&
		   (mi.base_addr > chunk->end_addr ||
		    writable != chunk->writable)) {
			flush();
		}
		if(!chunk) {
			chunk = Chunk{writable, mi.base_addr, 0};
		}
		chunk->end_addr = mi.base_addr + mi.size;
	}
	if(chunk) {
		flush();
	}

	ss << "</memory-map>" << std::endl;
	return ss.str();
}

GdbStub::Thread::Thread(Process &process, uint64_t thread_id, uint64_t tls_addr) : process(process), thread_id(thread_id), tls_addr(tls_addr) {
}

std::vector<uint64_t> GdbStub::Thread::GetRegisters() {
	return process.debugger.GetThreadContext(thread_id);
}

void GdbStub::Thread::SetRegisters(std::vector<uint64_t> registers) {
	return process.debugger.SetThreadContext(thread_id, registers);
}

GdbStub::Process::Process(uint64_t pid, ITwibDebugger debugger) : pid(pid), debugger(debugger) {
	async_wait_state = std::make_shared<AsyncWaitState>();
}

GdbStub::Logic::Logic(GdbStub &stub) : stub(stub) {
}

void GdbStub::Logic::Prepare(platform::EventLoop &loop) {
	LogMessage(Debug, "Logic::Prepare");
	loop.Clear();
	if(stub.run_state != RunState::Stopped) {
		for(auto &p : stub.attached_processes) {
			if(p.second.IngestEvents(stub)) {
				LogMessage(Debug, "stopping due to received event");
				stub.RequestStop();
				break;
			}
		}
	}
	while(1) {
		if(stub.run_state == RunState::Running && stub.queued_interrupts) {
			// "Interrupts received while the program is stopped are queued and the
			// program will be interrupted when it is resumed next time."
			// https://sourceware.org/gdb/onlinedocs/gdb/Interrupts.html
			stub.RequestStop();
			stub.queued_interrupts--;
			continue;
		}

		if(stub.run_state == RunState::WaitingForStop) {
			LogMessage(Debug, "Logic::Prepare checking for stopped processes");
			for(auto &p : stub.attached_processes) {
				if(p.second.running) {
					// We sent BreakProcess but haven't gotten the corresponding
					// event(s) yet.  Once we have, we will get notified; wait
					// until then (with the loop still cleared).
					LogMessage(Debug, "Logic::Prepare returning early");
					return;
				}
			}
			LogMessage(Debug, "Logic::Prepare actually stopping");
			// All processes have stopped.
			stub.run_state = RunState::Stopped;
			stub.HandleGetStopReason(); // send reason
			// TODO: this can drop events if multiple processes stop at the same time
		}

		// If we get here, we're either running or fully stopped.
		bool interrupted;
		util::Buffer *buffer = stub.connection.Process(interrupted);
		if(interrupted) {
			stub.queued_interrupts++;
			continue;
		}

		if(!buffer) {
			break;
		}

		LogMessage(Debug, "got message (0x%lx bytes)", buffer->ReadAvailable());
		char ident;
		if(!buffer->Read(ident)) {
			LogMessage(Error, "invalid packet (zero-length?)");
			stub.connection.SignalError();
			return;
		}
		LogMessage(Debug, "got packet, ident: %c", ident);
		if(stub.run_state != RunState::Stopped) {
			LogMessage(Error, "unexpectedly received message while not stopped; dropping!");
			continue;
		}

		switch(ident) {
		case '!': // extended mode
			stub.connection.RespondOk();
			break;
		case '?': // stop reason
			stub.HandleGetStopReason();
			break;
		case 'D': // detach
			stub.HandleDetach(*buffer);
			break;
		case 'g': // read general registers
			stub.HandleReadGeneralRegisters();
			break;
		case 'G': // write general registers
			stub.HandleWriteGeneralRegisters(*buffer);
			break;
		case 'H': // set current thread
			stub.HandleSetCurrentThread(*buffer);
			break;
		case 'm': // read memory
			stub.HandleReadMemory(*buffer);
			break;
		case 'M': // write memory
			stub.HandleWriteMemory(*buffer);
			break;
		case 'q': // general get query
			stub.HandleGeneralGetQuery(*buffer);
			break;
		case 'Q': // general set query
			stub.HandleGeneralSetQuery(*buffer);
			break;
		case 'T': // is thread alive
			stub.HandleIsThreadAlive(*buffer);
			break;
		case 'v': // variable
			stub.HandleMultiletterPacket(*buffer);
			break;
		case 'Z': // set breakpoint
			stub.HandleBreakpointCommand(*buffer, BreakpointCommand::Set);
			break;
		case 'z': // clear breakpoint
			stub.HandleBreakpointCommand(*buffer, BreakpointCommand::Clear);
			break;
		default:
			LogMessage(Info, "unrecognized packet: %c", ident);
			stub.connection.RespondEmpty();
			break;
		}
	}
	LogMessage(Debug, "Logic::Prepare returning normally");
	loop.AddMember(stub.connection.in_member);
}

void GdbStub::RequestStop() {
	if(run_state == RunState::Running) {
		run_state = RunState::WaitingForStop;
		for(auto &p : attached_processes) {
			if(p.second.running) {
				p.second.debugger.BreakProcess();
			}
		}
	}
}

bool GdbStub::XferObject::AdvertiseRead() {
	return false;
}

bool GdbStub::XferObject::AdvertiseWrite() {
	return false;
}

GdbStub::ReadOnlyStringXferObject::ReadOnlyStringXferObject(GdbStub &stub, std::string (GdbStub::*generator)()) : stub(stub), generator(generator) {
}


void GdbStub::ReadOnlyStringXferObject::InvalidateCache() {
	cache = std::nullopt;
}

void GdbStub::ReadOnlyStringXferObject::Read(std::string annex, size_t offset, size_t length) {
	if(!annex.empty()) {
		stub.connection.RespondError(0);
		return;
	}

	if(!cache) {
		cache = std::invoke(generator, stub);
	}
	std::string &string = *cache;

	util::Buffer response;
	if(offset + length >= string.size()) {
		response.Write('l');
	} else {
		response.Write('m');
	}

	response.Write((uint8_t*) string.data() + offset, std::min(string.size() - offset, length));
	stub.connection.Respond(response);
}

void GdbStub::ReadOnlyStringXferObject::Write(std::string annex, size_t offst, util::Buffer &data) {
	stub.connection.RespondError(30); // EROFS
}

bool GdbStub::ReadOnlyStringXferObject::AdvertiseRead() {
	return true;
}

std::string GdbStub::XferReadLibraries() {
	if(current_thread == nullptr) {
		return "<library-list></library-list>";
	} else {
		return current_thread->process.BuildLibraryList();
	}
}

std::string GdbStub::XferReadMemoryMap() {
	if(current_thread == nullptr) {
		return "<memory-map></memory-map>";
	} else {
		return current_thread->process.BuildMemoryMap();
	}
}

} // namespace gdb
} // namespace tool
} // namespace twib
} // namespace twili
