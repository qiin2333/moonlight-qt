#pragma once

#include <QtGlobal>

#include "vfs_item.h"

namespace FileMapping {

struct ReadHandle {
    quint64 id = 0;
    VfsItem item;

    bool isValid() const { return id != 0 && !item.id.isEmpty(); }
};

} // namespace FileMapping
