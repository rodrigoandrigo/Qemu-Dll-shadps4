#pragma once

#include <cstddef>
#include <cstdint>

bool shadps4_gcn_bridge_read_memory(uint64_t address, void *data,
                                    size_t size);
