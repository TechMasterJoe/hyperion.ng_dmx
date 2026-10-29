#include "LedDeviceDMX.h"

#include <QSerialPort>
#include <QJsonArray>
#include <QJsonValue>
#ifndef _WIN32
#include <time.h>
#endif

LedDeviceDMX::LedDeviceDMX(const QJsonObject &deviceConfig)
	: ProviderRs232(deviceConfig)
	, _dmxDeviceType(0)
	, _dmxStart(1)
	, _dmxSlotsPerLed(3)
	, _dmxLedCount(0)
	, _dmxChannelCount(0)
{
}

LedDevice* LedDeviceDMX::construct(const QJsonObject &deviceConfig)
{
	return new LedDeviceDMX(deviceConfig);
}

bool LedDeviceDMX::parseCustomFixtures(const QJsonObject &deviceConfig)
{
	_fixtures.clear();
	_ledFixtureMap.clear();
	_useCustomFixtures = false;

	if (!deviceConfig.contains("customFixtures") || !deviceConfig["customFixtures"].isArray())
	{
		return true;
	}

	QJsonArray fixturesArray = deviceConfig["customFixtures"].toArray();
	if (fixturesArray.isEmpty())
		return true;

	for (const QJsonValue& val : fixturesArray)
	{
		if (!val.isObject())
			continue;

		QJsonObject obj = val.toObject();
		DmxFixture fixture;
		fixture.name = obj["name"].toString("Unnamed");
		fixture.channelCount = obj["channelCount"].toInt(3);
		if (fixture.channelCount < 1 || fixture.channelCount > 512)
			fixture.channelCount = 3;

		if (obj.contains("channelRoles") && obj["channelRoles"].isArray())
		{
			QJsonArray roles = obj["channelRoles"].toArray();
			for (const QJsonValue& r : roles)
				fixture.roles.push_back(r.toString().toUpper());
		}
		else if (obj.contains("channelConfig"))
		{
			QString cfg = obj["channelConfig"].toString().toUpper();
			for (QChar c : cfg)
				fixture.roles.push_back(QString(c));
			fixture.channelCount = static_cast<int>(fixture.roles.size());
		}

		while (static_cast<int>(fixture.roles.size()) < fixture.channelCount)
			fixture.roles.push_back("L");
		if (static_cast<int>(fixture.roles.size()) > fixture.channelCount)
			fixture.roles.resize(fixture.channelCount);

		if (obj.contains("fixedValues") && obj["fixedValues"].isArray())
		{
			QJsonArray fixed = obj["fixedValues"].toArray();
			for (const QJsonValue& f : fixed)
				fixture.fixedValues.push_back(static_cast<uint8_t>(qBound(0, f.toInt(0), 255)));
		}
		while (fixture.fixedValues.size() < static_cast<size_t>(fixture.channelCount))
			fixture.fixedValues.push_back(0);

		_fixtures.push_back(fixture);
		Debug(_log, "Loaded custom USB-DMX fixture '%s' with %d channels", QSTRING_CSTR(fixture.name), fixture.channelCount);
	}

	if (_fixtures.empty())
		return true;

	_useCustomFixtures = true;

	if (deviceConfig.contains("ledFixtureMap") && deviceConfig["ledFixtureMap"].isArray())
	{
		QJsonArray map = deviceConfig["ledFixtureMap"].toArray();
		for (const QJsonValue& m : map)
		{
			int idx = m.toInt(0);
			if (idx < 0 || idx >= static_cast<int>(_fixtures.size()))
				idx = 0;
			_ledFixtureMap.push_back(idx);
		}
	}

	Info(_log, "Custom USB-DMX fixtures enabled: %d fixture type(s)", static_cast<int>(_fixtures.size()));
	return true;
}

bool LedDeviceDMX::init(const QJsonObject &deviceConfig)
{
	// Initialise sub-class
	if ( !ProviderRs232::init(deviceConfig) )
	{
		return false;
	}

	QString dmxTypeString = deviceConfig["dmxtype"].toString("invalid");
	if (dmxTypeString == "raw")
	{
		_dmxDeviceType = 0;
		_dmxStart = 1;
		_dmxSlotsPerLed = 3;
	}
	else if (dmxTypeString == "McCrypt")
	{
		_dmxDeviceType = 1;
		_dmxStart = 1;
		_dmxSlotsPerLed = 4;
	}
	else if (dmxTypeString == "custom")
	{
		_dmxDeviceType = 2;
		_dmxStart = 1;
		_dmxSlotsPerLed = 3; // placeholder, overridden by fixtures
	}
	else
	{
		QString errortext = QString ("unknown dmx device type: %1").arg(dmxTypeString);
		this->setInError(errortext);
		return false;
	}

	Debug(_log, "_dmxTypeString \"%s\", _dmxDeviceType %d", QSTRING_CSTR(dmxTypeString), _dmxDeviceType );
	_rs232Port.setStopBits(QSerialPort::TwoStop);

	// White algorithm (optional, for W channels)
	QString whiteAlgorithmStr = deviceConfig["whiteAlgorithm"].toString("white_off");
	_whiteAlgorithm = RGBW::stringToWhiteAlgorithm(whiteAlgorithmStr);
	if (_whiteAlgorithm == RGBW::WhiteAlgorithm::INVALID)
	{
		_whiteAlgorithm = RGBW::WhiteAlgorithm::WHITE_OFF;
	}

	// Parse custom fixtures
	if (!parseCustomFixtures(deviceConfig))
		return false;

	if (_useCustomFixtures && !_fixtures.empty())
	{
		// Estimate max channels needed (worst case all LEDs use the largest fixture)
		int maxCh = 3;
		for (const auto& f : _fixtures)
			if (f.channelCount > maxCh)
				maxCh = f.channelCount;
		_dmxSlotsPerLed = maxCh;
		_dmxLedCount = qMin(static_cast<int>(_ledCount), 512 / _dmxSlotsPerLed);
		_dmxChannelCount = 1 + _dmxSlotsPerLed * _dmxLedCount; // conservative upper bound
		if (_dmxChannelCount > 513)
			_dmxChannelCount = 513;
	}
	else
	{
		_dmxLedCount  =  qMin(static_cast<int>(_ledCount), 512/_dmxSlotsPerLed);
		_dmxChannelCount  = 1 + _dmxSlotsPerLed * _dmxLedCount;
	}

	Debug(_log, "_dmxStart %d, _dmxSlotsPerLed %d", _dmxStart, _dmxSlotsPerLed);
	Debug(_log, "_ledCount %d, _dmxLedCount %d, _dmxChannelCount %d", _ledCount, _dmxLedCount, _dmxChannelCount);

	_ledBuffer.fill(0x00, _dmxChannelCount);
	_ledBuffer[0] = 0x00;	// NULL START code

	return true;
}

void LedDeviceDMX::writeFixtureChannels(uint8_t* dmxData, int& dmxIdx, const ColorRgb& color, const DmxFixture& fixture)
{
	ColorRgbw rgbw;
	bool hasWhite = false;
	for (const QString& role : fixture.roles)
	{
		if (role == "W")
		{
			hasWhite = true;
			break;
		}
	}

	if (hasWhite && _whiteAlgorithm != RGBW::WhiteAlgorithm::WHITE_OFF)
	{
		RGBW::Rgb_to_Rgbw(color, &rgbw, _whiteAlgorithm);
	}
	else
	{
		rgbw.red = color.red;
		rgbw.green = color.green;
		rgbw.blue = color.blue;
		rgbw.white = 0;
	}

	for (int i = 0; i < fixture.channelCount; ++i)
	{
		if (dmxIdx >= static_cast<int>(_ledBuffer.size()))
			break;

		const QString& role = fixture.roles[static_cast<size_t>(i)];
		uint8_t value = 0;

		if (role == "R")
			value = rgbw.red;
		else if (role == "G")
			value = rgbw.green;
		else if (role == "B")
			value = rgbw.blue;
		else if (role == "W")
			value = rgbw.white;
		else if (role == "H")
			value = 255;
		else if (role == "L")
			value = 0;
		else if (role == "F" && i < static_cast<int>(fixture.fixedValues.size()))
			value = fixture.fixedValues[static_cast<size_t>(i)];
		else
			value = (i < static_cast<int>(fixture.fixedValues.size())) ? fixture.fixedValues[static_cast<size_t>(i)] : 0;

		dmxData[dmxIdx++] = value;
	}
}

int LedDeviceDMX::write(const QVector<ColorRgb> &ledValues)
{
	if (_useCustomFixtures && !_fixtures.empty())
	{
		_ledBuffer.fill(0x00);
		_ledBuffer[0] = 0x00; // start code

		int dmxIdx = _dmxStart;
		int ledIndex = 0;
		int maxLeds = qMin(ledValues.size(), _dmxLedCount);

		for (int d = 0; d < maxLeds; ++d)
		{
			int fixtureIdx = 0;
			if (ledIndex < static_cast<int>(_ledFixtureMap.size()))
				fixtureIdx = _ledFixtureMap[static_cast<size_t>(ledIndex)];
			else if (!_ledFixtureMap.empty())
				fixtureIdx = _ledFixtureMap.back();

			if (fixtureIdx >= 0 && fixtureIdx < static_cast<int>(_fixtures.size()))
			{
				writeFixtureChannels(reinterpret_cast<uint8_t*>(_ledBuffer.data()), dmxIdx, ledValues[d], _fixtures[static_cast<size_t>(fixtureIdx)]);
			}
			ledIndex++;
		}

		// Update actual channel count used
		unsigned int used = static_cast<unsigned int>(dmxIdx);
		if (used > 0 && used <= _ledBuffer.size())
		{
			// Keep buffer size as allocated; writeBytes uses the allocated size for safety
		}
	}
	else
	{
		switch (_dmxDeviceType) {
		case 0:
			memcpy(_ledBuffer.data()+1, ledValues.data(), _dmxChannelCount-1);
			break;
		case 1:
			int l =_dmxStart;
			for (int d=0; d<_dmxLedCount; d++)
			{
				_ledBuffer[l++] = ledValues[d].red;
				_ledBuffer[l++] = ledValues[d].green;
				_ledBuffer[l++] = ledValues[d].blue;
				_ledBuffer[l++] = 255;
			}
			break;
		}
	}

	_rs232Port.setBreakEnabled(true);
// Note Windows: There is no concept of ns sleeptime, the closest possible is 1ms but requested is 0,000176ms
#ifndef _WIN32
	nanosleep((const struct timespec[]){{0, 176000L}}, NULL);	// 176 uSec break time
#endif
	_rs232Port.setBreakEnabled(false);
#ifndef _WIN32
	nanosleep((const struct timespec[]){{0, 12000L}}, NULL);	// 176 uSec make after break time
#endif

#undef uberdebug
#ifdef uberdebug
	printf ("Writing %d bytes", _dmxChannelCount);
	for (unsigned int i=0; i < _dmxChannelCount; i++)
	{
		if (i%32 == 0) {
			printf ("\n%04x: ", i);
		}
		printf ("%02x ", _ledBuffer[i]);
	}
	printf ("\n");
#endif

	return writeBytes(_dmxChannelCount, _ledBuffer.data());
}
