/* Copyright 2018, OpenSoft Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice, this list of
 * conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright notice, this list of
 * conditions and the following disclaimer in the documentation and/or other materials provided
 * with the distribution.
 *     * Neither the name of OpenSoft Inc. nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: denis.kormalev@opensoftdev.com (Denis Kormalev)
 *
 */
#include "weighingscalehandler.h"

#include "proofservice-weighing-scale_global.h"

#include "proofcore/proofobject.h"

#include <cmath>

constexpr int READ_TIMEOUT = 500;
constexpr int READ_ERROR_TIMEOUT = 5000;
constexpr int ERROR_COOLDOWN_TIMEOUT = 100;

uint qHash(WeighingScaleHandler::Unit value, uint seed)
{
    return qHash(static_cast<int>(value), seed);
}

uint qHash(WeighingScaleHandler::Status value, uint seed)
{
    return qHash(static_cast<int>(value), seed);
}

WeighingScaleHandler::WeighingScaleHandler(unsigned short vendorId, unsigned short productId, QObject *parent)
    : QThread(parent), m_vendorId(vendorId), m_productId(productId)
{}

WeighingScaleHandler::State WeighingScaleHandler::lastStableState() const
{
    return extractState(m_lastStableState);
}

WeighingScaleHandler::State WeighingScaleHandler::instantState() const
{
    return extractState(m_instantState);
}

void WeighingScaleHandler::execOnNextStableWeight(std::function<void(State)> &&handler)
{
    m_stableWaitersLock.lockForWrite();
    m_stableWaiters << handler;
    m_stableWaitersLock.unlock();
}

bool WeighingScaleHandler::isAlive() const
{
    return m_hidHandle;
}

int WeighingScaleHandler::elapsedSinceLastMessage() const
{
    return m_lastSuccessfulRead.elapsed();
}

unsigned short WeighingScaleHandler::vendorId() const
{
    return m_vendorId;
}

unsigned short WeighingScaleHandler::productId() const
{
    return m_productId;
}

void WeighingScaleHandler::stop()
{
    m_stopped = true;
}

void WeighingScaleHandler::run()
{
    int hidResult = hid_init();
    if (hidResult < 0) {
        qCCritical(proofServiceWeighingScaleLog) << "HID API can't be initialized, going down";
        return;
    }

    m_hidHandle = hid_open(m_vendorId, m_productId, nullptr);
    if (!m_hidHandle) {
        qCCritical(proofServiceWeighingScaleLog) << "HID device can't be opened, going down";
        return;
    }

    auto stateUpdater = [this](uint64_t packedState) {
        m_instantState = packedState;
        if (extractStateStatus(packedState) != Status::InMotion) {
            m_lastStableState = packedState;
            m_stableWaitersLock.lockForRead();
            bool waitersExist = !m_stableWaiters.empty();
            m_stableWaitersLock.unlock();
            if (waitersExist) {
                auto state = extractState(packedState);
                m_stableWaitersLock.lockForWrite();
                while (!m_stableWaiters.isEmpty())
                    m_stableWaiters.takeFirst()(state);
                m_stableWaitersLock.unlock();
            }
        }
    };

    unsigned char data[6];
    memset(data, 0, 6);
    bool warningShown = false;
    m_lastSuccessfulRead.start();
    while (!m_stopped) {
        hidResult = m_hidHandle ? hid_read_timeout(m_hidHandle, data, 6, READ_TIMEOUT) : 0;
        if (hidResult < 6 || data[0] != 3) {
            if (m_lastSuccessfulRead.elapsed() > READ_ERROR_TIMEOUT) {
                if (!warningShown) {
                    qCWarning(proofServiceWeighingScaleLog) << "No readable data from device, setting state to error";
                    warningShown = true;
                }
                stateUpdater(packState(Status::Fault, Unit::Tael, 0, 0));
                hid_close(m_hidHandle);
                QThread::msleep(ERROR_COOLDOWN_TIMEOUT);
                m_hidHandle = hid_open(m_vendorId, m_productId, nullptr);
            }
            continue;
        }
        if (warningShown) {
            warningShown = false;
            qCDebug(proofServiceWeighingScaleLog) << "Device connection is live again";
        }
        m_lastSuccessfulRead.restart();
        stateUpdater(packState(static_cast<Status>(qMin(data[1], static_cast<unsigned char>(Status::NotInitialized))),
                               static_cast<Unit>(qMin(data[2], static_cast<unsigned char>(Unit::Pound))),
                               static_cast<short>((static_cast<uint32_t>(data[5]) << 8u) | static_cast<uint32_t>(data[4])),
                               static_cast<char>(data[3])));
    }
    hid_close(m_hidHandle);
    hid_exit();
}

uint64_t WeighingScaleHandler::packState(Status status, Unit unit, short weight, char scaleFactor) const
{
    uint64_t result = static_cast<unsigned short>(weight);
    result |= static_cast<uint64_t>(scaleFactor) << 16u;
    result |= static_cast<uint64_t>(unit) << 24u;
    result |= static_cast<uint64_t>(status) << 32u;
    return result;
}

WeighingScaleHandler::State WeighingScaleHandler::extractState(uint64_t state) const
{
    double value = static_cast<double>(static_cast<short>(state & 0xFFFFu))
                   * pow(10, static_cast<char>((state >> 16u) & 0xFFu));
    return State{static_cast<Status>((state >> 32u) & 0xFFu), static_cast<Unit>((state >> 24u) & 0xFFu), value};
}

WeighingScaleHandler::Status WeighingScaleHandler::extractStateStatus(uint64_t state) const
{
    return static_cast<Status>((state >> 32u) & 0xFFu);
}

QString WeighingScaleHandler::State::stringifiedStatus() const
{
    static const QHash<WeighingScaleHandler::Status, QString> STATUSES =
        {{WeighingScaleHandler::Status::UnknownStatus, "error"},
         {WeighingScaleHandler::Status::Fault, "error"},
         {WeighingScaleHandler::Status::StableZero, "stable"},
         {WeighingScaleHandler::Status::InMotion, "in motion"},
         {WeighingScaleHandler::Status::Stable, "stable"},
         {WeighingScaleHandler::Status::UnderZero, "under zero"},
         {WeighingScaleHandler::Status::Overweight, "overweight"},
         {WeighingScaleHandler::Status::DummyValue, "error"},
         {WeighingScaleHandler::Status::NotInitialized, "not initialized"}};
    return STATUSES.value(status, "");
}

QString WeighingScaleHandler::State::stringifiedUnit() const
{
    static const QHash<WeighingScaleHandler::Unit, QString> UNITS =
        {{WeighingScaleHandler::Unit::UnknownUnit, ""},   {WeighingScaleHandler::Unit::Milligram, "mg"},
         {WeighingScaleHandler::Unit::Gram, "g"},         {WeighingScaleHandler::Unit::Kilogram, "kg"},
         {WeighingScaleHandler::Unit::Carat, "ct"},       {WeighingScaleHandler::Unit::Tael, "tael"},
         {WeighingScaleHandler::Unit::Grain, "gr"},       {WeighingScaleHandler::Unit::Pennyweight, "dwt"},
         {WeighingScaleHandler::Unit::MetricTon, "ton"},  {WeighingScaleHandler::Unit::AvoirTon, "short ton"},
         {WeighingScaleHandler::Unit::TroyOunce, "oz t"}, {WeighingScaleHandler::Unit::Ounce, "oz"},
         {WeighingScaleHandler::Unit::Pound, "lb"}};
    return UNITS.value(unit, "");
}
