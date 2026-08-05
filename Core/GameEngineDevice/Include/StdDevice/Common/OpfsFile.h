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

///// OpfsFile.h ///////////////////////////////
// GeneralsX @build dx8wasm - read-only File served from the browser's Origin Private
// File System on demand, instead of from a copy held in the wasm heap.
//
// Why: the shrunk asset set is ~1.3 GiB of .big archives, and holding all of it resident
// for the whole session is the last structural reason a phone cannot run this build. The
// archives are already in OPFS (web/byo-assets.js puts them there); this class lets the
// engine read them there rather than copying them into MEMFS first.
//
// It subclasses LocalFile rather than File because File has ~15 pure virtuals, most of
// which LocalFile already gives sensible shapes. But note what LocalFile does NOT give:
// readChar/readWideChar/scanInt/scanReal/scanString/nextLine read the private C file
// handle directly (_read/fread), not through read(). Inheriting those would mean silently
// reading from a handle that was never opened — an EOF that looks like an empty file. So
// they are overridden here too, against a small lookahead buffer, which is also what keeps
// them from costing one cross-thread round trip per byte.
//////////////////////////////////////////////////

#pragma once

#include "Common/LocalFile.h"

class OpfsFile : public LocalFile
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(OpfsFile, "OpfsFile")
public:
	OpfsFile();

	// Bind to a registered archive by base name. FALSE if it is not registered, or if write
	// access was requested (archives are read-only). No file descriptor is ever opened.
	Bool openOpfs(const Char* name, Int access);

	virtual Int  read(void* buffer, Int bytes) override;
	virtual Int  seek(Int bytes, seekMode mode = CURRENT) override;
	virtual Int  write(const void* buffer, Int bytes) override;
	virtual Bool flush() override;
	virtual void close() override;
	virtual Int  size() override;
	virtual Int  position() override;

	// Text primitives, re-expressed against read()/seek() — see the note above.
	virtual Int  readChar() override;
	virtual Int  readWideChar() override;
	virtual Bool scanInt(Int& newInt) override;
	virtual Bool scanReal(Real& newReal) override;
	virtual Bool scanString(AsciiString& newString) override;
	virtual void nextLine(Char* buf = nullptr, Int bufSize = 0) override;

protected:
	// One byte at the current position, or -1 at end of archive. Served from m_buf, which is
	// refilled in LOOKAHEAD-sized ranged reads.
	Int  readByte();
	Bool fill(Int pos);             ///< make the lookahead buffer cover `pos`
	void dropLookahead();

	enum { LOOKAHEAD = 4096 };

	Int  m_idx;                     ///< index into the dx8wasm archive registry, -1 if unbound
	Int  m_size;                    ///< archive size in bytes
	Int  m_pos;                     ///< logical read position
	Int  m_bufAt;                   ///< archive offset the lookahead buffer starts at
	Int  m_bufLen;                  ///< valid bytes in the lookahead buffer
	unsigned char m_buf[LOOKAHEAD];
};
