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

#pragma once

#include<libtransistor/cpp/types.hpp>

#include<variant>
#include<functional>

#include "Buffer.hpp"

namespace twili {

class TwibPipe {
 public:
	TwibPipe(size_t buffer_limit);
	~TwibPipe();
	
	// Callback returns how much data was read.
	// Callback may not call Read or Write.
	void Read(std::function<size_t(uint8_t *data, size_t actual_size)> cb);
	void Write(uint8_t *data, size_t size, std::function<void(bool eof)> cb);
	void CloseReader();
	void CloseWriter();

	void PrintDebugInfo(const char *indent);

	bool IsWriterClosed();
 private:
	struct IdleState {
	};
	
	struct WritePendingState {
		WritePendingState(uint8_t *data, size_t size, std::function<void(bool eof)> cb);

		void Read(std::function<size_t(uint8_t *data, size_t actual_size)> read_cb);
		
		uint8_t *data;
		size_t size;
		std::function<void(bool eof)> cb;
	};
	
	struct ReadPendingState {
		ReadPendingState(std::function<size_t(uint8_t *data, size_t actual_size)> cb);
		
		std::function<size_t(uint8_t *data, size_t actual_size)> cb;
	};
	
	using state_variant = std::variant<IdleState, WritePendingState, ReadPendingState>;
	state_variant state;
	bool hit_eof = false;
	
	static const char *StateName(state_variant &v);
	
	// Try to flush WPS to buffer. If we flush the whole thing, exit it.
	bool FlushWritePendingState(WritePendingState &wps);
	
	// Exit WPS.
	void ExitWritePendingState(WritePendingState &wps);
	
	util::Buffer buffer;
};

}
