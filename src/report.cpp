#include "report.hpp"

#include "messages/collect_trigger_id.hpp"
#include "messages/trigger_presence_changed_ind.hpp"
#include "messages/update_report_ind.hpp"
#include "report_manager.hpp"
#include "utils/clock.hpp"
#include "utils/contains.hpp"
#include "utils/ensure.hpp"
#include "utils/transform.hpp"

#include <phosphor-logging/log.hpp>
#include <sdbusplus/vtable.hpp>

#include <limits>
#include <numeric>
#include <optional>

Report::Report(boost::asio::io_context& ioc,
               const std::shared_ptr<sdbusplus::asio::object_server>& objServer,
               const std::string& reportId, const std::string& reportName,
               const ReportingType reportingTypeIn,
               std::vector<ReportAction> reportActionsIn,
               const Milliseconds intervalIn, const uint64_t appendLimitIn,
               const ReportUpdates reportUpdatesIn,
               interfaces::ReportManager& reportManager,
               interfaces::JsonStorage& reportStorageIn,
               std::vector<std::shared_ptr<interfaces::Metric>> metricsIn,
               const interfaces::ReportFactory& reportFactory,
               const bool enabledIn, std::unique_ptr<interfaces::Clock> clock,
               Readings readingsIn) :
    id(reportId),
    name(reportName), reportingType(reportingTypeIn), interval(intervalIn),
    reportActions(reportActionsIn.begin(), reportActionsIn.end()),
    sensorCount(getSensorCount(metricsIn)),
    appendLimit(deduceAppendLimit(appendLimitIn)),
    reportUpdates(reportUpdatesIn), readings(std::move(readingsIn)),
    readingsBuffer(std::get<1>(readings),
                   deduceBufferSize(reportUpdates, reportingType)),
    objServer(objServer), metrics(std::move(metricsIn)), timer(ioc),
    triggerIds(collectTriggerIds(ioc)), reportStorage(reportStorageIn),
    enabled(enabledIn), clock(std::move(clock)), messanger(ioc)
{
    readingParameters =
        toReadingParameters(utils::transform(metrics, [](const auto& metric) {
            return metric->dumpConfiguration();
        }));

    readingParametersPastVersion =
        utils::transform(readingParameters, [](const auto& item) {
            const auto& [sensorData, operationType, id, collectionTimeScope,
                         collectionDuration] = item;

            return ReadingParametersPastVersion::value_type(
                std::get<0>(sensorData.front()), operationType, id,
                std::get<1>(sensorData.front()));
        });

    reportActions.insert(ReportAction::logToMetricReportsCollection);

    deleteIface = objServer->add_unique_interface(
        getPath(), deleteIfaceName,
        [this, &ioc, &reportManager](auto& dbusIface) {
            dbusIface.register_method("Delete", [this, &ioc, &reportManager] {
                if (persistency)
                {
                    persistency = false;

                    reportIface->signal_property("Persistency");
                }

                boost::asio::post(ioc, [this, &reportManager] {
                    reportManager.removeReport(this);
                });
            });
        });

    persistency = storeConfiguration();
    reportIface = makeReportInterface(reportFactory);

    updateReportingType(reportingType);

    if (enabled)
    {
        for (auto& metric : this->metrics)
        {
            metric->initialize();
        }
    }

    messanger.on_receive<messages::TriggerPresenceChangedInd>(
        [this](const auto& msg) {
            const auto oldSize = triggerIds.size();

            if (msg.presence == messages::Presence::Exist)
            {
                if (utils::contains(msg.reportIds, id))
                {
                    triggerIds.insert(msg.triggerId);
                }
                else if (!utils::contains(msg.reportIds, id))
                {
                    triggerIds.erase(msg.triggerId);
                }
            }
            else if (msg.presence == messages::Presence::Removed)
            {
                triggerIds.erase(msg.triggerId);
            }

            if (triggerIds.size() != oldSize)
            {
                reportIface->signal_property("TriggerIds");
            }
        });

    messanger.on_receive<messages::UpdateReportInd>([this](const auto& msg) {
        if (utils::contains(msg.reportIds, id))
        {
            updateReadings();
        }
    });
}

Report::~Report()
{
    if (persistency)
    {
        if (shouldStoreMetricValues())
        {
            storeConfiguration();
        }
    }
    else
    {
        reportStorage.remove(reportFileName());
    }
}

uint64_t Report::getSensorCount(
    const std::vector<std::shared_ptr<interfaces::Metric>>& metrics)
{
    uint64_t sensorCount = 0;
    for (auto& metric : metrics)
    {
        sensorCount += metric->sensorCount();
    }
    return sensorCount;
}

std::optional<uint64_t>
    Report::deduceAppendLimit(const uint64_t appendLimitIn) const
{
    if (appendLimitIn == std::numeric_limits<uint64_t>::max())
    {
        return std::nullopt;
    }
    else
    {
        return appendLimitIn;
    }
}

uint64_t Report::deduceBufferSize(const ReportUpdates reportUpdatesIn,
                                  const ReportingType reportingTypeIn) const
{
    if (reportUpdatesIn == ReportUpdates::overwrite ||
        reportingTypeIn == ReportingType::onRequest)
    {
        return sensorCount;
    }
    else
    {
        return appendLimit.value_or(sensorCount);
    }
}

void Report::setReadingBuffer(const ReportUpdates newReportUpdates)
{
    if (reportingType != ReportingType::onRequest &&
        (reportUpdates == ReportUpdates::overwrite ||
         newReportUpdates == ReportUpdates::overwrite))
    {
        readingsBuffer.clearAndResize(
            deduceBufferSize(newReportUpdates, reportingType));
    }
}

void Report::setReportUpdates(const ReportUpdates newReportUpdates)
{
    if (reportUpdates != newReportUpdates)
    {
        setReadingBuffer(newReportUpdates);
        reportUpdates = newReportUpdates;
    }
}

std::unique_ptr<sdbusplus::asio::dbus_interface>
    Report::makeReportInterface(const interfaces::ReportFactory& reportFactory)
{
    auto dbusIface =
        objServer->add_unique_interface(getPath(), reportIfaceName);
    dbusIface->register_property_rw(
        "Enabled", enabled, sdbusplus::vtable::property_::emits_change,
        [this](bool newVal, const auto&) {
            if (newVal != enabled)
            {
                if (true == newVal && ReportingType::periodic == reportingType)
                {
                    scheduleTimerForPeriodicReport(interval);
                }
                if (newVal)
                {
                    for (auto& metric : metrics)
                    {
                        metric->initialize();
                    }
                }
                else
                {
                    for (auto& metric : metrics)
                    {
                        metric->deinitialize();
                    }
                }

                enabled = newVal;
                persistency = storeConfiguration();
            }
            return 1;
        },
        [this](const auto&) { return enabled; });
    dbusIface->register_property_rw(
        "Interval", interval.count(),
        sdbusplus::vtable::property_::emits_change,
        [this](uint64_t newVal, auto&) {
            if (Milliseconds newValT{newVal};
                newValT >= ReportManager::minInterval)
            {
                if (newValT != interval)
                {
                    interval = newValT;
                    persistency = storeConfiguration();
                }
                return 1;
            }
            throw sdbusplus::exception::SdBusError(
                static_cast<int>(std::errc::invalid_argument),
                "Invalid interval");
        },
        [this](const auto&) { return interval.count(); });
    dbusIface->register_property_rw(
        "Persistency", persistency, sdbusplus::vtable::property_::emits_change,
        [this](bool newVal, const auto&) {
            if (newVal == persistency)
            {
                return 1;
            }
            if (newVal)
            {
                persistency = storeConfiguration();
            }
            else
            {
                reportStorage.remove(reportFileName());
                persistency = false;
            }
            return 1;
        },
        [this](const auto&) { return persistency; });

    dbusIface->register_property_r("Readings", readings,
                                   sdbusplus::vtable::property_::emits_change,
                                   [this](const auto&) { return readings; });
    dbusIface->register_property_rw(
        "ReportingType", std::string(),
        sdbusplus::vtable::property_::emits_change,
        [this](auto newVal, auto& oldVal) {
            ReportingType tmp = utils::toReportingType(newVal);
            if (tmp != reportingType)
            {
                if (tmp == ReportingType::periodic)
                {
                    if (interval < ReportManager::minInterval)
                    {
                        throw sdbusplus::exception::SdBusError(
                            static_cast<int>(std::errc::invalid_argument),
                            "Invalid interval");
                    }
                }

                updateReportingType(tmp);
                setReadingBuffer(reportUpdates);
                persistency = storeConfiguration();
                oldVal = std::move(newVal);
            }
            return 1;
        },
        [this](const auto&) { return utils::enumToString(reportingType); });
    dbusIface->register_property_r(
        "ReadingParameters", readingParametersPastVersion,
        sdbusplus::vtable::property_::const_,
        [this](const auto&) { return readingParametersPastVersion; });
    dbusIface->register_property_rw(
        "ReadingParametersFutureVersion", readingParameters,
        sdbusplus::vtable::property_::emits_change,
        [this, &reportFactory](auto newVal, auto& oldVal) {
            reportFactory.updateMetrics(metrics, enabled, newVal);
            readingParameters = toReadingParameters(
                utils::transform(metrics, [](const auto& metric) {
                    return metric->dumpConfiguration();
                }));
            persistency = storeConfiguration();
            oldVal = std::move(newVal);
            return 1;
        },
        [this](const auto&) { return readingParameters; });
    dbusIface->register_property_r(
        "EmitsReadingsUpdate", bool{}, sdbusplus::vtable::property_::none,
        [this](const auto&) {
            return reportActions.contains(ReportAction::emitsReadingsUpdate);
        });
    dbusIface->register_property_r("Name", std::string{},
                                   sdbusplus::vtable::property_::const_,
                                   [this](const auto&) { return name; });
    dbusIface->register_property_r(
        "LogToMetricReportsCollection", bool{},
        sdbusplus::vtable::property_::const_, [this](const auto&) {
            return reportActions.contains(
                ReportAction::logToMetricReportsCollection);
        });
    dbusIface->register_property_rw(
        "ReportActions", std::vector<std::string>{},
        sdbusplus::vtable::property_::emits_change,
        [this](auto newVal, auto& oldVal) {
            auto tmp = utils::transform<std::unordered_set>(
                newVal, [](const auto& reportAction) {
                    return utils::toReportAction(reportAction);
                });
            tmp.insert(ReportAction::logToMetricReportsCollection);

            if (tmp != reportActions)
            {
                reportActions = tmp;
                persistency = storeConfiguration();
                oldVal = std::move(newVal);
            }
            return 1;
        },
        [this](const auto&) {
            return utils::transform<std::vector>(
                reportActions, [](const auto reportAction) {
                    return utils::enumToString(reportAction);
                });
        });
    dbusIface->register_property_r(
        "AppendLimit", appendLimit.value_or(sensorCount),
        sdbusplus::vtable::property_::emits_change,
        [this](const auto&) { return appendLimit.value_or(sensorCount); });
    dbusIface->register_property_rw(
        "ReportUpdates", std::string(),
        sdbusplus::vtable::property_::emits_change,
        [this](auto newVal, auto& oldVal) {
            setReportUpdates(utils::toReportUpdates(newVal));
            oldVal = newVal;
            return 1;
        },
        [this](const auto&) { return utils::enumToString(reportUpdates); });
    dbusIface->register_property_r(
        "TriggerIds", std::vector<std::string>{},
        sdbusplus::vtable::property_::emits_change, [this](const auto&) {
            return std::vector<std::string>(triggerIds.begin(),
                                            triggerIds.end());
        });
    dbusIface->register_method("Update", [this] {
        if (reportingType == ReportingType::onRequest)
        {
            updateReadings();
        }
    });
    constexpr bool skipPropertiesChangedSignal = true;
    dbusIface->initialize(skipPropertiesChangedSignal);
    return dbusIface;
}

void Report::timerProcForPeriodicReport(boost::system::error_code ec,
                                        Report& self)
{
    if (ec)
    {
        return;
    }

    self.updateReadings();
    self.scheduleTimerForPeriodicReport(self.interval);
}

void Report::timerProcForOnChangeReport(boost::system::error_code ec,
                                        Report& self)
{
    if (ec)
    {
        return;
    }

    const auto ensure =
        utils::Ensure{[&self] { self.onChangeContext = std::nullopt; }};

    self.onChangeContext.emplace(self);

    const auto steadyTimestamp = self.clock->steadyTimestamp();

    for (auto& metric : self.metrics)
    {
        metric->updateReadings(steadyTimestamp);
    }

    self.scheduleTimerForOnChangeReport();
}

void Report::scheduleTimerForPeriodicReport(Milliseconds timerInterval)
{
    if (!enabled)
    {
        return;
    }

    timer.expires_after(timerInterval);
    timer.async_wait([this](boost::system::error_code ec) {
        timerProcForPeriodicReport(ec, *this);
    });
}

void Report::scheduleTimerForOnChangeReport()
{
    if (!enabled)
    {
        return;
    }

    constexpr Milliseconds timerInterval{100};

    timer.expires_after(timerInterval);
    timer.async_wait([this](boost::system::error_code ec) {
        timerProcForOnChangeReport(ec, *this);
    });
}

void Report::updateReadings()
{
    if (!enabled)
    {
        return;
    }

    if (reportUpdates == ReportUpdates::overwrite ||
        reportingType == ReportingType::onRequest)
    {
        readingsBuffer.clear();
    }

    for (const auto& metric : metrics)
    {
        for (const auto& [id, metadata, value, timestamp] :
             metric->getUpdatedReadings())
        {
            if (reportUpdates == ReportUpdates::appendStopsWhenFull &&
                readingsBuffer.isFull())
            {
                enabled = false;
                for (auto& m : metrics)
                {
                    m->deinitialize();
                }
                break;
            }
            readingsBuffer.emplace(id, metadata, value, timestamp);
        }
    }

    std::get<0>(readings) =
        std::chrono::duration_cast<Milliseconds>(clock->systemTimestamp())
            .count();

    if (utils::contains(reportActions, ReportAction::emitsReadingsUpdate))
    {
        reportIface->signal_property("Readings");
    }
}

bool Report::shouldStoreMetricValues() const
{
    return reportingType != ReportingType::onRequest &&
           reportUpdates == ReportUpdates::appendStopsWhenFull;
}

bool Report::storeConfiguration() const
{
    try
    {
        nlohmann::json data;

        data["Enabled"] = enabled;
        data["Version"] = reportVersion;
        data["Id"] = id;
        data["Name"] = name;
        data["ReportingType"] = utils::toUnderlying(reportingType);
        data["ReportActions"] =
            utils::transform(reportActions, [](const auto reportAction) {
                return utils::toUnderlying(reportAction);
            });
        data["Interval"] = interval.count();
        data["AppendLimit"] =
            appendLimit.value_or(std::numeric_limits<uint64_t>::max());
        data["ReportUpdates"] = utils::toUnderlying(reportUpdates);
        data["ReadingParameters"] =
            utils::transform(metrics, [](const auto& metric) {
                return metric->dumpConfiguration();
            });

        if (shouldStoreMetricValues())
        {
            data["MetricValues"] = utils::toLabeledReadings(readings);
        }

        reportStorage.store(reportFileName(), data);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to store a report in storage",
            phosphor::logging::entry("EXCEPTION_MSG=%s", e.what()));
        return false;
    }

    return true;
}

interfaces::JsonStorage::FilePath Report::reportFileName() const
{
    return interfaces::JsonStorage::FilePath{
        std::to_string(std::hash<std::string>{}(id))};
}

std::unordered_set<std::string>
    Report::collectTriggerIds(boost::asio::io_context& ioc) const
{
    utils::Messanger tmp(ioc);

    auto result = std::unordered_set<std::string>();

    tmp.on_receive<messages::CollectTriggerIdResp>(
        [&result](const auto& msg) { result.insert(msg.triggerId); });

    tmp.send(messages::CollectTriggerIdReq{id});

    return result;
}

void Report::metricUpdated()
{
    if (onChangeContext)
    {
        onChangeContext->metricUpdated();
        return;
    }

    updateReadings();
}

void Report::updateReportingType(ReportingType newReportingType)
{
    if (reportingType != newReportingType)
    {
        timer.cancel();
        unregisterFromMetrics = nullptr;
    }

    reportingType = newReportingType;

    switch (reportingType)
    {
        case ReportingType::periodic:
        {
            scheduleTimerForPeriodicReport(interval);
            break;
        }
        case ReportingType::onChange:
        {
            unregisterFromMetrics = [this] {
                for (auto& metric : metrics)
                {
                    metric->unregisterFromUpdates(*this);
                }
            };

            bool isTimerRequired = false;

            for (auto& metric : metrics)
            {
                metric->registerForUpdates(*this);
                if (metric->isTimerRequired())
                {
                    isTimerRequired = true;
                }
            }

            if (isTimerRequired)
            {
                scheduleTimerForOnChangeReport();
            }
            break;
        }
        default:
            break;
    }
}
