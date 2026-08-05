/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

///// OpfsFile.cpp /////////////////////////////
// GeneralsX @build dx8wasm - see OpfsFile.h.
//////////////////////////////////////////////////

#include "StdDevice/Common/OpfsFile.h"

#include "Common/AsciiString.h"
#include "Common/GameMemory.h"

#ifdef __EMSCRIPTEN__
#include <dx8wasm/opfs.h>
#include <ctype.h>
#include <stdlib.h>
#endif

OpfsFile::OpfsFile()
	: m_idx(-1), m_size(0), m_pos(0), m_bufAt(0), m_bufLen(0)
{
}

OpfsFile::~OpfsFile()
{
	// Release state only — deliberately NOT close(), matching LocalFile/RAMFile. File::close()
	// deletes the instance when m_deleteOnClose is set, which every OpfsFile from
	// StdLocalFileSystem::openFile has, so calling it from the destructor would re-enter the
	// destructor on an object already being destroyed.
	m_idx = -1;
	dropLookahead();
}

void OpfsFile::dropLookahead()
{
	m_bufAt = 0;
	m_bufLen = 0;
}

#ifdef __EMSCRIPTEN__

Bool OpfsFile::openOpfs(const Char* name, Int access)
{
	if (name == nullptr) {
		return FALSE;
	}
	// Archives are read-only. A write request must fall through to the real filesystem
	// rather than be quietly accepted and then dropped.
	if (access & (File::WRITE | File::APPEND | File::TRUNCATE | File::CREATE | File::ONLYNEW)) {
		return FALSE;
	}
	m_idx = dx8wasm_opfs_index_of(name);
	if (m_idx < 0) {
		return FALSE;
	}
	m_size = dx8wasm_opfs_size_of(m_idx);
	if (m_size < 0) {
		m_idx = -1;
		return FALSE;
	}
	m_pos = 0;
	dropLookahead();
	// Take File's bookkeeping (name, access flags, m_open) but not LocalFile::open, which
	// would go looking for a real file descriptor.
	return File::open(name, access);
}

Int OpfsFile::read(void* buffer, Int bytes)
{
	if (!m_open || m_idx < 0 || bytes < 0) {
		return -1;
	}
	if (m_pos >= m_size) {
		return 0;
	}
	if (m_pos + bytes > m_size) {
		bytes = m_size - m_pos;
	}
	// A null buffer means "skip these bytes" in this codebase's File contract, and several
	// callers rely on it; doing a real read into nowhere would be both wrong and expensive.
	if (buffer == nullptr) {
		m_pos += bytes;
		return bytes;
	}
	const Int got = dx8wasm_opfs_read(m_idx, (uint32_t)m_pos, buffer, (uint32_t)bytes);
	if (got < 0) {
		return -1;
	}
	m_pos += got;
	return got;
}

Int OpfsFile::readByte()
{
	if (m_pos < 0 || m_pos >= m_size) {
		return -1;
	}
	if (m_pos < m_bufAt || m_pos >= m_bufAt + m_bufLen) {
		Int want = m_size - m_pos;
		if (want > LOOKAHEAD) {
			want = LOOKAHEAD;
		}
		const Int got = dx8wasm_opfs_read(m_idx, (uint32_t)m_pos, m_buf, (uint32_t)want);
		if (got <= 0) {
			dropLookahead();
			return -1;
		}
		m_bufAt = m_pos;
		m_bufLen = got;
	}
	const Int b = m_buf[m_pos - m_bufAt];
	m_pos++;
	return b;
}

#else   // native builds: nothing to bind to

Bool OpfsFile::openOpfs(const Char*, Int) { return FALSE; }
Int  OpfsFile::read(void*, Int) { return -1; }
Int  OpfsFile::readByte() { return -1; }

#endif  // __EMSCRIPTEN__

Int OpfsFile::seek(Int bytes, seekMode mode)
{
	Int p;
	switch (mode) {
		case File::START:   p = bytes; break;
		case File::CURRENT: p = m_pos + bytes; break;
		case File::END:     p = m_size + bytes; break;   // END with a negative offset, as fseek
		default:            return -1;
	}
	if (p < 0) {
		p = 0;
	}
	if (p > m_size) {
		p = m_size;
	}
	m_pos = p;
	return m_pos;
}

Int  OpfsFile::size(void) { return m_size; }
Int  OpfsFile::position(void) { return m_pos; }
Int  OpfsFile::write(const void*, Int) { return -1; }   // read-only, and says so
Bool OpfsFile::flush(void) { return TRUE; }

void OpfsFile::close(void)
{
	m_idx = -1;
	m_pos = 0;
	dropLookahead();
	// File::close(), not LocalFile::close(): there is no descriptor to close, and
	// LocalFile::close() would operate on its own uninitialised handle.
	File::close();
}

// --- text primitives, re-expressed against readByte() -----------------------------------
// These mirror LocalFile's versions byte for byte, including the "put the last character
// back" step, so a caller cannot tell the two apart. The only difference is where the bytes
// come from.

Int OpfsFile::readChar()
{
	return readByte();      // -1 at EOF, matching LocalFile
}

Int OpfsFile::readWideChar()
{
	const Int lo = readByte();
	if (lo < 0) {
		return -1;
	}
	const Int hi = readByte();
	if (hi < 0) {
		return -1;
	}
	return (Int)((unsigned short)((hi << 8) | lo));
}

Bool OpfsFile::scanInt(Int& newInt)
{
	newInt = 0;
	AsciiString tempstr;
	Int c;

	do {
		c = readByte();
	} while ((c >= 0) && (((c < '0') || (c > '9')) && (c != '-')));

	if (c < 0) {
		return FALSE;
	}

	do {
		tempstr.concat((Char)c);
		c = readByte();
	} while ((c >= 0) && (c >= '0') && (c <= '9'));

	if (c >= 0) {
		seek(-1, File::CURRENT);
	}

	newInt = atoi(tempstr.str());
	return TRUE;
}

Bool OpfsFile::scanReal(Real& newReal)
{
	newReal = 0.0;
	AsciiString tempstr;
	Int c;
	Bool sawDec = FALSE;

	do {
		c = readByte();
	} while ((c >= 0) && (((c < '0') || (c > '9')) && (c != '-') && (c != '.')));

	if (c < 0) {
		return FALSE;
	}

	do {
		tempstr.concat((Char)c);
		if (c == '.') {
			sawDec = TRUE;
		}
		c = readByte();
	} while ((c >= 0) && (((c >= '0') && (c <= '9')) || ((c == '.') && !sawDec)));

	if (c >= 0) {
		seek(-1, File::CURRENT);
	}

	newReal = (Real)atof(tempstr.str());
	return TRUE;
}

Bool OpfsFile::scanString(AsciiString& newString)
{
	Int c;
	newString.clear();

	do {
		c = readByte();
	} while ((c >= 0) && isspace(c));

	if (c < 0) {
		return FALSE;
	}

	do {
		newString.concat((Char)c);
		c = readByte();
	} while ((c >= 0) && !isspace(c));

	if (c >= 0) {
		seek(-1, File::CURRENT);
	}

	return TRUE;
}

void OpfsFile::nextLine(Char* buf, Int bufSize)
{
	Int c = 0;
	Int i = 0;

	do {
		c = readByte();
		if ((buf != nullptr) && (i < (bufSize - 1)) && (c >= 0)) {
			buf[i] = (Char)c;
		}
		++i;
	} while ((c >= 0) && (c != '\n'));

	if (buf != nullptr) {
		if (i < bufSize) {
			buf[i] = 0;
		} else {
			buf[bufSize - 1] = 0;
		}
	}
}
