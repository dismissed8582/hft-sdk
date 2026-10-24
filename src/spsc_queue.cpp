#include "hft/SPSCQueue.hpp"

namespace hft {

template class SPSCQueue<MarketDataMessage, 65536>;

SPSCQueue<MarketDataMessage, 65536> g_market_data_queue;

} // namespace hft
