#pragma once

// The C++ Client behind a c_share_http_client_t, for modules whose own C API
// builds on the http module's (e.g. evm's): they take a
// c_share_http_client_t* and use the Client inside it.

#include "c_share/http/client.hpp"
#include "c_share/http/client_c.h"

namespace c_share::http {

// `client` must not be null, and outlives the returned reference.
Client& client_of(c_share_http_client_t* client);

} // namespace c_share::http
