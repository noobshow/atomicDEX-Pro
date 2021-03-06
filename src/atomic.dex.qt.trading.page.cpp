/******************************************************************************
 * Copyright © 2013-2019 The Komodo Platform Developers.                      *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * Komodo Platform software, including this file may be copied, modified,     *
 * propagated or distributed except according to the terms contained in the   *
 * LICENSE file                                                               *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

//! Project Headers
#include "atomic.dex.qt.trading.page.hpp"
#include "atomic.dex.mm2.hpp"
#include "atomic.dex.provider.cex.prices.hpp"
#include "atomic.dex.qt.utilities.hpp"
#include "atomic.threadpool.hpp"

//! Consttructor / Destructor
namespace atomic_dex
{
    trading_page::trading_page(
        entt::registry& registry, ag::ecs::system_manager& system_manager, std::atomic_bool& exit_status, portfolio_model* portfolio, QObject* parent) :
        QObject(parent),
        system(registry), m_system_manager(system_manager),
        m_about_to_exit_the_app(exit_status), m_models{
                                                  {new qt_orderbook_wrapper(m_system_manager, this), new candlestick_charts_model(m_system_manager, this),
                                                   new market_pairs(portfolio, this)}}
    {
        //!
    }
} // namespace atomic_dex

//! Events callback
namespace atomic_dex
{
    void
    trading_page::on_process_orderbook_finished_event(const atomic_dex::process_orderbook_finished& evt) noexcept
    {
        if (not m_about_to_exit_the_app)
        {
            m_actions_queue.push(trading_actions::post_process_orderbook_finished);
            m_models_actions[orderbook_need_a_reset] = evt.is_a_reset;
        }
    }

    void
    trading_page::on_start_fetching_new_ohlc_data_event(const start_fetching_new_ohlc_data& evt)
    {
        get_candlestick_charts()->set_is_currently_fetching(evt.is_a_reset);
    }

    void
    atomic_dex::trading_page::on_refresh_ohlc_event(const atomic_dex::refresh_ohlc_needed& evt) noexcept
    {
        if (not m_about_to_exit_the_app)
        {
            m_actions_queue.push(trading_actions::refresh_ohlc);
            m_models_actions[candlestick_need_a_reset] = evt.is_a_reset;
        }
    }
} // namespace atomic_dex

//! Public QML API
namespace atomic_dex
{
    QVariant
    trading_page::get_raw_mm2_coin_cfg(const QString& ticker) const noexcept
    {
        QVariant       out;
        nlohmann::json j = m_system_manager.get_system<mm2>().get_raw_mm2_ticker_cfg(ticker.toStdString());
        out              = nlohmann_json_object_to_qt_json_object(j);
        return out;
    }

    void
    trading_page::set_current_orderbook(const QString& base, const QString& rel)
    {
        auto& provider        = m_system_manager.get_system<cex_prices_provider>();
        auto [normal, quoted] = provider.is_pair_supported(base.toStdString(), rel.toStdString());
        get_candlestick_charts()->set_is_pair_supported(normal || quoted);
        auto* market_selector_mdl = get_market_pairs_mdl();
        market_selector_mdl->set_left_selected_coin(base);
        market_selector_mdl->set_right_selected_coin(rel);
        dispatcher_.trigger<orderbook_refresh>(base.toStdString(), rel.toStdString());
    }

    void
    trading_page::swap_market_pair()
    {
        auto* market_selector_mdl = get_market_pairs_mdl();
        set_current_orderbook(market_selector_mdl->get_right_selected_coin(), market_selector_mdl->get_left_selected_coin());
    }

    void
    trading_page::on_gui_enter_dex()
    {
        dispatcher_.trigger<gui_enter_trading>();
    }

    void
    trading_page::on_gui_leave_dex()
    {
        dispatcher_.trigger<gui_leave_trading>();
    }

    void
    trading_page::cancel_order(const QString& order_id)
    {
        auto& mm2_system = m_system_manager.get_system<mm2>();
        spawn([&mm2_system, order_id]() {
            ::mm2::api::rpc_cancel_order({order_id.toStdString()});
            mm2_system.process_orders();
        });
    }

    void
    trading_page::cancel_all_orders()
    {
        auto& mm2_system = m_system_manager.get_system<mm2>();
        atomic_dex::spawn([&mm2_system]() {
            ::mm2::api::cancel_all_orders_request req;
            ::mm2::api::rpc_cancel_all_orders(std::move(req));
            mm2_system.process_orders();
        });
    }

    void
    trading_page::cancel_all_orders_by_ticker(const QString& ticker)
    {
        auto& mm2_system = m_system_manager.get_system<mm2>();
        atomic_dex::spawn([&mm2_system, &ticker]() {
            ::mm2::api::cancel_data cd;
            cd.ticker = ticker.toStdString();
            ::mm2::api::cancel_all_orders_request req{{"Coin", cd}};
            ::mm2::api::rpc_cancel_all_orders(std::move(req));
            mm2_system.process_orders();
        });
    }

    QString
    trading_page::place_buy_order(
        const QString& base, const QString& rel, const QString& price, const QString& volume, bool is_created_order, const QString& price_denom,
        const QString& price_numer, const QString& base_nota, const QString& base_confs)
    {
        t_float_50 price_f;
        t_float_50 amount_f;
        t_float_50 total_amount;

        price_f.assign(price.toStdString());
        amount_f.assign(volume.toStdString());
        total_amount = price_f * amount_f;

        t_buy_request req{
            .base             = base.toStdString(),
            .rel              = rel.toStdString(),
            .price            = price.toStdString(),
            .volume           = volume.toStdString(),
            .is_created_order = is_created_order,
            .price_denom      = price_denom.toStdString(),
            .price_numer      = price_numer.toStdString(),
            .base_nota        = base_nota.isEmpty() ? std::optional<bool>{std::nullopt} : boost::lexical_cast<bool>(base_nota.toStdString()),
            .base_confs       = base_confs.isEmpty() ? std::optional<std::size_t>{std::nullopt} : base_confs.toUInt()};
        std::error_code ec;
        auto            answer = m_system_manager.get_system<mm2>().place_buy_order(std::move(req), total_amount, ec);

        if (answer.error.has_value())
        {
            return QString::fromStdString(answer.error.value());
        }
        return "";
    }

    QString
    trading_page::place_sell_order(
        const QString& base, const QString& rel, const QString& price, const QString& volume, bool is_created_order, const QString& price_denom,
        const QString& price_numer, const QString& rel_nota, const QString& rel_confs)
    {
        t_float_50 amount_f;
        amount_f.assign(volume.toStdString());

        t_sell_request req{
            .base             = base.toStdString(),
            .rel              = rel.toStdString(),
            .price            = price.toStdString(),
            .volume           = volume.toStdString(),
            .is_created_order = is_created_order,
            .price_denom      = price_denom.toStdString(),
            .price_numer      = price_numer.toStdString(),
            .rel_nota         = rel_nota.isEmpty() ? std::optional<bool>{std::nullopt} : boost::lexical_cast<bool>(rel_nota.toStdString()),
            .rel_confs        = rel_confs.isEmpty() ? std::optional<std::size_t>{std::nullopt} : rel_confs.toUInt()};
        std::error_code ec;
        auto            answer = m_system_manager.get_system<mm2>().place_sell_order(std::move(req), amount_f, ec);

        if (answer.error.has_value())
        {
            return QString::fromStdString(answer.error.value());
        }
        return "";
    }
} // namespace atomic_dex

//! Public API
namespace atomic_dex
{
    void
    trading_page::disable_coin(const QString& coin) noexcept
    {
        auto* market_selector_mdl = get_market_pairs_mdl();
        if (market_selector_mdl->get_left_selected_coin() == coin)
        {
            market_selector_mdl->set_left_selected_coin("BTC");
            market_selector_mdl->set_right_selected_coin("KMD");
        }
        else if (market_selector_mdl->get_right_selected_coin() == coin)
        {
            market_selector_mdl->set_left_selected_coin("BTC");
            market_selector_mdl->set_right_selected_coin("KMD");
        }
        set_current_orderbook(market_selector_mdl->get_left_selected_coin(), market_selector_mdl->get_right_selected_coin());
    }

    void
    trading_page::clear_models()
    {
        get_market_pairs_mdl()->reset();
    }

    void
    trading_page::update() noexcept
    {
    }

    void
    trading_page::connect_signals()
    {
        dispatcher_.sink<process_orderbook_finished>().connect<&trading_page::on_process_orderbook_finished_event>(*this);
        dispatcher_.sink<start_fetching_new_ohlc_data>().connect<&trading_page::on_start_fetching_new_ohlc_data_event>(*this);
        dispatcher_.sink<refresh_ohlc_needed>().connect<&trading_page::on_refresh_ohlc_event>(*this);
    }

    void
    atomic_dex::trading_page::disconnect_signals()
    {
        dispatcher_.sink<process_orderbook_finished>().disconnect<&trading_page::on_process_orderbook_finished_event>(*this);
        dispatcher_.sink<start_fetching_new_ohlc_data>().disconnect<&trading_page::on_start_fetching_new_ohlc_data_event>(*this);
        dispatcher_.sink<refresh_ohlc_needed>().disconnect<&trading_page::on_refresh_ohlc_event>(*this);
    }

    void
    trading_page::process_action()
    {
        if (m_actions_queue.empty() || m_about_to_exit_the_app)
        {
            return;
        }
        const auto&     mm2_system = m_system_manager.get_system<mm2>();
        trading_actions last_action;
        this->m_actions_queue.pop(last_action);
        if (mm2_system.is_mm2_running())
        {
            switch (last_action)
            {
            case trading_actions::refresh_ohlc:
                m_models_actions[candlestick_need_a_reset] ? get_candlestick_charts()->init_data() : get_candlestick_charts()->update_data();
                break;
            case trading_actions::post_process_orderbook_finished:
            {
                std::error_code    ec;
                t_orderbook_answer result = mm2_system.get_orderbook(ec);
                if (!ec)
                {
                    auto* wrapper = get_orderbook_wrapper();
                    m_models_actions[orderbook_need_a_reset] ? wrapper->reset_orderbook(result) : wrapper->refresh_orderbook(result);
                }
                break;
            }
            default:
                break;
            }
        }
    }
} // namespace atomic_dex

//! Properties
namespace atomic_dex
{
    qt_orderbook_wrapper*
    trading_page::get_orderbook_wrapper() const noexcept
    {
        return qobject_cast<qt_orderbook_wrapper*>(m_models[models::orderbook]);
    }

    candlestick_charts_model*
    trading_page::get_candlestick_charts() const noexcept
    {
        return qobject_cast<candlestick_charts_model*>(m_models[models::ohlc]);
    }

    market_pairs*
    trading_page::get_market_pairs_mdl() const noexcept
    {
        return qobject_cast<market_pairs*>(m_models[models::market_selector]);
    }
} // namespace atomic_dex