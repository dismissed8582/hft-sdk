#include "hft/HftSdk.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cctype>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace {

using i64_array = py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>;

std::vector<hft::MarketEvent> events_from_arrays(
    const i64_array& order_id,
    const i64_array& timestamp_ns,
    const i64_array& price_ticks,
    const i64_array& quantity,
    const i64_array& bid_price_ticks,
    const i64_array& ask_price_ticks,
    const i64_array& type_codes,
    const i64_array& side_codes) {
    const py::ssize_t count = order_id.size();
    if (timestamp_ns.size() != count || price_ticks.size() != count ||
        quantity.size() != count || bid_price_ticks.size() != count ||
        ask_price_ticks.size() != count || type_codes.size() != count ||
        side_codes.size() != count) {
        throw std::runtime_error("all event arrays must have the same length");
    }

    const auto ids = order_id.unchecked<1>();
    const auto stamps = timestamp_ns.unchecked<1>();
    const auto prices = price_ticks.unchecked<1>();
    const auto sizes = quantity.unchecked<1>();
    const auto bids = bid_price_ticks.unchecked<1>();
    const auto asks = ask_price_ticks.unchecked<1>();
    const auto types = type_codes.unchecked<1>();
    const auto sides = side_codes.unchecked<1>();

    std::vector<hft::MarketEvent> events(static_cast<std::size_t>(count));
    for (py::ssize_t index = 0; index < count; ++index) {
        hft::MarketEvent& event = events[static_cast<std::size_t>(index)];
        event.order_id = static_cast<std::uint64_t>(ids(index));
        event.timestamp_ns = stamps(index);
        event.price_ticks = prices(index);
        event.quantity = sizes(index);
        event.bid_price_ticks = bids(index);
        event.ask_price_ticks = asks(index);
        const std::int64_t type = types(index);
        const std::int64_t side = sides(index);
        if (type < 0 || type > 3) {
            throw std::runtime_error("event type code must be in [0, 3]");
        }
        if (side < 0 || side > 4) {
            throw std::runtime_error("side code must be in [0, 4]");
        }
        event.type = static_cast<hft::EventType>(type);
        event.side = static_cast<hft::Side>(side);
    }
    return events;
}

i64_array series_to_i64(py::object frame, const char* primary, const char* fallback,
                        const char* kind) {
    const auto has_column = [&](const char* name) {
        return py::cast<bool>(frame.attr("__contains__")(name));
    };
    const char* selected = nullptr;
    if (has_column(primary)) {
        selected = primary;
    } else if (fallback != nullptr && has_column(fallback)) {
        selected = fallback;
    }
    if (selected == nullptr) {
        throw std::runtime_error(std::string("missing column: ") + primary);
    }

    const py::object series = frame.attr("__getitem__")(selected);
    const std::string dtype_kind = py::str(series.attr("dtype").attr("kind"));
    if (dtype_kind != "O" && dtype_kind != "U" && dtype_kind != "S") {
        return py::cast<i64_array>(series.attr("to_numpy")());
    }

    const py::list values = series.attr("tolist")();
    py::array_t<std::int64_t> result(static_cast<py::ssize_t>(values.size()));
    auto view = result.mutable_unchecked<1>();
    for (py::ssize_t index = 0; index < static_cast<py::ssize_t>(values.size()); ++index) {
        std::string text = py::str(values[index]);
        for (char& character : text) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        if (std::string(kind) == "event") {
            if (text == "NEW") view(index) = 0;
            else if (text == "MODIFY") view(index) = 1;
            else if (text == "CANCEL") view(index) = 2;
            else if (text == "TRADE") view(index) = 3;
            else throw std::runtime_error("unknown event type: " + text);
        } else {
            if (text == "NONE") view(index) = 0;
            else if (text == "BID") view(index) = 1;
            else if (text == "ASK") view(index) = 2;
            else if (text == "BUY") view(index) = 3;
            else if (text == "SELL") view(index) = 4;
            else throw std::runtime_error("unknown side: " + text);
        }
    }
    return result;
}

py::dict metrics_table(const hft::sdk::OBIFilterEngine& engine) {
    const std::vector<hft::signal::SignalMetrics> rows = engine.metrics();
    const py::ssize_t count = static_cast<py::ssize_t>(rows.size());
    py::dict table;
    py::list schemes;
    for (const auto& row : rows) {
        schemes.append(std::string(row.scheme));
    }
    table["scheme"] = schemes;

    const auto add_i32 = [&](const char* name, auto getter) {
        py::array_t<std::int32_t> values(count);
        auto view = values.mutable_unchecked<1>();
        for (py::ssize_t index = 0; index < count; ++index) {
            view(index) = static_cast<std::int32_t>(
                getter(rows[static_cast<std::size_t>(index)]));
        }
        table[name] = values;
    };
    const auto add_size = [&](const char* name, auto getter) {
        py::array_t<std::int64_t> values(count);
        auto view = values.mutable_unchecked<1>();
        for (py::ssize_t index = 0; index < count; ++index) {
            view(index) = static_cast<std::int64_t>(
                getter(rows[static_cast<std::size_t>(index)]));
        }
        table[name] = values;
    };
    const auto add_double = [&](const char* name, auto getter) {
        py::array_t<double> values(count);
        auto view = values.mutable_unchecked<1>();
        for (py::ssize_t index = 0; index < count; ++index) {
            view(index) = getter(rows[static_cast<std::size_t>(index)]);
        }
        table[name] = values;
    };

    add_i32("horizon_s", [](const auto& row) { return row.horizon_seconds; });
    add_size("samples", [](const auto& row) { return row.samples; });
    add_double("pearson", [](const auto& row) { return row.pearson.pearson; });
    add_double("r2", [](const auto& row) { return row.pearson.r2; });
    add_double("regime_pearson", [](const auto& row) { return row.regime.pearson; });
    add_double("regime_r2", [](const auto& row) { return row.regime.r2; });
    add_size("trade_samples", [](const auto& row) { return row.trade_samples; });
    add_double("trade_pearson", [](const auto& row) { return row.trade_pearson.pearson; });
    add_double("trade_r2", [](const auto& row) { return row.trade_pearson.r2; });
    add_double("trade_regime_pearson", [](const auto& row) { return row.trade_regime.pearson; });
    add_double("trade_regime_r2", [](const auto& row) { return row.trade_regime.r2; });
    add_size("volume_samples", [](const auto& row) { return row.volume_samples; });
    add_double("volume_pearson", [](const auto& row) { return row.volume_pearson.pearson; });
    add_double("volume_r2", [](const auto& row) { return row.volume_pearson.r2; });
    add_double("volume_regime_pearson", [](const auto& row) { return row.volume_regime.pearson; });
    add_double("volume_regime_r2", [](const auto& row) { return row.volume_regime.r2; });
    add_size("volume_trade_samples", [](const auto& row) { return row.volume_trade_samples; });
    add_double("volume_trade_pearson", [](const auto& row) { return row.volume_trade_pearson.pearson; });
    add_double("volume_trade_r2", [](const auto& row) { return row.volume_trade_pearson.r2; });
    add_double("volume_trade_regime_pearson", [](const auto& row) { return row.volume_trade_regime.pearson; });
    add_double("volume_trade_regime_r2", [](const auto& row) { return row.volume_trade_regime.r2; });
    return table;
}

} // namespace

PYBIND11_MODULE(hft_sdk, m) {
    m.doc() = "C++20 low-latency LOB signal SDK";

    py::enum_<hft::EventType>(m, "EventType")
        .value("New", hft::EventType::New)
        .value("Modify", hft::EventType::Modify)
        .value("Cancel", hft::EventType::Cancel)
        .value("Trade", hft::EventType::Trade)
        .export_values();
    py::enum_<hft::Side>(m, "Side")
        .value("None", hft::Side::None)
        .value("Bid", hft::Side::Bid)
        .value("Ask", hft::Side::Ask)
        .value("Buy", hft::Side::Buy)
        .value("Sell", hft::Side::Sell)
        .export_values();
    py::enum_<hft::obi::FilterScheme>(m, "FilterScheme")
        .value("Unfiltered", hft::obi::FilterScheme::Unfiltered)
        .value("Lifetime", hft::obi::FilterScheme::Lifetime)
        .value("ModificationCount", hft::obi::FilterScheme::ModificationCount)
        .value("ModificationTime", hft::obi::FilterScheme::ModificationTime)
        .export_values();
    py::enum_<hft::backtest::SignalWeight>(m, "SignalWeight")
        .value("Count", hft::backtest::SignalWeight::Count)
        .value("Volume", hft::backtest::SignalWeight::Volume)
        .export_values();

    py::class_<hft::obi::FilterThresholds>(m, "FilterThresholds")
        .def(py::init<>())
        .def_readwrite("lifetime_ns", &hft::obi::FilterThresholds::lifetime_ns)
        .def_readwrite("max_modifications", &hft::obi::FilterThresholds::max_modifications)
        .def_readwrite("min_modification_interval_ns",
                       &hft::obi::FilterThresholds::min_modification_interval_ns);

    py::class_<hft::obi::OBIValue>(m, "OBIValue")
        .def_readonly("value", &hft::obi::OBIValue::value)
        .def_readonly("valid", &hft::obi::OBIValue::valid);

    py::class_<hft::scoring::CorrelationScore>(m, "CorrelationScore")
        .def_readonly("pearson", &hft::scoring::CorrelationScore::pearson)
        .def_readonly("r2", &hft::scoring::CorrelationScore::r2)
        .def_readonly("samples", &hft::scoring::CorrelationScore::samples)
        .def_readonly("valid", &hft::scoring::CorrelationScore::valid);

    py::class_<hft::MarketEvent>(m, "MarketEvent")
        .def(py::init<>())
        .def_readwrite("order_id", &hft::MarketEvent::order_id)
        .def_readwrite("timestamp_ns", &hft::MarketEvent::timestamp_ns)
        .def_readwrite("price_ticks", &hft::MarketEvent::price_ticks)
        .def_readwrite("quantity", &hft::MarketEvent::quantity)
        .def_readwrite("bid_price_ticks", &hft::MarketEvent::bid_price_ticks)
        .def_readwrite("ask_price_ticks", &hft::MarketEvent::ask_price_ticks)
        .def_readwrite("type", &hft::MarketEvent::type)
        .def_readwrite("side", &hft::MarketEvent::side);

    py::class_<hft::signal::SignalSnapshot>(m, "SignalSnapshot")
        .def_readonly("timestamp_ns", &hft::signal::SignalSnapshot::timestamp_ns)
        .def_readonly("mid", &hft::signal::SignalSnapshot::mid)
        .def_readonly("lob_events", &hft::signal::SignalSnapshot::lob_events)
        .def_readonly("trade_events", &hft::signal::SignalSnapshot::trade_events)
        .def("lob", [](const hft::signal::SignalSnapshot& snapshot,
                       hft::obi::FilterScheme scheme, std::size_t horizon) {
            return snapshot.lob.at(static_cast<std::size_t>(scheme)).at(horizon);
        }, py::arg("scheme") = hft::obi::FilterScheme::Unfiltered, py::arg("horizon") = 0)
        .def("lob_volume", [](const hft::signal::SignalSnapshot& snapshot,
                              hft::obi::FilterScheme scheme, std::size_t horizon) {
            return snapshot.lob_volume.at(static_cast<std::size_t>(scheme)).at(horizon);
        }, py::arg("scheme") = hft::obi::FilterScheme::Unfiltered, py::arg("horizon") = 0)
        .def("trade", [](const hft::signal::SignalSnapshot& snapshot,
                         hft::obi::FilterScheme scheme, std::size_t horizon) {
            return snapshot.trade.at(static_cast<std::size_t>(scheme)).at(horizon);
        }, py::arg("scheme") = hft::obi::FilterScheme::Unfiltered, py::arg("horizon") = 0)
        .def("trade_volume", [](const hft::signal::SignalSnapshot& snapshot,
                                hft::obi::FilterScheme scheme, std::size_t horizon) {
            return snapshot.trade_volume.at(static_cast<std::size_t>(scheme)).at(horizon);
        }, py::arg("scheme") = hft::obi::FilterScheme::Unfiltered, py::arg("horizon") = 0);

    py::class_<hft::signal::SignalMetrics>(m, "SignalMetrics")
        .def_property_readonly("scheme", [](const hft::signal::SignalMetrics& row) {
            return std::string(row.scheme);
        })
        .def_readonly("horizon_s", &hft::signal::SignalMetrics::horizon_seconds)
        .def_readonly("samples", &hft::signal::SignalMetrics::samples)
        .def_readonly("trade_samples", &hft::signal::SignalMetrics::trade_samples)
        .def_readonly("volume_samples", &hft::signal::SignalMetrics::volume_samples)
        .def_readonly("volume_trade_samples", &hft::signal::SignalMetrics::volume_trade_samples)
        .def_readonly("pearson", &hft::signal::SignalMetrics::pearson)
        .def_readonly("regime", &hft::signal::SignalMetrics::regime)
        .def_readonly("trade_pearson", &hft::signal::SignalMetrics::trade_pearson)
        .def_readonly("trade_regime", &hft::signal::SignalMetrics::trade_regime)
        .def_readonly("volume_pearson", &hft::signal::SignalMetrics::volume_pearson)
        .def_readonly("volume_regime", &hft::signal::SignalMetrics::volume_regime)
        .def_readonly("volume_trade_pearson", &hft::signal::SignalMetrics::volume_trade_pearson)
        .def_readonly("volume_trade_regime", &hft::signal::SignalMetrics::volume_trade_regime);

    py::class_<hft::signal::RegimeTuple>(m, "RegimeTuple")
        .def_readonly("timestamp_ns", &hft::signal::RegimeTuple::timestamp_ns)
        .def_readonly("obi_regime", &hft::signal::RegimeTuple::obi_regime)
        .def_readonly("return_regime", &hft::signal::RegimeTuple::return_regime);

    py::class_<hft::backtest::Config>(m, "BacktestConfig")
        .def(py::init<>())
        .def_readwrite("scheme", &hft::backtest::Config::scheme)
        .def_readwrite("horizon", &hft::backtest::Config::horizon)
        .def_readwrite("weight", &hft::backtest::Config::weight)
        .def_readwrite("threshold", &hft::backtest::Config::threshold)
        .def_readwrite("maker_fee_bps", &hft::backtest::Config::maker_fee_bps)
        .def_readwrite("taker_fee_bps", &hft::backtest::Config::taker_fee_bps)
        .def_readwrite("maker", &hft::backtest::Config::maker)
        .def_readwrite("latency_ns", &hft::backtest::Config::latency_ns);

    py::class_<hft::backtest::Trade>(m, "BacktestTrade")
        .def_readonly("signal_ns", &hft::backtest::Trade::signal_ns)
        .def_readonly("entry_ns", &hft::backtest::Trade::entry_ns)
        .def_readonly("exit_ns", &hft::backtest::Trade::exit_ns)
        .def_readonly("side", &hft::backtest::Trade::side)
        .def_readonly("signal", &hft::backtest::Trade::signal)
        .def_readonly("entry_mid", &hft::backtest::Trade::entry_mid)
        .def_readonly("exit_mid", &hft::backtest::Trade::exit_mid)
        .def_readonly("gross_return", &hft::backtest::Trade::gross_return)
        .def_readonly("fee_return", &hft::backtest::Trade::fee_return)
        .def_readonly("net_return", &hft::backtest::Trade::net_return);

    py::class_<hft::backtest::Summary>(m, "BacktestSummary")
        .def_readonly("trade_count", &hft::backtest::Summary::trade_count)
        .def_readonly("total_return", &hft::backtest::Summary::total_return)
        .def_readonly("mean_trade_return", &hft::backtest::Summary::mean_trade_return)
        .def_readonly("median_trade_return", &hft::backtest::Summary::median_trade_return)
        .def_readonly("std_trade_return", &hft::backtest::Summary::std_trade_return)
        .def_readonly("win_rate", &hft::backtest::Summary::win_rate)
        .def_readonly("profit_factor", &hft::backtest::Summary::profit_factor)
        .def_readonly("sharpe_trade", &hft::backtest::Summary::sharpe_trade)
        .def_readonly("sharpe_daily_annualized",
                      &hft::backtest::Summary::sharpe_daily_annualized)
        .def_readonly("max_drawdown", &hft::backtest::Summary::max_drawdown)
        .def_readonly("total_fees", &hft::backtest::Summary::total_fees)
        .def_readonly("average_holding_ns", &hft::backtest::Summary::average_holding_ns)
        .def_readonly("trades", &hft::backtest::Summary::trades);

    py::class_<hft::sdk::EngineConfig>(m, "EngineConfig")
        .def(py::init<>())
        .def_readwrite("thresholds", &hft::sdk::EngineConfig::thresholds)
        .def_readwrite("sample_interval_ns", &hft::sdk::EngineConfig::sample_interval_ns)
        .def_readwrite("max_snapshots", &hft::sdk::EngineConfig::max_snapshots);

    py::class_<hft::sdk::OBIFilterEngine>(m, "OBIFilterEngine")
        .def(py::init<hft::sdk::EngineConfig>(), py::arg("config") = hft::sdk::EngineConfig{})
        .def("process", [](hft::sdk::OBIFilterEngine& engine,
                           const std::vector<hft::MarketEvent>& events) {
            engine.process(events);
        }, py::arg("events"))
        .def("process_arrays", [](hft::sdk::OBIFilterEngine& engine,
                                  const i64_array& order_id,
                                  const i64_array& timestamp_ns,
                                  const i64_array& price_ticks,
                                  const i64_array& quantity,
                                  const i64_array& bid_price_ticks,
                                  const i64_array& ask_price_ticks,
                                  const i64_array& type_codes,
                                  const i64_array& side_codes) {
            engine.process(events_from_arrays(order_id, timestamp_ns, price_ticks,
                                              quantity, bid_price_ticks,
                                              ask_price_ticks, type_codes,
                                              side_codes));
        }, py::arg("order_id"), py::arg("timestamp_ns"), py::arg("price_ticks"),
           py::arg("quantity"), py::arg("bid_price_ticks"), py::arg("ask_price_ticks"),
           py::arg("type_codes"), py::arg("side_codes"))
        .def("process_frame", [](hft::sdk::OBIFilterEngine& engine, py::object frame) {
            const auto numeric = [&](const char* name) {
                const py::object series = frame.attr("__getitem__")(name);
                return py::cast<i64_array>(series.attr("to_numpy")());
            };
            engine.process(events_from_arrays(
                numeric("order_id"), numeric("timestamp_ns"), numeric("price_ticks"),
                numeric("quantity"), numeric("bid_price_ticks"),
                numeric("ask_price_ticks"),
                series_to_i64(frame, "type", "event_type", "event"),
                series_to_i64(frame, "side", nullptr, "side")));
        }, py::arg("frame"))
        .def("snapshots", &hft::sdk::OBIFilterEngine::snapshots)
        .def("metrics", &hft::sdk::OBIFilterEngine::metrics)
        .def("metrics_table", &metrics_table)
        .def("regimes", &hft::sdk::OBIFilterEngine::regimes,
             py::arg("scheme") = hft::obi::FilterScheme::Unfiltered,
             py::arg("horizon") = 0, py::arg("volume") = true)
        .def("backtest", &hft::sdk::OBIFilterEngine::backtest, py::arg("config"))
        .def("reset", &hft::sdk::OBIFilterEngine::reset);

    py::class_<hft::signal::SignalPipeline<>>(m, "SignalPipeline")
        .def(py::init<hft::obi::FilterThresholds, std::int64_t, std::size_t, std::size_t>(),
             py::arg("thresholds") = hft::obi::FilterThresholds{},
             py::arg("sample_interval_ns") = hft::kNanosecondsPerMillisecond * 250,
             py::arg("max_snapshots") = std::size_t{1} << 19,
             py::arg("max_catchup_samples") = std::size_t{10'000})
        .def("process", [](hft::signal::SignalPipeline<>& pipeline,
                           const std::vector<hft::MarketEvent>& events) {
            pipeline.process(events);
        }, py::arg("events"))
        .def("snapshots", &hft::signal::SignalPipeline<>::snapshots)
        .def("metrics", [](const hft::signal::SignalPipeline<>& pipeline) {
            return hft::signal::calculate_metrics(pipeline.snapshots());
        })
        .def("regimes", [](const hft::signal::SignalPipeline<>& pipeline,
                           hft::obi::FilterScheme scheme, std::size_t horizon,
                           bool volume) {
            return hft::signal::calculate_regimes(pipeline.snapshots(), scheme,
                                                   horizon, volume);
        }, py::arg("scheme") = hft::obi::FilterScheme::Unfiltered,
           py::arg("horizon") = 0, py::arg("volume") = true)
        .def("dropped_events", &hft::signal::SignalPipeline<>::dropped_events)
        .def("tracker_dropped", &hft::signal::SignalPipeline<>::tracker_dropped)
        .def("dropped_snapshots", &hft::signal::SignalPipeline<>::dropped_snapshots)
        .def("skipped_samples", &hft::signal::SignalPipeline<>::skipped_samples);
}
