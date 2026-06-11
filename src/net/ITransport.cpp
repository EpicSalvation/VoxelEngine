#include "net/ITransport.h"

// ITransport is a pure-virtual interface; the vtable destructor is defined here
// so that ENetTransport.cpp is the only TU that includes ENet headers.
// This TU exists so the interface is a translation unit (not header-only),
// keeping the compiler honest about the separation of transport backends.
