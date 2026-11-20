#pragma once

// Windows only: puts "virtual" files on the clipboard. Explorer (and any
// target that understands CFSTR_FILEDESCRIPTORW / CFSTR_FILECONTENTS) creates
// the folders and files on paste and reads each file through an IStream,
// which pulls the bytes from a VirtualFiles::Source only then. The object
// offers IDataObjectAsyncCapability, so Explorer copies on a worker thread of
// its own and shows its usual progress dialog.
//
// Every function must be called on the thread that owns the OLE clipboard:
// the helper's GUI thread (an OLE-initialized STA). Stream reads arrive on that
// thread too, through COM, and block it while waiting for data.

#include "virtualfilesource.h"

#include <memory>
#include <vector>

namespace VirtualFileClipboard {

// Replaces the clipboard. Returns false when the clipboard could not be set.
bool set(std::vector<VirtualFiles::Entry> entries, std::shared_ptr<VirtualFiles::Source> source);

// True while the clipboard still holds the files from the last set().
bool ownsClipboard();

// Empties the clipboard if it still holds our files (session end: a later
// paste could not be served).
void clearIfOwned();

} // namespace VirtualFileClipboard
