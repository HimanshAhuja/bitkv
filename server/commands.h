// Maps parsed RESP commands onto DB calls. Kept separate from the network
// loop so every command can be unit-tested without opening a socket.
#pragma once

#include <string>
#include <vector>

#include "bitkv/db.h"

namespace bitkv {

// Executes one command and appends the RESP reply to *out. Sets *quit when
// the client asked to close the connection.
void ExecuteCommand(DB* db, const std::vector<std::string>& args, std::string* out,
                    bool* quit);

}  // namespace bitkv
