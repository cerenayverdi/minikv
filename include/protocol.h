#pragma once

#include <string>
#include <string_view>

#include "kv_store.h"

namespace minikv {

// Parses one command line (without the trailing '\n'), runs it against the
// store and returns the response line (including the trailing '\n').
//
// Commands:  PING | SET <key> <value> | GET <key> | DEL <key> | SIZE
// Responses: PONG | OK | VALUE <value> | NOT_FOUND | DELETED | SIZE <n> | ERR <msg>
std::string handle_command(std::string_view line, KVStore& store);

}  // namespace minikv
